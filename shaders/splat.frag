#version 460
#extension GL_GOOGLE_include_directive : require
#define SPLAT_BACKEND 1
// Voxelforge Gaussian-surfel rasterizer (fragment stage).
// Exact ray/disk intersection gives per-fragment plane depth (no centroid
// z-fighting); a compact smooth kernel with an opaque core reads as a solid
// surface, with analytic alpha only in a ~1px annulus for AA. Shading is
// shadeSurfel() from common_splat.glsl - shadeTerrain's twin.

layout(set = 0, binding = 1, rg32f) uniform readonly highp image2D uHeight;
layout(set = 0, binding = 2, r8_snorm) uniform readonly highp image3D uObjVol;

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

// per-frame kernel tuning (HUD): x=kernel (0 compact / 1 gaussian),
// y=core threshold, z=quad extent, w=spare. Persistently mapped UBO,
// flushed by SplatPass::record like the SVO highlight feeds.
layout(std140, set = 0, binding = 3) uniform SplatUBO {
    vec4 uSplat;  // x=buried, y=coreD2, z=quad extent, w=debug mode
    vec4 uSplat2; // x=radius scale (hotkeys [/]), yzw=spare
} sp;

layout(location = 0) in vec3 vCenter;
layout(location = 1) in vec3 vT;
layout(location = 2) in vec3 vB;
layout(location = 3) in vec3 vN;
layout(location = 4) in vec2 vRadii;
layout(location = 5) in vec4 vMat;   // mat, refl, rough, aoB(+2 if water)
layout(location = 6) in vec3 vView;  // interpolated ray (cornerWorld - camPos)
layout(location = 7) in float vFace; // dot(n, camPos-c): <0 would-collapse
layout(location = 8) in vec4 vShade; // xyz = baked bent normal, w = baked shadow

layout(location = 0) out vec4 oHdr;
layout(location = 1) out vec4 oGPos;

layout(constant_id = 0) const int SKY_MODE = 0;
// Pass split for correct translucent compositing (opaque cores must settle
// depth/color before rims blend, so they are separate draws):
//   0 = full disk (water path, single pass),
//   1 = core only  (d2 <= coreD2; writes depth),
//   2 = rim only   (d2 > coreD2; depth-tested, no depth write, blends over).
layout(constant_id = 1) const int PASS_MODE = 0;

const float kWaterLevel = -0.9;

#include "common_base.glsl"
#include "common_splat.glsl"

void main()
{
    kSunDir = normalize(pc.sunDir.xyz);
    gRenderFlags = int(pc.misc.x + 0.5);
    vec3 ro = pc.camPos.xyz;

    if (SKY_MODE == 1) {
        // fullscreen sky: vView carries the per-vertex view direction
        vec3 rd = normalize(vView);
        vec3 col = skyColor(rd);
        bool uw = ((gRenderFlags & 8) != 0) && ro.y < kWaterLevel && underWater(ro.xz);
        gUnderwater = uw;
        if (uw) {
            // camera submerged staring at sky: tint like the SVO miss branch
            float t0, t1;
            float tU = 60.0;
            if (rayAABB(ro, 1.0 / rd, t0, t1)) {
                float tExit = (rd.y > 0.001) ? (kWaterLevel - ro.y) / rd.y : t1;
                tU = max(0.0, min(tExit, t1));
            }
            col *= exp(-tU * vec3(0.35, 0.18, 0.12) * 3.0);
            col = mix(col, vec3(0.05, 0.14, 0.13), clamp(tU * 0.8, 0.0, 0.85));
        }
        oHdr = vec4(col, 1.0);
        oGPos = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    gUnderwater = ((gRenderFlags & 8) != 0) && ro.y < kWaterLevel && underWater(ro.xz);

    vec3 rd = normalize(vView);
    vec3 n = normalize(vN);
    // two-sided foliage + roof shells (VS never collapses mat 7/8 there):
    // light the visible side so undersides close the silhouette
    if (vFace < 0.0 && (abs(vMat.x - 8.0) < 0.5 || abs(vMat.x - 7.0) < 0.5))
        n = -n;
    float denom = dot(rd, n);
    if (abs(denom) < 1e-6)
        discard;
    float t = dot(vCenter - ro, n) / denom;
    if (t <= 0.0)
        discard;
    vec3 q = ro + rd * t;

    float extent = max(sp.uSplat.z, 1.0);
    float u = dot(q - vCenter, vT) / max(vRadii.x, 1e-5);
    float v = dot(q - vCenter, vB) / max(vRadii.y, 1e-5);
    float d2 = u * u + v * v;
    if (d2 > extent * extent)
        discard;

    // kernel: opaque core + true Gaussian rim. The core (d2 < coreD2)
    // keeps the surface watertight - with 0.1 m cells and r = 0.11 the
    // corner sits at d2 = 0.41, so it is always inside the core and no
    // background can leak through the surface interior. Outside the core
    // the falloff is Gaussian (matched to 1 at the boundary), giving the
    // soft blurred silhouette edges; back-to-front chunk order makes the
    // rim blend against the surface/sky correctly. Far away the core
    // expands to the full disk (sub-pixel disks would otherwise wash out).
    float coreD2 = mix(sp.uSplat.y, 1.0, smoothstep(10.0, 40.0, t));
    if (PASS_MODE == 1 && d2 > coreD2)
        discard;
    if (PASS_MODE == 2 && d2 <= coreD2)
        discard;
    if (PASS_MODE == 3) {
        if (d2 > coreD2) discard;
        // depth-only prepass: replicate the core pass depth exactly
        // so the Hi-Z pyramid sees the same depth the core pass writes.
        // No shading, no colour output.
        float fragDepth = 1.0 - exp(-t * 0.02);
        fragDepth = floor(fragDepth * 100000.0 + 0.5) / 100000.0;
        float winBias = 0.0;
        bool isWater = vMat.w > 1.5;
        if (!isWater) {
            float shB = ((gRenderFlags & 2) != 0 && dot(n, kSunDir) > 0.02) ? vShade.w : 1.0;
            uint mId = uint(vMat.x + 0.5);
            winBias = (1.0 - shB) * 5e-5 + float(mId) * 3e-6;
        }
        fragDepth = max(fragDepth - (5e-5 + winBias), 0.0);
        gl_FragDepth = fragDepth;
        return;
    }
    float alpha;
    if (d2 < coreD2) {
        alpha = 1.0;
    } else {
        float rn = (d2 - coreD2) / max(1.0 - coreD2, 1e-3);
        alpha = exp(-4.0 * rn * rn);
    }
    if (alpha < 0.004)
        discard;

    bool isWater = vMat.w > 1.5;
    float aoBaked = vMat.w - (isWater ? 2.0 : 0.0);
    vec3 col;
    float hitType = 1.0;
    // depth-tie preference for the shadowed side (set in the opaque branch):
    // at same-surface overlaps the shadowed disk deterministically wins core
    // ties, so borders can't shimmer between lit/dark winners with the view.
    float winBias = 0.0;
    float dbg = sp.uSplat.w;
    if (dbg > 0.5) {
        // debug views (flat, no lighting)
        if (dbg > 13.5) {
            // rim mask: white = rim fragment (d2 > core), grey = core.
            // Shows the boundary web directly.
            col = (d2 > coreD2) ? vec3(1.0) : vec3(0.35);
            alpha = 1.0;
        } else if (dbg > 12.5) {
            vec3 roDbg = q + normalize(vN) * 0.35;
            vec3 spDbg = roDbg + kSunDir * 0.05;
            float sDbg = spDbg.y - heightAt(spDbg.xz);
            col = vec3(clamp(sDbg * 2.0, 0.0, 1.0), clamp(0.6 - abs(sDbg - 0.4) * 2.0, 0.0, 1.0), clamp(-sDbg * 4.0 + 0.6, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 11.5) {
            // heightfield residual at the hit: green = hit ~0.05 above the
            // heightfield (correct), red/blue = mismatch (texture misbound)
            float hd = heightAt(q.xz) - q.y;
            col = vec3(clamp(hd * 2.0 + 0.5, 0.0, 1.0), clamp(0.5 - abs(hd + 0.05) * 4.0, 0.0, 1.0), clamp(-hd * 2.0 + 0.5, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 10.5) {
            float resDbg = 1.0; float tDbg = 0.05;
            vec3 roDbg = q + normalize(vN) * 0.35;
            for (int i = 0; i < 28; ++i) {
                vec3 spDbg = roDbg + kSunDir * tDbg;
                float sDbg = spDbg.y - heightAt(spDbg.xz);
                resDbg = min(resDbg, 9.0 * max(sDbg, pc.b.y * 0.45) / tDbg);
                tDbg += clamp(max(sDbg, pc.b.y * 0.35) * 0.85, 0.05, 1.2);
                if (resDbg < 0.004 || tDbg > 40.0) break;
            }
            col = vec3(clamp(resDbg, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 9.5) {
            // object-volume distance at the hit (magenta = no info / far)
            float soDbg = objDist(q);
            col = (soDbg >= kObjVolMax * 0.99) ? vec3(1.0, 0.0, 1.0)
                                               : vec3(clamp(soDbg * 0.5 + 0.5, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 8.5) {
            // baked AO as grey
            col = vec3(clamp(vMat.w > 1.5 ? vMat.w - 2.0 : vMat.w, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 7.5) {
            // baked shadow factor as grey
            col = vec3(clamp(vShade.w, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 6.5) {
            // roughness as grey
            col = vec3(clamp(vMat.z / 255.0, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 5.5) {
            // albedo as-is
            uint mDbg = uint(vMat.x + 0.5);
            col = kPalette[clamp(mDbg, 0u, 16u)];
            alpha = 1.0;
        } else if (dbg > 4.5) {
            // collapse visualization: RED = would-collapse (facing<0)
            col = (vFace < 0.0) ? vec3(1.0, 0.1, 0.1) : vec3(0.1, 1.0, 0.1);
            alpha = 1.0;
        } else if (dbg < 1.5) {
            col = vec3(0.8, 0.8, 0.8);
            alpha = 1.0;
        } else if (dbg < 2.5) {
            col = normalize(vN) * 0.5 + 0.5;
            alpha = 1.0;
        } else {
            float dd = clamp(t * 0.03, 0.0, 1.0);
            col = mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), dd);
            alpha = 1.0;
        }
    } else if (isWater) {
        // PLANAR water: intersect the global water plane, never the disk.
        // Every water fragment on the plane then shades identically no
        // matter which disk covered it (reflection/shadow marches restart
        // from the same point), so adjacent splats are indistinguishable
        // and no disk borders can form. Disks remain pure coverage stamps.
        float tW = (kWaterLevel - ro.y) / rd.y;
        if (!(tW > 0.0))
            discard;
        vec3 pw = ro + rd * tW;
        float wa = 1.0;
        col = shadeWaterSplat(pw, rd, ro, tW, wa);
        // uniform absorption alpha (no kernel falloff): per-disk rim alpha
        // would mottle overlapping translucent layers; the d2 discard above
        // already clips coverage, TAA resolves the 1px shore step
        alpha = wa;
        hitType = 2.0;
        t = tW;
        q = pw;
    } else {
        uint mId = uint(vMat.x + 0.5);
        vec3 alb = kPalette[clamp(mId, 0u, 16u)];
        vec2 rr = vec2(vMat.y, vMat.z) / 255.0;
        // baked sun shadow + bent AO (built from the exact oracle); the
        // render-flag gates stay live so keys 1/2 keep working
        float shB = ((gRenderFlags & 2) != 0 && dot(n, kSunDir) > 0.02) ? vShade.w : 1.0;
        // Deterministic overlap winner: shadowed side first, then higher
        // material id — same pair always resolves the same way regardless of
        // viewpoint, so adjacent surfels of different shadow/color can't
        // shimmer between winners. Steps (5e-5 / 3e-6) dwarf float jitter
        // (~1e-7) but stay mm-scale in t, far below real occlusions.
        winBias = (1.0 - shB) * 5e-5 + float(mId) * 3e-6;
        float aoB = ((gRenderFlags & 1) != 0) ? clamp(aoBaked, 0.0, 1.0) : 1.0;
        vec3 bentB = normalize(vShade.xyz);
        col = shadeSurfel(q, rd, alb, rr, ro, n, bentB, shB, aoB, mId);
        float fog = 1.0 - exp(-t * 0.0016);
        vec3 fc = fogColor(rd, q);
        // valley mist over the stream + damp hollows (reference river mood)
        float mist = mistFactor(ro, q, t);
        fc = mix(fc, fc * vec3(1.04, 0.99, 0.94) + vec3(0.02, 0.015, 0.01), mist);
        col = mix(col, fc, clamp(fog + mist * 0.45, 0.0, 1.0));
    }

    // underwater volumetric when the camera itself is submerged
    if (gUnderwater) {
        col *= exp(-t * vec3(0.35, 0.18, 0.12) * 3.0);
        col = mix(col, vec3(0.05, 0.14, 0.13), clamp(t * 0.8, 0.0, 0.85));
    }

    // Core depth bias: cores settle slightly toward the camera so same-surface
    // overlap rims — whose plane depths equal the settled depth up to float
    // jitter — deterministically FAIL the rim strict-LESS test instead of
    // flickering on an LEQUAL coin-flip with subpixel camera moves. 5e-5 depth
    // units is mm-scale in t (far below real silhouette gaps, far above
    // jitter); water writes no depth so only cores are biased. winBias adds
    // a shadow-side-first, higher-material-id-first preference so core ties
    // between different shadow/color neighbours also resolve deterministically.
    float fragDepth = 1.0 - exp(-t * 0.02);
    // quantize depth to 1e-5 units (≈0.5 mm in t): near-tie cores
    // (same-mat neighbours agreeing to fp jitter) resolve identically in
    // every backend (same quantum -> first-in-order wins), so settled
    // depths — and hence rim depth tests — agree bit-for-bit. The quantum
    // dwarfs jitter (~1e-7) but is far below visible relief.
    fragDepth = floor(fragDepth * 100000.0 + 0.5) / 100000.0;
    if (PASS_MODE == 1)
        fragDepth = max(fragDepth - (5e-5 + winBias), 0.0);
    gl_FragDepth = fragDepth;
    // rim passes (PASS_MODE 2, MAX blend) output straight color: MAX takes
    // the brighter of src/dst, so premultiplying would dim rims into dark
    // fringes. Core/water keep premultiplied (their alpha is 1 / absorptive).
    oHdr = (PASS_MODE == 2) ? vec4(col, alpha) : vec4(col * alpha, alpha);
    oGPos = vec4(q, hitType);
}
