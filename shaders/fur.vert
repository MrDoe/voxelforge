#version 460
#extension GL_GOOGLE_include_directive : require
// Fur cards, RASTER path (vertex stage).
//
// The coat used to be ray-marched: every pixel walked a uniform card grid and
// blind-tested every card in every cell it crossed (~300 card tests per
// pixel, each one a 96 B record fetch + two mat3 multiplies). The pass cost
// ~180 ms at 640x360 on a view where the alpaca filled a tenth of the screen,
// i.e. 98 % of the whole frame.
//
// Rasterizing turns that around: one instanced quad per card, four vertices,
// no vertex buffer. The cost becomes the card's COVERED PIXELS - the thing a
// GPU is actually built for - instead of a per-pixel search through the card
// population. The fragment shader evaluates exactly the same strand alpha
// profile the ray version did, so the coat looks the same.
//
// ONE draw call for every object: gl_InstanceIndex is the global card index
// and furWorldOf() recovers the owner from the card ranges. No per-object
// state, no per-object draw.
//
// The quad is the ribbon's bounding rectangle in its own plane, WIDENED by
// the curl amplitude so the waved centre line never leaves the geometry: the
// ribbon's plane is the quad's plane, so a rigid transform of the card maps
// the quad exactly.

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a; // tanHalfFov, aspect, extentX, extentY
    vec4 b; // worldSize, voxelSize, rectMinX, rectMinY (the last two: composite only)
    vec4 sunDir;
    vec4 misc; // x=renderFlags, y=animTime, z=tonemapLook, w=exposure
} pc;

#include "common_fur.glsl"

// The shared model's resource names (declared by common_base in the OTHER
// shaders; the vertex stage needs none of them).
const float kMinCardPx = 1.5;

layout(location = 0) out vec3 vRest;  // the REST-frame position (texture frame)
layout(location = 1) out vec3 vRestN; // the REST-frame card normal
layout(location = 2) out float vS;    // 0 at the root, 1 at the tip
layout(location = 3) out float vUGeo; // the across-axis coordinate (m)
layout(location = 4) flat out vec4 vA; // wEff, energy, peak alpha, baked AO
layout(location = 5) flat out vec4 vB; // baked shadow, material, curl amp/cycles
layout(location = 6) flat out float vCurlPhase;
layout(location = 7) flat out vec3 vTint;
layout(location = 8) flat out int vBone;
layout(location = 9) flat out int vWorld;
layout(location = 10) flat out float vJit; // per-card tone: clumps are not clones

void main()
{
    const uint ci = uint(gl_InstanceIndex);
    const int w = furWorldOf(ci);
    if (w < 0) {
        // Not owned by any object (a stale instance count): collapse the quad.
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        vRest = vec3(0.0); vRestN = vec3(0.0, 1.0, 0.0); vS = 0.0; vUGeo = 0.0;
        vA = vec4(0.0); vB = vec4(0.0); vCurlPhase = 0.0; vTint = vec3(0.0);
        vBone = -1; vWorld = 0; vJit = 0.0;
        return;
    }

    const vec4 rootLen = fCard[ci * 6u + 0u];
    const vec4 dirWidth = fCard[ci * 6u + 1u];
    const vec4 nrmAlpha = fCard[ci * 6u + 2u];
    const vec4 tint = fCard[ci * 6u + 3u];
    const vec4 shade = fCard[ci * 6u + 4u];
    const vec4 curl = fCard[ci * 6u + 5u];

    const vec3 root = rootLen.xyz;
    const vec3 dir = dirWidth.xyz;
    const float len = max(rootLen.w, 1e-4);
    const float wid = max(dirWidth.w, 1e-4);
    const vec3 n = nrmAlpha.xyz;
    const int bone = int(shade.w);

    // The camera in the CARD's own (rest) frame: the ribbon is billboarded
    // about its strand, so the width axis depends on where the viewer is.
    // Un-boning the camera point is the same operation the ray version did to
    // the ray origin, so the axis matches it exactly.
    vec3 roL = furToLocal(w, pc.camPos.xyz);
    if (bone >= 0)
        roL = furUnbone(w, roL, bone);

    vec3 toCam = roL - root;
    const float tc = length(toCam);
    vec3 side = cross(dir, tc > 1e-6 ? toCam / tc : vec3(0.0, 0.0, 1.0));
    float sl = length(side);
    if (sl < 1e-4) {
        // The strand points at the camera: fall back to the surface frame.
        side = cross(dir, n);
        sl = length(side);
    }
    side = sl > 1e-5 ? side / sl : vec3(1.0, 0.0, 0.0);
    const float tw = tint.w;
    if (abs(tw) > 1e-4) {
        const vec3 k = dir;
        const float c = cos(tw), s = sin(tw);
        side = side * c + cross(k, side) * s;
    }

    // The MINIMUM SCREEN-SPACE WIDTH: a card thinner than a pixel either
    // misses the pixel grid or lands on it whole, and the coat turns into
    // speckle. Widening it and scaling the alpha by the same factor conserves
    // the ink (the same rule the ray version applied, and the same feather).
    //
    // The distance is taken in the card's REST frame on purpose: every
    // transform between it and the world is a rigid motion, so it preserves
    // the distance - and this avoids running the whole transform chain a
    // second time per vertex just to measure one length.
    const float dist = max(length(roL - root), 1e-3);
    const float mpp = 2.0 * pc.a.x * dist / max(pc.a.w, 1.0);
    const float wEff = max(wid, kMinCardPx * mpp);
    const float energy = wid / wEff;

    // The quad: s along the strand, u across it. TRIANGLE_STRIP order
    // (-,-) (+,-) (-,+) (+,+).
    const float s = (gl_VertexIndex & 2) == 0 ? 0.0 : 1.0;
    const float us = (gl_VertexIndex & 1) == 0 ? -1.0 : 1.0;
    const float halfW = 0.5 * wEff + abs(curl.x);
    const float uGeo = us * halfW;
    const vec3 rest = root + dir * (s * len) + side * uGeo;

    // The REST point is what the texture and the detail normal are sampled
    // at (the fur must not slide), so it travels to the fragment untouched;
    // the ANIMATED world point is what gets rasterized and depth-tested.
    const vec3 animPos = furRebone(w, rest, bone);
    const vec3 pWorld = furToWorld(w, animPos);

    vRest = rest;
    vRestN = n;
    vS = s;
    vUGeo = uGeo;
    vA = vec4(wEff, energy, nrmAlpha.w, shade.x);
    vB = vec4(shade.y, shade.z, curl.x, curl.y);
    vCurlPhase = curl.z;
    vTint = tint.rgb;
    vBone = bone;
    vWorld = w;
    // A per-card tone offset. Every card is a CLUMP, not a clone: real
    // fleece varies from tuft to tuft, and without it the coat reads as one
    // flat wash of the material colour (measured: the gate's luma-spread
    // assertion sat ~5 % under the bare body's no matter the card density).
    vJit = fract(sin(float(ci) * 12.9898 + 78.233) * 43758.5453);

    // The shared camera convention (see splat.vert): w = view depth exactly,
    // NDC y negated, no near-plane clamp - the clipper handles straddling
    // quads, and clamping smears behind-camera corners across the screen.
    const vec3 rel = pWorld - pc.camPos.xyz;
    const float vz = dot(rel, pc.camFwd.xyz);
    const float vx = dot(rel, pc.camRight.xyz);
    const float vy = dot(rel, pc.camUp.xyz);
    const float wq = vz;
    const vec2 ndc = vec2(vx / (wq * pc.a.x * pc.a.y), -vy / (wq * pc.a.x));
    gl_Position = vec4(ndc * wq, 0.0, wq);
}
