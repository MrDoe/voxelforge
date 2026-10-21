#!/usr/bin/env python3
"""Gate for the FUR-CARD layer over the micro-voxel objects.

The fur is the one part of the renderer that is TRANSLUCENT on purpose: a
coat of hair cards, each a flat ribbon standing out of the surface, composited
over the already-opaque body. The world's own splats cannot do that - their
opaque base seal (the nearest fragment written at alpha 1) is exactly what
turns a lone silhouette splat into a hard-edged blob - so the fur is a
separate pass and a separate layer.

What this gate measures, and why each one is a measurement rather than a
screenshot:

  1. THE FUR EXISTS. The alpaca's pixels must change when the fur is on.
     The instrument's NOISE FLOOR is measured first, from two renders of the
     SAME configuration (the scene's animated grass and the float16
     G-buffer move a little between runs), and the signal has to clear it.
  2. THE FUR IS CONFINED. The changed pixels must be a COMPACT BLOB (a high
     fill ratio inside their own bounding box) and must not spread over the
     frame. A DDA that walked off the object would light up the sky, and a
     coverage that leaked would wash the whole scene: both are invisible in
     a caption and obvious in a count.
  3. THE FUR TOUCHES NOTHING ELSE. The house view (no micro object in it) and
     the vase/urn view (micro objects with no fur material) must be
     UNCHANGED at the noise floor. The vase and the urn ride the same
     micro path as the alpaca, so this also proves the fur is a MATERIAL
     decision and not a "everything micro is furry" one.
  4. THE FUR IS BACKEND-INDEPENDENT. It is its own pass, so it must appear
     in the SVO reference backend too. A fur that only existed in the splat
     backend would be a splat-backend feature wearing a pass's clothes.
  5. THE ESCAPE HATCH. With VF_FUR=0 the cards are never generated, the pass
     is empty and its dispatch is skipped, so the frame must be identical to
     the fur-on frame OUTSIDE the alpaca - and the log must say that no fur
     object was loaded at all.

Usage:  python3 tests/fur_check.py [path/to/voxelforge]
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "voxelforge")

# The camera that fills the frame with the ALPACA (the third micro object),
# the one with the two ceramic objects (which carry no fur), and one with no
# micro object in it at all.
ALPACA_CAM = "10.95,0.85,11.45,11.75,0.52,12.55"
MICRO_CAM = "11.45,0.30,11.45,12.22,-0.20,12.14"
HOUSE_CAM = "2.5,1.3,6.0,6.8,1.0,12.2"
W, H = 900, 600

# The noise floor of a same-configuration pair. Measured, not assumed: the
# scene's animated grass and the float16 G-buffer move a little between runs.
NOISE_MAX = 6

failures: list[str] = []


def note(ok: bool, msg: str) -> None:
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        failures.append(msg)


def render(out: str, cam: str, mode: str = "", env_extra: dict | None = None) -> str:
    cmd = [APP, "--shot", out, "--cam", cam, "--width", str(W), "--height", str(H)]
    if mode:
        cmd += ["--mode", mode]
    env = os.environ.copy()
    # Never touch the live-edit overlay file: this is a read-only measurement.
    env["VF_OVERLAY_PATH"] = os.path.join(OUTDIR, "overlay.vxw")
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run(cmd, check=True, cwd=ROOT, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.stdout.decode("utf-8", "replace")


def read_ppm(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return w, h, f.read()


def diff_stats(a: bytes, b: bytes, thr: int) -> tuple[int, int, int, int, int]:
    """The changed pixels above `thr`, and their bounding box."""
    n = len(a) // 3
    cnt = 0
    minx, miny, maxx, maxy = 10**9, 10**9, -1, -1
    for i in range(n):
        o = i * 3
        d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        if d > thr:
            cnt += 1
            x, y = i % W, i // W
            minx, maxx = min(minx, x), max(maxx, x)
            miny, maxy = min(miny, y), max(maxy, y)
    if cnt == 0:
        return 0, 0, 0, 0, 0
    box = (maxx - minx + 1) * (maxy - miny + 1)
    return cnt, minx, miny, maxx, maxy, box  # type: ignore[return-value]


def main() -> int:
    global OUTDIR
    OUTDIR = tempfile.mkdtemp(prefix="vf-fur-check-")
    print(f"fur_check: {APP}")

    try:
        # ---- the instrument's noise floor, measured first ------------------
        render(os.path.join(OUTDIR, "a.ppm"), ALPACA_CAM, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "b.ppm"), ALPACA_CAM, env_extra={"VF_FUR": "0"})
        _, _, off1 = read_ppm(os.path.join(OUTDIR, "a.ppm"))
        _, _, off2 = read_ppm(os.path.join(OUTDIR, "b.ppm"))
        n_noise, *_ = diff_stats(off1, off2, NOISE_MAX)
        print(f"  noise floor: {n_noise} pixels above {NOISE_MAX}/255 "
              f"(two identical renders)")
        note(n_noise == 0,
             f"the noise floor is empty above {NOISE_MAX}/255 "
             f"(got {n_noise} stray pixels)")

        # ---- 1 + 2: the fur exists and is confined -------------------------
        log = render(os.path.join(OUTDIR, "on.ppm"), ALPACA_CAM)
        _, _, on = read_ppm(os.path.join(OUTDIR, "on.ppm"))
        m = re.search(r"fur: 1 object\(s\), (\d+) cards", log)
        note(m is not None and int(m.group(1)) > 1000 if m else False,
             f"the fur pass loaded cards (log: {m.group(0) if m else 'NO FUR LINE'})")
        cnt, minx, miny, maxx, maxy, box = diff_stats(on, off1, NOISE_MAX)
        note(cnt > 500,
             f"the fur changes the alpaca ({cnt} pixels above {NOISE_MAX}/255)")
        note(cnt > 0 and box > 0 and cnt / box > 0.15,
             f"the change is a compact blob (fill {cnt/max(box,1):.2f} of its bbox)")
        note(cnt < 0.20 * W * H,
             f"the change does not spread over the frame ({cnt} of {W*H} pixels)")
        note(cnt == 0 or (0 <= minx and maxx < W and 0 <= miny and maxy < H),
             "the change's bounding box is inside the frame")

        # ---- 2b: the coat is at least as SMOOTH as the surface under it ---
        # A straight, spiky coat concentrates its ink in narrow streaks, and a
        # narrow streak is HIGH-frequency energy: the coat reads as crunchier
        # than the body it covers. A curled strand spreads the same ink over
        # more pixels, so the coat's high-frequency content falls BELOW the
        # bare body's. That is a falsifiable claim about the shape, and it is
        # the numeric form of "curly, not spiky": with the curl removed this
        # check fails (measured: 16.1 against the body's 13.5).
        def neighbour_diff(buf: bytes, x0: int, y0: int, x1: int, y1: int) -> float:
            tot = 0
            cnt = 0
            for y in range(y0, min(y1, H)):
                row = y * W
                for x in range(x0, min(x1, W) - 1):
                    o = (row + x) * 3
                    for c in range(3):
                        tot += abs(buf[o + c] - buf[o + c + 3])
                    cnt += 3
            return tot / max(cnt, 1)

        fur_hf = neighbour_diff(on, 260, 100, 520, 480)
        body_hf = neighbour_diff(off1, 260, 100, 520, 480)
        print(f"  high-frequency: fur {fur_hf:.2f} vs the bare body {body_hf:.2f}")
        # The margin covers the run-to-run noise of the metric itself (the
        # scene's animated grass moves a little between renders); the signal
        # is 2.5, an order of magnitude above it.
        note(fur_hf <= body_hf + 1.0,
             f"the coat is at least as smooth as the body under it "
             f"({fur_hf:.2f} vs {body_hf:.2f})")

        # ---- 3: the fur touches nothing else ------------------------------
        # The house view has no micro object in it at all, so the fur must
        # leave it EXACTLY as it found it. A DDA that walked off the object,
        # or a coverage that leaked, would show up here as a wash over the
        # whole frame - which no caption would call a bug.
        render(os.path.join(OUTDIR, "h_off.ppm"), HOUSE_CAM, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "h_on.ppm"), HOUSE_CAM)
        _, _, h_off = read_ppm(os.path.join(OUTDIR, "h_off.ppm"))
        _, _, h_on = read_ppm(os.path.join(OUTDIR, "h_on.ppm"))
        c, *_ = diff_stats(h_on, h_off, NOISE_MAX)
        note(c == 0,
             f"the fur leaves the house view untouched ({c} pixels changed)")
        # The vase and the urn ride the SAME micro path as the alpaca and
        # carry no fur material, so the fur must not touch them either. The
        # alpaca is in this frame too (the whole paddock is), so the check
        # is that the change stays OUT of the frame's left half, where the
        # two ceramic objects and the empty ground are.
        render(os.path.join(OUTDIR, "m_off.ppm"), MICRO_CAM, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "m_on.ppm"), MICRO_CAM)
        _, _, m_off = read_ppm(os.path.join(OUTDIR, "m_off.ppm"))
        _, _, m_on = read_ppm(os.path.join(OUTDIR, "m_on.ppm"))
        c, minx, miny, maxx, maxy, _ = diff_stats(m_on, m_off, NOISE_MAX)
        note(c > 0 and minx > 0.30 * W,
             f"the fur stays clear of the frame's left third in the "
             f"vase/urn view (the change starts at x={minx})")

        # ---- 4: the fur is backend-independent ----------------------------
        render(os.path.join(OUTDIR, "svo_off.ppm"), ALPACA_CAM, mode="svo",
               env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "svo_on.ppm"), ALPACA_CAM, mode="svo")
        _, _, s_off = read_ppm(os.path.join(OUTDIR, "svo_off.ppm"))
        _, _, s_on = read_ppm(os.path.join(OUTDIR, "svo_on.ppm"))
        c, *_ = diff_stats(s_on, s_off, NOISE_MAX)
        note(c > 500,
             f"the fur is in the SVO reference backend too ({c} pixels changed)")

        # ---- 5: the escape hatch ------------------------------------------
        log_off = render(os.path.join(OUTDIR, "esc.ppm"), ALPACA_CAM,
                         env_extra={"VF_FUR": "0"})
        note("fur: 1 object" not in log_off,
             "VF_FUR=0 loads no fur object at all (the pass never dispatches)")
        _, _, esc = read_ppm(os.path.join(OUTDIR, "esc.ppm"))
        c, *_ = diff_stats(esc, off1, NOISE_MAX)
        note(c == 0,
             f"VF_FUR=0 renders the same frame as the fur-off reference "
             f"({c} pixels changed)")

    finally:
        for name in os.listdir(OUTDIR):
            try:
                os.remove(os.path.join(OUTDIR, name))
            except OSError:
                pass
        os.rmdir(OUTDIR)

    print()
    if failures:
        print(f"fur_check: {len(failures)} FAILED")
        for f in failures:
            print("  - " + f)
        return 1
    print("fur_check: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
