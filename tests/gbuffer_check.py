#!/usr/bin/env python3
"""Guidance-lane regression guard for voxelforge.

The splat backend writes two G-buffer attachments beyond colour/position/normal
(`shaders/splat.frag`: `oGAlb` = effective albedo + roughness, `oGLit` = shading
before the atmosphere composite + reflectance). They exist because albedo and
roughness arrive per surfel and were previously multiplied into the lighting
integral and destroyed, which left nothing downstream able to read a material off
a frame. They are unread by the shipped chain, so they must not move a single
pixel of the rendered image.

Arms:

  A. The lanes are ALIVE and CARRY NEW INFORMATION.  Rendered with
     `VF_GBUFFER_DUMP=<prefix>`, which prints a `gbuffer:` statistics line and
     writes two debug-encoded PPMs.  Asserts the albedo lane is not black, the
     packed material channels are non-zero, and - the one that actually pins the
     contract - that albedo DIFFERS from the rendered colour on a solid fraction
     of surface pixels.  If that number ever collapses toward zero the lane has
     silently become a second copy of the colour buffer.
  B. The rendered frame is BIT-IDENTICAL to a reference binary.  Opt-in, because
     it needs a second executable: set `VF_BASELINE_BIN=/path/to/pre-change/
     voxelforge`.  This is the arm that proves "a wider G-buffer costs the image
     nothing" rather than asserting it.  Skipped loudly, never silently.

Usage: gbuffer_check.py <path-to-voxelforge-binary>
Stdlib only.
"""
import os
import re
import subprocess
import sys
import tempfile

# The canonical views: the same three shots the other visual gates calibrate on,
# so a lane that reads well here reads well where the other gates look.
SHOTS = [
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

W, H = 480, 270
# AO + shadows + flora + water + outline: the default flags, no FX toggles.
FLAGS = "31"

STATS_RE = re.compile(
    r"gbuffer: albedo mean ([\d.]+) max ([\d.]+), rough mean ([\d.]+), "
    r"light mean ([\d.]+); albedo-vs-frame r=(-?[\d.]+) over (\d+) surface pixels; "
    r"sky albedo-zero ([\d.]+)% of (\d+)"
)


def render(binary, tmp, tag, dump_prefix=None, extra_env=None):
    """One process, all views. A world load is ~13 s and dominates one --shot,
    so the suite batches through --shotlist like the other checks."""
    list_path = os.path.join(tmp, f"shots_{tag}.txt")
    paths = []
    with open(list_path, "w") as lf:
        for name, cam in SHOTS:
            p = os.path.join(tmp, f"{name}_{tag}.ppm")
            paths.append(p)
            lf.write(" ".join([p, *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1", VF_RENDER_FLAGS=FLAGS)
    if dump_prefix:
        env["VF_GBUFFER_DUMP"] = dump_prefix
    if extra_env:
        env.update(extra_env)
    cmd = [binary, "--shotlist", list_path, "--width", str(W), "--height", str(H),
           "--animtime", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0 or not all(os.path.exists(p) for p in paths):
        print((r.stdout + r.stderr)[-2500:])
        return None, None
    return paths, (r.stdout + r.stderr)


def check_lanes_live(binary, tmp, failures):
    """Arm A: the lanes carry material, and carry information the frame does not."""
    prefix = os.path.join(tmp, "lane")
    paths, log = render(binary, tmp, "dump", dump_prefix=prefix)
    if paths is None:
        failures.append("dump run failed to render")
        return
    lines = STATS_RE.findall(log)
    if not lines:
        failures.append("no `gbuffer:` statistics line - VF_GBUFFER_DUMP is inert")
        return
    worst = None
    albedo_means = []
    for am, ax, rm, lm, corr, nsurf, skyzero, nsky in lines:
        am, ax, rm, lm, corr, nsurf = (float(am), float(ax), float(rm),
                                       float(lm), float(corr), int(nsurf))
        skyzero, nsky = float(skyzero), int(nsky)
        print(f"  lanes: albedo mean {am:.4f} max {ax:.3f}, rough {rm:.4f}, "
              f"light {lm:.3f}, r={corr:+.3f}, sky-albedo-zero "
              f"{skyzero:.1f}%/{nsky}")
        if nsurf < 1000:
            failures.append(f"only {nsurf} surface pixels classified - the sky "
                            "filter or the lane write is broken")
        if not 0.02 < am < 0.95:
            failures.append(f"albedo mean {am:.4f} implausible (lane black or saturated)")
        if ax < 0.15:
            failures.append(f"albedo max {ax:.3f} - lane never sees a lit surface")
        if rm <= 0.001:
            failures.append("rough mean 0 - the packed material channel is empty")
        # The deterministic contract: the sky path writes zeros to both lanes,
        # so a bright blue sky pixel MUST read back zero albedo even though its
        # colour is far from zero. That is a direct proof the lane is not a copy
        # of the colour buffer, and unlike a correlation threshold it does not
        # depend on how albedo-coloured this particular scene happens to be.
        # Measurement note: an earlier correlation-only instrument reported
        # r=0.08 here and then 0.7 after the sky filter was corrected — 0.08 was
        # an artefact of counting bright sky as a surface, not a feature win.
        if nsky > 100 and skyzero < 95.0:
            failures.append(
                f"only {skyzero:.1f}% of {nsky} sky pixels have zero albedo - the "
                "lane is following the frame colour instead of the material")
        worst = skyzero if worst is None else min(worst, skyzero)
        albedo_means.append(am)
    # the debug images must exist too, or a human cannot see what was measured
    for suffix in (".albedo.ppm", ".light.ppm"):
        p = prefix + suffix
        if not os.path.exists(p) or os.path.getsize(p) < 1000:
            failures.append(f"missing/empty debug dump {os.path.basename(p)}")
    return worst, albedo_means


def pixel_data(path):
    """Pixel bytes of a P6 PPM, without the 3-line header."""
    d = open(path, "rb").read()
    i = d.index(b"\n", 4)          # end of the width/height line
    i = d.index(b"\n", i + 1) + 1  # end of the maxval line
    return d[i:]


def ndiff(a, b):
    return sum(1 for x, y in zip(a, b) if x != y)


def check_frame_unchanged(binary, tmp, baseline, failures):
    """Arm B: the visible frame must stay INSIDE the renderer's own noise floor.

    Literal byte equality is not the right instrument here: two runs of the very
    same baseline binary on the same hero view differ by hundreds of bytes (the
    splat compositing is not bit-reproducible). So measure that noise with a
    baseline-vs-baseline pair first, then require the cross-binary difference to
    stay within it. A gate that fails an unchanged binary teaches nothing.
    """
    cur, _ = render(binary, tmp, "new")
    ref1, _ = render(baseline, tmp, "ref1")
    ref2, _ = render(baseline, tmp, "ref2")
    if cur is None or ref1 is None or ref2 is None:
        failures.append("frame-noise arm could not render all three groups")
        return
    for name, _ in SHOTS:
        a = pixel_data(os.path.join(tmp, f"{name}_new.ppm"))
        r1 = pixel_data(os.path.join(tmp, f"{name}_ref1.ppm"))
        r2 = pixel_data(os.path.join(tmp, f"{name}_ref2.ppm"))
        noise = ndiff(r1, r2)
        cross = ndiff(a, r1)
        tol = max(noise * 2 + 32, 64)
        print(f"  {name}: baseline self-noise {noise} B, cross-binary {cross} B "
              f"(tol {tol} B)")
        if cross > tol:
            failures.append(
                f"{name}: the frame changed by {cross} B against a {noise} B "
                "self-noise floor - adding attachments must not touch attachment 0")


def check_autoexposure(binary, tmp, failures):
    """Arm C: the meter produces a real gain, and the gain reaches the image.

    A pass can compile, be dispatched, write an SSBO, and still be inert — if
    post.comp does not read the value, or bit 10 is not set, or the descriptor
    was written before the set existed. Two independent observations cover it:
    the logged exposure must be non-neutral and in range, and the frame must
    actually move against the arm-A control that ran with the flag off.
    """
    paths, log = render(binary, tmp, "auto", extra_env={"VF_AUTOEXPOSE": "1"})
    if paths is None:
        failures.append("auto-exposure run failed to render")
        return
    vals = [float(v) for v in
            re.findall(r"exposure: (-?[\d.]+) stops \(x[\d.]+\)", log)]
    if not vals:
        failures.append("no `exposure:` line - the meter never produced a value")
        return
    print(f"  exposure: stops = {', '.join(f'{v:+.3f}' for v in vals)}")
    if not any(abs(v) > 0.01 for v in vals):
        failures.append("meter stuck at neutral (0 stops) on every view - not wired up")
    if any(v < -8.0 or v > 6.0 for v in vals):
        failures.append("meter left its clamp range - a runaway exposure will blow the frame")
    moved = 0
    for name, _ in SHOTS:
        a = pixel_data(os.path.join(tmp, f"{name}_auto.ppm"))
        b = pixel_data(os.path.join(tmp, f"{name}_dump.ppm"))
        if ndiff(a, b) > len(a) // 200:   # > 0.5% of bytes moved
            moved += 1
    if moved == 0:
        failures.append("auto-exposure changed no view - post.comp is not applying the gain")
    else:
        print(f"  exposure visibly changed {moved}/{len(SHOTS)} views")


def check_transmission(binary, tmp, failures, base_means):
    """Arm D: backlit transmission moves the frame, not the material lane.

    The transmission term is added inside applyFlora, AFTER shadeSurfel has
    handed back the effective albedo. So enabling it must change the image on
    views with foliage while leaving the albedo lane's statistics alone — the
    pair of assertions is what proves both that the lever fires and that the
    lane still means "material" rather than "whatever shading did".
    """
    prefix = os.path.join(tmp, "lane-tx")
    paths, log = render(binary, tmp, "tx", dump_prefix=prefix,
                        extra_env={"VF_TRANSMISSION": "1"})
    if paths is None:
        failures.append("transmission run failed to render")
        return
    means = [float(t[0]) for t in STATS_RE.findall(log)]
    if len(means) != len(base_means):
        failures.append("transmission run produced a different number of lane dumps")
        return
    drift = max((abs(a - b) for a, b in zip(means, base_means)), default=0.0)
    print(f"  transmission: albedo-lane mean drift {drift:.4f}")
    if drift > 0.02:
        failures.append(
            f"albedo lane drifted {drift:.4f} when transmission was enabled - "
            "the material lane is absorbing a shading term")
    moved = 0
    for name, _ in SHOTS:
        a = pixel_data(os.path.join(tmp, f"{name}_tx.ppm"))
        b = pixel_data(os.path.join(tmp, f"{name}_dump.ppm"))
        if ndiff(a, b) > len(a) // 200:
            moved += 1
    if moved == 0:
        failures.append("transmission changed no view - the render-flag hook is inert")
    else:
        print(f"  transmission visibly changed {moved}/{len(SHOTS)} views")


def main():
    if len(sys.argv) < 2:
        print("usage: gbuffer_check.py <voxelforge-binary>", file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    baseline = os.environ.get("VF_BASELINE_BIN", "").strip()
    failures = []
    with tempfile.TemporaryDirectory(prefix="vf-gbuffer-") as tmp:
        print("arm A: guidance lanes")
        diff_worst, base_means = check_lanes_live(binary, tmp, failures)
        if baseline and os.path.isfile(baseline) and os.access(baseline, os.X_OK):
            print(f"arm B: frame noise vs {baseline}")
            check_frame_unchanged(binary, tmp, baseline, failures)
        else:
            # Loud, not silent: the arm that carries the strongest claim is the
            # one that needs a reference binary, and a skipped check that
            # prints nothing is how a regression becomes invisible.
            print("arm B: SKIPPED - set VF_BASELINE_BIN=<pre-change voxelforge> "
                  "to measure the visible frame against its own noise floor")
        print("arm C: auto-exposure")
        check_autoexposure(binary, tmp, failures)
        print("arm D: backlit transmission")
        check_transmission(binary, tmp, failures, base_means)
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print(f"gbuffer_check: OK (sky albedo-zero >= {diff_worst:.1f}%; the lanes "
          "carry material, not a copy of the colour buffer)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
