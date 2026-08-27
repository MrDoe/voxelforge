# Voxelforge documentation

Voxelforge is a real-time dynamic voxel world rendered by a chunked sparse-voxel-octree
(SVO) sphere tracer, where a local LLM edits the world at runtime through chat or MCP
tool calls. Every piece of geometry lives in plain `.vxw` voxel-record files that are
merged, signed-distance-transformed and synthesized into GPU octrees on the fly.

## Reading paths

**I just want to run it**

1. [Getting started](getting-started.md) — build, bake assets, controls, troubleshooting

**I want to drive the world with AI**

1. [AI editing](ai-editing.md) — chat backend config, MCP server protocol and tool reference

**I want to change the code**

1. [Architecture](architecture.md) — how the pieces fit, module tour, reload model
2. [Contributing](contributing.md) — invariants, conventions, gotchas, recipes
3. [Testing](testing.md) — verification gates and debug workflows
4. Topic references as needed: [World format](world-format.md),
   [Rendering & GPU contract](rendering.md), [Tooling & CLI](tooling.md)

## Document map

| document | audience | contents |
|---|---|---|
| [`getting-started.md`](getting-started.md) | users, new devs | requirements, build, asset bake, controls, headless modes, troubleshooting |
| [`architecture.md`](architecture.md) | contributors | data flow, per-module responsibilities, synthesis pipeline, hot-reload model, frame loop |
| [`world-format.md`](world-format.md) | contributors | VXW v1 binary spec, `VoxelRecord`, manifest schema, layer merge semantics, `worldfile` API |
| [`rendering.md`](rendering.md) | contributors | descriptor/bindings, `GpuWorld` handle encoding, brick packing, push block, textures, shading overview |
| [`ai-editing.md`](ai-editing.md) | users, contributors | chat env vars, tool normalization, `EditableWorld` API, `vf_mcp` JSON-RPC reference |
| [`tooling.md`](tooling.md) | users, contributors | full CLI/env reference (`voxelforge`, `vf_mcp`, `vf_slice`, `heightmap_gen`), `start.sh` |
| [`testing.md`](testing.md) | contributors | test gates in order, suite breakdown, `visual_check` internals, debugging workflows |
| [`contributing.md`](contributing.md) | contributors | project invariants, code conventions, gotcha catalog, how-to recipes |

## History

Design history lives under [`history/`](history/) — kept for archaeology, **not**
safe as a reference for current code:

- [`history/rework.md`](history/rework.md) — the records-only architecture plan (executed)
- [`history/ImplementationPlan.md`](history/ImplementationPlan.md) — milestone history & roadmap
- [`history/THREAD_SUMMARY.md`](history/THREAD_SUMMARY.md) — dated design log (pre-rework systems described therein are gone)

`AGENTS.md` at the repo root is the engineering conventions file for coding agents
and stays authoritative for session behavior; these pages carry the deep detail.
