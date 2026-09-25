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

#endif
