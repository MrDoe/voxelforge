#!/usr/bin/env python3
"""Minimal OpenCode V2 HTTP stub for vf_tests' client integration test.

Serves just the endpoints OpenCodeClient uses and emits a few SSE events so
the streaming path is exercised without a real model or network account.

Usage: python3 tests/mock_opencode.py [port]
       VF_OPENCODE_URL=http://127.0.0.1:<port> ./build/vf_tests --test-case=*mock*
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45999
SESSION_ID = "ses_mock00000000000000001"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/api/info":
            return self.send_json({"version": "mock", "pid": 1, "urls": []})
        if p == "/api/mcp":
            return self.send_json({"data": [
                {"name": "voxelforge", "status": {"status": "connected"}}]})
        if p == "/api/model":
            return self.send_json({"data": [
                {"id": "beam-free", "name": "Beam Free", "providerID": "opencode-go",
                 "type": "chat", "capabilities": {"tools": True}},
                {"id": "other", "name": "Other", "providerID": "mock",
                 "type": "chat", "capabilities": {"tools": True}}]})
        if p == "/api/event":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            events = [
                {"type": "server.connected", "data": {}},
                {"type": "session.tool.input.started", "data": {
                    "sessionID": SESSION_ID, "id": "call-mock-1",
                    "name": "voxelforge_get_layer"}},
                {"type": "session.tool.called", "data": {
                    "sessionID": SESSION_ID, "id": "call-mock-1",
                    "input": {"layer": "isle_barn"}}},
                {"type": "session.tool.success", "data": {
                    "sessionID": SESSION_ID, "id": "call-mock-1",
                    "content": [{"type": "text", "text":
                                 "file=isle_barn.vxw voxels=1234"}]}},
                {"type": "session.text.delta", "data": {
                    "sessionID": SESSION_ID,
                    "delta": "Found it: isle_barn has 1234 voxels.\nSecond line after a newline.\nThird line.", "ordinal": 0}},
                {"type": "session.idle", "data": {"sessionID": SESSION_ID}},
            ]
            for ev in events:
                chunk = ("data: " + json.dumps(ev) + "\n\n").encode()
                self.wfile.write(("%X\r\n" % len(chunk)).encode() + chunk + b"\r\n")
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return
        return self.send_json({"_tag": "NotFound"}, 404)

    def do_POST(self):
        p = self.path.split("?")[0]
        if p == "/api/session":
            body = self.body()
            return self.send_json({"data": {"id": SESSION_ID,
                                            "agent": body.get("agent"),
                                            "model": body.get("model")}})
        if p == "/api/experimental/mcp/voxelforge/connect":
            self.body()
            return self.send_json({})
        if p.endswith("/prompt"):
            self.body()
            return self.send_json({"data": {"ok": True}})
        if p.endswith("/model") or p.endswith("/interrupt"):
            self.body()
            return self.send_json({"data": {"ok": True}})
        return self.send_json({"_tag": "NotFound"}, 404)

    def do_PUT(self):
        if self.path.split("?")[0].endswith("/environment"):
            self.body()
            return self.send_json({})
        return self.send_json({"_tag": "NotFound"}, 404)


if __name__ == "__main__":
    with ThreadingHTTPServer(("127.0.0.1", PORT), Handler) as httpd:
        httpd.serve_forever()
