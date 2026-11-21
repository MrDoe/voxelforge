#!/usr/bin/env python3
"""Selector-grammar functional gate: masked component delete must take ONE
tree out of a forest layer without deleting the ground bridge or its neighbour.
Runs in a throwaway scenery; never touches shipped .vxw files.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MCP = ROOT / "build" / "vf_mcp"
SRC = ROOT / "scenery" / "LakesideHamlet"
TMP = Path("/tmp/opencode")


def mcp(env, tool, args):
    p = subprocess.run([str(MCP)], input=json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": tool, "arguments": args}}) + "\n",
        capture_output=True, text=True, env=env, timeout=300)
    for line in p.stdout.splitlines():
        if not line.strip().startswith("{"):
            continue
        o = json.loads(line)
        if "result" in o:
            return o["result"]["content"][0]["text"], bool(o["result"].get("isError"))
    raise RuntimeError(f"no result for {tool}: {p.stdout[-500:]!r}")


def main():
    scratch = TMP / f"mcp_selector_{int(time.time())}"
    scratch.mkdir(parents=True)
    for f in SRC.iterdir():
        if f.name in {"world.json", "runtime_edits.vxw", "ai_edits.vxw"}:
            continue
        if f.is_dir():
            continue
        (scratch / f.name).symlink_to(f)
    shutil.copy2(SRC / "world.json", scratch / "world.json")
    env = os.environ.copy()
    env["VF_SCENERY"] = str(scratch)
    env["VF_NO_OVERLAY"] = "1"
    env["VF_OVERLAY_PATH"] = str(TMP / "mcp_selector_overlay.vxw")
    try:
        name = "mcp_two_trees"
        # Two 3-cell trees at x offset 4 cells, connected by a ground row of
        # mat 2. An UNMASKED component fill removes everything (13k-cell bug).
        voxels = []
        for x in range(100, 110):
            voxels.append({"x": x, "y": 500, "z": 600, "mat": 2})
        for base in (103, 107):
            for dy in range(3):
                voxels.append({"x": base, "y": 501 + dy, "z": 600, "mat": 8})
        text, err = mcp(env, "write_object", {"name": name, "voxels": voxels})
        if err:
            raise RuntimeError(f"write_object: {text}")
        dry, err = mcp(env, "delete_voxels", {
            "layer": name, "anchor": [103, 501, 600], "component": True,
            "mat": 8, "dry_run": True})
        if err or "would carve 3" not in dry:
            raise RuntimeError(f"masked dry_run: {dry}")
        text, err = mcp(env, "delete_voxels", {
            "layer": name, "anchor": [103, 501, 600], "component": True, "mat": 8})
        if err or "carved 3" not in text:
            raise RuntimeError(f"masked component delete: {text}")
        after, err = mcp(env, "read_object", {"name": name})
        n8 = after.count('"mat":8')
        n2 = after.count('"mat":2')
        if n8 != 3:
            raise RuntimeError(f"expected exactly one 3-cell tree left, got {n8} mat-8 cells")
        if n2 != 10:
            raise RuntimeError(f"ground bridge was deleted ({n2} mat-2 cells left)")
        print("PASS: mcp_selector")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
