// Splats have no SVO behind them: shadow + AO march the records-derived
// heightfield and the coarse object volume instead. Same 9.0*s/t kernel as
// softShadowTerrain so the two backends agree on terrain shadowing.
float splatSceneDist(vec3 p)
{
    return min(p.y - heightAt(p.xz), objDist(p));
}

// Directional-light occlusion for splats: BINARY hit test like the SVO
// backend's exactSVOHit (softShadow), not a penumbra kernel. A 9*s/t kernel
// converts every sub-meter bump of the smoothed heightfield at range into
// grey wash (measured: uniform ~0.55-0.86 floor); binary matches the SVO
// reference (no TAA-visible penumbras in either backend). The -0.03 m
// tolerance absorbs bilinear-smoothing ripple so crests don't acne; contact
// grounding comes from splatAO, same split as the SVO backend.
float softShadowSplat(vec3 ro, vec3 rd)
{
    // PCF soft shadows for splats: 16-tap penumbra pattern.
    float shadow = 0.0;
    float t = 0.05;
    for (int i = 0; i < 16; ++i) {
        vec3 sp = ro + rd * t;
        float sHf = sp.y - heightAt(sp.xz);
        if (sHf < -0.03) { shadow += 1.0; t += 0.5; continue; }
        float sObj = objDist(sp);
        if (sObj < 1.2474 && sObj < -0.05) { shadow += 1.0; t += 0.5; continue; }
        shadow += 0.5;
        t += clamp(max(min(sHf, sObj), pc.b.y * 0.35) * 0.85, 0.05, 1.2);
        if (t > 20.0) break;
    }
    shadow /= 16.0;
    return clamp(1.0 - shadow, 0.0, 1.0);
}
// Few-tap bent-normal AO over the same combined distance field.
void splatAO(vec3 p, vec3 n, out float ao, out vec3 bent)
{
    const float rad[2] = float[2](0.18, 0.60);
    vec3 tv = cross(n, vec3(0.0001, 1.0, 0.0001));
    vec3 tang = normalize(tv + vec3(1e-5));
    vec3 bitan = normalize(cross(n, tang));
    float occ = 0.0, wsum = 0.0;
    vec3 bd = n * 0.5;
    for (int ring = 0; ring < 2; ++ring) {
        float r = rad[ring];
        for (int a = 0; a < 4; ++a) {
            float ang = float(a) * 1.5708 + float(ring) * 0.785;
            vec3 dir = normalize(n * 0.85 + (cos(ang) * tang + sin(ang) * bitan) * 0.7);
            float s = splatSceneDist(p + dir * r);
            float fall = clamp(1.0 - s / r, 0.0, 1.0);
            fall = fall * fall * (3.0 - 2.0 * fall);
            float w = 1.0 / (1.0 + fall * fall * 4.0);
            occ += w * fall;
            wsum += w;
            bd += dir * w * (1.0 - fall);
        }
    }
    ao = clamp(1.0 - 0.85 * occ / max(wsum, 1e-4), 0.0, 1.0);
    bent = normalize(bd + vec3(1e-6));
}

// Sky transmission along the bent normal: 1 = open sky, 0.05 = enclosed.
// OBJECT VOLUME ONLY, deliberately: the splat backend has no SVO, and the
// heightfield says "solid" for ANY point below the terrain surface - which
// makes every room dug into a hillside (and every interior at all, since the
// smoothed height texture can sit above a floor) read as underground. That
// disagreed with the SVO reference by ~40 points of enclosed coverage. The
// object field knows overhangs and cavities; a heightfield cannot have them, so
// terrain can never occlude an upward ray and need not be tested.
float skyVisibilitySPlat(vec3 p, vec3 bent)
{
    // Terrain IS an occluder for an upward ray, and this is the one case the
    // object volume can never see: a cave carved into a hillside sits below
    // its own heightfield column, with no object present at all. Only the
    // START column is tested - a heightfield has no overhangs, so terrain can
    // never hide a ray that has already left the surface, and testing p alone
    // keeps an outdoor point (p.y == heightAt) from being called buried. The
    // 0.35 m clearance absorbs half-voxel quantisation plus the surfel's
    // n*(0.5*VOXEL) offset on a slope; a real cave roof is metres above.
    // Without this the whole product below was 0 for a terrain cave (the
    // caller also forces sh=1 on every backfacing wall), and a carved
    // interior rendered 45.26 mean luma with enclosure ON vs 45.84 OFF.
    if (heightAt(p.xz) - p.y > 0.35)
        return 0.05;

    vec3 dir = normalize(mix(bent, vec3(0.0, 1.0, 0.0), 0.65));
    float t = 0.25;
    for (int i = 0; i < 16; ++i) {
        float sObj = objDist(p + dir * t);
        if (sObj < 1.2474 && sObj < -0.05)
            return 0.05;
        t += clamp(max(sObj, pc.b.y * 0.35) * 0.85, 0.05, 1.2);
        if (t > 24.0)
            return 1.0;
    }
    return 1.0;
}

// Per-light occlusion for the splat backend. Terrain IS included here (unlike
// the sky test): a lamp behind a hill must not light the far side. The march is
// bounded by the coarser of the two fields, as in softShadowSplat.
float lightVisibilitySPlat(vec3 ro, vec3 rd, float maxDist)
{
    vec3 d = normalize(rd);
    // A lamp INSIDE a room or cave cut into a hillside has BOTH endpoints
    // below the heightfield column, and the heightfield can only answer
    // "solid" for both - so the very first tap returned 0 and the light never
    // reached its own room (measured: an interior lamp moved the splat cave
    // frame by 0.00 mean luma, while the SVO backend, which traverses the real
    // carved geometry, moved it by 0.51). A heightfield cannot have overhangs,
    // so a segment that never crosses the surface cannot be crossing terrain
    // either: skipping the terrain tap while BOTH ends stay buried is sound.
    // The moment either end is above ground the normal test resumes, so a lamp
    // behind a hill still does not light the far side. Known limit: two sealed
    // caves in the same hill are treated as connected.
    const vec3 end = ro + d * maxDist;
    const bool buriedBoth = (heightAt(ro.xz) - ro.y > 0.05) &&
                            (heightAt(end.xz) - end.y > 0.05);
    float t = 0.05;
    for (int i = 0; i < 16; ++i) {
        vec3 sp = ro + d * t;
        float sHf = sp.y - heightAt(sp.xz);
        if (!buriedBoth && sHf < -0.03)
            return 0.0;
        float sObj = objDist(sp);
        if (sObj < 1.2474 && sObj < -0.05)
            return 0.0;
        // Underground, sHf is a large negative and would collapse the step to
        // ~a third of a voxel, so the march never reaches the lamp; advance on
        // the object field alone instead.
        t += buriedBoth ? max(sObj, pc.b.y * 0.35)
                        : clamp(max(min(sHf, sObj), pc.b.y * 0.35) * 0.85, 0.05, 1.2);
        if (t >= maxDist)
            return 1.0;
    }
    return 1.0;
}

// Two-scale analytic heightfield normal (same formula as calcNormal's
// terrain branch): used for reflected beds and water where no SVO exists.
vec3 splatHeightNormal(vec3 p)
{
    float e = 0.35;
    vec3 nWide = normalize(vec3(heightAt(p.xz - vec2(e, 0)) - heightAt(p.xz + vec2(e, 0)),
                                2.0 * e,
                                heightAt(p.xz - vec2(0, e)) - heightAt(p.xz + vec2(0, e))));
    float e2 = 0.10;
    vec3 nFine = normalize(vec3(heightAt(p.xz - vec2(e2, 0)) - heightAt(p.xz + vec2(e2, 0)),
                                2.0 * e2,
                                heightAt(p.xz - vec2(0, e2)) - heightAt(p.xz + vec2(0, e2))));
    return normalize(mix(nFine, nWide, 0.55));
}

// Surfel shading: shadeTerrain's exact twin, but the normal comes from the
// surfel (already smooth) and sun shadow + bent AO are baked per-surfel on
// the CPU from the exact oracle (zero march cost per fragment); the flora
// shadow re-evaluation reuses the baked value.
vec3 shadeSurfel(vec3 p, vec3 rd, vec3 alb, vec2 rr, vec3 ro, vec3 n,
                 vec3 bent, float sh, float ao, uint mId, float shRaw)
{
    alb = detailAlbedo(alb, p, n, mId);
    n = detailNormal(n, p, mId);
    applyFlora(p, rd, ro, mId, true, sh, alb, n, sh, ao);

    float ndl = max(dot(n, kSunDir), 0.0);
    // Bit 8 = enclosed-space sky occlusion: 0 = open sky, 1 = fully enclosed.
    // Daylight ambient is scaled down hard in enclosed space, and a dim,
    // albedo-scaled fill replaces it so cave rock stays readable instead of
    // collapsing to black (authored lights add on top of that).
    float enclosed = 1.0 - ((gRenderFlags & 256) != 0
                            ? skyVisibilitySPlat(p, bent) : 1.0);
    // A carve brush cuts the cave out of TERRAIN, which the object volume
    // cannot see. The baked sun shadow does know about it, so use it here -
    // but the RAW one: the caller gates `sh` to 1.0 whenever the surfel faces
    // away from the sun (`dot(n, kSunDir) > 0.02`), and that gate fires on
    // almost every wall of a cave, which silently pinned this product to 0.
    // Measured: a carved interior rendered 45.26 mean luma with enclosure ON
    // and 45.84 with it OFF, because `sh` was 1 exactly where it was needed.
    enclosed = max(enclosed, aoShEnclosure(ao, shRaw));
    float skyAmb = mix(1.0, 0.16, enclosed);
    float skyIbl = mix(1.0, 0.04, enclosed);

    vec3 amb = skyIrradiance(bent) * (0.28 + 0.30 * ao) * skyAmb;
    // Moon takes over from the sun at night; scaled by skyAmb so a cave still
    // gets almost none of it (the lamp inside is what should read).
    amb += moonLight(n, ao) * skyAmb;
    float fold = clamp(-n.y, 0.0, 1.0);
    // Indirect term. The irradiance volume's RGB is what ARRIVES here from the
    // light set, so an interior is tinted by its own emitters instead of by
    // the old fixed warm constant, and - unlike that stand-in - it applies at
    // every orientation: the volume carries no direction, and folding it to
    // down-facing would drop the FLOORS, which are the main receivers indoors.
    // The .a sky channel is deliberately NOT added here: daylight ambient
    // stays skyIrradiance()'s job above, so the sky term cannot double-count.
    // The zero test is the "term absent" path (never uploaded, VF_NO_IRR_VOLUME
    // or no emitters at all): it reads 0 and falls back to the old stand-in
    // unchanged, which is what keeps that A/B bit-exact. Known trade-off, in
    // case it ever shows on screen: a down-facing surface just OUTSIDE a
    // lamp's radius keeps the old glow while just INSIDE it reads ~0 from the
    // volume, so a ceiling halo would mean "drop the fallback for pure
    // replace", not "turn the gain down".
    vec3 irr = irradianceVolume(p);
    vec3 bounce = any(greaterThan(irr, vec3(0.0)))
                      ? alb * irr * (0.3 + 0.7 * ao) * skyAmb
                      : (alb * 0.65 + vec3(0.10, 0.09, 0.07)) * fold *
                            (0.3 + 0.7 * ao) * skyAmb;

    vec3 V = -rd;
    vec3 h = normalize(kSunDir + V);
    float vdh = max(dot(V, h), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    vec3 f0 = vec3(0.04) + vec3(rr.x) * 0.70;
    vec3 F = fresnelSchlick(f0, vdh);
    float fAvg = (F.r + F.g + F.b) * 0.3333;
    vec3 spec = pbrSpec(n, V, kSunDir, f0, rough);

vec3 col = alb * (1.0 - fAvg) * (sunCol() * ndl * sh + amb + bounce)
              + spec * sunCol() * ndl * sh
              + spec * alb * 0.30 * sunCol() * sh; // albedo-scale multi-scatter compensation

    // IBL: image-based lighting for realistic ambient fill
    vec3 ibl = iblContribution(n, V, f0, rough) * (0.28 + 0.30 * ao) * skyIbl;
    col += ibl;

    // Enclosed-space fill: a dim, albedo-scaled bounce standing in for the
    // multi-bounce a cave wall would actually return. Without it, removing the
    // daylight ambient just pushes cave surfaces under the black-pixel gate.
    col += alb * vec3(0.075, 0.080, 0.090) * enclosed * (0.35 + 0.65 * ao);

    // backlit foliage translucency (canopy scatters light through leaves)
    if ((gRenderFlags & 4) != 0 && mId == 8u) {
        float back = pow(1.0 - max(dot(n, kSunDir), 0.0), 2.0);
        float thick = clamp(0.5 - splatSceneDist(p + n * 0.3) * 2.0, 0.0, 1.0);
        col += alb * sunCol() * back * (1.0 - thick * 0.85) * 0.38;
    }

    // Subsurface scattering for foliage
    if ((gRenderFlags & 4) != 0 && mId == 8u) {
        float thickness = clamp(0.5 - splatSceneDist(p + n * 0.3) * 2.0, 0.0, 1.0);
        float sss = pow(max(1.0 - dot(-rd, n), 0.0), 2.0) * thickness;
        vec3 sssColor = vec3(0.4, 0.7, 0.2) * sss * 0.40;
        col += sssColor * sunCol() * ndl * sh;
    }

    if (p.y < kWaterLevel && underWater(p.xz) && !gUnderwater) {
        float depth = kWaterLevel - p.y;
        col *= exp(-depth * vec3(0.35, 0.18, 0.12) * 3.0);
        col = mix(col, vec3(0.05, 0.14, 0.13), clamp(depth * 0.8, 0.0, 0.85));
        // refracted-sun caustics on the bed: the brightest thing in shallow
        // water, and the depth cue that tells the eye the water is moving.
        // Fades fast with depth (the focus is lost) and follows the sun.
        col += sunCol() * causticAt(p.xz, pc.misc.y) * ndl * sh
               * 0.25 * exp(-depth * 0.55);
    }
    col += applyLights(p, n, V, rr, alb);
    col += emissiveTerm(p, n, mId, alb);
    return col;
}

// Submerged bed as seen through / reflected by water: shadeFloor's twin
// with the analytic heightfield normal (no SVO behind splats).
vec3 shadeFloorSplat(vec3 q, vec3 r)
{
    uint mId = heightMatNearest(q.xz);
    vec3 n = splatHeightNormal(q);
    float sh = ((gRenderFlags & 2) != 0) ? softShadowSplat(q + n * 0.3, kSunDir) : 1.0;
    float ndl = max(dot(n, kSunDir), 0.0);
    vec3 alb = kPalette[mId];
    vec2 rr = kMatRefl[mId];
    alb = detailAlbedo(alb, q, n, mId);
    n = detailNormal(n, q, mId);
    vec3 V = -r;
    vec3 h = normalize(kSunDir + V);
    float vdh = max(dot(V, h), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    vec3 f0 = vec3(0.04) + vec3(rr.x) * 0.70;
    vec3 F = fresnelSchlick(f0, vdh);
    float fAvg = (F.r + F.g + F.b) * 0.3333;
    vec3 spec = pbrSpec(n, V, kSunDir, f0, rough);
    vec3 amb = skyIrradiance(n) * 0.5 + moonLight(n, 1.0);
    vec3 ibl = iblContribution(n, V, f0, rough) * 0.5;
    vec3 col = alb * (1.0 - fAvg) * (sunCol() * ndl * sh + amb + ibl)
         + spec * sunCol() * ndl * sh;
    col += emissiveTerm(q, n, mId, alb);
    return col;
}

// March a reflected ray against the heightfield; returns bed hit distance
// or -1. Water reflections only ever show terrain (objects poke through the
// plane and are handled by depth-tested splats in front).
float splatReflectBed(vec3 pw, vec3 R)
{
    float t = 0.05;
    for (int i = 0; i < 24; ++i) {
        vec3 sp = pw + R * t;
        float s = sp.y - heightAt(sp.xz);
        if (s < 0.02)
            return t;
        t += clamp(s * 0.85, 0.05, 1.2);
        if (t > 60.0)
            break;
    }
    return -1.0;
}

// Animated ripple normal shared by every water path (same formula as the
// SVO main's inline block) plus a fine chop layer for sun glitter.
vec3 waterRippleN(vec3 p)
{
    float r1 = sin(p.x*4.5 + pc.misc.y*0.45)*0.5 + sin(p.z*6.0 - p.x*2.5)*0.5;
    float r2 = sin(p.x*9.5 + pc.misc.y*0.8 + p.z*3.5)*0.3 + sin(p.z*13.0 - p.x*6.0 + pc.misc.y*0.3)*0.3;
    float c1 = sin(p.x*23.0 + p.z*19.0 + pc.misc.y*1.1) * 0.5 + sin(p.z*31.0 - p.x*17.0) * 0.5;
    return normalize(vec3(r1*0.045 + r2*0.05 + c1*0.018, 1.0, sin(p.x*7.0 - p.z*11.0)*0.06 + cos(p.x*11.0 + p.z*5.0)*0.045 + c1*0.014));
}

// Full water shading for a water-surfel hit (mirrors the SVO waterView
// branch; reflections march the heightfield instead of the SVO).
// Returns rgb; alpha (absorption) via outA.
vec3 shadeWaterSplat(vec3 pw, vec3 rd, vec3 ro, float tWater, out float outA)
{
    vec3 n = waterRippleN(pw);
    bool fromBelow = ro.y < kWaterLevel;
    vec3 nFace = fromBelow ? -n : n;
    float ndv = max(dot(-rd, nFace), 0.0);
    float fres = 0.02 + 0.98*pow(1.0 - ndv, 5.0);
    // glitter lobe: tight sun path + broader sparkle halo for golden hour
    float reflSun = max(dot(reflect(rd, nFace), kSunDir), 0.0);
    float sunGlint = pow(reflSun, 700.0) * 5.0 + pow(reflSun, 90.0) * 0.55;
    float wsh = 1.0; // water is always lit

    vec3 col;
    if (fromBelow) {
        vec3 R = reflect(rd, nFace);
        vec3 rc = skyColor(vec3(R.x, -abs(R.y), R.z));
        float rth = splatReflectBed(pw, R);
        if (rth > 0.0)
            rc = shadeFloorSplat(pw + R * rth, R);
        vec3 skyT = skyColor(vec3(rd.x, abs(rd.y), rd.z));
        skyT *= vec3(0.55, 0.80, 0.85);
        col = mix(skyT, rc * mix(vec3(0.45,0.55,0.55), vec3(1.0), wsh), fres)
              + sunCol() * sunGlint * wsh;
    } else {
        vec3 R = reflect(rd, n);
        R.y = abs(R.y);
        vec3 rc = skyColor(R);
        float rth = splatReflectBed(pw, R);
        if (rth > 0.0)
            rc = shadeFloorSplat(pw + R * rth, R);
        col = mix(vec3(0.04,0.10,0.09), rc * mix(vec3(0.45,0.55,0.55), vec3(1.0), wsh), clamp(fres+0.25,0.0,1.0))
              + sunCol() * sunGlint * wsh;
    }
    // clear-water pebbles: sparkle the shallow bed like the reference stream
    float peb = vnoise(pw.xz * 28.0) * 0.6 + vnoise(pw.xz * 9.0) * 0.4;
    col *= 0.92 + 0.16 * peb;
    // shoreline foam: thin streaks hugging the waterline
    float bedH = heightAt(pw.xz);
    float shoreF = 1.0 - smoothstep(0.02, 0.26, kWaterLevel - bedH);
    float fp = vnoise(pw.xz * 3.7 + vec2(pc.misc.y * 0.35, -pc.misc.y * 0.22));
    fp = smoothstep(0.66, 0.90, fp + 0.28 * shoreF - 0.20);
    col = mix(col, vec3(0.92, 0.94, 0.95), fp * shoreF * 0.50);
    // refracted-sun caustic web, applied AT THE SURFACE: the light path is
    // sun -> surface -> bed -> back up, but the bed itself is hidden behind
    // the absorption alpha (0.35 + depth*0.9 saturates at ~0.7 m), so the web
    // has to be composited here to be visible at all. Scaled by shallowness
    // so deep water stays dark; this is the depth cue that says "moving water".
    col += sunCol() * causticAt(pw.xz, pc.misc.y) * 0.85 * exp(-max(kWaterLevel - bedH, 0.0) * 0.45);
    col = mix(col, fogColor(rd, pw), 1.0-exp(-tWater*0.004));
    // depth absorption -> alpha: shallow water shows the bed splat behind
    float depth = max(kWaterLevel - bedH, 0.0);
    outA = clamp(0.35 + depth * 0.9, 0.0, 0.97);
    return col;
}