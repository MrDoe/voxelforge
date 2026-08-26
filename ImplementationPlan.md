# Voxelforge — Status & Roadmap

> Historical milestone log (M0–M2) lives in `THREAD_SUMMARY.md`; the splat and
> dense-renderer phases were removed on the `VoxelsOnly` branch (see
> `rework.md`). This file tracks the current architecture state and what's next.

## Current state (post-rework)

| area | status |
|---|---|
| Chunked-SVO renderer (single path) | ✅ `svo_raymarch.comp`, TAA resolve |
| Records-only geometry (`VoxelField`) | ✅ terrain columns + EDT object grids |
| Splat renderer | ❌ removed (`21c5cb4`) |
| Dense reference backend + `--compare` | ❌ removed (`b4b30fd`) |
| Analytic runtime geometry / mask machinery | ❌ removed (`272dbb6`, `7847e90`) |
| Layered hot-reload (~0.5 s disk poll) | ✅ incl. incremental chunk rebuilds |
| AI chat UI + tool normalization | ✅ Ollama / OpenAI-compatible backends |
| MCP server (`vf_mcp`, 9 tools) | ✅ stdio JSON-RPC, opencode-registered |
| Data-driven materials & shadows | ✅ rg32f height texture, object flag bit, r8_snorm shadow volume |

## Verification gates

`ninja -C build && ctest --test-dir build` (unit + visual_check),
`--selftest`, `--probe`, `vf_slice` cross-sections. The old SVO↔dense parity
assert is gone with the dense backend; correctness is anchored by the unit
suite's analytic-truth cross-check instead.

## Roadmap

1. **GPU-side synthesis** — move VoxelField build/SVO bake to compute for
   sub-second full rebuilds (CPU currently ~1.5 s for 4.4 M records).
2. **Removal tools** — `remove_box`/paint-style MCP tools complementing the
   adders; per-layer undo.
3. **Object interaction** — pick-and-move semantics on top of the anchor flow,
   physics-lite (gravity settle) for AI-placed props.
4. **Streaming/larger worlds** — chunk LRU beyond the 102.4 m lattice.
5. **Material polish** — per-record roughness maps in brick word1 already
   carried; wire more of it into shading.
