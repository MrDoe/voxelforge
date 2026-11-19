#version 460
#extension GL_GOOGLE_include_directive : require
// Ground foliage cards, RASTER path (vertex stage), modelled on fur.vert.
//
// One instanced quad per card, four vertices, no vertex buffer. Unlike fur,
// grass has no skeleton: each card is a vertical blade standing on the
// terrain, billboarded about its own strand so it never turns edge-on. The
// data (root, layer, orientation, size, tint) comes from the CPU-generated
// grass-card SSBO at binding 31; the atlas layer is the one bound to the
// ground material in the scenery's "foliageCards" table (matTex[mat].w).
//
// gl_InstanceIndex is the global card index; one draw covers the whole world.

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a; // tanHalfFov, aspect, extentX, extentY
    vec4 b; // worldSize, voxelSize, rectMinX, rectMinY
    vec4 sunDir;
    vec4 misc; // x=renderFlags, y=animTime, z=tonemapLook, w=exposure
} pc;

layout(set = 0, binding = 31, std430) readonly buffer GrassCards {
    vec4 gCards[];
};

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vN;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out vec4 vCard; // layer, alpha, material id, tone
layout(location = 4) flat out float vEnergy; // ink conservation on widen

const float kMinCardPx = 1.5;

void main()
{
    const uint ci = uint(gl_InstanceIndex);
    const vec4 rootLen = gCards[ci * 4u + 0u];    // xyz root, w = atlas layer
    const vec4 dirW   = gCards[ci * 4u + 1u];    // xyz strand dir, w = width
    const vec4 nA     = gCards[ci * 4u + 2u];    // xyz normal, w = peak alpha
    const vec4 tintM  = gCards[ci * 4u + 3u];    // rgb tone, w = material id

    const vec3 root = rootLen.xyz;
    const float layer = rootLen.w;
    const vec3 dir = normalize(dirW.xyz);
    const float len = max(length(dirW.xyz), 1e-4); // stored pre-normalised dir*len
    const float wid = max(dirW.w, 1e-4);
    const vec3 n = normalize(nA.xyz);
    const float alpha = nA.w;

    // Billboard the card about its strand: the width axis is perpendicular
    // to the strand AND to the camera, so the blade is never edge-on.
    const vec3 toCam = pc.camPos.xyz - root;
    const float dist = max(length(toCam), 1e-3);
    vec3 side = cross(dir, toCam / dist);
    float sl = length(side);
    if (sl < 1e-4)
        side = cross(dir, n), sl = length(side);
    side = sl > 1e-5 ? side / sl : vec3(1.0, 0.0, 0.0);

    // A sub-pixel blade either misses the pixel grid or lands on it whole:
    // widen it to 1.5 px and scale alpha by the same factor (ink conserved).
    const float mpp = 2.0 * pc.a.x * dist / max(pc.a.w, 1.0);
    const float wEff = max(wid, kMinCardPx * mpp);
    const float energy = wid / wEff;

    const float s = (gl_VertexIndex & 2) == 0 ? 0.0 : 1.0;
    const float us = (gl_VertexIndex & 1) == 0 ? -1.0 : 1.0;
    const vec3 pos = root + dir * (s * len) + side * (us * wEff * 0.5);

    vWorld = pos;
    vN = n;
    vUV = vec2(us * 0.5 + 0.5, s);
    vCard = vec4(layer, alpha, tintM.w, fract(sin(float(ci) * 12.9898 + 78.233) * 43758.5453));
    vEnergy = energy;

    const vec3 rel = pos - pc.camPos.xyz;
    const float vz = dot(rel, pc.camFwd.xyz);
    const float vx = dot(rel, pc.camRight.xyz);
    const float vy = dot(rel, pc.camUp.xyz);
    const vec2 ndc = vec2(vx / (vz * pc.a.x * pc.a.y), -vy / (vz * pc.a.x));
    gl_Position = vec4(ndc * vz, 0.0, vz);
}
