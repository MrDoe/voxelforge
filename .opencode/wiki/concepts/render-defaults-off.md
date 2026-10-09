---
title: Render defaults shipped off
tags: [rendering, taatrivial, defaults, measurement, taa]
sourceRefs: [src/app/app.hpp, src/app/frame/record_interactive.cpp, src/app/ui/panel_render.cpp, src/voxel/surfelize.hpp, docs/rendering.md]
lastReviewed: 2026-10-09
---

The three optional appearance features that a viewer can toggle but that a
headless reference shot never exercised were shipped **off** on 2026-10-09:

| feature | member / flag | default |
|---|---|---|
| crease bridges | `App::m_edgeFill` / `VF_EDGE_FILL` | `false` |
| corner caps | `App::m_cornerFill` / `VF_CORNER_FILL` | `false` |
| TAA | `App::m_taaEnabled` | `false` |

`SurfelParams::edgeFill` / `cornerFill` were flipped with them so the library
default and the app default cannot drift apart (see
[[concepts/uncommitted-edit-is-not-yours]] — a second "default" is a
stale-truth hazard).

## The measured split (why two of these are defensible and one is not)

A 2x2 A/B at 960x540 on hero/house/water plus a thin-beam close-up, each
against a same-settings rerun as the noise floor:

| comparison | mean L2/code | px >12 | verdict |
|---|---|---|---|
| `cornerFill` off | 0.0076-0.024 | 0.00 % | **no-op on the hamlet** |
| `edgeFill` off | 0.059-0.575 | 1.14-1.24 % | **not a no-op — crease sealing** |
| noise floor | 0.0088 | 0.00 % | — |

Corner caps are a no-op *on this content* only. The guard is live: a genuine
thin rod emits 8 caps, unit-test pinned. Do not read "no-op" as "dead code".

Edge bridges are load-bearing. The same ~0.54 L2 magnitude appears at the
cabin corner posts, which are 2-3 cells across — there the hard-edge shrink
opens a real hole and the bridge closes it. Interior one-cell posts read 0 px,
because their parents were never shrunk. See
[[concepts/edge-aware-surfel-radius]] for the full table.

## Why TAA shipped off anyway

TAA does have a real effect (`taa_resolve.comp` reprojects through the G-buffer
and variance-clamps). It is off because the interactive view now equals what
`--shot` has always produced, so a comparison against a reference shot needs no
"and TAA was off" caveat, and the viewer no longer carries the NaN-sticky-history
failure mode documented in `AGENTS.md`.

## The coupling fix this required

`m_prevCam` was advanced **only inside** the TAA branch of
`App::recordInteractiveFrame`. The opt-in motion blur (`K`) reprojects against
the same `m_prevCam` in `App::recordPhotorealism`, so with TAA off the first
motion-blur frame reprojected against a zero camera. The update now happens in
both branches, once per interactive frame. Any future gate on TAA must keep that
invariant: `m_prevCam` is not TAA-private state.

## Reverting

Flip the three booleans back and remove the "default **off**" wording from
`AGENTS.md`, `docs/rendering.md`, `docs/tooling.md`,
`docs/getting-started.md` and
[[concepts/edge-aware-surfel-radius]]. The env overrides
(`VF_EDGE_FILL=1`, `VF_CORNER_FILL=1`) still work, so an A/B does not need a
rebuild.

## The one gate interaction this caused

Nothing in the flip changes the live-edit defeat, but it blinds one of its
loudest symptoms. `live_edit_check.py`'s `check_object_undo_surgical` has three
assertions; its edge-bridge secondary (`after[1] > 200`, "N edge bridges for a
one-voxel undo") **cannot fire any more**, because the app now emits zero edge
bridges by default. The run split reads 11002+0 against 11008+0 — the primary
content-free `before != after` assertion is still red, so the undo-margin defect
is **untouched**: only the bridge count moved to zero and that particular
message stopped appearing. live_edit_check therefore reports 4 failures instead
of 5, which looks like an improvement and is not one. Treat any change in a
failure count after a default flip as "which assertion stopped firing", never as
"what got fixed" - the same reasoning as
[[concepts/measurement-discipline]]'s "silence is not health".

Corollary for every measurement in this page: a bridges ON-vs-OFF delta is only
meaningful if that run re-enables them explicitly (`VF_EDGE_FILL=1` /
`VF_CORNER_FILL=1`). All the numbers quoted here were taken that way; none
inherit the new default.

Related: [[concepts/surfel-holes]] (the coverage problem both fills address),
[[concepts/surfel-thin-rule]] (thin structures that keep their full footprint)
and [[concepts/focused-test-groups]] (`test-surfel` + `test-visual` are the gates
that cover a default change like this).
