---
title: The texture atlas (phase 1)
tags: [textures, atlas, rendering, splat, svo, world-json, gui]
sourceRefs: [src/render/texture_atlas.cpp, src/render/texture_atlas.hpp, src/voxel/worldfile.cpp, shaders/common_base.glsl, src/app/main.cpp, tools/fetch_textures.py, tools/gen_textures.py, tests/texture_check.py]
lastReviewed: 2026-09-19
---

# The texture atlas (phase 1)

Optional photo textures replace the palette albedo for a *material*. Declared
as a top-level array in `world.json`:

```json
"textures": [
  { "file": "textures/wood.png", "mat": 6, "scale": 1.0 },
  { "file": "textures/rock.png", "mat": 4, "scale": 1.2 }
]
```

`scale` is metres per tile; missing/invalid entries fall back to the palette
and never abort the world. `VF_TEXTURES=0` zeroes the slot table and is a
bit-exact escape hatch (measured 0.002% diff).

## Layout

- `TexAtlas` (`src/render/texture_atlas.cpp`): a fixed **17 layers × 512²**
  `R8G8B8A8_UNORM` 2D-array image, one layer per material id (layer == mat).
  Unbound layers get neutral grey filler and a `-1` slot. CPU box-resample,
  blit-built mip chain, repeat+mip sampler, persistently-mapped `TexTable`
  UBO (`matTex[17]`: x = layer/-1, y = m per tile).
  The box filter handles any source size (downscale) so a dropped 4K photo
  works; the loader takes **PNG and JPEG** (`STBI_ONLY_PNG` +
  `STBI_ONLY_JPEG` in `heightmap.cpp`).
  `kTexSize` (the header constant) is the **only** hardcode — mip count,
  image extent, staging `layerBytes` and the blit chain all derive from it.
  Raised 256 → 512 on 2026-09-19: the shipped ambientCG sources are already
  512² and were being downsampled on upload. VRAM 5.9 → 23.8 MB (full mip
  chain × 17 layers). See [[concepts/texture-resolution]] for the measured
  effect (real but modest — a sharpness fix, not a realism lever).
- Bindings **22** (`sampler2DArray uTexAtlas`) and **23** (`TexTable`) in
  both the splat and SVO pipelines (added to the descriptor layouts — the
  array sizes must match `bindingCount` or the build stack-smashes).
- `loadTextureManifest` resolves paths against the manifest directory, so
  `assets/textures/` and absolute paths both work.

## Shader

`texTriplanar(p, n, scale, slot)` projects world-space UVs onto the three
axis planes with `pow(abs(n), 8)` weights and a dominant-axis single-tap
fast path. `sampleTex` returns `vec3(-1)` when a material has no texture.
`detailAlbedo` replaces `alb` with the sample for textured materials but
keeps the universal mottling + grain, and skips the per-material accents.
`detailNormal` (render-flag bit 7, key **B**) derives a tangent-space bump
from the albedo's luminance over the same projection — see
[[concepts/detail-normals]].

Triplanar means the texture is **world-locked** (resolution-independent, no
UV unwrap) but cannot do unique per-face art — a decal mechanism would be
needed for that.

## `VF_TEXTURES=0` is the bit-exact escape hatch — and it was not, on the override path

`VF_TEXTURES=0` zeroes the slot table and is documented as a **bit-exact** escape
hatch (measured 0.002 % diff). It is the one control that lets a gate prove a
texture change did nothing, so its correctness is load-bearing in a way an
ordinary feature's is not.

**It was broken on the per-cell override path** (found 2026-10-08, fixed by
George, **verified green**: `texture_check` passes with the `vf_off` residual now
**0** and bit-exact). The old code decoded every texture and *then* cleared the
slot table.
That is sufficient for the material path — `matTex[mId].x` becomes `-1`, so
`sampleTex` returns `vec3(-1)` and the palette is used. It is **not** sufficient
for the per-cell override, because `texSlotFor` gives `gTexOv` precedence:

```glsl
return gTexOv > 0.5 ? floor(gTexOv + 0.5) : uTex.matTex[mId].x;
```

Override cells therefore kept sampling the **decoded** layer with the slot table
already cleared, and the hatch left **0.83 % of the hero frame** showing the very
checker it promises to remove — caught by the `texture_check` `vf_off` arm.

The fix is to skip the decode entirely (`if (disabled) break;` before the
decode loop), so every layer keeps the neutral filler and the result is
byte-identical to the no-table palette arm.

**Why this class of bug is expensive rather than merely wrong:** the escape
hatch is only ever exercised when someone thinks to check it, and its entire
value is that it is *provably* identical. A hole in it is invisible in normal
use, and it silently invalidates every A/B that relied on it as a control —
including any measurement that used `VF_TEXTURES=0` as its "textures off" arm.
See [[concepts/per-cell-texture]] for the override path and
[[concepts/measurement-discipline]] for why a control that can fail quietly is
worse than no control.

**`VF_TEXTURES=0` is a bit-exact escape hatch, NOT a lights-only control.** It
kills the derived lights *and* swaps photo→palette albedo in **both** arms, so
any delta it produces is unattributable to either cause. Found 2026-10-08 when
it was used as the positive control for "does the night gate see the emissive
feature": the numbers moved, but house's night went **up** (+1.82) while its day
also went up (+3.6) — and removing light sources cannot raise night luma, so the
sign itself rules out the lights-only explanation. A two-variable control is not
a control; it answers a different question than the one asked, which is the
dangerous version because the result looks like an attribution.

**A lights-only control needs its own switch** — an env hook in
`uploadLightSources` that skips *only* the derived-light derivation while leaving
textures fully on (e.g. `VF_NO_EMISSIVE_LIGHTS=1`). Then the three arms are
clean: textures-on + lights-on (baseline), textures-on + lights-off (lights-only
delta), and `VF_TEXTURES=0` (both off — still the bit-exact hatch, but not a
lights control). The hook should carry a one-line comment saying it exists
because `VF_TEXTURES=0` is not a lights-only control, or the next session reaches
for the wrong switch.

## Conformance

Any PNG dropped into `assets/textures/` is a candidate, but the tiling and
fixed-sun model impose hard requirements (seamless, albedo-only, no stamp,
on-palette). Gate with `tools/check_texture.py` and repair with
`tools/prepare_texture.py` — see [[concepts/texture-conformance]].

## Getting textures

- **Shipped set**: `tools/fetch_textures.py` downloads ambientCG (CC0) 1K
  JPGs, extracts the Color map, resizes to 512² and **mean-matches each photo
  to the material's palette target** (`TARGETS` in `gen_textures.py`, which
  is the colour the palette + detail accents produced before textures —
  e.g. wood is the palette red-brown knocked back, foliage is the accented
  dark green). Matching preserves all of the photo's *variation* while
  keeping the material's identity: the A/B against `VF_TEXTURES=0` shows a
  frame-mean delta under 1/255.
- **Offline fallback**: `tools/gen_textures.py` (`ninja -C build vf_textures`)
  writes seamless procedural textures as `assets/textures/proc_*.png`;
  periodic value noise + integer-frequency sines make every field tile
  exactly. Both sets appear side by side in the picker.
- The source list is curated in `SOURCES` (one ambientCG id per slot); the
  mean-match gain is clamped (0.3–4.0) so a very dark source cannot blow up
  noise — if a slot lands off-target, pick a better-lit asset rather than
  raising the clamp (PineNeedles001 needed an 11x green gain and was
  replaced with Foliage006).

## Swapping from the GUI

The **"Textures (material albedo)"** window (`m_showTextures`, opened from
the main panel's "Textures window" checkbox) lists every PNG/JPG in
`assets/textures/` per material, with the current binding, a "m/tile" field
and `(palette)` to untexture a material. Emissive materials (9–15) are
listed as untextured.

A change calls `App::applyTextureBindings()` →
`worldfile::writeTextureManifest()` (rewrites **only** the `textures` key;
layers and every unknown key are re-emitted verbatim from the file text via
a balanced-brace scan) → `reloadTexAtlas()` (re-uploads pixels + mips,
re-writes the UBO; the descriptor is written once and never rebuilt).

Rather than hand-editing `world.json`, drop files into `assets/textures/`
and press **Rescan textures folder**.

## Verification

- `tests/texture_check.py` (CTest `texture_check`): a magenta/green checker
  bound to a material must change ~64% of the hero frame, `VF_TEXTURES=0`
  must be bit-exact against no-table, sky and black-in-silhouette guards,
  and the `[phase2]` per-cell override section
  ([[concepts/per-cell-texture]]).
- `tests/test_worldfile.cpp`: manifest parse/fallback, the reserved-byte
  roundtrip, and `writeTextureManifest` preserving layers + unknown keys.
- Atlas teardown: `App::destroy()` must call `m_texAtlas.destroy()` **before**
  `m_ctx.shutdown()`, or the sampler/image are destroyed against a dead
  device (`vkDestroySampler: Invalid device` + core dump).
