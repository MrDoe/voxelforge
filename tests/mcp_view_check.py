#!/usr/bin/env python3
"""Functional gate for vf_mcp's view/edit tools.

Proves an agent can click into the live VIEW (logical window pixels) and drive
every edit tool through the App's real paths: pick/select, all five cell
brushes, Rotate, Move, the selection box, undo and clear. Runs against a
throwaway scenery made of symlinks to the shipped layers plus a copied
manifest, and every app render uses a /tmp overlay, so no authored file can be
touched by the test.
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
    fail(f"{tool}: no JSON-RPC result (stderr={p.stderr[-800:]!r})")


def md5(path):
    return hashlib.md5(path.read_bytes()).hexdigest() if path.exists() else "<none>"


def main():
    if not MCP.exists() or not APP.exists():
        fail("build vf_mcp and voxelforge first")
    TMP.mkdir(parents=True, exist_ok=True)
    scratch = TMP / f"mcp_view_{int(time.time())}"
    scratch.mkdir(parents=True)
    for f in SRC.iterdir():
        if f.name in {"world.json", "runtime_edits.vxw", "last.json", "ai_edits.vxw"}:
            continue
        if f.is_dir():
            if f.name == "micro":
                shutil.copytree(f, scratch / f.name)
        elif f.is_file():
            (scratch / f.name).symlink_to(f)
    shutil.copy2(SRC / "world.json", scratch / "world.json")
    if (SRC / "ai_edits.vxw").exists():
        shutil.copy2(SRC / "ai_edits.vxw", scratch / "ai_edits.vxw")
    overlay = TMP / f"mcp_view_overlay_{int(time.time())}.vxw"
    env = os.environ.copy()
    env["VF_SCENERY"] = str(scratch)
    env["VF_NO_OVERLAY"] = "1"
    env["VF_OVERLAY_PATH"] = str(overlay)
    try:
        # --- 0. tool catalogue ---------------------------------------------
        p = subprocess.run([str(MCP)], input=json.dumps(
            {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}) + "\n",
            capture_output=True, text=True, env=env, timeout=120)
        names = {t["name"] for t in json.loads(p.stdout.strip().splitlines()[0])["result"]["tools"]}
        need = {"view_pick", "view_click", "view_undo", "view_clear", "view_box",
                "edit_result"}
        if not need <= names:
            fail(f"tools/list missing {need - names}")

        # --- 1. a known cell and a small layer to transform ----------------
        text, err = mcp(env, "ground", {"x": 0, "z": 0})
        m = re.search(r"anchor cell \[(\d+) (\d+) (\d+)\]", text)
        if not m:
            fail(f"ground did not report an anchor: {text!r}")
        cell = [int(m.group(i)) for i in (1, 2, 3)]
        text, err = mcp(env, "write_object", {
            "name": "mcp_view_prop", "anchor": cell,
            "shapes": [{"type": "box", "at": [0, 0, 0], "size": [4, 4, 4], "mat": 6}]})
        if err or "voxels" not in text:
            fail(f"write_object: {text!r}")
        before, err = mcp(env, "get_layer", {"layer": "mcp_view_prop"})
        pm = re.search(r"pos=\[([-\d.]+),([-\d.]+),([-\d.]+)\]", before)
        rm = re.search(r" rot=([-\d.]+)", before)
        if not pm or not rm:
            fail(f"get_layer baseline: {before!r}")
        pos0 = [float(x) for x in pm.groups()]
        rot0 = float(rm.group(1))

        # --- 2. argument validation is instant, not an app round-trip ------
        text, err = mcp(env, "view_click", {"screen": [160, 90], "mode": "teleport"})
        if not err or "mode:" not in text:
            fail(f"view_click accepted a bogus mode: {text!r}")
        text, err = mcp(env, "view_box", {})
        if not err or "box:" not in text:
            fail(f"view_box accepted an empty selector: {text!r}")

        # --- 3. queue the intents through the MCP tools themselves ---------
        q = scratch / "mcp_intents.jsonl"
        r = scratch / "mcp_results.jsonl"
        for f in (q, r):
            if f.exists():
                f.unlink()
        screen = [160, 90]  # logical centre of the 320x180 headless view
        calls = [
            ("view_pick", {"screen": screen}),
            ("view_click", {"screen": screen, "mode": "select"}),
            ("view_click", {"screen": screen, "mode": "carve",
                            "diameter": 0.1, "depth": 0.1}),
            ("view_click", {"cell": cell, "mode": "add",
                            "diameter": 0.1, "depth": 0.1}),
            ("view_box", {"box": [cell[0] - 2, cell[1], cell[2] - 2,
                                  cell[0] + 2, cell[1] + 3, cell[2] + 2]}),
            ("view_click", {"cell": cell, "mode": "rotate",
                            "layer": "mcp_view_prop", "yaw": 30, "commit": True}),
            ("view_click", {"cell": cell, "mode": "move",
                            "layer": "mcp_view_prop", "dx": 0.5, "commit": True}),
            ("view_undo", {}),
            ("view_clear", {}),
        ]
        ids = []
        for tool, args in calls:
            text, err = mcp(env, tool, args)
            if err or "queued" not in text:
                fail(f"{tool}: {text!r}")
            mid = re.search(r"id=([^\s;]+)", text)
            if not mid:
                fail(f"{tool}: no id in {text!r}")
            ids.append(mid.group(1))

        # edit_result before the app runs must say pending (not hang/fake).
        text, err = mcp(env, "edit_result", {"id": ids[0]})
        if not err or "pending" not in text:
            fail(f"edit_result pending: {text!r}")

        # --- 4. one headless app run consumes the whole queue --------------
        real_overlay = SRC / "runtime_edits.vxw"
        before_overlay = md5(real_overlay)
        cmd = [str(APP), "--smoke", "3", "--width", "320", "--height", "180"]
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            fail(f"app --smoke exited {p.returncode}: {p.stderr[-2000:]}")
        if q.exists() and q.read_text().strip():
            fail("intent queue was not consumed")
        if not r.exists():
            fail("app did not write mcp_results.jsonl")
        got = {}
        order = []
        for line in r.read_text().splitlines():
            o = json.loads(line)
            got[o["id"]] = o
            order.append(o)
        if len(order) != len(ids):
            fail(f"expected {len(ids)} results, got {len(order)}")
        for i, o in enumerate(order):
            if not o.get("ok"):
                fail(f"intent {i} failed: {o!r}")

        # pick and select must agree on the pixel (one real pick path).
        pick, select, carve, add = (got[i] for i in ids[:4])
        for key in ("cell", "world", "material", "owner", "dist", "pick"):
            if key not in pick:
                fail(f"view_pick report missing {key}: {pick!r}")
        if pick["cell"] != select["cell"] or pick["cell"] != carve["cell"]:
            fail(f"view pick/select/carve resolved different cells: "
                 f"{pick['cell']} / {select['cell']} / {carve['cell']}")
        if csel := select.get("selected", ""):
            print("view select:", csel)
        for o in (carve, add):
            if o.get("cells", 0) < 1 or o.get("chunks", 0) < 1:
                fail(f"brush did not fire: {o!r}")
            if o.get("undoSteps", 0) != 1:
                fail(f"brush did not create one undo step: {o!r}")
        box = got[ids[4]]
        if box.get("cells") != 100:
            fail(f"selection box cell count: {box!r}")
        rot = got[ids[5]]
        mv = got[ids[6]]
        if not rot.get("committed") or abs(rot["placed"][0] - (rot0 + 30)) > 1e-3:
            fail(f"view_click rotate did not commit yaw: {rot!r}")
        if not mv.get("committed") or abs(mv["placed"][0] - (pos0[0] + 0.5)) > 1e-3:
            fail(f"view_click move did not commit dx: {mv!r}")
        if got[ids[7]].get("undone", 0) < 1:
            fail(f"view_undo did not revert the stroke: {got[ids[7]]!r}")
        if not got[ids[8]].get("ok"):
            fail(f"view_clear failed: {got[ids[8]]!r}")
        if md5(real_overlay) != before_overlay:
            fail("the REAL scenery overlay changed - hermeticity broken")

        # --- 5. post-hoc edit_result and manifest persistence ---------------
        text, err = mcp(env, "edit_result", {"id": ids[0]})
        if err or '"ok":true' not in text:
            fail(f"edit_result after run: {text!r}")
        after, err = mcp(env, "get_layer", {"layer": "mcp_view_prop"})
        pm2 = re.search(r"pos=\[([-\d.]+),([-\d.]+),([-\d.]+)\]", after)
        rm2 = re.search(r" rot=([-\d.]+)", after)
        if not pm2 or not rm2:
            fail(f"get_layer after: {after!r}")
        if abs(float(rm2.group(1)) - (rot0 + 30)) > 1e-3:
            fail(f"rotate not persisted in world.json: {after!r}")
        if abs(float(pm2.group(1)) - (pos0[0] + 0.5)) > 1e-3:
            fail(f"move not persisted in world.json: {after!r}")
        print("view_pick:", json.dumps(pick, sort_keys=True))
        print("rotate:", json.dumps(rot, sort_keys=True))
        print("move:", json.dumps(mv, sort_keys=True))
        print("PASS: mcp view/edit tools")
    finally:
        if not any(sys.exc_info()):
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
