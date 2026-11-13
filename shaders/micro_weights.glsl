// The SKINNING WEIGHT KERNEL (micro_weights.glsl).
//
// ONE expression, included by every shader that skins a micro object: the
// raster mesh (micro_rig.glsl -> micro_mesh.vert/frag) and the fur cards
// (common_fur.glsl -> fur.vert/frag). It mirrors the CPU's
// `MicroRig::boneInfluences` (src/voxel/micro_rig.cpp) term for term - the
// deformer's whole proof (that its per-frame delta equals a full
// re-voxelization of the posed mesh) rests on the two agreeing.
//
// WHY WEIGHTS AND NOT "THE" BONE. One bone per point is what tore the alpaca
// apart: the upper back is equidistant from the spine and the last neck link,
// so one vertex went with the neck and its neighbours stayed with the spine,
// and the neck carried that vertex 0.318 m. The measured worst triangle
// stretched 108.8x (2 137 triangles past 8x) on the graze clip. A kernel over
// every bone is continuous in the point, so the surface can bend but cannot
// split: 4.60x worst, zero triangles past 8x, hooves still 0.0 mm from rigid.
//
// THE KERNEL
//   w_i = (1 - (d_i / (MICRO_WEIGHT_REACH * r_i))^2)^4
// d_i = the distance to bone i's segment, r_i = the bone's capsule radius
// (the per-bone influence radius). The reach is 3.5 because that is where the
// tear closes (2.0 -> 52x, 2.5 -> 10.3x, 3.0 -> 5.9x, 3.5 -> 4.6x; 4.0 adds
// leg weight to the belly for no gain). The quartic's first two derivatives
// vanish at the band's edge, so a bone entering or leaving the blend leaves no
// crease. Do NOT prune to the top few bones: pruning to 4 measures 17.2x at
// reach 3.5 - the tail of the blend is doing real work.
#ifndef VF_MICRO_WEIGHTS_GLSL
#define VF_MICRO_WEIGHTS_GLSL

// Must match kMicroMaxInfluences in src/voxel/micro_rig.hpp, and the bone
// table's length in the micro and fur world rows.
const int MICRO_MAX_INFLUENCES = 12;
const float MICRO_WEIGHT_REACH = 3.5;
// The CPU spells the quartic as two squarings; so does this.
float microWeightQuad(float x)
{
    const float q = 1.0 - x * x;
    const float q2 = q * q;
    return q2 * q2;
}

// One bone's unnormalized weight for a point, given its rest segment and
// radius. 0 outside the band.
float microWeightOne(vec3 p, vec3 a, vec3 b, float radius)
{
    const vec3 ab = b - a;
    const float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
    const float d = length(p - (a + ab * t));
    const float reach = max(radius * MICRO_WEIGHT_REACH, 1e-3);
    const float x = d / reach;
    if (x >= 1.0)
        return 0.0;
    return microWeightQuad(x);
}

// Normalize a raw weight list in place and drop the bones that do not reach.
// Returns the influence count; the order is whatever the table walked in
// (blending does not care), and callers that need the DOMINANT bone sort.
int microNormalizeWeights(inout int raw[12], inout float wgt[12])
{
    float sum = 0.0;
    for (int i = 0; i < MICRO_MAX_INFLUENCES; ++i)
        sum += wgt[i];
    if (!(sum > 0.0))
        return 0;
    int k = 0;
    for (int i = 0; i < MICRO_MAX_INFLUENCES; ++i) {
        if (!(wgt[i] > 0.0))
            continue;
        raw[k] = raw[i];
        wgt[k] = wgt[i] / sum;
        ++k;
    }
    return k;
}

#endif // VF_MICRO_WEIGHTS_GLSL
