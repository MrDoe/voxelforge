---
title: Enclosed-space shading and light sources
tags: [shading, lighting, cave, ambient, ibl, worldfile, manifest, vulkan, bit-1, render-flags]
sourceRefs: [shaders/common_base.glsl, shaders/common_svo.glsl, shaders/common_splat.glsl, src/voxel/worldfile.cpp, src/voxel/worldfile.hpp, src/render/texture_atlas.cpp, src/render/texture_atlas.hpp, src/render/splat_pass.cpp, src/render/svo_pass.cpp, src/app/rhi/surface.cpp, src/app/world/world_layers.cpp, src/app/ui/panel_render.cpp, tests/test_worldfile.cpp, docs/rendering.md]
lastReviewed: 2026-10-08
---

> **Landed 2026-10-08:** the emission path below is live (store enumeration,
> budget 192, parity green). Line numbers drift with the tree — re-verify
> before citing.

# Why a cave stayed as bright as a meadow

The renderer had exactly one light: the sun. Everything else — the analytic
`skyIrradiance()` taps and the pre-filtered environment cubemap
(`iblContribution()`) — is **ambient with no occlusion term at all**. Baked AO
only reaches 0.18 m and 0.60 m (`aoBake` / `splatAO`, 2 rings × 4 taps), so it
grounds a contact shadow and nothing more. A cave 3 m across is far outside
that radius: the interior read as bright as open ground, because it *was*
being lit like open ground.

## The fix is two independent things

1. **A one-ray enclosure test** (render-flag **bit 8**, default ON;
   `m_renderFlags` 255 → 511, and `VF_RENDER_FLAGS=255` restores the old
   image bit-exactly).
   `skyVisibilitySvo` / `skyVisibilitySPlat` cast a single ray along
   `mix(bent, +Y, 0.65)` and return 0/1. `enclosed = 1 - vis` then scales
   ambient `mix(1, 0.16)` and IBL `mix(1, 0.04)`.
2. **A light-source lane**, because darkening alone only makes caves dim,
   not lit.

### The cave fill is load-bearing, not cosmetic

Removing the daylight without adding anything back pushed the `house` shot to
**7.21 %** black-in-silhouette and tripped `visual_check`'s 5 % hollow-voxel
gate. The gate is a good proxy for "did you just make everything black", so
the fix is an albedo-scaled fill standing in for wall multi-bounce:

```glsl
col += alb * vec3(0.075, 0.080, 0.090) * enclosed * (0.35 + 0.65 * ao);
```

Measured `house` black-in-silhouette: **2.70 %** (bit 8 off) → **7.21 %** (dark,
no fill) → **4.26 %** (dark + fill).

### The splat sky test must NOT consult the heightfield

`heightAt()` returns the terrain *top*, so `sp.y - heightAt(sp.xz) < 0` is true
for **every** point below the surface — including the floor of any room, and
especially of a room dug into a hillside. Including it in the sky test marks
interiors as underground and diverged from the SVO reference by tens of
points of enclosed coverage. A heightfield cannot have overhangs, so an upward
ray can never be occluded by terrain: the splat sky test uses `objDist` alone.

The **per-light** shadow test is the opposite case and does march terrain — a
lamp behind a hill must not light the far side.

## Light sources

```json
"lights": [
  { "pos": [6.0, 1.6, 11.5], "color": [1.0, 0.55, 0.18],
    "radius": 7.0, "intensity": 6.0 }
]
```

- `worldfile::loadLightManifest` → `std::vector<LightSource>`; max
  `kMaxLights` (256); entries with `radius <= 0` or `intensity <= 0` are
  dropped, not uploaded as lights that can never contribute. A light may
  carry `"follow": "<layer>.vxw"` — then `pos` is a pivot-relative local
  offset resolved as placedPivot + placementR * pos, so Move/Rotate of that
  layer carries the lamp (live in the splat preview, exact after reload).
- Packed into `LightUBO` — two `vec4[256]` (pos+radius, color+intensity),
  then `count`, `perPixelK`, 2 pad ints (`alignas(16)` std140). Fill is
  authored-first via the shared `fillLightUBO`, truncated at the budget
  (`kLightBudgetDefault` = 192, room for the measured ~170 store clusters;
  adjustable live via the Emitter-budget slider / `VF_LIGHT_BUDGET`). A unit
  test pins every offset, because moving either side of the contract
  silently reads a garbage count rather than failing to compile.
- Binding **25** in all three descriptor sets (SVO, splat forward, splat tile).
  The tile set's binding array is indexed by *binding number*, so a new
  binding at the end means growing `tb[]` **and** `tli.bindingCount` **and**
  the descriptor pool's uniform count — the array is sized to the highest
  index + 1, not to how many entries are filled.
- `applyLights()` lives in `common_base.glsl` and dispatches the shadow march
  through `#ifdef SPLAT_BACKEND`, so forward, GPU-cull and tile all agree.

> **Landed 2026-10-08:** bake and trigger enumerate the store (169 clusters
> incl. submerged lava; budget 192, knee 169, 3 probe-verified air ghosts
> excluded from the parity gate). Parity test green (7/7), store group 2/2.
> Night cost at 169 live lights: 11.68 ms avg / 150 frames (960×540 night
> hero) vs 9.51 ms at 16 — holds ~85 fps under the per-pixel K=4 march cap.

## A call-site comparison is not a behaviour comparison

*(2026-10-08. This section exists because of a finding that was WRONG and was
caught before it cost anyone an edit. The claim is deleted; the reasoning is
kept because it is the reusable part.)*

An asymmetry was reported here and to the shading session: the splat path folds
the **ungated** baked shadow into the enclosure proxy (`splat.frag:321` passes
`vShade.w` as `shRaw`, `common_splat.glsl:178` folds it) while the SVO path
folds its **flag-gated** `sh` (`common_svo.glsl:583`, folded at `:594-595`). The
inference was that clearing bit 1 leaves splat interiors darkening through
`(1 - ao) * (1 - sh)` while SVO interiors lose the proxy — a backend-asymmetric
version of the escape-hatch bug documented above, invisible to the
splat-vs-SVO comparison that is this project's primary detector for that class.
A test was proposed: splat vs SVO at `VF_RENDER_FLAGS = 253`.

**It is unreachable.** `aoShEnclosure` opens with its own guard:

```glsl
if ((gRenderFlags & (256 | 3)) != (256 | 3))
    return 0.0;
```

`256 | 3` = 259 needs bits 8, 1 **and** 0. At 253: `253 & 259 = 1 ≠ 259`, so the
function returns 0.0 **before `sh` is ever read, identically on both backends.**
The proposed A/B was guaranteed flat — and flat falsifies, it does not
corroborate a smaller claim.

> **This is now stated in `AGENTS.md`, which is the authoritative version.**
> The bit-1 widening section now reads that clearing bit 1 (`=253`) disables the
> enclosure proxy too, and that "an A/B which clears only bit 1 changes cave
> shading on both backends **by design**". Read that first; the reasoning below
> is how the design acquired that property, not a proposal to change it.

Two things worth keeping from the failure:

1. **Read the callee's guards before comparing its call sites.** The guard was
   three lines above the comparison in the same file and had already been read
   earlier in the same session. Having the evidence is not the same as using it.
2. **The `shRaw`/gated-`sh` difference is not an inconsistency — it is the
   mitigation for the bug documented above.** `splat.frag`'s caller gates `sh` to
   1.0 whenever the surfel faces away from the sun, which silently pinned the
   proxy to 0 on cave walls (45.26 vs 45.84 mean luma). SVO has no caller-side
   backface gate, so its ungated `sh` satisfies the same contract. Same rule,
   different inputs, because one caller has a gate the other lacks — a sharper
   design than "one site drifted".

The residual is **not** this finding: with bits 0/1/8 all set, splat folds a
**baked** shadow and SVO a **frame-time march**. Those differ numerically, and
that is the pre-existing baked-vs-marched difference the design accepts.

See [[concepts/measurement-discipline]] for the general form.

## Bit 1 ("Shadows") now gates point-light occlusion too

The panel toggle was renamed from **"Sun shadows"** to **"Shadows"**, and to make
that label true the bit had to stop meaning only the sun: `applyLights()` runs
`lightVisibilitySPlat` / `lightVisibilitySvo` only when `(gRenderFlags & 2) != 0`.
Point lights used to march unconditionally, so the toggle could be off with every
lamp still casting. With the bit clear, a lamp's light passes through geometry —
the same reading the sun march has when the bit is clear.

Three consequences that are easy to lose, because none of them are about
shadows:

1. **Bit 1 is an input to enclosure.** `aoShEnclosure` requires
   `(gRenderFlags & (256 | 3)) == (256 | 3)` — bits 1 **and** 2 **and** 8 — and
   folds the **RAW** baked shadow: `clamp((1 - ao) * (1 - shRaw), 0, 1)`. So
   flipping bit 1 changes how caves and interiors shade, not just who casts
   what. A test that toggles "Shadows" and sees interiors move is reading this
   proxy. The raw value matters because the fragment stage gates `sh` to 1.0 on
   backfacing surfels, which fires on nearly every cave wall
   ([[concepts/heightfield-blindness-enclosure]],
   [[concepts/baked-sun-shadow-contract]]).
2. **`VF_RENDER_FLAGS=255` is not a bit-1 escape hatch.** It only restores
   pre-enclosure behaviour because bit 8 is *also* clear there; leave bit 8 on
   and the proxy keeps folding a bit-1-cleared shadow value into enclosure. This
   is the same trap that made the cave A/B come back identical either way
   (45.26 mean luma on vs off) before the bit-8 term was added to the guard.
3. **The gate description must match both.** Any doc, gate or hotkey tooltip
   that describes bit 1 as sun-only is now wrong; it governs sun **and** every
   point-light occlusion march.

## The descriptor-ordering trap that cost the feature its first test

`setLights()` writes a descriptor of an already-allocated set. `initVulkan()`
originally called it **before** `m_splatPass.init()`, so on the splat path
`m_ctx` was null and the call returned silently: the manifest logged
"lighting: 1 explicit point lights", the SVO backend lit up, and the splat
backend did not change by a single code. The lamp looked broken, not
uninitialised.

**Rule: anything that writes a descriptor lives after every pass `init()`.**
`App::uploadLightSources()` is now the single writer, called from
`initVulkan()` after the last pass and from `applyWorldReload()`.

### Binding 26 is the same trap one slot up — and it is now live, reading zero

The irradiance volume's descriptor inherits the rule exactly. **Status:
landed and handed to Vega** (was George's; code ownership moved with the
coordinator hand-off). Build green on a **dirty, uncommitted tree** — `ninja`
66/66, `.spv` 12:05:38, `--selftest` PASSED at 81.4 % coverage.

> **A green gate here is a negative control, not a pass.** The volume's RGB is
> currently **all-zero** — `14 seen / 0 used / 0 cells lit` — so the correct
> expected frame change today is **zero**. A green gate proves the term is *dead*,
> not that it is *correct*. Any gate that passes while the frame is unchanged is
> only meaningful read together with the reason it is unchanged.

This is the trap in its most seductive form: the binding exists, the set is
allocated, the draw runs, and nothing complains. **Descriptor presence is not
descriptor content.**

**Known diagnostic ambiguity (reported, unverified by instrument):** the
`used=0` log line is **byte-identical** to the `VF_NO_IRR_VOLUME` fallback path,
which returns its stats after emit collection. So the log line cannot distinguish
*the volume is present but empty* from *the volume was not loaded at all* — a
correct value, present, and unreadable as a cause. See
[[concepts/measurement-discipline]].

## Emissive materials derive real point lights

> ### ⚠ Spec change — IN PROGRESS, gated on Wiki landing
>
> Coordinator order 2026-10-08 (topic `emissive-lights`): **every emissive voxel
> becomes a shadow-casting point light**, superseding the thinning rule and the
> 16-slot cap below. Gate: **Wiki landing** — until then, everything in this
> section describes the tree as it is.
>
> Why this is flagged rather than silently updated: the current chain (1.5 m
> thinning, `kMaxLights` = 16, authored-first, truncation logged) is load-bearing
> against binding 25's fixed UBO size. A per-voxel light set does not fit that
> shape, so the landing must restructure the path, not just the constants — and
> any reader comparing this page against the code before the landing should see
> the *current* contract, not the future one.

*(Landed 2026-10-08, verified from source. This closes the seam that was open
while the emissive texture flag was in place but unconsumed.)*

The emissive texture flag only made a surface *glow*. What makes it *illuminate*
is `App::uploadLightSources()` (`src/app/rhi/surface.cpp:112`) building a
per-material emission table and handing it to `VoxelField::collectEmissive()`
(`src/voxel/voxel_field.cpp:858`):

```
kEmissive[mats 9-15]                      palette emitters, CPU mirror
  + meanColor(mat) * (3.0 * emissiveScale)   every texture flagged "emissive"
        ↓
  collectEmissive(emission, clusters)    1 m sparse buckets, count-desc/key-asc
        ↓
  authored lights fill kMaxLights first, derived fill the rest, truncation logged
```

Four decisions in that chain are worth keeping, because each looks arbitrary and
each removes a specific failure:

- **The texture part is read from the ATLAS, not the manifest.** `VF_TEXTURES=0`
  zeroes `m_emis`, so the derived light dies together with the glow. Reading the
  flag from the manifest instead would have left a light in a run that claims to
  be bit-exact.
- **Count-desc / key-asc sorting** makes the light set a pure function of
  content: same scene ⇒ same lights, every reload, in the same order. Without a
  total order the 16-slot cut would reshuffle between reloads.
- **Thinning to 1.5 m separation** (interim — superseded by the N=64/K=4 flip
  once it lands; see the banner above). A hearth is one light, not twenty.
- **Each centroid is lifted to the nearest AIR cell.** A light buried inside its
  own emitter is occluded by every receiver's march, so it would occupy a slot
  and contribute nothing — the worst outcome available, because it looks like a
  working light in a log.

**Authored lights fill the 16 slots first** (16 is interim — the N=64/K=4 flip
supersedes the cap; see the banner above). They are explicit intent; derived
lights are content. Otherwise emissive scenery could silently evict a lamp the
user authored, and the manifest would disagree with the GPU with nothing said.
Truncation is logged.

**Derived lights are never persisted.** `writeLightManifest` must not learn about
them: they are a function of content, so writing them would freeze a derived
value into a tracked file and make it survive the edit that should remove it.

**`kEmissive` is duplicated** — CPU-side in `src/voxel/common.hpp` (indexed
straight by material id, `kPaletteN` long) and GPU-side in `common_base.glsl`
(stops at 16). Change one without the other and the lit colour and the glowing
colour disagree.

Re-derived on every `reloadTexAtlas()` and world reload; the default hamlet
derives **14**. **They are not day/night gated**, which is why the night gate's
ratio band went stale — see [[concepts/night-gate-thresholds]].

## Editing lights is a different problem from loading them

**The writer now exists** (2026-10-06, `writeLightManifest`,
`src/voxel/worldfile.cpp:984`) and all three requirements below are met and
pinned by `tests/test_worldfile.cpp`:

| # | Requirement | Status |
|---|---|---|
| a | **Preserve every top-level key it does not own** — reuse `scanTopLevel` + `emitPreserved` | met (`writeLightManifest` uses both) |
| b | **temp + rename**, never a bare in-place `fopen("wb")` | met — writes `path + ".tmp"`, then **self-checks the temp before renaming** |
| c | **A round-trip unit test** asserting an unknown key survives | met — `"writeLightManifest round-trips and preserves every key it does not own"` (`:848`), plus refuses-malformed (`:952`), self-check (`:1034`), clamps-to-UBO (`:1149`) |

So the earlier "lights stay loader-only until that test exists" is **retired**.
Two behaviours from the implementation are worth knowing because they are not
obvious from the signature:

- **It fails closed.** If the top level does not parse, it warns and returns
  `false` *before the temp file exists*, leaving the on-disk manifest untouched.
  The reasoning: temp+rename protects against a failed write, but promoting a
  partial key set is a *successful* write of wrong content — the one failure
  mode temp+rename does not catch.
- **It self-verifies the temp before renaming**, because routing the check back
  through `loadLightManifest` was a mistake: that made "did we write the lights
  we meant to" depend on the loader tolerating every foreign key in the file.

**Known limitation, written down in the source:** `prev` is read at the top and
the rename happens at the end, so a *concurrent* writer landing inside that
window has its key clobbered. The texture picker reaches
`writeTextureManifest` from a different call path, and three sessions share one
manifest. Apply-only and rare makes it tolerable.

### `writeTextureManifest` is NOT atomic — do not copy its write mechanism

This is the trap that makes (b) non-obvious. `writeTextureManifest` and
`writeManifest` both do:

```cpp
std::FILE* f = std::fopen(path.c_str(), "wb");   // truncates HERE
... fprintf the whole document in place ...
return std::fclose(f) == 0;
```

`"wb"` **truncates the live manifest before the first byte of the replacement
exists**, and `return fclose(f) == 0` cannot detect a partial write — `fclose`
succeeds on whatever it managed to flush. So an interrupt, a crash, or a full
disk leaves a **truncated `world.json` with no error reported anywhere**. That is
exactly how `assets/world_all.json` got mangled today, and it is the reason
`OverlayWriter` uses temp + rename.

So the precedent splits cleanly:

- **Reuse its key-preservation** — `scanTopLevel` is balanced-brace aware, so
  nested objects/arrays and odd spacing survive verbatim. That is the right
  part.
- **Do not reuse its write mechanism.** Write `path + ".tmp"`, and **check
  `fclose`'s return and remove the temp file on failure *before* the rename**.
  It is not enough to rename and then notice failure: promoting a partial temp
  file over a good manifest trades a truncated file for a differently truncated
  one.

This is Robin's finding, and it is the second time this session that a
plausible-looking precedent was the wrong thing to copy — the first being a
day/night page that called a CPU-baked rebake "async" because the plumbing
around it was threaded. **Verify the precedent's failure mode, not its shape.**

### The writer also clamps to `kMaxLights`

`loadLightManifest` drops entries beyond 16 and the `LightUBO` holds 16, so
emitting 17 would persist a lamp that silently never renders — the manifest
would disagree with the GPU with no error anywhere. The writer emits
`min(lights.size(), kMaxLights)`.

### A moved light is staged, not written through

Same UX contract as the trackball and Move: preview live, commit on **Apply**.
Three reasons, in order of weight:

1. A light that wrote straight through would be the **only transform in the app
   that silently rewrites a tracked file mid-drag**.
2. An accidental write to `world.json` is not hypothetical here.
3. A staged light **cannot desync the UBO from the manifest**: preview by
   patching `LightUBO` in RAM — the same in-RAM lane
   [[concepts/sun-direction-pipeline]] uses for the sun — so `Apply` is the
   single call site of the writer. There is no code path where a drag reaches
   the disk.

### Move mode has no lane for lights

Move currently drags a **selected layer owner** and writes that layer's `pos`.
A light has no `.vxw` and no layer ID, so it needs its own selection and its own
translation path. Picking resolves to a voxel and its **winning `.vxw` owner**,
never a layer AABB (`src/voxel/picking`) — so "what did the cursor hit" has no
answer for a light until selection gains a second, non-voxel answer.
Note `App::uploadLightSources()` **re-reads the manifest**, so a preview that
only patched the UBO must not be followed by an unrelated reload re-deriving
lights from disk and undoing it.

## Verification

One lamp inside the cabin, 320×180, camera inside the shell:

| shot | mean luma | pixels < 30 |
|---|---|---|
| splat, no light | 42.01 | 44.86 % |
| splat, 1 lamp | **87.10** | **11.04 %** |
| SVO, 1 lamp | 101.37 | 2.06 % |
| splat↔SVO delta, unlit | −16.68 | — |
| splat↔SVO delta, lit | −14.28 | — |

The light does **not** add backend divergence: the delta is unchanged within
noise, so the two backends agree on what the lamp does.

Cross-links: [[concepts/shading-model]], [[concepts/measurement-discipline]],
[[concepts/ssao-gbuffer]], [[entities/svo-render]].
