#!/usr/bin/env python3
"""Emissive-texture reflection / wall-occlusion regression guard.

A flat glossy floor reflects an emissive textured panel.  A thin wall is then
placed between the floor and the panel: the direct panel and its reflection
must disappear, rather than the SSR ray marching through the wall and leaking
sky/background.  The same check runs on the splat and SVO backends.

The scene is authored in a temporary scenery through vf_mcp; the shipped
manifest, overlays and authored layers are never modified.
"""
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

W, H = 480, 270
SSR_ON = 31 | (1 << 5) | (1 << 12)   # 4159: historical SSR + wet-surface guidance
SSR_OFF = 31 | (1 << 12)             # 4127: same scene, no SSR


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a P6 PPM: %s" % path)
    i, vals = 2, []
    while len(vals) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        start = i
        while not data[i:i + 1].isspace():
            i += 1
        vals.append(int(data[start:i]))
    i += 1
    w, h, _ = vals
    return w, h, data[i:i + w * h * 3]


def write_png(path, size=64):
    """Small RGBA checker, stdlib only (the gate has no Pillow dependency)."""
    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter: None
        for x in range(size):
            if ((x // 8) + (y // 8)) & 1:
                row.extend((255, 72, 24, 255))
            else:
                row.extend((255, 210, 92, 255))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def mcp(env, binary, tool, args):
    payload = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": tool, "arguments": args}}
    p = subprocess.run([binary], input=json.dumps(payload) + "\n",
                       capture_output=True, text=True, env=env, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("%s exited %d: %s" % (tool, p.returncode, p.stderr[-800:]))
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "result" not in obj:
            continue
        result = obj["result"]
        text = result.get("content", [{}])[0].get("text", "")
        return text, bool(result.get("isError"))
    raise RuntimeError("%s: no JSON-RPC result (%r)" % (tool, p.stdout[-800:]))


def cell_world(c):
    return (c + 0.5) * 0.1 - 51.2


def project(p, cam, target, w=W, h=H):
    fwd = [target[i] - cam[i] for i in range(3)]
    fl = math.sqrt(sum(v * v for v in fwd))
    fwd = [v / fl for v in fwd]
    # right = normalize(cross(fwd, worldUp)); up = cross(right, fwd)
    right = [fwd[2], 0.0, -fwd[0]]
    rl = math.sqrt(sum(v * v for v in right))
    right = [v / rl for v in right]
    up = [right[1] * fwd[2] - right[2] * fwd[1],
          right[2] * fwd[0] - right[0] * fwd[2],
          right[0] * fwd[1] - right[1] * fwd[0]]
    rel = [p[i] - cam[i] for i in range(3)]
    cz = sum(rel[i] * fwd[i] for i in range(3))
    cx = sum(rel[i] * right[i] for i in range(3))
    cy = sum(rel[i] * up[i] for i in range(3))
    tan = math.tan(math.radians(60.0) * 0.5)
    ndc = [cx / (cz * tan * (w / h)), -cy / (cz * tan)]
    return [(ndc[0] * 0.5 + 0.5) * w, (ndc[1] * 0.5 + 0.5) * h]


def poly_mask(points, w=W, h=H):
    mask = bytearray(w * h)
    minx = max(0, int(math.floor(min(p[0] for p in points))))
    maxx = min(w - 1, int(math.ceil(max(p[0] for p in points))))
    miny = max(0, int(math.floor(min(p[1] for p in points))))
    maxy = min(h - 1, int(math.ceil(max(p[1] for p in points))))

    def inside(x, y):
        hit = False
        j = len(points) - 1
        for i in range(len(points)):
            xi, yi = points[i]
            xj, yj = points[j]
            if ((yi > y) != (yj > y)) and \
                    x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    for y in range(miny, maxy + 1):
        for x in range(minx, maxx + 1):
            if inside(x + 0.5, y + 0.5):
                mask[y * w + x] = 1
    return mask


def diff_metrics(a, b, mask):
    _, _, pa = a
    _, _, pb = b
    total = changed = 0
    acc = 0.0
    for i, on in enumerate(mask):
        if not on:
            continue
        j = 3 * i
        d = (abs(pa[j] - pb[j]) + abs(pa[j + 1] - pb[j + 1]) +
             abs(pa[j + 2] - pb[j + 2])) / 3.0
        acc += d
        changed += d > 12.0
        total += 1
    if not total:
        return 0.0, 0.0
    return acc / total, 100.0 * changed / total


def render(binary, tmp, tag, cam, target, wall, ssr, backend):
    world = os.path.join(tmp, "scenery", "world.json")
    with open(world) as fh:
        manifest = json.load(fh)
    for layer in manifest.get("layers", []):
        if layer.get("file") == "reflection_wall.vxw":
            layer["enabled"] = bool(wall)
    with open(world, "w") as fh:
        json.dump(manifest, fh, indent=2)

    out = os.path.join(tmp, tag + ".ppm")
    env = dict(os.environ,
               VF_SCENERY=os.path.join(tmp, "scenery"),
               VF_NO_OVERLAY="1",
               VF_OVERLAY_PATH=os.path.join(tmp, "overlay.vxw"),
               VF_RENDER_FLAGS=str(SSR_ON if ssr else SSR_OFF),
               VF_FUR="0", VF_TEST_SUN_PHASE="night")
    # --cam consumes camera position + target (six values).
    cmd = [binary, "--mode", backend, "--width", str(W), "--height", str(H),
           "--shot", out, "--cam", *[str(v) for v in cam + target],
           "--animtime", "0"]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=600)
    if p.returncode != 0 or not os.path.exists(out):
        raise RuntimeError("%s render failed (rc=%d): %s" %
                           (tag, p.returncode, (p.stdout + p.stderr)[-2000:]))
    return read_ppm(out), p


def box_voxels(x0, y0, z0, nx, ny, nz, mat, refl=-1, rough=-1):
    out = []
    for y in range(y0, y0 + ny):
        for z in range(z0, z0 + nz):
            for x in range(x0, x0 + nx):
                v = {"x": x, "y": y, "z": z, "mat": mat}
                if refl >= 0:
                    v["refl"] = refl
                if rough >= 0:
                    v["rough"] = rough
                out.append(v)
    return out


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/voxelforge"
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mcp_binary = os.path.join(os.path.dirname(binary), "vf_mcp")
    if not os.path.exists(mcp_binary):
        mcp_binary = os.path.join(root, "build", "vf_mcp")
    source = os.path.join(root, "scenery", "LakesideHamlet")
    if not os.path.isfile(os.path.join(source, "world.json")) or not os.path.isfile(mcp_binary):
        print("FAIL: need the shipped LakesideHamlet scenery and build/vf_mcp")
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="vf_reflection_") as tmp:
        scene = os.path.join(tmp, "scenery")
        os.makedirs(scene)
        for name in os.listdir(source):
            src = os.path.join(source, name)
            dst = os.path.join(scene, name)
            if name == "world.json":
                shutil.copyfile(src, dst)
            elif name not in ("runtime_edits.vxw", "last.json") and not os.path.exists(dst):
                os.symlink(src, dst)

        world = os.path.join(scene, "world.json")
        with open(world) as fh:
            manifest = json.load(fh)
        for layer in manifest.get("layers", []):
            layer["enabled"] = layer.get("name") == "landscape" or \
                               layer.get("file") == "landscape.vxw"
        manifest["lights"] = []
        manifest["textures"] = []
        manifest.pop("ui", None)
        with open(world, "w") as fh:
            json.dump(manifest, fh, indent=2)

        env = dict(os.environ, VF_SCENERY=scene, VF_NO_OVERLAY="1",
                   VF_OVERLAY_PATH=os.path.join(tmp, "author_overlay.vxw"))
        text, err = mcp(env, mcp_binary, "ground", {"x": 25.0, "z": 25.0})
        match = re.search(r"anchor cell \[(\d+) (\d+) (\d+)\]", text)
        if err or not match:
            raise RuntimeError("ground did not return an anchor: %r" % text)
        gx, gy, gz = [int(v) for v in match.groups()]

        # A raised platform makes the test independent of the terrain slope.
        x0, z0 = gx - 20, gz - 20
        y0 = gy + 12
        objects = {
            "reflection_floor": box_voxels(x0, y0 - 2, z0, 40, 3, 40,
                                           22, 255, 10),
            "reflection_emitter": box_voxels(x0 + 10, y0 + 2, z0 + 34,
                                             20, 20, 1, 15, 0, 255),
            "reflection_wall": box_voxels(x0 + 2, y0 + 1, z0 + 22,
                                          36, 30, 2, 20, 0, 255),
        }
        for name, voxels in objects.items():
            text, err = mcp(env, mcp_binary, "write_object",
                            {"name": name, "voxels": voxels})
            if err:
                raise RuntimeError("write_object %s failed: %s" % (name, text))

        texture = os.path.join(tmp, "emissive_checker.png")
        write_png(texture)
        with open(world) as fh:
            manifest = json.load(fh)
        for layer in manifest.get("layers", []):
            if layer.get("file") == "reflection_wall.vxw":
                layer["enabled"] = True
        manifest["lights"] = []
        manifest["textures"] = [{"file": texture, "mat": 15, "scale": 0.5,
                                 "emissive": True, "emissiveScale": 2.0}]
        with open(world, "w") as fh:
            json.dump(manifest, fh, indent=2)

        cx = cell_world(x0 + 20)
        cam = [cx, cell_world(y0 + 14), cell_world(z0 - 8)]
        target = [cx, cell_world(y0 + 8), cell_world(z0 + 18)]
        floor = [cell_world(y0 + 0.05)]
        # Keep the ROI in FRONT of the wall (wall front face is z0+22), so
        # wall-surface SSR cannot contaminate the floor-reflection metric.
        floor_poly = [
            project([cell_world(x0 + 5), floor[0], cell_world(z0 + 2)], cam, target),
            project([cell_world(x0 + 35), floor[0], cell_world(z0 + 2)], cam, target),
            project([cell_world(x0 + 35), floor[0], cell_world(z0 + 20)], cam, target),
            project([cell_world(x0 + 5), floor[0], cell_world(z0 + 20)], cam, target),
        ]
        emitter_poly = [
            project([cell_world(x0 + 10), cell_world(y0 + 2), cell_world(z0 + 34)], cam, target),
            project([cell_world(x0 + 30), cell_world(y0 + 2), cell_world(z0 + 34)], cam, target),
            project([cell_world(x0 + 30), cell_world(y0 + 22), cell_world(z0 + 34)], cam, target),
            project([cell_world(x0 + 10), cell_world(y0 + 22), cell_world(z0 + 34)], cam, target),
        ]
        floor_mask = poly_mask(floor_poly)
        emitter_mask = poly_mask(emitter_poly)
        if sum(floor_mask) < 500 or sum(emitter_mask) < 100:
            raise RuntimeError("test camera does not frame the floor/emitter")

        for backend in ("splat", "svo"):
            off_on, r_on = render(binary, tmp, backend + "_open_ssr", cam, target,
                                  False, True, backend)
            off_off, r_off = render(binary, tmp, backend + "_open_base", cam, target,
                                    False, False, backend)
            on_on, _ = render(binary, tmp, backend + "_wall_ssr", cam, target,
                              True, True, backend)
            on_off, _ = render(binary, tmp, backend + "_wall_base", cam, target,
                              True, False, backend)

            open_mean, open_frac = diff_metrics(off_on, off_off, floor_mask)
            wall_mean, wall_frac = diff_metrics(on_on, on_off, floor_mask)
            direct_mean, direct_frac = diff_metrics(off_off, on_off, emitter_mask)
            print("[%s] open reflection mean=%.3f frac=%.3f%%; "
                  "behind-wall mean=%.3f frac=%.3f%%; direct occlusion "
                  "mean=%.3f frac=%.3f%%" %
                  (backend, open_mean, open_frac, wall_mean, wall_frac,
                   direct_mean, direct_frac))

            log = r_off.stdout + r_off.stderr
            if "texture atlas: material 15 <-" not in log or "emissive" not in log:
                failures.append("%s: emissive texture binding was not loaded" % backend)
            if "visible emissive clusters" not in log:
                failures.append("%s: emissive texture produced no derived light" % backend)
            if open_mean < 0.50 or open_frac < 0.40:
                failures.append("%s: open emissive reflection is not visible" % backend)
            # Mean delta is the primary leak metric. The changed-pixel fraction
            # also contains tiny SSR differences on floor pixels whose reflected
            # background changed when the wall was added, so keep only a loose
            # absolute guard on it.
            if wall_mean > 0.25 * open_mean or wall_frac > 1.0:
                failures.append("%s: SSR reflection leaks through the wall" % backend)
            if direct_mean < 0.75 or direct_frac < 5.0:
                failures.append("%s: wall does not occlude the direct emitter" % backend)

        # Authored point lights are legacy data, not a legal light source. A
        # very bright lamp at the test camera must change no pixel: only the
        # visible emissive panel and sun/moon may light this scene.
        with open(world) as fh:
            manifest = json.load(fh)
        manifest.pop("lights", None)
        with open(world, "w") as fh:
            json.dump(manifest, fh, indent=2)
        policy_none, _ = render(binary, tmp, "policy_none", cam, target,
                                False, False, "splat")
        manifest["lights"] = [{"pos": [cx, cell_world(y0 + 10), cell_world(z0 - 5)],
                               "color": [1.0, 0.7, 0.4], "radius": 20.0,
                               "intensity": 50.0}]
        with open(world, "w") as fh:
            json.dump(manifest, fh, indent=2)
        policy_authored, policy_log = render(binary, tmp, "policy_authored",
                                             cam, target, False, False, "splat")
        policy_mean, policy_frac = diff_metrics(
            policy_none, policy_authored, bytearray([1]) * (W * H))
        print("[policy] authored-light mean=%.6f frac=%.6f%%" %
              (policy_mean, policy_frac))
        log = policy_log.stdout + policy_log.stderr
        if "ignoring 1 authored point light" not in log:
            failures.append("authored light was not reported as ignored")
        if policy_mean > 0.001 or policy_frac > 0.0:
            failures.append("invisible authored point light changed the frame")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("reflection_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
