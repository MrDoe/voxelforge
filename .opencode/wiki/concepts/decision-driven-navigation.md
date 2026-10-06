---
title: Decision-Driven Navigation (vf_nav)
tags: [tooling, navigation, tev1, ascii, x11, testing]
sourceRefs: [tools/vf_nav.py, tools/ascii_view.py, tools/vf_input.py]
lastReviewed: 2026-10-04
---

`tools/vf_nav.py` flies the live Voxelforge window around the hamlet with a
decision loop that never calls a vision model. The point is speed: no
`describe_image` (8-12B VL, seconds per call), no agent turn between steps.

1. **capture** - `wmctrl -i -a <win>` raises the app, `ffmpeg -f x11grab`
   grabs the client rect (proven path from `record_demo.py`).
2. **digest** - `tools/ascii_view.py` `block_map()` returns an ASCII layout
   map plus mean luma / blue / green fractions; a one-line verbal digest is
   derived from the same numbers (the map alone underuses the 0.8B model).
3. **decide** - state (digest + map + `Last move: ...`) goes to **tev1:0.8b**
   - the same model behind the `make_decision` tool - via a direct
   `POST http://127.0.0.1:11434/v1/systemone`; no node CLI startup.
4. **move** - one basic move: key holds (`w/s/a/d/e/q`) or a yaw/pitch sweep,
   injected with XTEST (`tools/vf_input.py`); a post-move capture is saved.

## Measured speed (2026-10-04)

| stage | cost |
|---|---|
| capture (x11grab) | ~0.4 s |
| ASCII digest + stats | ~0.1-0.2 s |
| tev1 decision, direct HTTP | 0.10-0.68 s warm |
| tev1 decision via `opencode-rag decide` (node) | ~1.9 s |
| vision describe (describe_image) | seconds - **excluded from the loop** |

Whole runs: 6 stop-heavy steps 25.8 s; 6 forward-heavy steps 37.2 s
(movement holds dominate). `run --steps N` executes the loop in one process,
so no LLM round-trips sit between steps. `--policy fast` skips tev1 entirely
(a small rule table on the digest) - a latency floor, not a navigator.

## Rules that matter

- **Raise before capture.** x11grab photographs whatever is stacked top-most;
  a peer's near-fullscreen terminal was captured as a "dark frame" (mean luma
  ~23) for minutes. Xlib `set_input_focus` did not raise it; `wmctrl -i -a`
  did. Distinguish "display dark" from "wrong window" by grabbing the root
  screen: the root was equally dark, and no locker/DPMS was active.
- **Crop the sidebar** (`--skip-left 340`) or the UI chrome pollutes the map.
- **The 0.8B model has a stop bias** on ambiguous frames (five consecutive
  `stop`s in a dark close-up). Mitigations now in `one_step()`: the state
  carries the last move; the `stop` criterion says "use ONLY when the view
  already shows the village, water, or open landscape"; and a second
  consecutive `stop` is overridden to `turn_right`.
- **It responds to the state, weakly.** A controlled A/B (open lake vs
  close-up wall) gave different choices, but raw ASCII maps produced
  near-identical posteriors for visibly different frames - hence the verbal
  digest leads, the map is supporting detail.
- **Humans co-piloting is a confound.** A user poking at the window shows up
  as `edit tool ->`, `TAA ->`, `Swapchain:` lines in the app log and moves
  the camera too; don't attribute every change to injected input.

## Instrument: pick-miss detection

`Ctrl+LMB` logs `pick selected ... world X Y Z mat M layer L` (see
[[entities/live-edit-brush]]). No pick line after a click means the centre
ray hit no geometry - a cheap "looking at nothing / outside the world"
signal, and the same ray reports the world position of whatever is ahead.
