#version 460
#extension GL_GOOGLE_include_directive : require
// Fur cards, RASTER path (fragment stage).
//
// Per fragment this evaluates EXACTLY the strand profile the ray-marched
// version evaluated per ray/card hit:
//   u   = (across - the curl wave) / the effective width
//   alpha = peak * exp(-2.6*(2u)^2) * along-taper * energy
// with the minimum-screen-width rule applied in the vertex stage (the quad is
// widened and the alpha scaled by the same factor, so the ink is conserved).
//
// It accumulates the SAME six alpha-weighted terms the ray version did, into
// four additive screen-space targets, so the composite pass downstream can
// reproduce the per-pixel lighting model unchanged rather than approximating
// it per card (evaluating the sky per card costs a sky evaluation per card
// hit, and a pixel through the coat hits a dozen):
//   A = (sum a, sum a*ndl*shadow, sum a*ao, sum a*mat)
//   B = sum a * worldPos
//   C = sum a * worldNormal
//   D = sum a * albedo
//
// The depth reference is the G-buffer's world position (the world, the water
// plane, or the micro body the overlay drew), so a card behind the body or
// behind the world is dropped - the same rule the ray version's tExit used,
// and there is still no depth attachment anywhere.

const float kMinCardPx = 1.5;

layout(set = 0, binding = 14, rgba16f) uniform readonly highp image2D uOutGPos;

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

// The shared model declares uHeight/uObjVol itself via these two names.
layout(set = 0, binding = 8, rg32f) uniform readonly highp image2D uHeight;
layout(set = 0, binding = 9, r8_snorm) uniform readonly highp image3D uObjVol;

const float kWaterLevel = -0.9;

#include "common_base.glsl"
#include "common_fur.glsl"

layout(location = 0) in vec3 vRest;
layout(location = 1) in vec3 vRestN;
layout(location = 2) in float vS;
layout(location = 3) in float vUGeo;
layout(location = 4) flat in vec4 vA; // wEff, energy, peak alpha, baked AO
layout(location = 5) flat in vec4 vB; // baked shadow, material, curl amp/cycles
layout(location = 6) flat in float vCurlPhase;
layout(location = 7) flat in vec3 vTint;
layout(location = 8) flat in int vBone;
layout(location = 9) flat in int vWorld;
layout(location = 10) flat in float vJit;

layout(location = 0) out vec4 oA;
layout(location = 1) out vec4 oB;
layout(location = 2) out vec4 oC;
layout(location = 3) out vec4 oD;
layout(location = 4) out vec4 oE;

void main()
{
    kSunDir = normalize(pc.sunDir.xyz);
    gRenderFlags = int(pc.misc.x + 0.5);
    gTexOv = 0.0;
    gUnderwater = false;

    // The curl: the strand's centre line wanders across the ribbon's plane in
    // a wave that is zero at the root and grows toward the tip. The quad is
    // widened by the amplitude in the vertex stage, so this only has to move
    // the ALPHA across the ribbon - the geometry already contains it.
    const float wave = vB.z * sin(6.2831853 * vB.w * vS + vCurlPhase) * vS;
    const float u = (vUGeo - wave) / max(vA.x, 1e-6);
    if (abs(u) > 0.5)
        discard;

    const float x = u * 2.0;
    const float across = exp(-2.6 * x * x);
    const float along = (1.0 - 0.45 * vS * vS) *
                        smoothstep(0.0, 0.05, vS) *
                        smoothstep(1.0, 0.72, vS);
    const float alpha = vA.z * across * along * vA.y;
    if (alpha < 0.004)
        discard;

    // The shaded point, in the REST frame first: the coat samples its texture
    // and its detail normal there, so the fleece cannot slide over the body
    // as the bones swing.
    const vec3 restPos = vRest;
    const vec3 restN = normalize(vRestN);
    const uint mId = uint(vB.y + 0.5);

    const vec3 animPos = furRebone(vWorld, restPos, vBone);
    const vec3 pWorld = furToWorld(vWorld, animPos);

    // The depth reference: the nearest opaque surface, in ray terms. The
    // G-buffer's world position is exact per pixel, so the test is the same
    // one the ray version's tExit made against sceneT.
    const vec2 ndc = gl_FragCoord.xy / vec2(pc.a.z, pc.a.w) * 2.0 - 1.0;
    const vec3 rd = normalize(pc.camFwd.xyz + ndc.x * pc.a.x * pc.a.y * pc.camRight.xyz -
                              ndc.y * pc.a.x * pc.camUp.xyz);
    const vec4 gp = imageLoad(uOutGPos, ivec2(gl_FragCoord.xy));
    const bool hasScene = !(gp.xyz == vec3(0.0));
    const float sceneT = hasScene ? dot(gp.xyz - pc.camPos.xyz, rd) : 1e9;
    if (dot(pWorld - pc.camPos.xyz, rd) > sceneT + 0.01)
        discard;

    // The shading normal: the card's surface normal, bumped by the SAME
    // detail normal the body uses (render-flag bit 7) and then carried into
    // the world. Sampling the bump in the REST frame is what keeps the
    // relief glued to the skin: projecting with the animated normal makes
    // the pattern crawl over the coat on every bone rotation. `detailNormal`
    // is a no-op when the feature is off or the material is untextured, so
    // the bit-7-off frame stays bit-exact.
    const vec3 restBumped = detailNormal(restN, restPos, mId);
    const vec3 nAnim = furDirRebone(vWorld, restBumped, vBone);
    vec3 nWorld = normalize(furDirToWorld(vWorld, nAnim));
    if (dot(nWorld, rd) > 0.0)
        nWorld = -nWorld; // a card is two-sided

    const float ndl = max(dot(nWorld, kSunDir), 0.0);
    const float shadow = ((gRenderFlags & 2) != 0) ? vB.x : 1.0;

    // A TEXTURED material takes its colour from the atlas and IGNORES the
    // record's colour (the same rule the world follows); an untextured one
    // keeps the record's colour. Sampled PER FRAGMENT, at the rest position,
    // so the fleece keeps its centimetre-scale variation.
    vec3 alb = kPalette[clamp(mId, 0u, 21u)];
    const vec3 tex = sampleTex(restPos, restN, mId);
    alb = tex.r >= 0.0 ? tex : vTint;
    // Clump-to-clump tone. Uniform albedo makes the coat a flat wash of the
    // material colour; real fleece varies tuft by tuft. The range is a LOOK
    // constant (about +-18 %), not a physical one - it is what makes the
    // coat keep its own texture instead of reading as a lit shell.
    alb *= 0.82 + 0.36 * vJit;

    oA = vec4(alpha, alpha * ndl * shadow, alpha * vA.w, alpha * float(mId));
    oB = vec4(alpha * pWorld, 0.0);
    oC = vec4(alpha * nWorld, 0.0);
    oD = vec4(alpha * alb, 0.0);
    // The REST position, so the composite can evaluate the detail normal at
    // the PIXEL's own place instead of the card's. Per-card relief averages
    // out across the dozen cards a pixel sees; the pixel-scale version is
    // what keeps the coat's shading as detailed as the body's.
    oE = vec4(alpha * restPos, 0.0);
}
