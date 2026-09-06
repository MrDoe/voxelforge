#version 460
#extension GL_GOOGLE_include_directive : require
// Voxelforge Gaussian-surfel rasterizer (vertex stage).
// One instanced TRIANGLE_STRIP quad (4 verts) per surfel; surfel data comes
// from the chunk-sorted SSBO, indexed by gl_InstanceIndex (the draw's
// firstInstance is the chunk's start index, so one vkCmdDraw per chunk).
// The quad is the surfel disk's in-plane bounding box; the fragment shader
// does the exact ray/disk intersection, kernel falloff and plane depth.

struct Surfel {
    vec4 pos_rU;    // xyz = centre (m), w = radiusU (m)
    vec4 normal_rV; // xyz = geometric normal, w = radiusV (m)
    vec4 bent_sh;   // xyz = baked bent (AO) normal, w = baked shadow 0..1
    vec4 mat_ao;    // x = mat id, y = refl*255, z = rough*255, w = baked AO +2 if water
};

layout(set = 0, binding = 0) readonly buffer Surfels {
    Surfel uSurfels[];
};

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a; // tanHalfFov, aspect, extentX, extentY
    vec4 b; // worldSize, voxelSize, gridN, frameIdx
    vec4 sunDir;
    vec4 misc; // x=renderFlags, y=animTime, z=tonemapLook, w=exposure
} pc;

// quad extent mirror of the fragment UBO (z lane); flushed per record.
layout(std140, set = 0, binding = 3) uniform SplatUBO {
    vec4 uSplat;  // x=buried, y=coreD2, z=quad extent, w=debug mode
    vec4 uSplat2; // x=radius scale (hotkeys [/]), yzw=spare
} sp;

layout(location = 0) out vec3 vCenter;
layout(location = 1) out vec3 vT;
layout(location = 2) out vec3 vB;
layout(location = 3) out vec3 vN;
layout(location = 4) out vec2 vRadii;
layout(location = 5) out vec4 vMat;   // mat, refl, rough, aoB(+2 if water)
layout(location = 6) out vec3 vView;  // cornerWorld - camPos (ray, perspective-correct)
layout(location = 7) out float vFace; // dot(n, camPos-c): <0 would-collapse
layout(location = 8) out vec4 vShade; // xyz = baked bent normal, w = baked shadow

layout(constant_id = 0) const int SKY_MODE = 0;

void main()
{
    if (SKY_MODE == 1) {
        // fullscreen triangle: NDC corners (-1,-1), (3,-1), (-1,3); the
        // interpolated vView is the exact per-pixel view direction.
        vec2 ndc = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0,
                        gl_VertexIndex == 2 ? 3.0 : -1.0);
        vView = pc.camFwd.xyz + ndc.x * pc.a.x * pc.a.y * pc.camRight.xyz
                                - ndc.y * pc.a.x * pc.camUp.xyz;
        vCenter = vec3(0.0); vT = vec3(1.0, 0.0, 0.0); vB = vec3(0.0, 0.0, 1.0);
        vN = vec3(0.0, 1.0, 0.0); vRadii = vec2(1.0); vMat = vec4(0.0); vFace = 1.0;
        vShade = vec4(0.0, 1.0, 0.0, 1.0);
        gl_Position = vec4(ndc, 0.5, 1.0);
        return;
    }
    Surfel s = uSurfels[gl_InstanceIndex];
    vec3 c = s.pos_rU.xyz;
    vec3 n = normalize(s.normal_rV.xyz);
    // footprints are isotropic (rU == rV), so any orthonormal in-plane frame
    // matches the baked disk; recompute deterministically (tangent_a now
    // carries the baked bent normal instead)
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(n, up));
    vec3 b = normalize(cross(n, t));
    // runtime radius scale (hotkeys [/]): grows/shrinks disk AND quad
    // together (vRadii carries the scaled radii so d2 stays consistent)
    float rScale = clamp(sp.uSplat2.x, 0.5, 2.0);
    float rU = max(s.pos_rU.w, 1e-4) * rScale;
    float rV = max(s.normal_rV.w, 1e-4) * rScale;

    // Opaque surfels are single-sided: backfaces collapse to a zero-area
    // quad (reliably culled). Skipped in debug views, when the camera is
    // buried inside solid (uSplat.x), and for foliage (mat 8) + roof slabs
    // (mat 7): leaf shells are sparse, so the far side must still cover
    // sightlines through near-side gaps (SVO hits both sides
    // indiscriminately); stepped roof slabs are thin shells whose top faces
    // collapse when viewed from below the eaves, leaving see-through slits
    // between steps unless both sides render.
    float facing = dot(n, pc.camPos.xyz - c);
    bool foliage = abs(s.mat_ao.x - 8.0) < 0.5;
    bool thinShell = foliage || abs(s.mat_ao.x - 7.0) < 0.5;
    vec2 corner = vec2((gl_VertexIndex & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexIndex & 2) == 0 ? -1.0 : 1.0);
    float extent = max(sp.uSplat.z, 1.0);
    vec3 q = c + (t * (corner.x * rU) + b * (corner.y * rV)) * extent;
    if (facing < 0.0 && sp.uSplat.w < 0.5 && sp.uSplat.x < 0.5 && !thinShell)
        q = c;

    vCenter = c;
    vT = t;
    vB = b;
    vN = n;
    vRadii = vec2(rU, rV);
    vMat = s.mat_ao;
    vFace = facing;
    vShade = s.bent_sh;
    vView = q - pc.camPos.xyz;

    // Manual projection with the shared camera convention. w = view depth
    // EXACTLY (no near-plane clamp): the GPU clips near-straddling quads
    // itself, which is exact; clamping w > 0 smears behind-camera corners
    // across the whole screen (giant blob artefacts when looking up past
    // nearby geometry). Fully-behind quads are culled by the clipper.
    // NDC z is any monotonic function of view depth (only relative order
    // matters - the fragment shader overwrites depth with the exact plane
    // hit anyway).
    vec3 rel = q - pc.camPos.xyz;
    float vz = dot(rel, pc.camFwd.xyz);
    float vx = dot(rel, pc.camRight.xyz);
    float vy = dot(rel, pc.camUp.xyz);
    float w = vz;
    vec2 ndc = vec2(vx / (w * pc.a.x * pc.a.y), -vy / (w * pc.a.x));
    float dep = 1.0 - exp(-max(vz, 0.0) * 0.02);
    gl_Position = vec4(ndc * w, dep * w, w);
}
