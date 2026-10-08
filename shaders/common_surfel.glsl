// Shared Gaussian-surfel layout and packed metadata helpers.
//
// The CPU Surfel record remains five vec4s (80 B). mat_ao.w stores baked AO
// plus one metadata encoding:
//   terrain: AO
//   water:   AO + 2
//   object:  AO + 8 + 16 * owningLayerId
//
// Layer IDs are 1..254; zero means unowned/live-only geometry. At ID 254 the
// largest encoded value is about 4073, where float32 still resolves AO much
// more finely than one code. Keeping the ID here avoids a sixth 16-byte GPU
// record and makes vertex, cull, tile, tint and debug paths use one contract.
#ifndef VOXELFORGE_COMMON_SURFEL_GLSL
#define VOXELFORGE_COMMON_SURFEL_GLSL

// SplatUBO contract shared by the forward (splat.frag), vertex and tile
// (splat_tile_render.comp) stages - they must never disagree about which
// pixels get sealed.
//
//   uSplat3.x = base-seal alpha threshold (VF_SPLAT_SEAL_ALPHA, DEFAULT 0 =
//               seal every fragment = the long-standing behaviour).
//
// The base pass (PASS_MODE 4) seals the nearest fragment at alpha 1, which is
// not only the surface's colour: it also stands in for every fragment the
// depth band rejects behind it, because nothing occluded is ever drawn. A
// fragment too thin to stand in for anything - a disk rim, an overhang - is
// therefore better left unsealed, letting the band draw it at its own alpha so
// it fades to zero at the clip instead of ending in an opaque step.
//
// The test reads the UN-WINDOWED kernel alpha (alphaKernel), not the windowed
// alpha: the edge window (uSplat2.w) dominates alpha at every radius, so
// testing the windowed value collapses this into a plain radius test and the
// threshold stops being a threshold (measured: 0.65/0.8/0.9 all render the
// same frame).
//
// It is a run-time field rather than a literal because the correct value is a
// property of the scene's coverage, not of these constants - and it cannot be
// reasoned out: the base pass only sees the single nearest fragment at a pixel
// and cannot know a different fragment covers the same pixel. Measured cost of
// enabling it (threshold 0.5): silhouette 1.28x more transparent for interior
// +5.6/255 with 57% of it blue-dominant, i.e. the sky showing through. Note
// this is NOT the sub-centimetre micros - VF_MICRO=0 measures 5.96 vs 5.60.
// A real fix needs a coverage count, which the unused D24_S8 stencil (already
// allocated, bound and cleared) is the natural home for.

struct Surfel {
    vec4 pos_rU;    // xyz = centre (m), w = radiusU (m)
    vec4 normal_rV; // xyz = geometric normal, w = radiusV (m)
    vec4 bent_sh;   // xyz = baked bent normal, w = baked shadow
    vec4 mat_ao;    // xyz = material/reflectivity/roughness, w = packed AO metadata
    vec4 tan_aspect; // xyz = in-plane tangent, w = per-cell texture override
};

bool surfelIsWater(float packedAo)
{
    return packedAo > 1.5 && packedAo < 5.5;
}

bool surfelIsObject(float packedAo)
{
    return packedAo >= 7.5;
}

uint surfelLayerId(float packedAo)
{
    if (!surfelIsObject(packedAo))
        return 0u;
    // AO is in [0,1], so rounding recovers the integer lane even at the top
    // of the supported layer range where float spacing is ~0.0005.
    return uint(max(0.0, floor((packedAo - 8.0) / 16.0 + 0.5)));
}

float surfelBakedAo(float packedAo)
{
    if (surfelIsWater(packedAo))
        return packedAo - 2.0;
    if (surfelIsObject(packedAo))
        return packedAo - (8.0 + 16.0 * float(surfelLayerId(packedAo)));
    return packedAo;
}

bool surfelBelongsToLayer(float packedAo, uint layerId)
{
    return layerId > 0u && surfelLayerId(packedAo) == layerId;
}

// ---- Brush hover preview volume (shared by both backends) ----------------
// The app fills a std140 BrushUBO (binding 13 in BOTH the splat fragment
// pipeline and the SVO compute pipeline) with the active edit-brush volume so
// the geometry a stamp would affect can be seen before the click. The layout
// is byte-identical to SplatPass::setBrush, so one declaration serves both.
//
//   bVolume: xyz = centre (world m), w = radius m
//   bAxis:   xyz = unit axis, w = half length m (0 = ball/box, <0 = dome)
//   bTint:   rgb = tint colour, a = strength (a <= 0 disables the preview)
//   bMeta:   x   = optional owning layer ID; 0 = spatial volume only
//            y   = radial falloff CURVE index (0 = Constant, i.e. no taper)

// The CPU's EditableWorld::FalloffCurve set, indexed by bMeta.y. Duplicated
// here so the hover tint marks the volume the stamp actually emits; the two
// must agree curve for curve, and test-preview is the gate.
float brushFalloffCurve(int idx, float q)
{
    q = clamp(q, 0.0, 1.0);
    if (idx == 1) return sqrt(max(0.0, 1.0 - q * q));       // Sphere
    if (idx == 2) return sqrt(max(0.0, 1.0 - q));           // Root
    if (idx == 3) return 1.0 - q * q * (3.0 - 2.0 * q);     // Smooth
    if (idx == 4) return 1.0 - q;                           // Linear
    if (idx == 5) return (1.0 - q) * (1.0 - q);             // Sharp
    return 1.0;                                             // Constant
}
//
// w == 0 on the volume with w != 0 on the axis is a BOX (axis.xyz are half
// extents) - the rotate/move trackball preview tints a whole layer AABB.
// `voxelSize` is passed in rather than read from a push constant because this
// header is included before the pipeline's own constant block is declared.
bool sharedInBrushVolume(vec3 p, float packedAo, vec4 bVolume, vec4 bAxis,
                         vec4 bTint, vec4 bMeta, float voxelSize)
{
    if (bTint.a <= 0.0)
        return false;
    const uint layer = uint(max(0.0, bMeta.x));
    if (layer > 0u && !surfelBelongsToLayer(packedAo, layer))
        return false;
    vec3 rel = p - bVolume.xyz;
    const float r2 = bVolume.w * bVolume.w;
    if (bVolume.w == 0.0) {
        // box: rel within the half extents on every axis (the skin is
        // folded into the extents by the caller)
        vec3 he = bAxis.xyz;
        return all(lessThanEqual(abs(rel), he));
    }
    if (bAxis.w == 0.0) {
        // ball (Delete/Paint): a tapering curve grades the RADIUS, matching
        // EditableWorld::makeSphere. Constant is the original hard sphere.
        const int fo = int(bMeta.y + 0.5);
        if (fo == 0)
            return dot(rel, rel) <= r2;
        const float d = sqrt(dot(rel, rel));
        const float q = d / max(bVolume.w, 1e-6);
        return d <= bVolume.w * brushFalloffCurve(fo, q);
    }
    const float along = dot(rel, bAxis.xyz);
    const vec3 perp = rel - bAxis.xyz * along;
    if (bAxis.w < 0.0) {
        // dome: the footprint disk (radius r) extruded |w| up the axis, closed
        // by a fillet of radius min(r, |w|) / 2 - exactly the set
        // EditableWorld::makeDome emits. The base layer counts (makeDome's
        // -voxelSize epsilon), and the two profile branches meet at t = lipY.
        const float h = -bAxis.w;
        const float r = bVolume.w;
        if (along < -voxelSize)
            return false;
        // Radial falloff (bMeta.y = curve index): the same profile as
        // EditableWorld::falloffCurveAt, applied per column with the fillet
        // shrinking to match, so the tint marks the tapered growth rather than
        // the full-depth one. Constant (0) grades nothing, which keeps the
        // legacy flat-top footprint exact.
        const int fo = int(bMeta.y + 0.5);
        const float qq = sqrt(dot(perp, perp)) / max(r, 1e-6);
        const float prof = brushFalloffCurve(fo, qq);
        const float hf = h * prof;
        if (hf < 0.5 * voxelSize)
            return dot(perp, perp) <= r2; // tapered to nothing past the base layer
        const float c = 0.5 * min(r, hf);
        const float lipY = hf - c;
        if (along <= lipY)
            return dot(perp, perp) <= r2;
        const float k = along - lipY;
        if (k > c)
            return false;
        const float rr = (r - c) + sqrt(max(0.0, c * c - k * k));
        return dot(perp, perp) <= rr * rr;
    }
    // Carve cylinder. The near end (kCarveTopMargin above the hit) is NOT
    // tapered - it is what opens the ground the scoop starts at - so the far
    // end alone is scaled by the profile, mapping [−w, +w] onto
    // [−w, w·f] as f goes 0 → 1.
    const int fo = int(bMeta.y + 0.5);
    const float qq = sqrt(dot(perp, perp)) / max(bVolume.w, 1e-6);
    const float far = mix(-bAxis.w, bAxis.w, brushFalloffCurve(fo, qq));
    return along >= -bAxis.w && along <= far && dot(perp, perp) <= r2;
}

#endif
