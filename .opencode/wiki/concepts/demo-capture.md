---
title: Demo Recording and Headless Capture
tags: [tooling, capture, demo, shotlist, headless, testing]
sourceRefs: [tools/record_demo.py, tools/test_inject_iso.py, src/app/main.cpp, tests/visual_check.py]
lastReviewed: 2026-09-25
---

Voxelforge has two ways to produce images, and picking the wrong one wastes
minutes per attempt.

## `--shot` / `--shotlist` — offscreen, one load, many frames

The app parses a shotlist file and renders every entry after a **single**
world load:

```
# path camx camy camz tx ty tz
/tmp/a.ppm  -18.0 4.2 14.0  0.0 1.2 0.0
```

Blank lines and `#` comments are skipped; a line is used only if all seven
fields parse. The first triple is the camera position, the second the
look-at target. Because the load is ~17.6 s
([[concepts/load-time-field-build]]) and is paid **once per invocation**, an
N-shot reel costs the same wall-clock as a single `--shot`. This is why
`visual_check.py` renders its whole shot set per backend in one run.

**This is the right tool for any deterministic capture**: repeatable output,
no window manager involved, no GPU contention with whatever else is running.

**Critical limitation: the ImGui HUD is not in these frames.** `--shot`
reads back the offscreen image, so the sidebar, footer, and brush tints are
absent. No visual gate can catch a GUI regression this way — see
[[concepts/x11-input-injection]] for the live alternative.

Useful knobs: `--animtime <s>` freezes animation at a fixed time so water and
foliage are identical between runs; `--sun <elev> <azim>`; `--mode splat|svo`
to A/B the two backends; `--width/--height`.

## `tools/record_demo.py` — a keyframed tour, packaged

`record_demo.py` wraps the shotlist mechanism with a ten-shot tour of the
default lakeside hamlet, plus a **variant matrix** for A/B work.

```bash
python3 tools/record_demo.py --list                    # keyframes + variants
python3 tools/record_demo.py --out /tmp/opencode/demo --sheet
python3 tools/record_demo.py --variants splats voxel   # just the A/B
python3 tools/record_demo.py --mode live --out /tmp/opencode/live
```

### Where the hamlet actually is

All manifest `pos`/`rot` are zero — object content is baked in absolute
lattice coords. Convert with `world = (cell - 512) * VOXEL` (`VOXEL = 0.1`),
and read the bounds with `tools/vxw_dump.py --layer <name>`:

| layer | lattice bounds | world centre |
|---|---|---|
| hall | x[564..670] y[505..561] z[626..683] | (10.5, 2.0, 14.2) |
| tower | x[687..735] y[516..643] z[667..715] | (19.9, 6.7, 17.9), 12.8 m tall |
| pier | x[636..667] y[482..551] z[545..617] | (13.9, 0.5, 6.9) |
| boat | x[644..662] y[497..548] z[526..586] | (14.1, 1.0, 4.4) |
| well | x[636..667] y[509..548] z[615..648] | (13.9, 1.6, 12.0) |
| market | x[627..745] y[518..544] z[630..696] | (17.4, 1.9, 15.1) |
| garden | x[525..620] y[510..546] z[666..769] | (6.0, 1.6, 20.5) |

The village occupies **x 5–23, z 11–26 — not the origin.** A keyframe aimed
near (0,0,0) frames empty terrain.

The terrain also **sweeps** along that axis (sampled from
`assets/heightmap.png`): pier end ≈19.7 m, hall ≈10.9 m, tower ≈2.4 m,
well ≈−0.6 m, market ≈−3.3 m, garden ≈−5.6 m. Since the water plane is
y = −0.9, the market and garden sit *below* the waterline. Camera heights
therefore have to be per-shot; a single constant will drop shots into the lake.

### Variants cost one app launch each

`--shotlist` renders all keyframes under **one** world load, but the backend
(`--mode splat|svo`) and the splat knobs are read **per process**. So a
voxel-vs-splat or splat-tuning comparison needs one launch per variant; you
cannot put two backends in a single shotlist. Measured: ~8–11 s per variant
for ten shots.

The shipped variants:

| variant | mode | env | shows |
|---|---|---|---|
| `splats` | splat | defaults | reference pass |
| `voxel` | **svo** | defaults | the raymarcher vs the splat raster |
| `radius_min` | splat | `VF_SPLAT_RADIUS=0.5` | small disks |
| `radius_max` | splat | `VF_SPLAT_RADIUS=2.0` | large disks |
| `no_lod` | splat | `VF_LOD=0` | LOD rings off (renamed from `no_micro` when the micro removal 2026-10-08 deleted the `VF_MICRO=0` half — micro detail is unconditionally off now; the variant differs from `splats` only by `VF_LOD=0`) |

`VF_SPLAT_RADIUS` sets `m_radiusScale`, clamped to 0.5..2.0 in
`SplatPass::record` — the same knob the sidebar slider and the `[` / `]`
keys drive, so the stills match what a user sees interactively.

`--sheet` writes a per-shot contact sheet (one row, one column per variant)
so the two backends and the radius change sit side by side instead of in
separate files.

## Practical rules

- Reach for `--shotlist` first. Only use live capture when the thing under
  test is the GUI itself.
- Set `--animtime` for any comparison; otherwise water animation makes two
  "identical" renders differ.
- **`--shot`/`--shotlist` still create a titled `Voxelforge` window** even
  though they render offscreen, so they block live X11 capture just like an
  interactive run. Guard live tooling on the *presence of a window*, not on
  running processes — see [[concepts/x11-input-injection]].
- ImageMagick and `xdotool` are not installed. Use ffmpeg for capture and
  python3-xlib for input.
