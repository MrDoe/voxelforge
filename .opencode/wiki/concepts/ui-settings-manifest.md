---
title: UI settings as manifest data (the MCP control surface)
tags: [vf_mcp, ui, manifest, world.json, hot-reload, settings]
sourceRefs: [src/app/world/ui_settings.cpp, src/voxel/worldfile.cpp, src/ai/mcp_server.cpp, src/app/frame/run_poll.cpp, docs/ai-editing.md]
lastReviewed: 2026-11-19
---

# UI settings as manifest data

`vf_mcp` is a **separate stdio process**: it can write files but cannot touch
the running app's members. Every authoring tool in the server therefore rides
one channel the app already watches — `world.json`, polled ~0.5 s in
`App::pollWorldAndTextures` (`frame/run_poll.cpp`). Layers, textures and
lights already used it; as of 2026-11-19 the **sidebar's Render and Edit
settings do too**, as a flat, all-optional top-level `"ui"` object.

## The shape

`worldfile::UiSettings` (`src/voxel/worldfile.hpp`) is one float (or string)
per control, each defaulting to "absent" (`kUiAbsent = -1e30f` / `""`), so a
patch touches only what it names and a manifest without the block renders
byte-identically to any older file. Keys (full table in
`docs/ai-editing.md`): `renderFlags` (the bitmask — 0 AO, 1 Shadows, 3 Water,
5 SSR, 6 SSAO, 7 DetailNormals, 8 Enclosure, 9 TerrainInterp, 10 AutoExpose,
11 Transmission, 13 PhotoCards), `backend`, the effect toggles, `exposure`,
`tonemapLook`, SSAO / splat / edge-kernel knobs, `lightBudget`/`lightK`,
camera (`camSpeed`, `camPos`, `camYaw`, `camPitch`), the fur-coat set,
brush (`brushMode`, `brushWidth` voxels, `brushDepth`, `brushMaterial`),
and `scenery`.

MCP tools: `get_ui`, `set_ui` (merge-patch), `set_sun` (its own block),
`set_scenery` + `list_sceneries`. Writers go through the shared
`writeOwnedTopKey` in `worldfile.cpp` — fail-closed scan, temp+rename, a
read-back self-check, and a dangling-comma shape guard (a byte-equality
self-check happily persisted a leading-comma bug once; see the quirk).
`loadUiManifest` locates the block with `scanTopLevel`, not a raw `Json`
walk, because `Json::skipValue`'s depth skip is not quote-aware.

## Why it cannot fight the GUI

Three guards in `App::applyUiSettingsFromManifest` (`src/app/world/ui_settings.cpp`):

1. **Member-diff**: a field lands only when the file value differs from the
   live member; rebuild triggers (grass cards, fur, `requestWorldReload` for
   the edge-kernel knobs, `uploadLightSources`) fire only on real change. An
   unchanged file re-applied is inert, so a slider the user dragged is never
   snapped back by an unrelated layer edit.
2. **Last-seen snapshot** (`m_seenUi`, `m_seenSun*`): distinguishes "the file
   changed" from "the file exists". The authored sun is followed only when
   the file value moved — the session-only Day/Night (P) rule from the sun
   pipeline page holds.
3. **Between frames**: everything applies inside the poll (no command buffer
   records against a swapped world); `ui.scenery` only sets
   `m_pendingScenerySwitch`, the same deferred path the sidebar uses after
   the 2026-11-19 mid-record deadlock lesson. `switchScenery` resets the
   snapshots (`resetUiSettingsState`) so the new scene's block is judged
   against this switch's state.

Precedence at startup: the block applies **before** `applyStartupEnvOverrides`,
so an explicit `VF_*` flag for this run still wins over persisted intent.
The `heavy` flag skips rebuilds/uploads at startup (the init path does them).

## Altering objects and textures through MCP

Same poll carries the rest of the "more": `recolor_layer` rewrites a layer
`.vxw`'s records (material swap with an optional `from` filter, plus
`r/g/b/refl/rough` **and `tex`**), `update_light` edits one authored light in
place, and `set_texture` now carries `card` and preserves existing
`card`/emissive state on partial edits (it used to silently drop the card).
See [[entities/hud-sidebar]] for the controls this mirrors and
[[concepts/sun-direction-pipeline]] for why shadow rebake needs the reload.

**Drawing textures onto cells** has three levels: `set_texture` binds an
image to a whole material; `add_voxels`/`write_object` accept per-cell `tex`
when creating new voxels (the record's `reserved` byte → `Sample::tex` →
surfel `tan_aspect.w` → shader slot override, albedo only — the cell keeps
its own material's response); and `recolor_layer {layer, from?, tex:N}` sets
that override on **existing** cells, with `tex:0` clearing it back to the
material's own slot. `read_object` prints `tex` on every cell line (it did
not before), so read → modify → write now round-trips the override. Caveats
from [[concepts/per-cell-texture]]: atlas slots 9–15 are the emissive band,
and the override inherits the *cell material's* m/tile scale. Splats have no
direct texture paint — they are derived from voxels, so a `tex` change flows
into their surfels on the next reload.

## Gate

`tests/test_worldfile.cpp` → "worldfile ui manifest: write, merge, preserve
and parse booleans" (sparse round-trip, merge, foreign-key preservation with
a brace inside a string, bool literals, empty block). Live path proven by
hand: scratch scenery + `set_ui`/`set_sun` against a running app shows
`ui: applied N setting(s)` / `ui: scene sun ->` within the poll interval.

## In-app AI panel (2026-11-20 follow-up)

The sidebar's AI section no longer talks to Ollama directly: it is a thin
client of a **dedicated OpenCode session per scenery** (IDs cached in
`.opencode/voxelforge_sessions.json`). The `voxelforge` agent
(`.opencode/agents/voxelforge.md`) inherits all 44 vf_mcp tools and does the
authoring; the app supplies scene context, shows tool cards and agent
screenshots (`workspace/screenshots/`, gitignored), and offers **Undo Last AI
Turn** (file snapshots in `/tmp/opencode/ai_undo/`). Permission asks are
auto-answered "once" by decision — undo is the safety net. `vf_mcp` re-adopts
its cached world when the authored files' mtimes change so `probe`/`ground`
follow an undo.

The same manifest block gained **`uiFontScale`** (0.8–2.0): the panel's Text
slider writes it, and `App::rebuildUiFont()` rebuilds the ImGui font atlas and
resets/scales the style between frames (the style must be reset first —
`ScaleAllSizes` compounds). Startup applies the persisted scale right after
atlas creation.

### Model + text defaults (same day follow-ups)

The panel's model dropdown defaults to **Beam Free** (`opencode-go/beam-free`),
persists the choice per scenery, and applies it with `POST /api/session/{id}/model`;
free models are listed by default with an **All** toggle for the full catalog.
Reasoning deltas are hidden from the chat view. Default UI text scale is now
**1.2** (up from 1.0) for high-resolution screens. Startup logs
`ui: ImGui font rebuilt at scale X.XX` so the applied value is observable.
The app also calls `POST /api/experimental/mcp/voxelforge/connect` when a
session is set up — the mcp-lazy plugin starts with the server disconnected,
and without this the agent sees no `voxelforge_*` tools at all.
