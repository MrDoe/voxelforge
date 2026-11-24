---
title: MCP intent bridge to the store lane
tags: [mcp, live-edit, store, architecture, parity]
sourceRefs: [src/ai/mcp_server.cpp, src/app/edit/mcp_intents.cpp, src/app/frame/run_poll.cpp, tests/mcp_v2_check.py, tests/mcp_view_check.py, .opencode/agents/voxelforge.md, .opencode/skills/vf-mcp/SKILL.md]
lastReviewed: 2026-11-24
---

# Why a bridge exists

`vf_mcp` is a separate long-lived stdio process. It has an `EditableWorld` and a
`LayeredWorld` built from **disk files**, but no `App`, no `ChunkStore`, no
`LiveEditor`, no GPU passes and no undo stack. The store lane — `runtime_edits.vxw`
plus the in-memory `ChunkStore` — belongs exclusively to the running app.

Therefore an MCP "stamp" tool cannot call `App::applyEditLive` inside `vf_mcp`.
The three tempting wrong answers are all harmful:
- reimplement the rasterizers in MCP → forked truth and GUI-vs-MCP parity bugs;
- write `runtime_edits.vxw` from MCP → `OverlayWriter` re-serialises only the
  subset it knows and silently shrinks a session (see [[concepts/overlay-silent-write-trap]]);
- shell out to `voxelforge --shot` with `VF_TEST_*` hooks → string-parsed,
  one-shot, no undo stack.

# The mechanism

- `vf_mcp` appends one JSON object per line to `scenery/<name>/mcp_intents.jsonl`,
  fsyncs, and returns `{id, status: "queued"}`.
- `App::pollWorldAndTextures()` calls `App::consumeMcpIntents()` **after** any
  world reload, so a stamp always edits the freshly adopted store.
- The consumer truncates the queue first (a crash leaves an unacknowledged
  intent, never a duplicate replay), then runs each intent. A stamp sets the
  real input state (`m_hoverHit`, `m_editBrush`, brush size, box scope) and
  calls `App::applyEditLive()` — the same function the GUI brush drives.
- Results go to `scenery/<name>/mcp_results.jsonl`; `stamp_result {id}` returns
  them. The report is **instrument-first**: edited cells, changed chunks, run
  surfels, elapsed ms, pick class and undo steps — before any pixel claim.

Undo, surfel/SVO patching, the height texture, water and overlay persistence
come for free because they live downstream of `applyEditLive`.

# Traps learned while building it

- `m_lastChangedChunks` is consumed and **cleared** by `refreshLiveLights()` at
  stroke end. An instrument that must survive to a report needs its own field
  (`m_lastEditChunks`, `m_lastEditCells` set in `commitStoreEdits`).
- Saving GUI state around an intent is mandatory: otherwise the brush stays
  armed at the MCP mode/size and the hover outline jumps.
- Hermeticity: every scratch render sets `VF_OVERLAY_PATH=<tmp>`; an MCP test
  that forgets can rewrite the real scenery overlay (see the linked trap page).
- Scenery env: `vf_mcp` is launched by the OpenCode service with the service's
  environment, so `VF_SCENERY` on the app does not reach MCP tools in normal
  use. Tests must set it on both processes.

# Latency budget: "dig a cave here" in seconds

Measured 2026-11-24 on a LakesideHamlet scratch scenery with a live headless
App (`tests/mcp_view_check.py`; fresh `vf_mcp` process per call):

| stage | measured | why |
|---|---|---|
| MCP process start | **4 ms** | `init()` only loads `ai_edits`; `LayeredWorld` is a lazy static |
| `help` | **3 ms** | now skips `refreshIfChanged()` — it never reads the world |
| `view_pick` / `view_click` / `stamp` | **3–5 ms** to queue | skip the world build; the App owns picking and rasterisation |
| `ground` / `surface_scan` | **~5 s** | these genuinely need `LayeredWorld`+`VoxelField` |
| `dig_cave` one-call round trip | **787 ms** | one intent + inline result poll; no second tool round trip |
| cave first stamp (mouth opens) | **89 ms** | first `applyEditLive` in the compound stroke |
| complete 13-stamp dripstone cave | **776 ms** | 1,365 touched cells, 2 chunks, 8,789 final run surfels |
| intent → `applyEditLive` | **one frame** | `consumeMcpIntents()` runs every frame in `pollWorldAndTextures`, NOT on the 0.5 s layer-poll |
| disk-lane `.vxw` rewrite → live view | **≥0.5 s**, sometimes never | poll watches `world.json`; rewriting an existing layer's `.vxw` does not bump it |

The app-side bridge was never the bottleneck. The seconds were spent **before**
the intent existed: the agent's old "read before you write" workflow issued up
to 8 inspection calls (the first paying the ~5 s field build), then wrote through
the disk lane, then rendered a verification shot (another ~5 s process). The
fix is a one-call fast path: `.opencode/agents/voxelforge.md` now requires a
local edit at the UI-context cell to be the FIRST tool call through
`dig_cave`/`view_click`, with no inspection, no `.vxw` write and no
`render_shot` before the reply. `dig_cave` returns the engine report inline,
including `firstStampMs`, so one tool call is enough. Residual latency is model
inference, not the engine.

# Beyond stamps: a click into the view

The bridge now carries every edit tool, not just cell stamps. `vf_mcp` exposes
`dig_cave`, `view_pick`, `view_click`, `view_undo`, `view_clear` and `view_box`;
each posts an intent (`dig_cave`, `pick`, `click`, `undo`, `clear`, `box`) that
the consumer dispatches to the App's own functions. `dig_cave` is the one-call
selection fast path: the App composes mouth/tunnel/chamber/dripstone from the
GUI brush rasterizers under one drag state, so the whole feature is one undo
step, and the stdio server waits up to two seconds for the engine report and
returns it inline. No `LayeredWorld` build is needed on that path. A
`screen:[x,y]` target — **logical window pixels**, the same space GLFW/ImGui
use, not framebuffer pixels — calls
`App::pickAtWindowPos`, so the ray conversion, the `ChunkStore` march and the
micro-vs-world competition are the mouse's, not a second implementation. A
`cell:[x,y,z]`/xyz target remains for camera-independent edits. Measured on the
shipped hamlet: screen [160,90] of a 320×180 headless view resolves to cell
[554,524,602], material 6, owner `hamlet_cabin.vxw`, distance 8.23 m.

Dispatch reuses one engine path per tool — `applyEditLive` for carve/add/
delete/paint/smooth, the selection box's `set`/`clear`, `finishStroke` +
`undoEdit`, `clearLiveEdits`, and (for rotate/move) `commitRotation`/
`commitMove`. `commit:false` arms the staged preview instead of writing. The
same state save/restore that protects the GUI brush wraps a cell stamp; a
transform intentionally leaves its staged pose visible as the result.

Two traps cost a gate cycle each:

- `commitRotation`/`commitMove` look a layer up by its manifest **file**; an
  agent passes the friendly `name` (`mcp_view_prop`), so the consumer matches
  name OR file and then normalises to `it->file` before staging. Without that,
  the commit silently no-ops and reports `placed:[0,0,0]` with `ok:true`.
- The queued message ends `id=…; call stamp_result…`; a test regex `id=(\S+)`
  captures the semicolon and every result lookup misses. Use `id=([^\s;]+)`.
  Also, writing your own intent lines with the MCP tool names (`view_pick`)
  instead of the posted intent names (`pick`) makes every intent "unknown" —
  the server's queue file is the source of truth, not the returned text.

Results are read by `stamp_result` or `edit_result` (the same
`mcp_results.jsonl`). Gate: `tests/mcp_view_check.py` (CTest `mcp_view_check`,
label `mcp`); it proves pick/select/screen-carve agree on one cell, exercises
all seven tools, checks manifest persistence and verifies the real scenery
overlay is untouched.

# Convergence

`commit_overlay` is also an intent: the App promotes live solid additions and
paints into `ai_edits.vxw` (the live layer the baker preserves), refuses pure
deletions (record layers cannot express air) and then clears the overlay.
See [[entities/vf-mcp-v2]] for the full tool surface.

## Camera pose is read through the same queue

`set_ui` now accepts `camTarget:[x,y,z]` (or `camTargetX/Y/Z`) in metres.
The App derives yaw/pitch from camera position → target on the next manifest
poll; explicit `camYaw`/`camPitch` remain the alternative. `camera_pose` is a
**read-only intent**: it posts to the same queue and the App reports the live
camera (position, yaw, pitch) in `mcp_results.jsonl`, so an agent can prove
where the running view actually landed. `get_ui` prints the persisted block as
AUTHORED and points at `camera_pose`, because the manifest is not the live
camera. The new `camTarget*` fields use the same `kUiAbsent` field-diffing as
the other UI keys, so a GUI drag is never snapped back by the poll.

## See also

[[concepts/decision-driven-world-editing]] (proposal) — a Clef decision loop
that generates option sentences from the observation and compiles the chosen
one into exactly these intents. It adds the lane-discipline rule (store burst
vs authored-file burst) and names the engine gaps this bridge still has:
`tex` on store-lane paint and an instrument for disk-lane tool results.
