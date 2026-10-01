#version 460
#extension GL_GOOGLE_include_directive : require
#define SPLAT_BACKEND 1
#include "common_surfel.glsl"
// Voxelforge Gaussian-surfel rasterizer (fragment stage).
// Exact ray/disk intersection gives per-fragment plane depth (no centroid
// z-fighting); each surfel contributes a single pure 2D Gaussian kernel
// (alpha = opacity * exp(-0.5*d2/sigma2)) whose peak sits at the disk centre.
// Back-to-front order + source-over blending accumulates overlapping disks
// into a solid, filled surface with soft 3DGS-style silhouette edges - no
// hard opaque core, no hollow rims. Shading is shadeSurfel() from
// common_splat.glsl - shadeTerrain's twin.

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

// per-frame kernel tuning (HUD): y = Gaussian variance sigma2, z = quad
// extent, w = debug mode. Persistently mapped UBO, flushed by
// SplatPass::record like the SVO highlight feeds.
layout(std140, set = 0, binding = 3) uniform SplatUBO {
    vec4 uSplat;  // x=buried, y=sigma2, z=quad extent, w=debug mode
    vec4 uSplat2; // x=radius scale (hotkeys [/]), y=opacity, z=depth tol, w=spare
} sp;

// Brush hover preview (bind 13): the app fills this with the active edit
// brush volume so the splats the next stamp would affect can be tinted
// before the click. The volume is the oriented carve cylinder (w > 0 on the
// axis), the delete/paint ball (w == 0) or the add dome (w < 0, |w| = the
// dome height along the axis -> how far the surface grows). a = 0 disables the preview. Water-plane splats
// are skipped at the tint site (their flag, not a height test: the plane is
// one fixed level a carve exposes instead of removes).
layout(std140, set = 0, binding = 13) uniform BrushUBO {
    vec4 bVolume; // xyz = centre (world), w = radius m
    vec4 bAxis;   // xyz = unit axis, w = half length m (0 = ball, <0 = dome)
    vec4 bTint;   // rgb = tint colour, a = strength
    vec4 bMeta;   // x = optional owning layer ID; 0 = spatial volume only
} uBrush;

layout(location = 0) in vec3 vCenter;
layout(location = 1) in vec3 vT;
layout(location = 2) in vec3 vB;
layout(location = 3) in vec3 vN;
layout(location = 4) in vec2 vRadii;
layout(location = 5) in vec4 vMat;   // mat, refl, rough, aoB(+2 if water)
layout(location = 6) in vec3 vView;  // interpolated ray (cornerWorld - camPos)
layout(location = 7) in float vFace; // dot(n, camPos-c): <0 would-collapse
layout(location = 8) in vec4 vShade; // xyz = baked bent normal, w = baked shadow
layout(location = 9) in float vTex;  // per-surfel texture override (0 = material slot)

layout(location = 0) out vec4 oHdr;
layout(location = 1) out vec4 oGPos;
layout(location = 2) out vec4 oGNorm; // shading normal (xyz), 0 on sky

layout(constant_id = 0) const int SKY_MODE = 0;
// Pass split for the depth-resolved composite:
//   0 = water path (full disk, planar water shading, uniform alpha),
//   1 = opaque Gaussian band (full disk; straight colour + alpha),
//   3 = depth-only prepass (nearest full-disk plane depth, no shading),
//   4 = opaque base (exact nearest fragment, straight colour, alpha 1).
layout(constant_id = 1) const int PASS_MODE = 0;

const float kWaterLevel = -0.9;

// Brush hover preview volume test (bind 13): is `p` inside the brush volume?
// The shape lives in common_surfel.glsl so the SVO reference backend tints the
// same volume - it had no preview at all until then. See that header for the
// ball / oriented cylinder / dome / box cases.
bool inBrushVolume(vec3 p, float packedAo)
{
    return sharedInBrushVolume(p, packedAo, uBrush.bVolume, uBrush.bAxis,
                               uBrush.bTint, uBrush.bMeta, pc.b.y);
}

#include "common_base.glsl"
#include "common_splat.glsl"

void main()
{
    kSunDir = normalize(pc.sunDir.xyz);
    gRenderFlags = int(pc.misc.x + 0.5);
    gTexOv = vTex; // per-surfel override; 0 = use the material's atlas slot
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
        oGNorm = vec4(0.0, 0.0, 0.0, 0.0);
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

    // Pure Gaussian alpha (3DGS-style): a single continuous kernel peaking at
    // the disk centre, clipped at the quad extent. With back-to-front source-
    // over blending, overlapping disks (r = 1.4 cells on a 1-cell grid) sum to
    // a filled, watertight-looking surface without any opaque core - the old
    // step-function core + Gaussian rim left hollow centres wherever a binary
    // core missed the fixed pixel grid. sigma2 sharpens/softens the kernel,
    // opacity caps the centre alpha so neighbours can still accumulate over it.
    const float sigma2 = max(sp.uSplat.y, 1e-3);
    float alpha = sp.uSplat2.y * exp(-0.5 * d2 / sigma2);
    if (PASS_MODE == 3) {
        // depth-only prepass: the nearest full-disk plane depth (no core
        // threshold) seeds both the Hi-Z occlusion pyramid and the water
        // depth test. The fixed-function depth from gl_Position is the
        // surfel-plane depth at this pixel; both are written by the same
        // hardware interpolation, so the base pass's EQUAL test stays
        // bit-exact while early-Z rejects occluded fragments (no
        // gl_FragDepth write).
        return;
    }
    // Debug views override alpha to 1.0 below, so skip the alpha discard
    // there (full-coverage flat views).
    if (PASS_MODE == 1 && sp.uSplat.w < 0.5 && alpha < 0.004)
        discard;

    bool isWater = surfelIsWater(vMat.w);
    float aoBaked = surfelBakedAo(vMat.w);
    vec3 col;
    float hitType = 1.0;
    float dbg = sp.uSplat.w;
    if (dbg > 0.5) {
        // debug views (flat, no lighting)
        if (dbg > 15.5) {
            // Ownership mask. With a rotate target, GREEN is the exact selected
            // layer, RED is another object layer, and DARK BLUE is terrain/water.
            const bool object = surfelIsObject(vMat.w);
            const uint target = uint(max(0.0, uBrush.bMeta.x));
            const bool selected = target > 0u && surfelBelongsToLayer(vMat.w, target);
            col = selected ? vec3(0.1, 1.0, 0.2)
                 : (object ? (target > 0u ? vec3(0.8, 0.08, 0.05)
                                          : vec3(0.1, 1.0, 0.2))
                           : vec3(0.02, 0.05, 0.3));
            alpha = 1.0;
        } else if (dbg > 14.5) {
            // brush mask: magenta = splat centre inside the edit-brush volume
            col = inBrushVolume(vCenter, vMat.w) ? vec3(1.0, 0.0, 1.0) : vec3(0.06);
            alpha = 1.0;
        } else if (dbg > 13.5) {
            // kernel mask: white = outside one sigma (soft edge region),
            // grey = inside. Shows the Gaussian boundary web directly.
            col = (d2 > sigma2) ? vec3(1.0) : vec3(0.35);
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
            // baked AO as grey; layer ownership is metadata, not exposure
            col = vec3(clamp(surfelBakedAo(vMat.w), 0.0, 1.0));
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
            // vTex: per-surfel texture override as a heat ramp
            // (0 = none -> dark blue, 3 -> green, higher -> red/white)
            float tv = clamp(vTex / 8.0, 0.0, 1.0);
            col = mix(vec3(0.02, 0.05, 0.3), vec3(0.1, 1.0, 0.2), tv);
            col = mix(col, vec3(1.0, 0.9, 1.0), clamp((vTex - 4.0) / 4.0, 0.0, 1.0));
            alpha = 1.0;
        } else if (dbg > 3.5) {
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

    // Brush hover preview: tint the splats whose centre lies inside the
    // active edit-brush volume (carve cylinder / delete+paint ball) so the set
    // the LMB stamp would affect is visible before clicking. Testing the
    // surfel CENTRE (not this fragment) keeps the highlight per-splat and
    // matches the CPU rasterizer's cell membership; the app grows the volume
    // by a small skin so the surface cells' emitter offset stays inside.
    // Applied as a flat overlay after shading/fog so it reads on dark and
    // bright surfaces alike. The water plane is not a splat a stamp can
    // remove (a carve floods it instead), so water splats stay untinted.
    if (!isWater && inBrushVolume(vCenter, vMat.w))
        col = mix(col, uBrush.bTint.rgb, uBrush.bTint.a);

    // Depth resolve: the PASS_MODE 3 prepass holds the nearest full-disk
    // plane depth at this pixel (fixed-function gl_Position depth, no
    // shader write). The opaque band's LESS test uses a negative rasterizer
    // depth bias (-VF_SPLAT_DEPTH_TOL, applied via vkCmdSetDepthBias), so
    // only fragments within [nearest, nearest + tol] pass - the front
    // surface band. Fragments farther behind (a second surface within a
    // chunk, the shadowed side) are rejected by early-Z before shading
    // instead of source-over-ing in draw order, which caused view-dependent
    // dark speckle. No depth write: the prepass depth stays for the water
    // test.
    //
    // The Gaussian band alone can leave accumulated alpha < 1 at disk rims
    // and grazing surfaces, letting the sky (the initial colour target)
    // bleed through as pale fringes. The PASS_MODE 4 base pass fixes that:
    // it EQUAL-tests against the prepass depth (same hardware interpolation,
    // bit-exact), so exactly the nearest fragment at every covered pixel
    // writes an OPAQUE (alpha 1, no blend) surface colour. The band then
    // softens that base; the sky can never show through a resolved surface.
    // Neither pass writes gl_FragDepth, so both keep early-Z: only the
    // nearest fragment (base) and the front surface band (band) shade
    // instead of every overlapping disk fragment.
    // Band fragments contribute straight (non-premultiplied) Gaussian
    // colour + alpha (SRC_ALPHA / ONE_MINUS_SRC_ALPHA source-over); the base
    // fragment writes opaque colour; water stays premultiplied.
    if (PASS_MODE == 4)
        oHdr = vec4(col, 1.0);
    else
        oHdr = (PASS_MODE == 1) ? vec4(col, alpha) : vec4(col * alpha, alpha);
    oGPos = vec4(q, hitType);
    // Shading normal for the screen-space effects (SSAO/SSR). Water writes
    // the flat plane normal: SSR's water reflection must stay plane-stable
    // (the ripple normal is a shading-only detail, applied in-shader).
    oGNorm = vec4(isWater ? vec3(0.0, 1.0, 0.0) : n, 0.0);
}
