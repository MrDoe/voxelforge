---
title: Texture conformance (the drop-in gate)
tags: [textures, tooling, conformance, ai, workflow]
sourceRefs: [tools/check_texture.py, tools/strip_stamp.py, tools/prepare_texture.py, tools/gen_textures.py, tools/fetch_textures.py, tests/texture_check.py]
lastReviewed: 2026-09-19
---

# Texture conformance

Any PNG/JPG in `assets/textures/` is a swap-in candidate (see
[[concepts/texture-atlas]]), but the sampling model imposes four hard
requirements that a raw photo or an AI generation usually violates. Dropping
a non-conformant file in is silent — it just looks wrong in the world.

## The contract

| check | metric | threshold | failure mode if violated |
|---|---|---|---|
| tileable | wrap edge-difference / interior adjacent-difference | ≤ 1.35 | a visible grid repeats every 0.9–2.8 m across the whole meadow |
| albedo-only | luminance plane-fit gradient / contrast | ≤ 0.35 | baked sunlight: the sun is fixed at 34°/238° while the camera moves, and it breaks PBR energy conservation |
| no stamp | share of the brightest 0.5 % pixels in one corner | ≤ 0.25 | a generator watermark/logos tiles across the terrain |
| identity | RGB residual to the palette target after mean-match | ≤ 26 | a hue shift silently changes the material's colour identity |

`tools/check_texture.py FILE...` reports all four (exit 1 on any failure);
`--material NAME` adds the identity check against `gen_textures.TARGETS`,
the single source of truth for what each material should look like.

## Repair

`tools/prepare_texture.py IN OUT [--material NAME]` runs the three repairs in
the order that matters — **strip stamp → flatten gradient → make seamless**:

1. **Strip stamp** (`tools/strip_stamp.py`): the stamp is a compact solid
   bright region, so it is found on a *high-pass of the blurred* luminance
   (blur 20 minus blur 80, threshold ~0.12). A raw local-contrast or
   absolute-brightness test fails — AI textures are high-frequency
   everywhere. The repair clones from `np.roll(img, (h//2, w//2))` through a
   Gaussian-feathered mask, and always unions a conservative generator-box
   region (a partial removal is worse than a generous one).
2. **Flatten**: subtract the fitted luminance plane as a multiplicative gain,
   so colour ratios survive. Only fixes *linear* gradients; a strong residual
   after flatten means the image is a scene, not a texture (reject it).
3. **Make seamless**: offset by half — the tile edges then become adjacent
   rows of the original, so they are wrap-continuous by construction — then
   heal the discontinuity the offset moved into the middle by cloning a
   further-shifted copy through a feathered cross mask.

## Measured on 13 AI candidates (2026-09-19)

**1 of 13 passed raw; 10 passed after `prepare_texture.py`.** The failures
were not marginal: seam ratios ran 1.44–11.43 (threshold 1.35) and light
ratios 0.05–2.13 (threshold 0.35). Two Gemini images had no seam problem at
all (0.04x, 0.99x) but were scenes with a real brightness gradient that
flattening cannot remove — those were rejected, not force-fixed.

Also worth knowing:
- **Microsoft Copilot stamps a visible watermark in the top-right corner**
  (x≈860–1015, y≈0–58 on 1024², plus C2PA `com.microsoft.invismark.1`
  metadata). All 7 Copilot candidates had it; the Gemini set did not.
- Some generators emit a **pre-tiled** image: one candidate had an exact
  512² period repeated 2×2 into 1024². Detect with normalized
  autocorrelation and extract the true tile — but check it: the repeat was
  only approximate (corr 0.90/0.97), so the extracted tile still needed the
  seamless pass.
- A 512² crop that then passes conformance beats a 1024² that does not.

Cross-refs: [[concepts/texture-atlas]] (binding + sampling),
[[concepts/texture-resolution]] (how much resolution actually buys),
[[concepts/detail-normals]] (the layer that makes a texture read as relief).
