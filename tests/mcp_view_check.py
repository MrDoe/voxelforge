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


def read_results(path):
    out = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return out


def wait_results(path, count, proc, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = read_results(path)
        if len(got) >= count:
            return got
        if proc.poll() is not None:
            fail(f"app exited early ({proc.returncode}) before {count} results")
        time.sleep(0.05)
    fail(f"timed out waiting for {count} app results")


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
    log_path = TMP / f"mcp_view_{scratch.name}.log"
    proc = None
    log = None
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
                "edit_result", "dig_cave"}
        if not need <= names:
            fail(f"tools/list missing {need - names}")

        # --- 1. a known cell and a small layer to transform ----------------
        text, err = mcp(env, "ground", {"x": 0, "z": 0})
        m = re.search(r"anchor cell \[(\d+) (\d+) (\d+)\]", text)
        if not m:
            fail(f"ground did not report an anchor: {text!r}")
        cell = [int(m.group(i)) for i in (1, 2, 3)]
        # A dry, high anchor for the cave (the origin cell is near the pond).
        text, err = mcp(env, "ground", {"x": 20, "z": -10})
        m = re.search(r"anchor cell \[(\d+) (\d+) (\d+)\]", text)
        if not m:
            fail(f"ground did not report a cave anchor: {text!r}")
        cave_cell = [int(m.group(i)) for i in (1, 2, 3)]
        # ground() reports the heightmap top; the selectable solid cell is one
        # lattice row below that surface plane.
        cave_cell[1] -= 1
        print("cave anchor:", cave_cell)
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

        # The fast path must validate without loading the 5 s voxel field.
        t_help = time.monotonic()
        text, err = mcp(env, "help", {})
        help_s = time.monotonic() - t_help
        if err or "dig_cave" not in text:
            fail(f"help missing dig_cave: {text[:200]!r}")
        if help_s > 2.0:
            fail(f"help cold path took {help_s:.3f}s (world build was not skipped)")
        for bad in ({"cell": cell, "style": "lava"},
                    {"cell": cell, "radius": 99}, {}):
            text, err = mcp(env, "dig_cave", bad)
            if not err:
                fail(f"dig_cave accepted invalid args {bad}: {text!r}")

        # --- 3. live app: readiness, inline cave, then every view tool ------
        q = scratch / "mcp_intents.jsonl"
        r = scratch / "mcp_results.jsonl"
        for f in (q, r):
            if f.exists():
                f.unlink()
        screen = [160, 90]  # logical centre of the 320x180 headless view
        real_overlay = SRC / "runtime_edits.vxw"
        before_overlay = md5(real_overlay)

        # A queued pick is the readiness probe: its result proves the app has
        # loaded the world and reached consumeMcpIntents(), without a sleep.
        text, err = mcp(env, "view_pick", {"screen": screen})
        if err or "queued" not in text:
            fail(f"view_pick readiness: {text!r}")
        ready_id = re.search(r"id=([^\s;]+)", text).group(1)
        text, err = mcp(env, "edit_result", {"id": ready_id})
        if not err or "pending" not in text:
            fail(f"edit_result pending: {text!r}")

        log = open(log_path, "w")
        proc = subprocess.Popen(
            [str(APP), "--smoke", "100000", "--width", "320", "--height", "180"],
            env=env, stdout=log, stderr=subprocess.STDOUT)
        wait_results(r, 1, proc)

        # The whole point: ONE tool call returns the engine report inline.
        t_cave = time.monotonic()
        text, err = mcp(env, "dig_cave", {
            "cell": cave_cell, "radius": 0.6, "depth": 1.2})
        cave_s = time.monotonic() - t_cave
        if err:
            fail(f"dig_cave inline call failed: {text!r}")
        try:
            cave = json.loads(text)
        except json.JSONDecodeError:
            fail(f"dig_cave did not return an inline JSON report: {text!r}")
        if not cave.get("ok") or cave.get("tool") != "dig_cave":
            fail(f"dig_cave report: {cave!r}")
        if cave.get("style") != "dripstone" or cave.get("stamps", 0) < 3:
            fail(f"dig_cave did not compose the default cave: {cave!r}")
        if cave.get("firstStampMs", 1e9) > 1000.0:
            fail(f"dig_cave took too long to begin digging: {cave!r}")
        if cave.get("cells", 0) < 100 or cave.get("chunks", 0) < 1:
            fail(f"dig_cave changed too little geometry: {cave!r}")
        if cave.get("undoSteps") != 1 or not cave.get("undoable"):
            fail(f"dig_cave was not one undoable stroke: {cave!r}")
        if cave.get("cellSolidAfter") != 0 or not cave.get("persisted"):
            fail(f"dig_cave did not open/persist the mouth: {cave!r}")
        if cave_s > 5.0:
            fail(f"dig_cave one-call latency {cave_s:.3f}s exceeds 5s")
        deadline = time.monotonic() + 2.0
        while not overlay.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        if not overlay.exists():
            fail("dig_cave did not queue its runtime overlay")

        # One undo must remove the whole compound cave, not one sub-stamp.
        text, err = mcp(env, "view_undo", {})
        if err or "queued" not in text:
            fail(f"view_undo cave: {text!r}")
        undo_id = re.search(r"id=([^\s;]+)", text).group(1)
        wait_results(r, 2, proc)

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

        order = wait_results(r, 2 + len(ids), proc)
        got = {o["id"]: o for o in order}
        expected = [ready_id, cave["id"], undo_id, *ids]
        if any(i not in got for i in expected):
            fail(f"missing app results: {set(expected) - set(got)}")
        for i in expected:
            if not got[i].get("ok"):
                fail(f"intent {i} failed: {got[i]!r}")
        if got[undo_id].get("undone", 0) != 1:
            fail(f"view_undo did not revert the cave as one stroke: {got[undo_id]!r}")
        if q.exists() and q.read_text().strip():
            fail("intent queue was not consumed")

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
        if not got[ids[7]].get("ok"):
            fail(f"view_clear failed: {got[ids[7]]!r}")
        if md5(real_overlay) != before_overlay:
            fail("the REAL scenery overlay changed - hermeticity broken")

        # --- 5. post-hoc edit_result and manifest persistence ---------------
        text, err = mcp(env, "edit_result", {"id": ready_id})
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
        print("dig_cave:", json.dumps(cave, sort_keys=True), f"{cave_s:.3f}s")
        print("help:", f"{help_s:.3f}s")
        print("view_pick:", json.dumps(pick, sort_keys=True))
        print("rotate:", json.dumps(rot, sort_keys=True))
        print("move:", json.dumps(mv, sort_keys=True))
        print("PASS: mcp view/edit tools")
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if log is not None:
            log.close()
        if any(sys.exc_info()):
            print("app log:", log_path)
            if log_path.exists():
                print("\n".join(log_path.read_text(errors="replace").splitlines()[-40:]))
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
