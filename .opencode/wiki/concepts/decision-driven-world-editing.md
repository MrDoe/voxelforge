---
title: Decision-driven world editing (Clef option loop)
tags: [ai, mcp, decision-model, clef, world-editing, proposal]
sourceRefs: [tools/vf_nav.py, src/ai/mcp_server.cpp, src/app/edit/mcp_intents.cpp, src/voxel/chunk_store.cpp, .opencode/agents/voxelforge.md, docs/ai-editing.md, https://github.com/swedishembedded/brain/tree/main/samples/decision/doom]
lastReviewed: 2026-11-24
---

**Status: proposal, not implemented.** Nothing in the app or `vf_mcp` changed
for this page. It synthesizes [[sources/brain-doom-decision-sample]] with the
existing [[concepts/mcp-intent-bridge]] and the chat panel's current
one-turn workflow.

# The idea

Today the in-app chat is turn-based: one prompt → an OpenCode session whose
agent (`.opencode/agents/voxelforge.md`) calls `vf_mcp` tools → a reply, with
a whole-turn undo snapshot. The fast decision loop in [[concepts/decision-driven-navigation]]
drives only the camera. The proposal joins them: the chat LLM sets a **mission
once** and narrates the result; a small Clef decision model runs a tight
**observe → choose one option → execute → re-observe** loop over the engine's
own action paths.

The point is not realtime. A stamp is 5–25 ms and a compound `dig_cave` is
~776 ms; the engine sets the cadence. The win is collapsing an 8-inspection-call
agent turn (first call paying the ~5 s `LayeredWorld` build) into one mission
plus cheap local decisions.

# Interface, not model, is the design work

From the Doom sample, four rules transfer directly:

1. **Options are sentences rebuilt from the current observation**, not a fixed
   verb vocabulary. An option exists only while it is meaningful; the model
   sees the sentence, never the hidden `tag`, engine args or instrument name.
2. **The mission is prepended to every option** — it selects which action
   families are generated, which facts are reported and what counts as done.
3. **Observation is honest**: surfaces actually observed, bearings rather than
   absolute coordinates, and the agent's own history in the state. Withhold
   manifest truth (distant AABBs, hidden layer metadata) until the run has
   looked; geometry from `ground`/`probe` leads to the frontier of probed cells
   until the target is seen.
4. **Turn and act are separate decisions**: `view_pick` (or a camera look),
   then `view_click`; `commit:false` staging, then commit. Combining them
   reintroduces the Doom sample's "walk the arc into the corner" failure.

## Mission-typed action families

| Family | Example option sentence | Lane | Support today |
|---|---|---|---|
| Cell brushes (carve/add/delete/paint/smooth) | "carve 0.6 m into the face ahead"; "smooth the ledge 40° left" | store → `view_click` → `applyEditLive` | supported; `edit_result` reports cells/chunks/ms |
| Object transform, world **and micro** | "stage a 30° yaw on the cabin about its base"; "commit the staged move" | store intent → `commitRotation`/`commitMove` | supported, including the micro re-bake/preview path; two decisions (stage, commit) |
| Object authoring (box/cyl/ellipsoid/stamp/write_object/import_mesh, duplicate/rename/delete layer) | "place a 0.8 m rock on the ground 2 m ahead" | authored data (`.vxw`, `world.json`) | supported, ~0.5 s reload, no per-action undo |
| Micro objects (transform, behaviour, pose, rig) | "turn the alpaca to face the path"; "let it graze" | transform = store intent; behaviour/pose = manifest | transform supported; behaviour/pose disk-lane; freeze gait for determinism |
| Textures / materials (bind art, per-cell atlas slot, recolor) | "bind gravel to material 5"; "set this face's atlas slot to 17" | `set_texture`/`recolor_layer` (disk); paint override (store) | disk supported; store paint carries `StoreCell::tags` but the intent does not expose `tex` |
| Selection / camera / undo / box | "turn to look at the object you saw to your right"; "scope the brush to this box" | `view_pick`/`view_box`/`view_undo`; camera via the XTEST path in `tools/vf_nav.py` | supported; `set_ui`/`set_scenery` stay denied to the agent |
| Lights | — | — | **excluded by policy**: only sun/moon and visible emissives light the scene; `add_light`/`update_light` are disabled. The legal verb is assigning an emissive material — see [[concepts/enclosed-space-lighting]] |

## Observation shape

Short, prose, family-relevant; mission header first, then surface facts from
the last pick (material, atlas slot + alignment, slope, bearing/distance),
last-seen memory, family-specific history ("you recoloured this face twice",
"you moved this layer 3 decisions ago and it still overlaps", "you are posing
neck link 2 and are seeing it through for 2 more decisions"), and a just-now
event from `edit_result`. The history block is load-bearing: in the Doom
sample removing it cost 2.67 return.

## Lane discipline — the constraint this broadens

Store-lane actions land next frame and are reversible per stroke
(`view_undo`). Authored-lane actions rewrite tracked files, hot-reload in
~0.5 s, and are only undoable through the panel's whole-turn snapshot.
Therefore: **one lane per burst, or an explicit checkpoint at transitions**,
announced in the observation ("you have committed this part; the next step
changes authored files"). A mixed burst that freely switches lanes leaves an
unreviewable blend of overlay edits and manifest writes.

## Decision backend: Clef on llama.cpp

- Endpoint `http://localhost:8080/v1/systemone` (llama-swap fronting llama.cpp),
  `Authorization: Bearer sk-llama-local` (env `LLAMA_SWAP_API_KEY`). **Not
  Ollama, not `make_decision`, not tev1** — [[concepts/decision-driven-navigation]]
  uses the legacy tev1-on-Ollama path and should not be copied for this loop.
- Models: `Clef-Flash` (GPU 1) for every loop decision; `Clef` (27B CPU) for
  low-confidence cases. `tools.clef.ask` is Code-Mode-only; a native loop calls
  the HTTP endpoint directly. Clef emits no prose and calls no tools, so the
  chat LLM remains the mission-setter and narrator.
- One call can answer three independent questions on the same state: `choice`
  for the next option, `noul` for "goal reached?", `score` for prefix progress.
  **Measured 2026-11-24**: one choice over 7 options, 515-token state →
  **195 ms** end to end (~5 decisions/s).
- **Escalation rule**: the same measurement picked option B at p=0.343 with A
  at p=0.339, confidence 0.234 — a near-tie caused by two near-duplicate carve
  options. Consolidate near-duplicate sentences (fix radius/depth/material in
  the mission) and re-ask with `Clef` (27B) or fall back to the deterministic
  `quick_edit` router when confidence < ~0.25 or the top two are within ~0.05.

# Engine gaps to close (all on the one-engine-path rule)

1. Expose `tex` on store-lane `stamp`/`view_click` paint → `StoreEdit::tags` →
   surfel `tan_aspect.w`; the store already carries it, only intent validation
   is missing.
2. Give disk-lane tools (`set_texture`, `recolor_layer`, `set_layer_transform`,
   `set_layer_behaviour`, `set_pose`) an instrument like `edit_result`, or
   no-op detection and the prefix score cannot see them.
3. Add a quiet/lockstep agent mode (freeze behaviour clocks as
   `VF_TEST_BEHAVIOUR_FREEZE` does) and log `tag|params|observation-digest` per
   decision so a burst replays from the authored scene — the Doom sample's
   "search may use snapshots, the artifact may not".
4. Generalize the existing `quick_edit` router (cave/tree/path/gravel) into
   the mission → family selector; the decision model chooses among generated
   options, not among raw tools.

# Verification

One scripted episode per family — carve a cave, soften a roofline, bind gravel
to a path, place and rotate a rock, pose and release the alpaca, scope a box
then delete inside it — each with a prefix-computable score (`return == progress`)
and a "no-op within N decisions" assertion. Compare two option-generator
configs by **paired sign test over the same episodes**, not one run each. Every
scratch run uses `VF_OVERLAY_PATH=<tmp>`; see [[concepts/overlay-silent-write-trap]].

Cross-references: [[concepts/crosstalk-extension-lessons]] (the coordination
policy to follow when several sessions edit the same wiki and world),
[[concepts/decision-driven-navigation]] (the camera loop and
its measured 0.8B stop bias), [[concepts/mcp-intent-bridge]], [[entities/vf-mcp-v2]],
[[sources/brain-doom-decision-sample]], [[concepts/focused-test-groups]].
