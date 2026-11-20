#version 460
#extension GL_GOOGLE_include_directive : require
// Raster mesh for a micro-voxel object (micro_mesh.frag).
//
// Writes the SAME G-buffer the traced path writes (hdr / gpos / gnorm, hit type
// 2), so everything downstream - the fur cards' depth reference, SSAO, SSR, the
// post pass - cannot tell which path drew the object.
//
// The surface comes from the mesh; everything SHADED comes from the packed
// per-vertex appearance lane plus the object's REST-POSE field: the material
// is resolved per vertex at load (byte 1 of `vWord`, with the field's cell as
// the fallback), and the detail albedo, the detail normal, the ambient
// occlusion and the self-shadow are sampled from the field. That split is the
// point of the whole path: the field is a shading texture here, so it can
// never open a hole, cut a notch or leave a ghost behind a bone - the failure
// class the traced path had.

#include "micro_shade.glsl"

// EARLY DEPTH TESTS - declared, and INERT on this driver. Recorded here because
// the reasoning is sound and the result is not.
//
// The theory (which the fork supports): this shader writes the G-buffer as
// imageStore SIDE EFFECTS, the pipeline runs with culling off, so on a closed
// surface every pixel is covered twice - near wall and far wall. A fragment
// shader with storage writes cannot be depth-tested early, so both fragments
// run and both store, and the late depth test decides only which framebuffer
// output survives: it cannot unstore the loser. The surviving value is then
// whichever fragment stored LAST, and that order is undefined. Measured: two
// identical renders of this pass differ by ~4 k px (max channel delta ~141 -
// the near wall is lit, the far wall is in shadow) while the same alpaca traced
// from its field differs by 0 px, and no barrier, snapshot, queue drain, cull
// toggle or depth slot can move it, because the race is inside ONE draw call.
//
// THE MEASUREMENT THAT KILLED THE CURE: with this line present, verified in the
// module by `spirv-dis` and accepted by the driver (`--selftest` passes), the
// confirmation pair came back 4,653 px against a 4,278 px baseline. NVIDIA
// honours the declaration and ignores the semantics for a shader that writes
// storage images - and because Vulkan has no `earlyFragmentTests` feature bit,
// the host cannot ask which one it did. So DO NOT read this line as the fix or
// remove it as dead code: it is intent, and the real fix is structural (write
// the G-buffer as colour attachments, which makes the depth test authoritative
// by construction). See .opencode/wiki/concepts/geometry-format-policy.md.
layout(early_fragment_tests) in;

// The PRE-MESH SNAPSHOT of gpos (binding 41), taken once per frame before this
// pass draws. The depth reference this shader needs - the world surface's
// distance along the view ray - must NOT come from uOutGPos: this pass writes
// that image, and a fragment that read a sibling's own store got a distance off
// by up to a pixel footprint (larger than the half-voxel slack), which flipped
// the world/micro verdict from run to run. Reading the snapshot is ordered.
layout(set = 0, binding = 41, rgba16f) uniform readonly highp image2D uSceneGPos;

layout(location = 0) in vec3 vRest;  // the object's REST pose, field frame (m)
layout(location = 1) in vec3 vWorld; // the posed world point (m)
// The packed appearance lane, verbatim from the vertex record
// (`meshVertexWord`, src/voxel/micro_mesh.hpp). FLAT, because a material is an
// ID: interpolating it would blend two palettes across a material seam. Only
// byte 1 is read here; vAo (byte 2) is deliberately not consumed yet - the AO
// the lighting uses is still the shader's own microAO march, and switching to
// the per-vertex value is a separate measurement, not a free win.
layout(location = 2) flat in uint vWord;
layout(location = 3) in float vAo;   // per-vertex baked AO (byte 2)
layout(location = 4) in vec3 vFaceN; // per-vertex REST face normal (E1)

void main()
{
    const int w = int(pc.b.z + 0.5);
    kSunDir = normalize(pc.sunDir.xyz);
    gRenderFlags = int(pc.misc.x + 0.5);
    gTexOv = 0.0;

    const ivec2 pix = ivec2(gl_FragCoord.xy);
    const vec3 ro = pc.camPos.xyz;
    const vec3 p = vWorld;
    const vec3 rd = normalize(p - ro);
    gUnderwater = ((gRenderFlags & 8) != 0) && ro.y < kWaterLevel && underWater(ro.xz);

    // THE WORLD/MICRO BOUNDARY is the same distance comparison the traced path
    // makes: the main backend already resolved this pixel, and a micro surface
    // only wins where it is NEARER. gpos is (world position, hitType); the sky
    // writes zero and the water pass writes the plane, so both cases compare
    // correctly and neither pass needs a depth attachment. It is read from the
    // SNAPSHOT (uSceneGPos), because this pass overwrites the live gpos.
    //
    // The half-voxel slack matters at the FEET: a ground-clamped animal stands
    // exactly on the terrain it is clamped to, and an exact test would shave
    // the soles off against the ground it is standing on.
    const float vs = microVoxelSize();
    const vec4 gp = imageLoad(uSceneGPos, pix);
    const float sceneT = dot(gp.xyz - ro, rd);
    const bool sceneHasGeometry = !(gp.xyz == vec3(0.0)) && sceneT > 0.0;
    const float t = length(p - ro);
    if (sceneHasGeometry && t >= sceneT - 0.5 * vs)
        discard;

    // Workstream E1: field-free mode uses the mesh's REST face normal (and
    // baked AO) so the raster path never samples the field. Default off keeps
    // the field-gradient normal byte-identical. uSunParams.y is the hatch.
    const bool fieldFree = sun.uSunParams.y > 0.5;
    const vec3 restN = fieldFree ? normalize(vFaceN)
                                 : microNormal(vRest, 1.0 * vs);

    // THE APPEARANCE. The packed per-vertex material is PRIMARY when it is
    // present (`assignMicroMeshAppearance` resolves it against the rest field
    // at load): the records carry no per-cell tint - measured colour/palette
    // 1.009 at p1/p50/p99 - so a material ID reproduces the look, and the
    // whole flesh is one palette lookup instead of an octree descent.
    //
    // The field lookup stays as the FALLBACK for a vertex that found no solid
    // rest cell, so the pass is additive and a mesh whose field disagrees in
    // one place loses nothing that worked before. The raster surface (the
    // mesh) and the field's surface (the voxelized mesh) disagree by up to
    // half a voxel, so that fallback can land in an air cell and fail; then
    // the tracer's own lookup (microSurface) at the same point is tried, and
    // if that also fails, a neutral albedo keeps the body solid. The field
    // shades the surface; it may never delete it.
    const uint packedMat = (vWord >> 8) & 0xFFu;
    vec3 alb;
    vec2 rr;
    uint mId;
    if (packedMat < uint(kPaletteN)) {
        mId = packedMat;
        alb = kPalette[int(mId)];
        rr = kMatRefl[int(mId)];
    } else if (!microCellAppearance(w, vRest, -restN, alb, rr, mId)) {
        vec3 _alb; vec2 _rr; uint _mId;
        if (microSurface(p, rd, _alb, _rr, _mId)) {
            alb = _alb; rr = _rr; mId = _mId;
        } else {
            alb = vec3(0.5); rr = vec2(0.15, 0.9); mId = 0u;
        }
    }

    // The texture and its bump are sampled in the REST frame and the normal is
    // brought back out: sampling in the animated frame makes the fleece crawl
    // over the body whenever a bone turns (the graze bows the spine 35 deg).
    //
    // The normal is re-boned with the SAME weights the vertex shader moved the
    // position by, recomputed from the interpolated REST point - which is what
    // the vertex stage evaluated, so the normal and the surface never disagree
    // about which bone they belong to. The blended rotation is not orthonormal
    // (a weighted sum of rotations is not a rotation), hence the normalize.
    alb = detailAlbedo(alb, vRest, restN, mId);
    const vec3 restBumped = detailNormal(restN, vRest, mId);
    int bi[MICRO_MAX_INFLUENCES];
    float bw[MICRO_MAX_INFLUENCES];
    int dominant;
    const int nb = microBoneWeights(w, vRest, bi, bw, dominant);
    vec3 bakedN = restBumped;
    if (nb > 0) {
        bakedN = vec3(0.0);
        for (int i = 0; i < nb; ++i)
            bakedN += microDirRebone(w, restBumped, bi[i]) * bw[i];
        const float nl = length(bakedN);
        bakedN = nl > 1e-8 ? bakedN / nl : restBumped;
    }
    const vec3 n = microDirFromBaked(w, bakedN);

    const vec3 col = shadeMicroLit(w, p, vRest, n, rd, alb, rr, mId,
                                   fieldFree ? clamp(vAo, 0.0, 1.0) : -1.0);
    if (any(isnan(col)) || any(isinf(col))) {
        // DIAGNOSTIC: paint yellow for NaN/Inf (proves the speckle source).
        imageStore(uOutHdr, pix, vec4(1.0, 1.0, 0.0, 1.0));
        imageStore(uOutGPos, pix, vec4(p, 2.0));
        imageStore(uOutGNorm, pix, vec4(0.0, 1.0, 0.0, 0.0));
        return;
    }

    imageStore(uOutHdr, pix, vec4(col, 1.0));
    imageStore(uOutGPos, pix, vec4(p, 2.0));
    imageStore(uOutGNorm, pix, vec4(n, 0.0));
}
