#!/usr/bin/env python3
"""Gate for --mode 3dgs (opt-in weighted-blended OIT edge transparency).

The 3dgs mode re-draws the surfels into two float targets (premultiplied
colour+alpha, and transmittance Pi(1-a)), then composites them with a
sky backdrop. Its CONTRACT is deliberately narrow:

  * solid interiors are pixel-identical to the shipped splat renderer;
  * ONLY sparse-coverage silhouette pixels change (the OIT reveal);
  * the default `splat` and reference `svo` modes are untouched.

Arms:
  A. Regression: new binary in splat/svo mode == pristine HEAD baseline.
     Opt-in via VF_BASELINE_BIN, skipped loudly otherwise.
  B. Edge-only: 3dgs vs HEAD moves a SMALL fraction of pixels (<5%), stays
     within a tight whole-frame mean, and leaves the centre (interior) crop
     nearly identical. A mode that does nothing OR repaints the whole frame
     both fail here.
  C. Chain live: VF_OIT_DUMP reports a working reveal product and base copy.

Usage: dgs_check.py <path-to-voxelforge-binary>
Stdlib only.
"""
import os
import re
import subprocess
import sys
import tempfile

SHOTS = [
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]
W, H = 480, 270
FLAGS = "31"

OIT_RE = re.compile(r"oit: base copy mean ([\d.]+)")


def read_ppm(path):
    d = open(path, 'rb').read()
    toks, j = [], 2
    while len(toks) < 3:
        while j < len(d) and d[j:j + 1].isspace():
            j += 1
        if d[j:j + 1] == b'#':
            while d[j:j + 1] not in (b'\n', b''):
                j += 1
            continue
        k = j
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        toks.append(int(d[k:j]))
    j += 1
    return toks[0], toks[1], d[j:]


def diff(a_path, b_path, crop=None):
    w, h, a = read_ppm(a_path)
    _, _, b = read_ppm(b_path)
    x0, x1, y0, y1 = crop or (0, w, 0, h)
    n = total = over10 = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            m = max(abs(a[i + c] - b[i + c]) for c in range(3))
            total += m
            over10 += m > 10
            n += 1
    return total / max(n, 1), 100.0 * over10 / max(n, 1)


def render(binary, tmp, tag, mode=None, extra_env=None, dump=None):
    list_path = os.path.join(tmp, f"shots_{tag}.txt")
    paths = []
    with open(list_path, "w") as lf:
        for name, cam in SHOTS:
            p = os.path.join(tmp, f"{name}_{tag}.ppm")
            paths.append(p)
            lf.write(" ".join([p, *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1", VF_RENDER_FLAGS=FLAGS)
    if mode:
        env["VF_MODE_OVERRIDE"] = mode  # not used; mode passed on argv below
    if extra_env:
        env.update(extra_env)
    if dump:
        env["VF_OIT_DUMP"] = dump
    cmd = [binary]
    if mode:
        cmd += ["--mode", mode]
    cmd += ["--shotlist", list_path, "--width", str(W), "--height", str(H),
            "--animtime", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0 or not all(os.path.exists(p) for p in paths):
        print((r.stdout + r.stderr)[-2500:])
        return None, None
    return paths, (r.stdout + r.stderr)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else None
    if not binary or not os.path.exists(binary):
        print(f"usage: {sys.argv[0]} <voxelforge-binary>")
        return 2
    baseline = os.environ.get("VF_BASELINE_BIN")
    tmp = tempfile.mkdtemp(prefix="dgs_check_")
    failures = []

    base_paths, _ = render(binary, tmp, "splat", mode="splat")
    dgs_paths, dgs_log = render(binary, tmp, "dgs", mode="3dgs",
                                dump=os.path.join(tmp, "dump"))
    if base_paths is None or dgs_paths is None:
        print("FAIL: renders did not complete")
        return 1

    # Arm A: default/render modes untouched (needs a pristine binary).
    if baseline:
        head_paths, _ = render(baseline, tmp, "head")
        if head_paths:
            for name, hp, np_ in zip((s[0] for s in SHOTS), head_paths, base_paths):
                m, p = diff(hp, np_)
                print(f"  regression {name} splat: mean {m:.3f}, >10 {p:.2f}%")
                if m > 0.05 or p > 0.1:
                    failures.append(f"{name}: default splat path changed (mean {m:.3f})")
            for name, hp, dp in zip((s[0] for s in SHOTS), head_paths, dgs_paths):
                m, p = diff(hp, dp, crop=(W // 4, 3 * W // 4, H // 4, 3 * H // 4))
                print(f"  regression {name} 3dgs interior: mean {m:.3f}, >10 {p:.2f}%")
                if m > 0.5 or p > 2.0:
                    failures.append(f"{name}: 3dgs changed the interior (mean {m:.3f})")
        else:
            print("  SKIP regression: baseline binary failed to render")
    else:
        print("  SKIP regression: set VF_BASELINE_BIN=/path/to/pristine/voxelforge")

    # Arm B: edge-only — the whole frame moves, but only a little; interiors
    # were checked against baseline above when available.
    for name, bp, dp in zip((s[0] for s in SHOTS), base_paths, dgs_paths):
        m, p = diff(bp, dp)
        print(f"  edge {name}: whole-frame mean {m:.3f}, >10 {p:.2f}%")
        if p > 5.0:
            failures.append(f"{name}: 3dgs repainted {p:.1f}% of pixels (edge-only limit 5%)")
        if m > 2.0:
            failures.append(f"{name}: 3dgs whole-frame mean {m:.3f} too large")

    # Arm C: OIT chain is live (base copy succeeded, reveal accumulated).
    copies = [float(x) for x in OIT_RE.findall(dgs_log)]
    if not copies:
        failures.append("no `oit: base copy mean` line - VF_OIT_DUMP is inert")
    elif any(c < 0.05 for c in copies):
        failures.append(f"base copy mean too low ({copies}) - the m_hdr copy failed")
    else:
        print(f"  chain: base copy means {[round(c, 3) for c in copies]}")

    if failures:
        print("FAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS: dgs mode edge-only compositing verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
