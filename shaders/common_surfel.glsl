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
    if (bAxis.w == 0.0)
        return dot(rel, rel) <= r2;
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
        const float c = 0.5 * min(r, h);
        const float lipY = h - c;
        if (along <= lipY)
            return dot(perp, perp) <= r2;
        const float k = along - lipY;
        if (k > c)
            return false;
        const float rr = (r - c) + sqrt(max(0.0, c * c - k * k));
        return dot(perp, perp) <= rr * rr;
    }
    return abs(along) <= bAxis.w && dot(perp, perp) <= r2;
}

#endif
