#!/usr/bin/env python3
"""Functional gate for vf_mcp v2: describe, selector paint, snapshots and the
store-lane stamp bridge. Runs against a THROWAWAY scenery made of symlinks to
the shipped world plus a copied manifest, so no authored file can be touched.
Every app render is hermetic: VF_OVERLAY_PATH points into /tmp/opencode.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MCP = ROOT / "build" / "vf_mcp"
APP = ROOT / "build" / "voxelforge"
TMP = Path("/tmp/opencode")
SRC = ROOT / "scenery" / "LakesideHamlet"


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def mcp(env, tool, args=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": tool, "arguments": args or {}}}
    p = subprocess.run([str(MCP)], input=json.dumps(payload) + "\n",
                       capture_output=True, text=True, env=env, timeout=300)
    if p.returncode != 0:
        fail(f"{tool} exited {p.returncode}: {p.stderr[-800:]}")
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "result" not in o:
            continue
        res = o["result"]
        return res.get("content", [{}])[0].get("text", ""), bool(res.get("isError"))
    fail(f"{tool}: no JSON-RPC result (stdout={p.stdout[-800:]!r}, stderr={p.stderr[-800:]!r})")


def md5(path):
    return hashlib.md5(path.read_bytes()).hexdigest() if path.exists() else "<none>"


def main():
    if not MCP.exists() or not APP.exists():
        fail("build vf_mcp and voxelforge first")
    TMP.mkdir(parents=True, exist_ok=True)
    scratch = TMP / f"mcp_v2_{int(time.time())}"
    scratch.mkdir(parents=True)
    # Symlink authored layers (read-only); COPY the manifest (vf_mcp/app rewrite it).
    for f in SRC.iterdir():
        if f.name in {"world.json", "runtime_edits.vxw", "last.json", "ai_edits.vxw"}:
            continue
        if f.is_dir():
            shutil.copytree(f, scratch / f.name) if f.name == "micro" else None
        elif f.is_file():
            (scratch / f.name).symlink_to(f)
    shutil.copy2(SRC / "world.json", scratch / "world.json")
    # ai_edits is WRITTEN by commit_overlay — copy it, never a symlink to the
    # shipped file (that would turn a scratch test into an authored-file edit).
    if (SRC / "ai_edits.vxw").exists():
        shutil.copy2(SRC / "ai_edits.vxw", scratch / "ai_edits.vxw")
    overlay = TMP / f"mcp_v2_overlay_{int(time.time())}.vxw"
    env = os.environ.copy()
    env["VF_SCENERY"] = str(scratch)
    env["VF_NO_OVERLAY"] = "1"
    env["VF_OVERLAY_PATH"] = str(overlay)
    print(f"scratch: {scratch}")
    try:
        # --- 0. tools/list must be valid JSON and carry the new tools --------
        p = subprocess.run([str(MCP)], input=json.dumps(
            {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}) + "\n",
            capture_output=True, text=True, env=env, timeout=120)
        o = json.loads(p.stdout.strip().splitlines()[0])
        names = {t["name"] for t in o["result"]["tools"]}
        need = {"describe", "paint", "stamp", "stamp_result", "snapshot", "restore_snapshot"}
        if not need <= names:
            fail(f"tools/list missing {need - names}")

        # --- 1. describe kills the probe guessing game -----------------------
        text, err = mcp(env, "ground", {"x": 0, "z": 0})
        m = re.search(r"anchor cell \[(\d+) (\d+) (\d+)\]", text)
        if not m:
            fail(f"ground did not report an anchor: {text!r}")
        cell = [int(m.group(i)) for i in (1, 2, 3)]
        text, err = mcp(env, "describe", {"cell": cell})
        for key in ("cell=", "material=", "owner=", "overlay=", "storeEdited=", "lane="):
            if key not in text:
                fail(f"describe missing {key}: {text!r}")
        print("describe:", text.strip().replace("\n", " | ")[:220])

        # --- 2. snapshot + selector paint on a small authored layer ----------
        text, err = mcp(env, "write_object", {
            "name": "mcp_v2_prop", "anchor": cell,
            "shapes": [{"type": "box", "at": [0, 0, 0], "size": [4, 4, 4], "mat": 6}]})
        if err or "voxels" not in text:
            fail(f"write_object: {text!r}")
        text, err = mcp(env, "read_object", {"name": "mcp_v2_prop"})
        first = re.search(r'\{"x":(\d+),"y":(\d+),"z":(\d+)', text)
        if not first:
            fail(f"read_object: {text[:400]!r}")
        pc = [int(first.group(i)) for i in (1, 2, 3)]

        text, err = mcp(env, "snapshot", {"layer": "mcp_v2_prop"})
        if err or ".bak" not in text:
            fail(f"snapshot: {text!r}")
        bak = re.search(r"(\S+\.bak)", text).group(1)
        if not Path(bak).exists():
            fail(f"snapshot file missing: {bak}")

        dry = {"layer": "mcp_v2_prop", "box": [*pc, pc[0] + 1, pc[1], pc[2]],
               "mat": 7, "dry_run": True}
        text, err = mcp(env, "paint", dry)
        if err or "dry run" not in text:
            fail(f"paint dry_run: {text!r}")
        text, err = mcp(env, "paint", {**dry, "dry_run": False})
        if err or "recoloured" not in text:
            fail(f"paint: {text!r}")
        if "mat 6:" not in text or "mat 7:" not in text:
            fail(f"paint histogram missing material rows: {text!r}")
        print("paint:", text.strip().replace("\n", " | ")[:220])

        # --- 2a. two-call transform: AABB preview, then Apply ----------------
        text, err = mcp(env, "transform", {
            "layer": "mcp_v2_prop", "yaw": 30, "commit": False})
        if err or "PREVIEW only" not in text or "bounds [" not in text:
            fail(f"transform preview: {text!r}")
        text, err = mcp(env, "transform", {
            "layer": "mcp_v2_prop", "yaw": 30, "commit": True})
        if err or "committed" not in text:
            fail(f"transform commit: {text!r}")
        text, err = mcp(env, "get_layer", {"layer": "mcp_v2_prop"})
        if "rot=30" not in text:
            fail(f"transform did not persist: {text!r}")

        # --- 2b. server-side builders + the per-layer Import button ----------
        tree_args = {"name": "mcp_v2_tree_a", "anchor": [600, 520, 600],
                     "height": 2.0, "canopy_r": 0.9, "seed": 42}
        text, err = mcp(env, "add_tree", tree_args)
        if err or "cells (seed 42)" not in text:
            fail(f"add_tree: {text!r}")
        n_a = int(re.search(r": (\d+) cells", text).group(1))
        text, err = mcp(env, "add_tree", {**tree_args, "name": "mcp_v2_tree_b"})
        if err:
            fail(f"add_tree second: {text!r}")
        n_b = int(re.search(r": (\d+) cells", text).group(1))
        if n_a != n_b:
            fail(f"seed determinism: {n_a} != {n_b}")
        text, err = mcp(env, "add_path", {
            "name": "mcp_v2_path", "from": [10, 10], "to": [12, 10],
            "width": 0.4, "step": 0.2, "mat": 3})
        if err or "ground sampled per cell" not in text:
            fail(f"add_path: {text!r}")
        text, err = mcp(env, "place_copy", {
            "layer": "mcp_v2_prop", "anchor": [600, 530, 600]})
        if err or "cells appended" not in text:
            fail(f"place_copy: {text!r}")

        # --- 3. stamp through the intent bridge (the App owns the store) -----
        real_overlay = SRC / "runtime_edits.vxw"
        before = md5(real_overlay)
        q = scratch / "mcp_intents.jsonl"
        r = scratch / "mcp_results.jsonl"
        if r.exists():
            r.unlink()
        intent = {"id": "mcpv2_test_stamp", "tool": "stamp",
                  "args": {"mode": "delete", "cell": cell,
                           "diameter": 0.1, "depth": 0.1}}
        q.write_text(json.dumps(intent) + "\n")
        cmd = [str(APP), "--smoke", "3", "--width", "320", "--height", "180"]
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            fail(f"app --smoke exited {p.returncode}: {p.stderr[-2000:]}")
        if not r.exists():
            fail("app did not write mcp_results.jsonl")
        res = None
        for line in r.read_text().splitlines():
            o = json.loads(line)
            if o.get("id") == intent["id"]:
                res = o
        if not res:
            fail(f"no result for {intent['id']}: {r.read_text()[:1000]}")
        if not res.get("ok"):
            fail(f"stamp result says error: {res}")
        for key in ("cells", "chunks", "runSurfels", "ms", "pick", "undoSteps"):
            if key not in res:
                fail(f"stamp report missing {key}: {res}")
        if res["cells"] < 1 or res["chunks"] < 1 or res["runSurfels"] < 1:
            fail(f"instrument did not fire (stamp went inert): {res}")
        if res["undoSteps"] != 1:
            fail(f"stamp did not create exactly one undo step: {res}")
        if res["cellSolidBefore"] == 1 and res["cellSolidAfter"] != 0:
            fail(f"delete stamp left the picked cell solid: {res}")
        if q.read_text().strip():
            fail("intent queue was not consumed")
        if md5(real_overlay) != before:
            fail("the REAL scenery overlay changed — hermeticity broken")
        print("stamp:", json.dumps(res, sort_keys=True))

        # --- 4. idempotence: a second stamp is a second undo step ------------
        q.write_text(json.dumps({**intent, "id": "mcpv2_test_stamp2"}) + "\n")
        subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600, check=True)
        ids = [json.loads(x)["id"] for x in r.read_text().splitlines()]
        if "mcpv2_test_stamp2" not in ids:
            fail("second stamp was not executed")
        # --- 5. commit_overlay promotes additions into ai_edits.vxw ----------
        ai_edits = scratch / "ai_edits.vxw"
        before_ai = md5(ai_edits)
        # A fresh process: clear any live edits first (the overlay is hermetic).
        clear_env = env.copy()
        clear_env["VF_TEST_CLEAR"] = "1"
        q.write_text("")
        subprocess.run(cmd, env=clear_env, capture_output=True, text=True,
                       timeout=600, check=True)
        q.write_text("\n".join(json.dumps(x) for x in [
            {"id": "mcpv2_add", "tool": "stamp",
             "args": {"mode": "add", "cell": cell, "diameter": 0.1, "depth": 0.1}},
            {"id": "mcpv2_commit", "tool": "commit_overlay", "args": {}},
        ]) + "\n")
        subprocess.run(cmd, env=env, capture_output=True, text=True,
                       timeout=600, check=True)
        commit = None
        for line in r.read_text().splitlines():
            o = json.loads(line)
            if o.get("id") == "mcpv2_commit":
                commit = o
        if not commit or not commit.get("ok"):
            fail(f"commit_overlay failed: {commit}")
        if commit.get("additions", 0) < 1 or commit.get("appended", 0) < 1:
            fail(f"commit_overlay promoted nothing: {commit}")
        if md5(ai_edits) == before_ai:
            fail("commit_overlay did not change ai_edits.vxw")
        print("commit:", json.dumps(commit, sort_keys=True))

        # --- 6. verification primitive: PPM pixel diff -----------------------
        def write_ppm(path, white_at=None):
            w = h = 4
            data = bytearray(w * h * 3)
            if white_at is not None:
                i = (white_at[1] * w + white_at[0]) * 3
                data[i:i + 3] = b"\xff\xff\xff"
            path.write_bytes(f"P6\n{w} {h}\n255\n".encode() + bytes(data))

        shot_a = TMP / "mcp_v2_diff_a.ppm"
        shot_b = TMP / "mcp_v2_diff_b.ppm"
        write_ppm(shot_a)
        write_ppm(shot_b, (1, 1))
        text, err = mcp(env, "diff_shots", {"a": str(shot_a), "b": str(shot_b)})
        if err or "changed=1" not in text or "6.25 %" not in text:
            fail(f"diff_shots: {text!r}")
        text, err = mcp(env, "diff_shots", {"a": str(shot_a), "b": str(shot_a)})
        if err or "changed=0" not in text:
            fail(f"diff_shots identity: {text!r}")
        print("verify:", text.strip())

        # --- 7. micro clip key: quantised, seeded, registered ----------------
        text, err = mcp(env, "clip_key", {
            "layer": "alpaca", "bone": 0, "time": 0.5, "rot": [10, 0, 0],
            "clip": "micro/mcp_v2_test.anim"})
        if err or "bone 0 t=" not in text:
            fail(f"clip_key: {text!r}")
        clip_path = scratch / "micro" / "mcp_v2_test.anim"
        if not clip_path.exists():
            fail("clip_key did not write the .anim")
        anim = json.loads(clip_path.read_text())
        if len(anim["tracks"]) != 1 or len(anim["tracks"][0]["keys"]) != 1:
            fail(f"clip_key track/key count: {anim}")
        key = anim["tracks"][0]["keys"][0]
        if abs(key["t"] - 0.5) > 1e-5 or abs(key["rot"][0] - 10) > 1e-5:
            fail(f"clip_key quantisation/rotation: {key}")
        text, err = mcp(env, "clip_key", {
            "layer": "alpaca", "bone": 0, "time": 0.5, "rot": [20, 0, 0],
            "clip": "micro/mcp_v2_test.anim"})
        if err:
            fail(f"clip_key update: {text!r}")
        anim = json.loads(clip_path.read_text())
        if len(anim["tracks"][0]["keys"]) != 1:
            fail("clip_key created a second key at the same quantised time")
        print("clip:", text.strip())
        print("PASS: mcp_v2")
    finally:
        # Keep the scratch on failure for forensics; a clean run can go.
        if not any(sys.exc_info()):
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
