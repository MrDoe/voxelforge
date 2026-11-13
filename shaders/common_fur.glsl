// Shared fur-card transforms: the card table, the object table and the
// rigid transforms that carry a card from its REST frame into the world.
//
// INCLUDE ORDER IS LOAD-BEARING, exactly like common_irradiance.glsl: the
// entry point declares `pc` (the push block) and its own resource bindings
// BEFORE including this file.
//
// WHY A SHARED HEADER: the vertex shader and the fragment shader both need
// the bone transforms (`furRebone`, `furDirRebone`, `furToWorld`), and the
// raster path splits work ACROSS them - the vertex places the quad, the
// fragment evaluates the strand's alpha profile and the shaded point. If
// each spelled the transforms out, the quad and the shading would drift
// apart at the first edit, and the coat would shear.
#ifndef VF_COMMON_FUR_GLSL
#define VF_COMMON_FUR_GLSL

// The ONE skinning-weight kernel, shared with the raster mesh and the CPU
// (src/voxel/micro_rig.cpp). Pure functions, no bindings, so including it from
// a vertex, a fragment and a compute stage is free.
#include "micro_weights.glsl"

// One card: six vec4s (rootLen, dirWidth, nrmAlpha, tint, shade, curl).
// The layout mirrors voxel::FurCard; see fur_cards.hpp for the field notes.
layout(std430, set = 0, binding = 0) readonly buffer FurCards { vec4 fCard[]; };

struct FurBone {
    vec4 a;      // xyz = the rest segment's start, w = the capsule radius
    vec4 b;      // xyz = the rest segment's end
    vec4 joint;  // xyz = the joint (the animated transform's translation)
    vec4 r0;
    vec4 r1;
    vec4 r2;     // the animated rotation (row-major)
};

// The per-object row. It mirrors the rentator's FurWorldRow (fur_pass.cpp)
// and the micro overlay's world table, so the preview, the rotation preview
// and the behaviour are the SAME values the body is drawn with.
struct FurWorld {
    vec4 cards;   // x = the first card, y = the card count
    vec4 grid;    // xyz = the grid's LOCAL origin, w = the cell size (m)
    vec4 gdim;    // xyz = the grid's dims, w = the cellStart base
    vec4 items;   // x = the cellItems base
    vec4 preview; // xyz = the Move preview offset (m), w = active
    vec4 rot0;
    vec4 rot1;
    vec4 rot2;    // the live rotation preview's matrix (row-major)
    vec4 pivot;   // xyz = the pivot (world m), w = active
    vec4 world;   // xyz = the object's world AABB min (the local frame's origin)
    vec4 anim;    // the behaviour: xyz = the offset (m), w = active
    vec4 arot0;
    vec4 arot1;
    vec4 arot2;   // the behaviour's rotation (row-major)
    vec4 apivot;  // xyz = the behaviour's pivot (world m)
    vec4 bones;   // x = the bone count
    FurBone bone[12];
};
layout(std140, set = 0, binding = 30) readonly uniform FurTable {
    FurWorld uWorlds[16];
    ivec4 uInfo; // x = the world count
} ft;

const int MAX_FUR_WORLDS = 16;

// The rotation matrix (row-major in the table, read as columns here - the
// same convention the micro overlay uses, so the two agree exactly).
mat3 furRot(int w)
{
    return mat3(ft.uWorlds[w].rot0.xyz, ft.uWorlds[w].rot1.xyz,
                ft.uWorlds[w].rot2.xyz);
}

// The BEHAVIOUR's transform, applied to a world point: the rotation about
// the pivot, then the offset. Its inverse undoes it. Same shape and order as
// the micro overlay's microAnimate/microUnanimate.
vec3 furAnimate(int w, vec3 p)
{
    if (ft.uWorlds[w].anim.w < 0.5)
        return p;
    const mat3 R = mat3(ft.uWorlds[w].arot0.xyz, ft.uWorlds[w].arot1.xyz,
                        ft.uWorlds[w].arot2.xyz);
    p = ft.uWorlds[w].apivot.xyz + R * (p - ft.uWorlds[w].apivot.xyz);
    return p + ft.uWorlds[w].anim.xyz;
}

vec3 furUnanimate(int w, vec3 p)
{
    if (ft.uWorlds[w].anim.w < 0.5)
        return p;
    const mat3 R = mat3(ft.uWorlds[w].arot0.xyz, ft.uWorlds[w].arot1.xyz,
                        ft.uWorlds[w].arot2.xyz);
    p -= ft.uWorlds[w].anim.xyz;
    return ft.uWorlds[w].apivot.xyz + transpose(R) * (p - ft.uWorlds[w].apivot.xyz);
}

vec3 furDirUnanimate(int w, vec3 d)
{
    if (ft.uWorlds[w].anim.w < 0.5)
        return d;
    return transpose(mat3(ft.uWorlds[w].arot0.xyz, ft.uWorlds[w].arot1.xyz,
                          ft.uWorlds[w].arot2.xyz)) * d;
}

vec3 furUnbone(int w, vec3 p, int bone)
{
    if (bone < 0 || bone >= int(ft.uWorlds[w].bones.x))
        return p;
    const FurBone mb = ft.uWorlds[w].bone[bone];
    const mat3 R = mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz);
    return transpose(R) * (p - mb.joint.xyz);
}

vec3 furRebone(int w, vec3 p, int bone)
{
    if (bone < 0 || bone >= int(ft.uWorlds[w].bones.x))
        return p;
    const FurBone mb = ft.uWorlds[w].bone[bone];
    const mat3 R = mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz);
    return R * p + mb.joint.xyz;
}

vec3 furDirUnbone(int w, vec3 d, int bone)
{
    if (bone < 0 || bone >= int(ft.uWorlds[w].bones.x))
        return d;
    const FurBone mb = ft.uWorlds[w].bone[bone];
    return transpose(mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz)) * d;
}

vec3 furDirRebone(int w, vec3 d, int bone)
{
    if (bone < 0 || bone >= int(ft.uWorlds[w].bones.x))
        return d;
    const FurBone mb = ft.uWorlds[w].bone[bone];
    return mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz) * d;
}

// THE COAT'S SKINNING WEIGHTS, the same kernel the body uses
// (micro_weights.glsl, and the CPU's MicroRig::boneInfluences).
//
// A card bound to ONE bone DRIFTS OFF the blended body: at mid-graze the
// single-bone card sat a mean 21.5 mm / p99 142 mm / max 230 mm away from the
// surface its root was grown on, which is a visible halo of fur standing off
// the shoulder and the hip. The coat therefore deforms with the SAME blend as
// the body, evaluated at the same rest point - the root for the card's quad,
// the fragment's own rest point for its shading - so a card cannot leave the
// skin it was grown on.
//
// This is the FurBone table's copy of microBoneWeights: the rows differ (a.w
// is the capsule radius here, the micro table keeps it in bone.a.w too), so
// the two spell the kernel once through microWeightOne and nothing else.
int furBoneWeights(int w, vec3 p, out int boneIdx[MICRO_MAX_INFLUENCES],
                   out float wgt[MICRO_MAX_INFLUENCES], out int dominant)
{
    dominant = -1;
    const int count = int(ft.uWorlds[w].bones.x);
    float best = -1.0;
    float sum = 0.0;
    int nearest = 0;
    float nearestD = 1e30;
    for (int i = 0; i < MICRO_MAX_INFLUENCES; ++i) {
        wgt[i] = 0.0;
        boneIdx[i] = -1;
    }
    for (int i = 0; i < count && i < MICRO_MAX_INFLUENCES; ++i) {
        const FurBone mb = ft.uWorlds[w].bone[i];
        const float wi = microWeightOne(p, mb.a.xyz, mb.b.xyz, mb.a.w);
        boneIdx[i] = i;
        wgt[i] = wi;
        sum += wi;
        if (wi > best) {
            best = wi;
            dominant = i;
        }
        const vec3 ab = mb.b.xyz - mb.a.xyz;
        const float t =
            clamp(dot(p - mb.a.xyz, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
        const float d = length(p - (mb.a.xyz + ab * t));
        if (d < nearestD) {
            nearestD = d;
            nearest = i;
        }
    }
    if (!(sum > 0.0)) {
        if (count <= 0)
            return 0;
        boneIdx[0] = nearest;
        wgt[0] = 1.0;
        dominant = nearest;
        return 1;
    }
    const int n = microNormalizeWeights(boneIdx, wgt);
    if (dominant < 0 && n > 0)
        dominant = boneIdx[0];
    return n;
}

// A rest point through the coat's blended transform (linear blend skinning,
// exactly what micro_mesh.vert does to a body vertex).
vec3 furBlendRebone(int w, vec3 p, int bi[MICRO_MAX_INFLUENCES],
                    float bw[MICRO_MAX_INFLUENCES], int n)
{
    if (n <= 0)
        return p;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < n; ++i)
        acc += furRebone(w, p, bi[i]) * bw[i];
    return acc;
}

// The blended inverse of the above, for the billboard's camera point. A
// weighted sum of inverse transforms is not the inverse of the weighted sum
// (LBS is not invertible), but for a SINGLE rigid bone - every card the
// side-car pins - it is exact, and across a blend it is the average frame,
// which is all the ribbon's width axis needs.
vec3 furBlendUnbone(int w, vec3 p, int bi[MICRO_MAX_INFLUENCES],
                    float bw[MICRO_MAX_INFLUENCES], int n)
{
    if (n <= 0)
        return p;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < n; ++i)
        acc += furUnbone(w, p, bi[i]) * bw[i];
    return acc;
}

// A direction through the blended transform (rotations only), renormalized.
vec3 furBlendDirRebone(int w, vec3 d, int bi[MICRO_MAX_INFLUENCES],
                       float bw[MICRO_MAX_INFLUENCES], int n)
{
    if (n <= 0)
        return d;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < n; ++i)
        acc += furDirRebone(w, d, bi[i]) * bw[i];
    const float l = length(acc);
    return l > 1e-8 ? acc / l : d;
}

// A WORLD point -> the object's LOCAL frame (the field's frame). The
// behaviour's transform is undone FIRST (it is the outermost one) and the
// live rotation preview second, exactly as the micro overlay does it.
vec3 furToLocal(int w, vec3 p)
{
    p = furUnanimate(w, p);
    if (ft.uWorlds[w].pivot.w > 0.5)
        p = ft.uWorlds[w].pivot.xyz + transpose(furRot(w)) * (p - ft.uWorlds[w].pivot.xyz);
    return p - ft.uWorlds[w].world.xyz;
}

// A direction (no translation) into the local frame.
vec3 furDirToLocal(int w, vec3 d)
{
    d = furDirUnanimate(w, d);
    if (ft.uWorlds[w].pivot.w > 0.5)
        return transpose(furRot(w)) * d;
    return d;
}

// The local frame -> the world, for the SHADING. The rotation preview is
// undone and the Move preview applied, which is what the micro overlay does
// with the position it shades.
vec3 furToWorld(int w, vec3 pLocal)
{
    vec3 p = pLocal + ft.uWorlds[w].world.xyz;
    if (ft.uWorlds[w].pivot.w > 0.5)
        p = ft.uWorlds[w].pivot.xyz + furRot(w) * (p - ft.uWorlds[w].pivot.xyz);
    p = furAnimate(w, p);
    if (ft.uWorlds[w].preview.w > 0.5)
        p += ft.uWorlds[w].preview.xyz;
    return p;
}

vec3 furDirToWorld(int w, vec3 d)
{
    vec3 v = d;
    if (ft.uWorlds[w].pivot.w > 0.5)
        v = furRot(w) * v;
    return furDirUnanimate(w, v);
}

// Which object owns a global card index. The raster path draws every card of
// every object in ONE instanced draw (one draw call, no per-object state), so
// the vertex shader has to recover the owner. MAX_FUR_WORLDS is 16.
//
// NOTE the direct member reads: copying `FurWorld` here would drag the 1.4 KB
// row (twelve bones included) through every iteration, and this runs PER
// VERTEX.
int furWorldOf(uint card)
{
    const int n = min(int(ft.uInfo.x), MAX_FUR_WORLDS);
    for (int i = 0; i < n; ++i) {
        const uint first = uint(ft.uWorlds[i].cards.x);
        if (card >= first && card < first + uint(ft.uWorlds[i].cards.y))
            return i;
    }
    return -1;
}

#endif // VF_COMMON_FUR_GLSL
