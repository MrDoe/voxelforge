// The micro worlds' DATA CONTRACT and RIG MATH (micro_rig.glsl).
//
// Included by micro_shade.glsl (the shared shading model) and by
// micro_mesh.vert (the raster mesh path). It carries the descriptor
// declarations as well as the code, because the compute overlay and the
// raster mesh share ONE descriptor set: binding numbers are the contract,
// and a second copy of them is how two paths drift.
//
// Nothing here samples the atlas or the sky - it is the world table, the
// field's byte layout and the rigid transforms (bones, previews, behaviour),
// so a vertex shader can pose a mesh without dragging in the lighting model.

// ---- the micro worlds (separate from the world's own SVO buffers) ----------
layout(std430, set = 0, binding = 1) readonly buffer MGrid { int uMGrid[]; };
layout(std430, set = 0, binding = 2) readonly buffer MChildBase { uint uMChildBase[]; };
layout(std430, set = 0, binding = 3) readonly buffer MPayload { uint uMPayload[]; };
layout(std430, set = 0, binding = 4) readonly buffer MHandles { uint uMHandles[]; };
layout(std430, set = 0, binding = 5) readonly buffer MBricks { uint uMBricks[]; };
layout(std430, set = 0, binding = 6) readonly buffer MChunkInfo { uvec4 uMChunkInfo[]; };
// The DENSE signed-distance fields: one signed byte per cell, in eighth-voxel
// units, all worlds concatenated. Separate from the bricks because the field
// has to be CONTINUOUS: the bricks' trilinear stencil can only reach inside
// its own brick, so a brick-stored field JUMPS at every brick boundary and
// the sphere tracer has no zero crossing to bisect to. See MicroWorld::field.
layout(std430, set = 0, binding = 10) readonly buffer MField { uint uMField[]; };

// Per-world SCALES. Everything world-specific the main shader hardcodes from
// the push constant (worldSize / voxelSize / gridN) lives here instead, which is
// what lets one dispatch serve N objects at N different voxel sizes.
//   geo  = (voxelSize m, gridN, chunkM m, _)
//   base = (gridBase into uMGrid, chunkInfoBase into uMChunkInfo, dimX, dimZ)
//   lo/hi= world-space AABB of the object (early-out + march bounds);
//          lo.w = this world's offset into uMField, in cells;
//          hi.w = 1 when the world is RASTER-OWNED (its geometry is a posed
//                 mesh and this field is only the rest-pose shading texture)
// The node/child/brick bases are NOT here: they are folded into each world's
// chunkInfo entries at upload time, so one flat index serves every world.
// One bone: the segment it occupies (a start and an end), the joint it
// turns about, and the animated rotation matrix (row-major). A bone is a
// rigid rotation about its joint, so the field can be sampled through it.
struct MicroBone {
    vec4 a;
    vec4 b;
    // The CHAINED transform's translation (xyz) - the bone's rigid transform
    // is R*p + t, and the inverse is R^T*(p - t).
    vec4 joint;
    vec4 r0;
    vec4 r1;
    vec4 r2;
};
struct MicroWorld {
    vec4 geo;
    uvec4 base;
    vec4 lo;
    vec4 hi;
    // Live preview offset (xyz) + an active flag (w). The staged Move delta
    // for a selected micro object: the field stays where it was baked and the
    // RENDERED position is offset, so a drag is instant and costs no re-bake.
    vec4 preview;
    // Live ROTATION preview: the matrix rows (3x3, row-major) and the pivot
    // (xyz = world m, w = 1 when the rotation preview is active). Same idea as
    // the Move offset: the field, the material lookup and the octree stay in
    // the BAKED frame, and the RENDERED position is rotated out of it, so a
    // drag costs no re-bake. A micro object has no world owner id, so the
    // splat pass's rotate preview cannot show it - this is its only live
    // rotation.
    vec4 rot0;
    vec4 rot1;
    vec4 rot2;
    vec4 pivot;
    // The AUTONOMOUS BEHAVIOUR's transform: an offset (xyz) + an active
    // flag (w), a rotation matrix (row-major) and its pivot (world m). It
    // turns a static micro object into one that walks on its own, and it
    // obeys the same rule as the rotation preview: the field stays in the
    // BAKED frame and the transform is applied when the field is sampled.
    vec4 anim;
    vec4 arot0;
    vec4 arot1;
    vec4 arot2;
    vec4 apivot;
    // The SKELETON: x = the bone count. The bones themselves follow, in the
    // world table - their segments and joints never change, and the gait
    // rewrites only their rotations.
    vec4 bones;
    MicroBone bone[12];
};
layout(std140, set = 0, binding = 7) readonly uniform WorldTable {
    MicroWorld uWorlds[16];
    ivec4 uWorldInfo; // x = world count, yzw = pad
} wt;

// ---- shared-model resources (same texture views the other passes use) ------
layout(set = 0, binding = 8, rg32f) uniform readonly highp image2D uHeight;
layout(set = 0, binding = 9, r8_snorm) uniform readonly highp image3D uObjVol;

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a; // tanHalfFov, aspect, extentX, extentY
    vec4 b; // unused in micro worlds (their scales come from uWorlds); w = frameIdx
    vec4 sunDir;
    vec4 misc; // x = renderFlags, y = animTime, z = tonemapLook, w = exposure
} pc;

const uint kEmpty = 0xFFFFFFFFu;
const uint kSolid = 0xFFFFFFFEu;
const int BRICK_N = 8;
const int BRICK_WORDS = BRICK_N * BRICK_N * BRICK_N * 2;
const float kWaterLevel = -0.9;
const int MAX_MICRO_WORLDS = 16;

struct MBases {
    uint node;
    uint child;
    uint brick;
};

// Local copy of the main shader's box SDF (the one line that needs it).
// common_svo.glsl is deliberately NOT included: including it would pull in the
// MAIN world's world-resolution code, which is the whole thing this pass keeps
// out of its own resolution path.
float sdBox(vec3 p, vec3 bmin, vec3 bmax)
{
    vec3 q = max(bmin - p, p - bmax);
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

MBases mChunkBases(int ci)
{
    uvec4 v = uMChunkInfo[ci];
    return MBases(v.x, v.y, v.z);
}

// The micro field's sdf byte counts EIGHTHS of a voxel, not whole voxels. The
// world's int8-voxel convention cannot be reused here: the world raymarcher is
// a DDA that tests the byte's SIGN, so its hit is always a cell, but this pass
// SPHERE-TRACES a continuous field, and truncating the BFS distance to whole
// voxels sends both the outermost solid cell (-0.5 voxel) and the adjacent air
// cell (+0.5 voxel) to byte 0 - a flat-zero band across the surface, with the
// zero crossing at the band's edge instead of at the surface. See
// src/voxel/micro_field.hpp for the full reasoning.
const float kMicroSdfQuantum = 8.0;

// The sdf byte is a signed count of kMicroSdfQuantumths of a voxel, so the
// decode scale is THIS world's voxel size - the one thing that must never be
// read from the push constant.
float mBrickVoxelSdf(uint bi, uint brickBase, ivec3 c, float voxel)
{
    c = clamp(c, ivec3(0), ivec3(BRICK_N - 1));
    uint v = uMBricks[(brickBase + bi) * BRICK_WORDS +
                      (uint(c.z) * BRICK_N * BRICK_N + uint(c.y) * BRICK_N +
                       uint(c.x)) * 2];
    int raw = int((v >> 24) & 255u);
    if (raw >= 128)
        raw -= 256;
    return float(raw) * voxel / kMicroSdfQuantum;
}

// NOTE: there is deliberately NO brick-based field sampler here any more. The
// field used to be interpolated out of the BRICK cells, whose trilinear
// stencil reaches one cell past the sample point - and a brick can only be
// indexed inside itself, so the stencil had to be clamped to the brick, and a
// clamped stencil makes the field JUMP at every brick boundary (measured:
// +0.5 voxel to -1.0 voxel in one step, with no zero crossing between). The
// sphere tracer then had nothing to bisect to and found the surface at a
// discontinuity. The field comes from the DENSE per-world array instead (see
// mFieldCell), which has no such boundary. Do not reintroduce a brick
// sampler for the field: the bricks carry MATERIAL, the dense array carries
// DISTANCE.

// Signed distance to ONE micro world at p. Outside the world's AABB it returns
// the TRUE distance to that box, never a flat constant.
//
// This matters more than it looks. The main shader's map() also returns a
// constant (6.0) outside its world, and is safe doing so only because the world
// is 102 m across and the SVO marches with a DDA, not by stepping. A micro
// object is centimetres across and IS sphere-traced, so a 6 m step from the
// camera sails straight past a vase half a metre away and the trace reports a
// miss. Returning the exact box distance makes the outside case a valid
// conservative step instead.
// The field value at one cell of the dense field, in metres. The byte counts
// eighth-voxels (kMicroSdfQuantum), negative inside the object.
float mFieldCell(uint off, ivec3 dim, ivec3 c, float voxel)
{
    c = clamp(c, ivec3(0), dim - ivec3(1));
    // The field is a BYTE array, but std430 packs uints four to a 16-byte
    // block, so a byte index is (word, shift). The world's byte offset is
    // padded to a multiple of four so the two never straddle a word.
    uint i = off + uint((c.z * dim.y + c.y) * dim.x + c.x);
    int raw = int((uMField[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu);
    if (raw >= 128)
        raw -= 256;
    return float(raw) * voxel / kMicroSdfQuantum;
}

// Is this world RASTER-OWNED, i.e. is its geometry a posed MESH rather than the
// traced field? Then the field is the REST POSE only: a shading texture, never
// a surface. Two consequences, both in the samplers below: the tracer must skip
// it, and a field query must NOT un-bone its point (the caller is already in
// the field's frame, and un-boning an undeformed point deforms it).
bool microWorldIsMesh(int w)
{
    return wt.uWorlds[w].hi.w > 0.5;
}

// Is a live rotation preview active for this world?
bool microRotOn(int w)
{
    return wt.uWorlds[w].pivot.w > 0.5;
}

// The rotation matrix (row-major) of a world's live rotation preview.
mat3 microRot(int w)
{
    return mat3(wt.uWorlds[w].rot0.xyz, wt.uWorlds[w].rot1.xyz,
                wt.uWorlds[w].rot2.xyz);
}

// The autonomous BEHAVIOUR's transform, applied to a world point: the
// offset, then the rotation about the pivot. Its inverse undoes it.
//
// The order matters and it is the same order as the CPU's: the behaviour's
// rotation is about the pivot in the ANIMATED frame, so the offset is
// removed first and the rotation second.
vec3 microAnimate(int w, vec3 p)
{
    if (wt.uWorlds[w].anim.w < 0.5)
        return p;
    const mat3 R = mat3(wt.uWorlds[w].arot0.xyz, wt.uWorlds[w].arot1.xyz,
                        wt.uWorlds[w].arot2.xyz);
    p = wt.uWorlds[w].apivot.xyz + R * (p - wt.uWorlds[w].apivot.xyz);
    return p + wt.uWorlds[w].anim.xyz;
}

// The inverse: a world point back into the object's baked frame.
vec3 microUnanimate(int w, vec3 p)
{
    if (wt.uWorlds[w].anim.w < 0.5)
        return p;
    const mat3 R = mat3(wt.uWorlds[w].arot0.xyz, wt.uWorlds[w].arot1.xyz,
                        wt.uWorlds[w].arot2.xyz);
    p -= wt.uWorlds[w].anim.xyz;
    return wt.uWorlds[w].apivot.xyz + transpose(R) * (p - wt.uWorlds[w].apivot.xyz);
}

// A direction through the behaviour's rotation (no translation).
vec3 microDirUnanimate(int w, vec3 d)
{
    if (wt.uWorlds[w].anim.w < 0.5)
        return d;
    return transpose(mat3(wt.uWorlds[w].arot0.xyz, wt.uWorlds[w].arot1.xyz,
                           wt.uWorlds[w].arot2.xyz)) * d;
}

// The SKELETON. A bone is a SEGMENT in the object's own REST frame (the rig is
// DERIVED from the object's voxels, so a newly imported model has one too),
// and a point belongs to the bone whose rest pose it lies nearest. The seams
// fall where two bones meet INSIDE the body, so they never show on the
// silhouette.
//
// WHICH BONE IS THE WHOLE PROBLEM, and it has to be answered in the REST
// frame. The segments are stored in the rest pose, so testing the POSED point
// against them (the old rule) reclassifies whatever the pose has moved: the
// moment the spine bows 35 deg to graze, the whole front of the torso stops
// being nearest its own segment and is handed to the HEAD or a LEG. Two things
// break at once and both are visible on screen:
//   * the surface is un-boned through the WRONG transform, so the neck and
//     torso deform as if they belonged to the head - the neck then bends
//     against the body instead of with it;
//   * its texture is sampled at the WRONG rest point, so the fleece slides
//     over the body as the animal moves.
//
// So ask each candidate the question that HAS an answer: un-bone the point
// with THAT bone's own transform and measure the distance to THAT bone's own
// segment. For the right bone the point lands back on its rest pose, at the
// distance the skin sits from the bone axis; a wrong transform throws it off.
// At rest every candidate returns the same point, so this reduces exactly to
// the plain nearest-segment rule.
int microBoneAt(int w, vec3 p)
{
    const int count = int(wt.uWorlds[w].bones.x);
    if (count <= 0)
        return -1;
    int best = -1;
    float bestD = 1e30;
    for (int i = 0; i < count; ++i) {
        const MicroBone mb = wt.uWorlds[w].bone[i];
        const mat3 R = mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz);
        const vec3 q = transpose(R) * (p - mb.joint.xyz);
        const vec3 a = mb.a.xyz;
        const vec3 ab = mb.b.xyz - a;
        const float t = clamp(dot(q - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
        const float d = max(0.0, length(q - (a + ab * t)) - mb.a.w);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// A bone's transform is a RIGID one (R*p + t), CHAINED: it is the parent's
// composed with the bone's own rotation. The inverse is R^T*(p - t).
//
// It has to be a general rigid transform and not a rotation about a pivot:
// the composition of two pivot-rotations is not a pivot-rotation, and a child
// that turns about a REST joint while the parent moves tears at the seam -
// which is exactly what the neck did (the user saw a broken neck when the
// animal bowed).
vec3 microUnbone(int w, vec3 p, int bone)
{
    if (bone < 0)
        return p;
    const MicroBone mb = wt.uWorlds[w].bone[bone];
    const mat3 R = mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz);
    return transpose(R) * (p - mb.joint.xyz);
}

// The bone's transform, applied to a point: the animated position of a rest
// pose point.
vec3 microRebone(int w, vec3 p, int bone)
{
    if (bone < 0)
        return p;
    const MicroBone mb = wt.uWorlds[w].bone[bone];
    const mat3 R = mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz);
    return R * p + mb.joint.xyz;
}

// A WORLD point -> the object's BAKED frame (the field's frame). The
// behaviour's transform is undone first (it is the OUTERMOST transform:
// the object walks, and the user's staged rotation rides on top of it), then
// the rotation preview's.
vec3 microToBaked(int w, vec3 p)
{
    p = microUnanimate(w, p);
    if (!microRotOn(w))
        return p;
    return wt.uWorlds[w].pivot.xyz +
           transpose(microRot(w)) * (p - wt.uWorlds[w].pivot.xyz);
}

// The RENDERED point -> the BAKED frame. THE one frame mapping: the field
// trace uses it (microMapWorld) and so does every surface lookup - texture,
// material cell, octree, normal, AO.
//
// It differs from microToBaked() by the staged Move preview, which is a FIELD
// TRANSFORM like the behaviour and the rotation preview, not a display offset.
// It used to be applied in main() AFTER the trace, which meant the trace saw
// the object where it was baked while the display moved the surface: the
// covered pixels were the old silhouette, the depth was the old depth, and
// shading sampled the object beside the one on screen. As part of this mapping
// the trace, the depth test and the shading agree while the object is dragged,
// and nothing downstream has to un-shift anything.
vec3 microShadeToBaked(int w, vec3 p)
{
    const vec4 pv = wt.uWorlds[w].preview;
    if (pv.w > 0.5)
        p -= pv.xyz;
    return microToBaked(w, p);
}

// The inverse of microShadeToBaked: a BAKED-frame point to the RENDERED one.
// The raster mesh path poses its vertices with it, in the same order the field
// is sampled through (behaviour, then the rotation preview's inverse, then the
// staged Move as the outermost offset) - so the mesh and the rest field agree
// on where the surface is even while an object is being dragged or is walking.
vec3 microShadeFromBaked(int w, vec3 p)
{
    if (wt.uWorlds[w].anim.w >= 0.5) {
        const mat3 A = mat3(wt.uWorlds[w].arot0.xyz, wt.uWorlds[w].arot1.xyz,
                            wt.uWorlds[w].arot2.xyz);
        p = wt.uWorlds[w].apivot.xyz + A * (p - wt.uWorlds[w].apivot.xyz) +
            wt.uWorlds[w].anim.xyz;
    }
    if (microRotOn(w))
        p = wt.uWorlds[w].pivot.xyz + microRot(w) * (p - wt.uWorlds[w].pivot.xyz);
    if (wt.uWorlds[w].preview.w > 0.5)
        p += wt.uWorlds[w].preview.xyz;
    return p;
}

// The point in the object's OWN frame (the rest pose). It is where the
// object's TEXTURE lives: a world-space sample slides over the animal as it
// walks and as its bones swing, because the texture is bound to the surface,
// not to the world.
vec3 microRestPoint(int w, vec3 p)
{
    const vec3 rest = microShadeToBaked(w, p);
    return microUnbone(w, rest, microBoneAt(w, rest));
}

// A DIRECTION through a bone's transform (rotations only; a direction has no
// translation). The fur's projection axes have to travel with the skin, so the
// texture is sampled in the REST frame with the REST normal - projecting with
// the ANIMATED normal makes the pattern crawl over the body whenever a bone
// turns (the graze bows the spine 35 degrees, which is plainly visible).
vec3 microDirUnbone(int w, vec3 d, int bone)
{
    if (bone < 0)
        return d;
    const MicroBone mb = wt.uWorlds[w].bone[bone];
    return transpose(mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz)) * d;
}

vec3 microDirRebone(int w, vec3 d, int bone)
{
    if (bone < 0)
        return d;
    const MicroBone mb = wt.uWorlds[w].bone[bone];
    return mat3(mb.r0.xyz, mb.r1.xyz, mb.r2.xyz) * d;
}

// A direction from the baked frame back into the world: the inverse of
// microDirToBaked (the preview's rotation, then the behaviour's).
vec3 microDirFromBaked(int w, vec3 d)
{
    if (microRotOn(w))
        d = microRot(w) * d;
    if (wt.uWorlds[w].anim.w >= 0.5)
        d = mat3(wt.uWorlds[w].arot0.xyz, wt.uWorlds[w].arot1.xyz,
                 wt.uWorlds[w].arot2.xyz) * d;
    return d;
}


// A direction (no translation) into the baked frame.
vec3 microDirToBaked(int w, vec3 d)
{
    d = microDirUnanimate(w, d);
    if (!microRotOn(w))
        return d;
    return transpose(microRot(w)) * d;
}

