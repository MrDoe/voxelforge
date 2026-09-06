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
                 vec3 bent, float sh, float ao, uint mId)
{
    alb = detailAlbedo(alb, p, n, mId);
    applyFlora(p, rd, ro, mId, true, sh, alb, n, sh, ao);

    float ndl = max(dot(n, kSunDir), 0.0);

    vec3 amb = skyIrradiance(bent) * (0.28 + 0.30 * ao);
    float fold = clamp(-n.y, 0.0, 1.0);
    vec3 bounce = (alb * 0.65 + vec3(0.10, 0.09, 0.07)) * fold * (0.3 + 0.7 * ao);

    vec3 V = -rd;
    vec3 h = normalize(kSunDir + V);
    float vdh = max(dot(V, h), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    vec3 f0 = vec3(0.04) + vec3(rr.x) * 0.70;
    vec3 F = fresnelSchlick(f0, vdh);
    float fAvg = (F.r + F.g + F.b) * 0.3333;
    vec3 spec = pbrSpec(n, V, kSunDir, f0, rough);

vec3 col = alb * (1.0 - fAvg) * (kSunCol * ndl * sh + amb + bounce)
              + spec * kSunCol * ndl * sh
              + spec * alb * 0.30 * kSunCol * sh; // albedo-scale multi-scatter compensation

    // IBL: image-based lighting for realistic ambient fill
    vec3 ibl = iblContribution(n, V, f0, rough) * (0.28 + 0.30 * ao);
    col += ibl;

    // backlit foliage translucency (canopy scatters light through leaves)
    if ((gRenderFlags & 4) != 0 && mId == 8u) {
        float back = pow(1.0 - max(dot(n, kSunDir), 0.0), 2.0);
        float thick = clamp(0.5 - splatSceneDist(p + n * 0.3) * 2.0, 0.0, 1.0);
        col += alb * kSunCol * back * (1.0 - thick * 0.85) * 0.38;
    }

    // Subsurface scattering for foliage
    if ((gRenderFlags & 4) != 0 && mId == 8u) {
        float thickness = clamp(0.5 - splatSceneDist(p + n * 0.3) * 2.0, 0.0, 1.0);
        float sss = pow(max(1.0 - dot(-rd, n), 0.0), 2.0) * thickness;
        vec3 sssColor = vec3(0.4, 0.7, 0.2) * sss * 0.40;
        col += sssColor * kSunCol * ndl * sh;
    }

    if (p.y < kWaterLevel && underWater(p.xz) && !gUnderwater) {
        float depth = kWaterLevel - p.y;
        col *= exp(-depth * vec3(0.35, 0.18, 0.12) * 3.0);
        col = mix(col, vec3(0.05, 0.14, 0.13), clamp(depth * 0.8, 0.0, 0.85));
    }
    col += (mId >= 9u && mId <= 15u) ? kEmissive[mId] : vec3(0.0);
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
    vec3 V = -r;
    vec3 h = normalize(kSunDir + V);
    float vdh = max(dot(V, h), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    vec3 f0 = vec3(0.04) + vec3(rr.x) * 0.70;
    vec3 F = fresnelSchlick(f0, vdh);
    float fAvg = (F.r + F.g + F.b) * 0.3333;
    vec3 spec = pbrSpec(n, V, kSunDir, f0, rough);
    vec3 amb = skyIrradiance(n) * 0.5;
    vec3 ibl = iblContribution(n, V, f0, rough) * 0.5;
    vec3 col = alb * (1.0 - fAvg) * (kSunCol * ndl * sh + amb + ibl)
         + spec * kSunCol * ndl * sh;
    col += (mId >= 9u && mId <= 15u) ? kEmissive[mId] : vec3(0.0);
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
              + kSunCol * sunGlint * wsh;
    } else {
        vec3 R = reflect(rd, n);
        R.y = abs(R.y);
        vec3 rc = skyColor(R);
        float rth = splatReflectBed(pw, R);
        if (rth > 0.0)
            rc = shadeFloorSplat(pw + R * rth, R);
        col = mix(vec3(0.04,0.10,0.09), rc * mix(vec3(0.45,0.55,0.55), vec3(1.0), wsh), clamp(fres+0.25,0.0,1.0))
              + kSunCol * sunGlint * wsh;
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
    col = mix(col, fogColor(rd, pw), 1.0-exp(-tWater*0.004));
    // depth absorption -> alpha: shallow water shows the bed splat behind
    float depth = max(kWaterLevel - bedH, 0.0);
    outA = clamp(0.35 + depth * 0.9, 0.0, 0.97);
    return col;
}