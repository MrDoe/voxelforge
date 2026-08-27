# AI editing: chat backend, tools, MCP server

Two entry points mutate the world:

1. the **in-app chat window** (`src/app/chat_ui.cpp`) talking to an
   Ollama / OpenAI-compatible server;
2. the **`vf_mcp` stdio MCP server** (`src/ai/mcp_server.cpp`) for external
   agent hosts.

Both funnel through `vf::ai::normalizeToolCall()` and write records into
`assets/ai_edits.vxw` via `EditableWorld`. A running voxelforge picks changes
up within ~0.5 s (disk poll) or instantly (chat triggers a reload directly).

## Chat backend configuration

Precedence: env var > CLI flag > default.

| source | variables |
|---|---|
| env | `VF_LLM_URL`, `VF_LLM_MODEL` |
| CLI | `--llm-url` / `--ollama-url`, `--llm-model` / `--ollama-model` |
| defaults | `http://127.0.0.1:11434`, model `gemma4:12b`, 120 s timeout |

Endpoint shapes are auto-detected in `OllamaClient::setBaseUrl()`: a URL
ending in `/v1` switches to `/v1/chat/completions` (llama.cpp, vLLM, …);
anything else uses Ollama's `/api/chat`. The choice is **sticky** — once a
request succeeds on one path it stays until the URL shape changes again.
`ping()` health-checks via `GET /api/tags` / `POST /api/show`.

Requests run on a worker thread; tokens (content + optional thinking trace)
stream into the UI, finished results are queued and polled each frame.
Tool calls execute on the main thread during poll.

### System prompt

`src/ai/system_prompt.hpp` pins down: world constants, the anchor rule
("the picked voxel IS the center-bottom voxel"), the palette ids, the six
**exact** tool names, placement rules ("here" → omit anchor), and response
style. If you change canonical tools or palette size, update this file too.

### Tool-call normalization (`src/ai/tools.hpp`)

Small models invent names ("add_rock", "rock_1", "place box") or omit
arguments. Normalization rescues them:

- `toolKey()` — lowercase alnum only, strip verb prefixes
  (`add/create/make/place/put/spawn/build/new`, repeatedly), strip trailing
  digits: `"Add_Rock_2"` → `"rock"`.
- `canonicalToolName(key)` — substring match onto the canonical set:
  cylinder/pillar/column/post → `create_cylinder`;
  ellipsoid/sphere/boulder/rock/stone/pebble → `create_ellipsoid`;
  stamp/mosaic/pixel → `create_stamp`; listworld/layers/list → `list_world`;
  box/crate/cube/block/chest/brick → `create_box`; exact `probe`.
  Structural nouns win over rock-family keywords ("stone_pillar" is a pillar).
- `injectToolDefaults()` — missing args get conservative defaults:
  box `[4,4,4]`, cylinder r0.35 h0.9, ellipsoid `[0.5,0.5,0.5]`
  (`[0.6,0.45,0.6]` flat boulder when the name says rock). Material inferred
  from the name when absent (white/grey/light-rock→5, rock/stone→4,
  wood/log/plank/crate→6, leaf/foliage/hedge→8, sand→3, soil/dirt→2).
  Explicit arguments are never overridden.
- Unknown keys return an error listing valid tools; the chat UI retries with
  that feedback up to 2 times.

Canonical chat tools: `create_box, create_cylinder, create_ellipsoid,
create_stamp, list_world, probe`.

## EditableWorld API (`src/voxel/editable_world.{hpp,cpp}`)

Persistent layer = `assets/ai_edits.vxw` + its manifest entry. All anchors are
**bottom-center lattice cells**: the selected voxel's center is where the new
object's bottom-center sits.

```cpp
EditableWorld(std::string assetDir = VOXELFORGE_ASSET_DIR);

bool   load();                 // reads ai_edits.vxw if present (missing OK)
bool   save() const;           // writes record-only .vxw + ensureManifest()
bool   ensureManifest();       // adds ai_edits entry (first object layer,
                               // starts disabled) if absent
void   enableInManifest() const; // flip enabled=true (done on first edit)

std::vector<VoxelRecord> makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const;
std::vector<VoxelRecord> makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const;
std::vector<VoxelRecord> makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const;
std::vector<VoxelRecord> makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const;
std::vector<VoxelRecord> makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const;

size_t importLayer(const std::string& vxwPath, glm::ivec3 anchor);
size_t append(const std::vector<VoxelRecord>&);      // dedupe, first wins
size_t appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat);
void   clear();                                      // wipe + persist
```

Notes:

- Rasterizers sample the SDF over the shape's AABB and keep cells within the
  ±0.20 m surface band (`kBand`) — shells with correct materials, matching how
  the baker sweeps authored objects.
- `makeBox`: `sizeVox` *includes* the bottom-center cell (y grows upward from
  the anchor row; x/z extend around it).
- `makeEllipsoid` centers at `anchorCenter + (0, radius.y, 0)` so the bottom
  pole touches the anchor voxel center.
- `importLayer` translates a foreign `.vxw` so its bounding box's bottom-center
  lands on `anchor` and appends the copy to `ai_edits.vxw` — the only runtime
  placement path for baked layers (enabling a layer shows it at its baked
  coordinates instead). Dedupes against existing edits; returns count added.
- `clear()` empties the layer but keeps the manifest entry (disabled).

## vf_mcp protocol

Transport: stdio, newline-delimited JSON-RPC 2.0 (MCP stdio transport).
All logging goes to **stderr** — stdout is the transport.

Methods: `initialize` (replies protocolVersion `2024-11-05`, tools capability),
`ping`, `tools/list`, `tools/call {params:{name, arguments}}`; notifications
are consumed silently; unknown methods get error `-32601`. Tool results are
`{content:[{type:"text",text:…}], isError?}`.

The server loads its own `LayeredWorld` for probe/ground queries (includes
ai_edits), and writes edits through its own `EditableWorld` instance.

### Tools

| tool | arguments | behavior |
|---|---|---|
| `list_layers` | – | name/file/role/enabled per layer (packed skipped) + session edit count |
| `enable_layer` | `layer` (name or file), `enabled` | rewrite manifest entry; refuses landscape ("always enabled") and packed roles |
| `probe` | `x,y,z` (meters) | signed distance + material id + solid/empty from the merged field |
| `ground` | `x,z` | terrain height + suggested bottom-center anchor cell |
| `add_box` | `anchor` \| `ground`, `material`, `size` (voxels) or `size_m` (meters) | default size 4³ voxels, clamped to [1,32]³ |
| `add_cylinder` | `anchor`\|`ground`, `radius`, `height`, `material` | defaults r=0.35 m h=0.9 m |
| `add_ellipsoid` | `anchor`\|`ground`, `radius:[rx,ry,rz]`, `material` | default [0.5,0.5,0.5] m |
| `add_stamp` | `anchor`\|`ground`, `cells:[{dx,dy,dz,mat}…]` | ≤1000 literal 0.1 m cells relative to anchor |
| `clear_edits` | – | wipe `ai_edits.vxw` |

Conventions:

- Anchors are bottom-center lattice cells `[0..1023]³`; alternatively pass
  `"ground":[x,z]` to snap onto the terrain surface (uses the live field).
  Out-of-range components clamp.
- `material` clamps to 0–8, default **6 (wood)**.
- Non-native tool names go through the fuzzy normalizer (`create_*` maps to
  `add_*`), so LLM bridges can send invented names safely.

Example session by hand:

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | ./build/vf_mcp
echo '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add_box",
      "arguments":{"ground":[3,9],"size":[6,4,6],"material":4}}}' | ./build/vf_mcp
```

Registration (opencode already wires this in `.opencode/opencode.json`):

```json
{ "command": "./build/vf_mcp", "args": [] }
```
