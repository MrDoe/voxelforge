int gRenderFlags = 31;

vec3 kSunDir = normalize(vec3(0.42, 0.78, 0.30)); // set from push in main()
const vec3 kSunCol = vec3(1.00, 0.95, 0.84) * 1.35;
const vec3 kZenith = vec3(0.20, 0.36, 0.62);
const vec3 kHorizon = vec3(0.72, 0.80, 0.90);
const vec3 kPalette[17] = vec3[17](
    vec3(0.07,0.52,0.06), vec3(0.16,0.68,0.10), vec3(0.62,0.36,0.14),
    vec3(0.84,0.72,0.38), vec3(0.48,0.42,0.38), vec3(0.66,0.64,0.60),
    vec3(0.62,0.33,0.10), vec3(0.42,0.22,0.10), vec3(0.04,0.52,0.03),
    vec3(1.00,0.35,0.06), vec3(0.95,0.20,0.05), vec3(0.10,0.55,0.95),
    vec3(0.15,0.85,0.25), vec3(0.55,0.10,0.85), vec3(0.10,0.35,0.95), vec3(0.95,0.90,0.85),
    vec3(0.92,0.95,0.99)
);
const vec2 kMatRefl[17] = vec2[17](
    vec2(35,235)/255.0, vec2(40,230)/255.0, vec2(55,225)/255.0,
    vec2(130,190)/255.0, vec2(95,150)/255.0, vec2(115,135)/255.0,
    vec2(70,160)/255.0, vec2(60,170)/255.0, vec2(30,235)/255.0,
    vec2(45,205)/255.0, vec2(45,205)/255.0, vec2(40,200)/255.0,
    vec2(40,200)/255.0, vec2(40,200)/255.0, vec2(40,200)/255.0, vec2(40,200)/255.0,
    vec2(50,200)/255.0
);
// emissive (self-illuminating) term per material id; 0-8 are inert, 9-15 glow.
const vec3 kEmissive[16] = vec3[16](
    vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0),
    vec3(0.0), vec3(0.0), vec3(0.0),
    vec3(3.0,0.9,0.18), vec3(2.4,0.5,0.10), vec3(0.2,1.6,2.8),
    vec3(0.3,2.4,0.6), vec3(1.6,0.3,2.6), vec3(0.3,0.9,3.2), vec3(3.2,3.0,2.8)
);

// bilinear height sample from the records-derived terrain texture (.r = top Y)
float heightAt(vec2 xz)
{
    ivec2 sz = imageSize(uHeight);
    vec2 tc = clamp(xz / pc.b.x + 0.5, vec2(0.0), vec2(1.0)) * vec2(sz - 1);
    ivec2 i0 = ivec2(floor(tc));
    ivec2 i1 = min(i0 + 1, sz - 1);
    vec2 f = tc - vec2(i0);
    float h00 = imageLoad(uHeight, ivec2(i0.x, i0.y)).r;
    float h10 = imageLoad(uHeight, ivec2(i1.x, i0.y)).r;
    float h01 = imageLoad(uHeight, ivec2(i0.x, i1.y)).r;
    float h11 = imageLoad(uHeight, ivec2(i1.x, i1.y)).r;
    return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

// nearest-texel terrain material straight from the baked records (.g)
uint heightMatNearest(vec2 xz)
{
    ivec2 sz = imageSize(uHeight);
    vec2 tc = clamp(xz / pc.b.x + 0.5, vec2(0.0), vec2(1.0)) * vec2(sz - 1);
    ivec2 c = clamp(ivec2(round(tc)), ivec2(0), sz - 1);
    return uint(clamp(imageLoad(uHeight, c).g, 0.0, 1.0) * 255.0 + 0.5);
}

bool underWater(vec2 xz) { return heightAt(xz) < kWaterLevel; }
bool gUnderwater = false; // camera currently below the fixed water plane

// trilinear object-only signed distance (metres) from the coarse bake; the
// CPU clamps at +-1.26 m so empty space reads far away
float objVolAt(ivec3 c)
{
    return float(imageLoad(uObjVol, c).r);
}
// NOTE: uObjVol is r8_snorm holding round(d / kObjVolMax * 127), so the raw
// imageLoad is normalized (-1..1); scale back to metres. Empty space reads
// exactly +kObjVolMax (no information beyond that range).
const float kObjVolMax = 1.26;
float objDist(vec3 p)
{
    ivec3 sz = imageSize(uObjVol);
    vec3 tc = clamp(p / pc.b.x + 0.5, vec3(0.0), vec3(1.0)) * vec3(sz - 1);
    ivec3 i0 = ivec3(floor(tc));
    vec3 f = tc - vec3(i0);
    ivec3 i1 = min(i0 + 1, sz - 1);
    float c00 = mix(objVolAt(ivec3(i0.x, i0.y, i0.z)), objVolAt(ivec3(i1.x, i0.y, i0.z)), f.x);
    float c10 = mix(objVolAt(ivec3(i0.x, i1.y, i0.z)), objVolAt(ivec3(i1.x, i1.y, i0.z)), f.x);
    float c01 = mix(objVolAt(ivec3(i0.x, i0.y, i1.z)), objVolAt(ivec3(i1.x, i0.y, i1.z)), f.x);
    float c11 = mix(objVolAt(ivec3(i0.x, i1.y, i1.z)), objVolAt(ivec3(i1.x, i1.y, i1.z)), f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z) * kObjVolMax;
}

// deterministic value noise (bank foam pattern)
float hashN(vec2 q) { return fract(sin(dot(q, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 q)
{
    vec2 i = floor(q), f = fract(q);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hashN(i), hashN(i + vec2(1, 0)), f.x),
               mix(hashN(i + vec2(0, 1)), hashN(i + vec2(1, 1)), f.x), f.y);
}

float fbm(vec2 q)
{
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i) {
        v += amp * vnoise(q);
        q = q * 2.03 + vec2(7.7, 3.1);
        amp *= 0.5;
    }
    return v;
}


// ---- landscape helpers ------------------------------------------------------
vec3 grassDetail(vec3 alb, vec3 p, inout vec3 n)
{
    float f = fbm(p.xz * 2.3);
    float blade = vnoise(p.xz * 16.0 + f * 2.4);
    n = normalize(n + 0.22 * vec3(blade - 0.5, 0.0, fract(blade * 7.31) - 0.5));
    return alb * (0.74 + 0.45 * blade) * (0.90 + 0.20 * f);
}

// ---- photoreal albedo grade: shared by SVO + splat so backends stay in sync --
// Tames the neon bake palette toward mossy/earthy tones and adds multi-scale
// detail (large mottling + fine grain + per-material accents like log courses,
// roof moss and shoreline pebbles). Pure function of (matId, worldPos, normal).
vec3 detailAlbedo(vec3 alb, vec3 p, vec3 n, uint mId)
{
    // large-scale mottling breaks up flat airbrushed fills
    float big = fbm(p.xz * 1.7 + p.y * 1.3);
    alb *= 0.86 + 0.28 * big;
    // fine grain for close-up texture
    alb *= 0.93 + 0.10 * vnoise(p.xz * 23.0 + p.y * 17.0);
    if (mId <= 1u) {
        // meadow: desaturate neon greens toward olive, patchy dry spots
        float lum = dot(alb, vec3(0.33));
        alb = mix(vec3(lum), alb, 0.68);
        alb *= vec3(0.86, 0.92, 0.78);
        float dry = smoothstep(0.25, 0.85, fbm(p.xz * 0.9 + 3.7));
        alb = mix(alb, alb * vec3(1.18, 1.02, 0.72), dry * 0.45);
    } else if (mId == 2u || mId == 3u) {
        // soil/sand: pebble speckle + shoreline darkening when damp
        float peb = vnoise(p.xz * 34.0);
        alb *= 0.88 + 0.24 * peb;
        float clod = fbm(p.xz * 5.5);
        alb *= 0.90 + 0.20 * clod;
    } else if (mId == 4u || mId == 5u || mId == 16u) {
        // rock/snow: strata bands + lichen tint on up-faces
        float strata = vnoise(vec2(p.y * 9.0, dot(p.xz, vec2(0.6, 0.8)) * 2.0));
        alb *= 0.86 + 0.26 * strata;
        float lichen = smoothstep(0.55, 0.85, fbm(p.xz * 4.2 + p.y * 2.0)) * clamp(n.y, 0.0, 1.0);
        alb = mix(alb, vec3(0.35, 0.38, 0.20), lichen * 0.35);
    } else if (mId == 6u) {
        // weathered logs: horizontal course grooves every 0.27 m + long grain
        float course = fract(p.y / 0.27);
        float groove = smoothstep(0.0, 0.14, course) * smoothstep(1.0, 0.86, course);
        alb *= 0.78 + 0.22 * groove;
        float grain = vnoise(vec2((p.x + p.z) * 34.0, p.y * 7.0));
        alb *= 0.88 + 0.20 * grain;
        alb *= vec3(0.82, 0.78, 0.75); // knock back the orange bake tint
    } else if (mId == 7u) {
        // shingles with moss creeping on up-faces (reference cabin roof)
        float shingle = vnoise(vec2((p.x + p.z) * 22.0, p.y * 30.0));
        alb *= 0.85 + 0.25 * shingle;
        float moss = smoothstep(0.30, 0.80, fbm(p.xz * 3.0 + p.y * 1.5)) * clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
        alb = mix(alb, vec3(0.26, 0.33, 0.12), moss * 0.60);
    } else if (mId == 8u) {
        // canopy: deep pine variation, sun-flecked
        float lum = dot(alb, vec3(0.33));
        alb = mix(vec3(lum), alb, 0.70);
        alb *= vec3(0.60, 0.70, 0.58);
        alb *= 0.70 + 0.35 * fbm(p.xz * 2.6 + p.y * 2.2);
    }
    return alb;
}

// ---- flora field (deterministic, shared by both backends) ------------------
// Shading-level micro geometry: tapered grass blades in tufts (kind 0) and
// irregular leaf clusters with random facet normals (kind 1, bushes/canopy).
// Evaluated once per shaded hit, NOT per march step, so it is cheap.

struct FloraInfo {
    vec3  n;     // blade / facet normal (unit)
    float tip;   // height fraction along blade 0..1 (grass) / 1 (leaves)
    float mask;  // 0..1 contribution strength
    float col;   // per-blade color variance
};

FloraInfo floraField(vec3 p, float groundH, uint kind)
{
    FloraInfo fi;
    fi.n = vec3(0.0, 1.0, 0.0);
    fi.tip = 0.0;
    fi.mask = 0.0;
    fi.col = 1.0;
    float cell = (kind == 1u) ? 0.50 : 0.20;
    float dens = (kind == 1u) ? 0.70 : 0.66;
    float reach = (kind == 1u) ? 0.42 : 0.55;
    int ix = int(floor(p.x / cell));
    int iz = int(floor(p.z / cell));
    float bestD = 1e9;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx) {
        int cx = ix + dx, cz = iz + dz;
        if (hashN(vec2(float(cx) * 19.1, float(cz) * 37.7)) > dens) continue;
        if (kind == 1u) {
            // leaf cluster: irregular ball with a random facet normal
            vec3 c = vec3((float(cx) + 0.5 + (hashN(vec2(float(cx)*41.3, float(cz)*53.7)) - 0.5) * 0.8) * cell,
                          groundH + hashN(vec2(float(cx)*59.9, float(cz)*71.3)) * 0.25,
                          (float(cz) + 0.5 + (hashN(vec2(float(cx)*79.7, float(cz)*83.9)) - 0.5) * 0.8) * cell);
            float r = 0.10 + hashN(vec2(float(cx)*97.3, float(cz)*103.1)) * 0.10;
            float d = length(p - c) - r;
            if (d > 0.05) continue;
            float prof = 1.0 - smoothstep(0.0, 0.05, d);
            if (prof < 0.001) continue;
            vec3 fn = normalize(vec3(hashN(vec2(float(cx)*113.3, float(cz)*127.1)) - 0.5,
                                     hashN(vec2(float(cx)*131.9, float(cz)*139.7)) - 0.5,
                                     hashN(vec2(float(cx)*149.3, float(cz)*151.9)) - 0.5));
            if (d < bestD) { bestD = d; fi.n = fn; fi.tip = 1.0; fi.mask = prof;
                             fi.col = 0.80 + 0.40 * hashN(vec2(float(cx)*157.3, float(cz)*173.9)); }
        } else {
            // grass tuft: 4-7 thin tapered blades (localized sheets, not stripes)
            vec2 tuft = vec2((float(cx) + 0.5 + (hashN(vec2(float(cx)*7.3, float(cz)*11.1)) - 0.5) * 0.7) * cell,
                             (float(cz) + 0.5 + (hashN(vec2(float(cx)*13.7, float(cz)*17.3)) - 0.5) * 0.7) * cell);
            vec2 q = p.xz - tuft;
            if (dot(q, q) > reach * reach) continue;
            int nb = 4 + int(hashN(vec2(float(cx) * 23.1, float(cz) * 29.7)) * 4.0);
            for (int b = 0; b < 8; ++b) {
                if (b >= nb) break;
                float hb1 = hashN(vec2(float(cx) * 43.1 + float(b) * 1.7, float(cz) * 53.3 - float(b) * 0.9));
                float hb2 = hashN(vec2(float(cx) * 61.9 + float(b) * 3.1, float(cz) * 67.3 - float(b) * 1.3));
                float az = hb1 * 6.28318;
                vec2 ldir = vec2(cos(az), sin(az));
                float hBlade = 0.12 + hb2 * 0.28;
                float lean = (hashN(vec2(float(cx)*83.3, float(cz)*97.9 + float(b)*2.7)) - 0.5) * 0.45 * hBlade;
                float wpb = hashN(vec2(float(cx)*89.1 + float(b)*0.5, float(cz)*101.3)) * 6.28318;
                float wind = 0.06 * hBlade * (sin(pc.misc.y * 1.2 + wpb) + 0.4 * sin(pc.misc.y * 3.7 + wpb * 2.1));
                vec3 base = vec3(tuft.x + ldir.x * hb1 * 0.05, groundH - 0.02, tuft.y + ldir.y * hb1 * 0.05);
                vec3 axis = normalize(vec3(ldir.x * (lean + wind), hBlade, ldir.y * (lean + wind)));
                vec3 nPl = normalize(vec3(-axis.z, 0.0, axis.x));
                vec3 rel = p - base;
                float s0 = clamp(dot(rel, axis), 0.0, hBlade);
                float dseg = length(rel - axis * s0);
                float wbl = mix(0.007, 0.014, hb2) * (1.0 - 0.6 * s0 / hBlade);
                float dBlade = dseg - wbl;
                float prof = 1.0 - smoothstep(0.0, 0.014, dBlade);
                if (prof < 0.001) continue;
                if (dBlade < bestD) {
                    bestD = dBlade;
                    fi.n = nPl;
                    fi.tip = s0 / hBlade;
                    fi.mask = prof;
                    fi.col = 0.78 + 0.44 * hashN(vec2(float(cx)*157.3 + float(b)*11.3, float(cz)*173.9 - float(b)*7.7));
                }
            }
        }
    }
    return fi;
}

// per-blade hue/luminance variation (grass: dry straw tips; leaves: hue jitter)
vec3 floraColor(vec3 p, uint mId, float tip, float col)
{
    vec3 var = vec3(col);
    vec3 fine = vec3(0.96 + 0.08 * hashN(p.xz * 113.7));
    if (mId <= 1u) {
        float dry = smoothstep(0.40, 0.95, tip) * (0.20 + 0.40 * hashN(p.xz * 91.7));
        var *= vec3(1.0 + 0.35 * dry, 1.0 - 0.08 * dry, 1.0 - 0.45 * dry);
    } else {
        var *= vec3(0.88 + 0.24 * hashN(p.xz * 71.3));
    }
    return var * fine;
}

// ---- high-res grass cards (alpha-tested cross quads) -------------------------
// Classic grass sprite: two vertical quads per tuft, crossed at a hashed angle.
// The alpha pattern is analytic (resolution-independent "texture"); the card
// ray is intersected against the camera ray so cards occlude the terrain.

float tuftAlpha(vec2 uv, float s1, float s2, out float tint)
{
    float a = 0.0;
    tint = 1.0;
    for (int i = 0; i < 9; ++i) {
        float f = fract(s1 * 11.17 + float(i) * 0.61803 + s2 * 0.37);
        float bx = f * 1.25 - 0.125;                  // blade base x offset
        float bh = 0.62 + 0.38 * fract(f * 7.31);     // per-blade height
        if (uv.y > bh) continue;
        float curve = (fract(f * 3.77) - 0.5) * 0.42; // wavy centerline
        float v = uv.y / bh;
        float halfw = 0.045 + 0.035 * fract(f * 1.91);
        halfw *= (1.0 - 0.72 * v);                    // taper toward tip
        float uc = bx + curve * v;
        float d = abs(uv.x - uc);
        float edge = 1.0 - smoothstep(halfw * 0.50, halfw, d);
        edge *= 1.0 - smoothstep(0.90, 1.0, v);       // wispy rounded tip
        if (edge >= a) { a = edge; tint = 0.70 + 0.62 * fract(f * 5.13); }
    }
    return a;
}

struct Card {
    bool  hit;
    float alpha;
    vec3  n;
    vec3  alb;
    float tip;
};

void cardPass(vec3 ro, vec3 rd, float tMax, float gh,
              float cell, float dens, float wBase, float wSpan, float hBase, float hSpan,
              float seedK, float passPhase, inout float bestT, inout Card c0)
{
    vec3 p = ro + rd * tMax;
    int ix = int(floor(p.x / cell));
    int iz = int(floor(p.z / cell));
    float reach2 = (wBase + wSpan) * (wBase + wSpan) * 0.25 + 0.04;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx) {
        int cx = ix + dx, cz = iz + dz;
        if (hashN(vec2(float(cx) * 19.1 * seedK, float(cz) * 37.7 * seedK)) > dens) continue;
        float s1 = hashN(vec2(float(cx) * 7.3 * seedK, float(cz) * 11.1 * seedK));
        float s2 = hashN(vec2(float(cx) * 13.7 * seedK, float(cz) * 17.3 * seedK));
        vec2 tpos = vec2((float(cx) + s1) * cell, (float(cz) + s2) * cell);
        vec2 dq = p.xz - tpos;
        if (dot(dq, dq) > reach2) continue;
        float ang = hashN(vec2(float(cx) * 23.1 * seedK, float(cz) * 29.7 * seedK)) * 3.14159;
        vec3 e1 = vec3(sin(ang), 0.0, cos(ang));
        vec3 nCard = vec3(e1.z, 0.0, -e1.x);
        float w = wBase + wSpan * hashN(vec2(float(cx) * 41.3 * seedK, float(cz) * 43.1 * seedK));
        float h = hBase + hSpan * hashN(vec2(float(cx) * 53.9 * seedK, float(cz) * 61.7 * seedK));
        vec3 ctr = vec3(tpos.x, gh, tpos.y);
        // near-field coverage: sample the blade pattern at the GROUND HIT so
        // steep/near views (where short cards cannot occlude) still get blades
        vec3 pHit = ro + rd * tMax;
        float halfW = w * 0.5;
        vec2 uvH = vec2(dot(pHit - ctr, e1) / halfW * 0.5 + 0.5, (pHit.y - ctr.y) / h);
        if (uvH.y >= -0.02 && uvH.y <= 1.0) {
            float ttH;
            float aaH = tuftAlpha(uvH, s1 * 97.3 + float(cx) * 7.7 + passPhase, s2 * 53.1 + float(cz) * 3.1, ttH);
            if (aaH > c0.alpha) {
                float v1h = hashN(vec2(float(cx) * 71.9 * seedK, float(cz) * 83.7 * seedK));
                vec3 gDk = vec3(0.07, 0.36, 0.06) * (0.70 + 0.45 * v1h);
                vec3 gLt = vec3(0.36, 0.70, 0.22) * (0.85 + 0.35 * hashN(vec2(float(cx) * 91.3 + passPhase, float(cz) * 97.1)));
                float tgh = 0.84;
                c0.hit = true;
                c0.alpha = aaH;
                c0.n = nCard;
                c0.tip = 0.75;
                c0.alb = min(mix(gDk, gLt, tgh) * ttH * (0.72 + 0.55 * aaH), vec3(0.90));
            }
        }
        float denom = dot(rd, nCard);
        if (abs(denom) < 0.04) continue;
        float tc = dot(ctr - ro, nCard) / denom;
        if (tc <= 0.0 || tc >= bestT) continue;
        vec3 q = ro + rd * tc;
        float du = dot(q - ctr, e1);
        if (abs(du) > halfW) continue;
        float dv = q.y - ctr.y;
        if (dv < -0.02 || dv > h) continue;
        vec2 uv = vec2(du / halfW * 0.5 + 0.5, dv / h);
        float tt; float aa = tuftAlpha(uv, s1 * 97.3 + float(cx) * 7.7 + passPhase, s2 * 53.1 + float(cz) * 3.1, tt);
        if (aa < 0.04) continue;
        bestT = tc;
        c0.hit = true;
        c0.alpha = aa;
        c0.n = nCard;
        c0.tip = uv.y;
        float v1 = hashN(vec2(float(cx) * 71.9 * seedK, float(cz) * 83.7 * seedK));
        vec3 gDark = vec3(0.07, 0.36, 0.06) * (0.70 + 0.45 * v1);
        vec3 gLit = vec3(0.36, 0.70, 0.22) * (0.85 + 0.35 * hashN(vec2(float(cx) * 91.3 + passPhase, float(cz) * 97.1)));
        float tg = uv.y * uv.y * (3.0 - 2.0 * uv.y);
        c0.alb = min(mix(gDark, gLit, tg) * tt * (0.72 + 0.55 * aa), vec3(0.90));
    }
}

Card grassCard(vec3 ro, vec3 rd, float tMax, float gh)
{
    Card c0;
    c0.hit = false; c0.alpha = 0.0; c0.n = vec3(0.0, 1.0, 0.0);
    c0.alb = vec3(0.0); c0.tip = 0.0;
    float bestT = tMax;
    // coarse meadow tufts + fine filler cards (decorrelated grids)
    cardPass(ro, rd, tMax, gh, 0.42, 0.66, 0.50, 0.38, 0.28, 0.40, 1.000, 0.0, bestT, c0);
    cardPass(ro, rd, tMax, gh, 0.17, 0.88, 0.14, 0.16, 0.12, 0.18, 0.618, 13.37, bestT, c0);
    return c0;
}

// analytic water surface hit along ray; returns t or -1.
// Global fixed sea level: the plane is hit wherever the ray crosses y =
// kWaterLevel within [0, tMax], regardless of terrain or object presence.
// Terrain/objects still occlude it because the caller only lets the hit win
// when it is in front of the solid surface.
float waterHit(vec3 ro, vec3 rd, float tMax)
{
    if (abs(rd.y) < 0.001) return -1.0;
    float tw = (kWaterLevel - ro.y) / rd.y;
    if (tw < 0.0 || tw > tMax) return -1.0;
    return tw;
}

vec3 skyColor(vec3 d)
{
    // Preetham / Hosek-inspired sky with turbidity-controlled Perez distribution
    // Turbidity 2.2 = clear day; Y distribution shared for RGB tint
    float cosTheta = clamp(d.y, 0.0, 1.0);
    float cosGamma = max(dot(d, kSunDir), 0.0);
    float gamma = acos(clamp(cosGamma, 0.0, 1.0));
    const float T = 2.2;
    float Ay = 0.1787 * T - 1.4630;
    float By = -0.3554 * T + 0.4275;
    float Cy = -0.0227 * T + 5.3251;
    float Dy = 0.1206 * T - 2.5771;
    float Ey = -0.0670 * T + 0.3703;
    float cosThetaSafe = max(cosTheta, 0.07);
    float Ftheta = (1.0 + Ay * exp(By / cosThetaSafe)) / (1.0 + Ay * exp(By));
    float Fgamma = 1.0 + Cy * exp(Dy * gamma) + Ey * cosGamma * cosGamma;
    float Y = Ftheta * Fgamma; // luminance distribution, 1 at zenith/sun
    vec3 base = mix(kHorizon * 1.05, kZenith * 0.95, pow(max(cosTheta, 0.0), 0.55));
    vec3 col = base * (0.82 + 0.30 * clamp(Y * 0.08, 0.0, 1.5));
    // sun disc (sharp) and broad glow, both turbidity-scaled
    col += kSunCol * 0.55 * pow(cosGamma, 1150.0) * clamp(Y * 0.15, 0.0, 2.0);
    col += vec3(1.0, 0.85, 0.6) * 0.48 * pow(cosGamma, 6.0) * clamp(Y * 0.06, 0.2, 1.2);
    // ---- golden-hour warmth: low sun tints the horizon amber/pink ----
    // kSunDir.y ~ sin(elev): 0.56 at 34 deg, ~0.31 at 18 deg, ~0.17 at 10 deg.
    float lowSun = 1.0 - smoothstep(0.08, 0.55, kSunDir.y);
    float horizBand = pow(1.0 - cosTheta, 3.0);
    vec3 sunsetTint = mix(vec3(1.0, 0.62, 0.32), vec3(0.95, 0.45, 0.45), clamp(cosGamma, 0.0, 1.0) * 0.5);
    col += sunsetTint * horizBand * lowSun * (0.25 + 0.55 * pow(cosGamma, 2.0));
    col = mix(col, col * vec3(1.06, 0.94, 0.86) + vec3(0.03, 0.01, 0.0), lowSun * 0.45 * horizBand);
    // ---- procedural clouds: fbm deck, thin at zenith (keeps sky probe blue),
    // thick toward the horizon like a valley overcast with a sunset break ----
    if (d.y > -0.02) {
        vec2 sk = d.xz / (abs(d.y) + 0.22);
        float drift = pc.misc.y * 0.006;
        float cm = fbm(sk * 1.35 + vec2(drift, drift * 0.4));
        cm = cm * 0.65 + 0.35 * fbm(sk * 3.1 - vec2(drift * 1.7, 0.0));
        float cover = 0.46 + 0.10 * (1.0 - smoothstep(0.0, 0.6, kSunDir.y));
        float cmask = smoothstep(cover, cover + 0.28, cm + (1.0 - cosTheta) * 0.12);
        float cweight = cmask * smoothstep(-0.02, 0.14, d.y);
        // keep zenith mostly clear so the top-strip sky probe stays blue
        cweight *= mix(0.35, 1.0, 1.0 - smoothstep(0.45, 0.95, d.y));
        vec3 cloudShadow = vec3(0.38, 0.42, 0.52);
        vec3 cloudLit = vec3(1.08, 0.98, 0.90);
        // warm lit edges near the sun, pink-grey away; low sun deepens contrast
        vec3 cloudCol = mix(cloudShadow, cloudLit, 0.35 + 0.65 * pow(cosGamma, 2.0));
        cloudCol += sunsetTint * lowSun * pow(cosGamma, 3.0) * 0.55;
        cloudCol *= 0.85 + 0.30 * cm;
        col = mix(col, cloudCol, clamp(cweight, 0.0, 1.0) * 0.85);
    }
    return col;
}

// Cloud-free sky variant for indirect taps (irradiance / fog): the fbm
// cloud deck dominates per-fragment ALU but barely modulates diffuse
// ambient, so indirect lighting uses the analytic gradient + sun only.
// Direct sky pixels and reflections keep the full cloudy skyColor.
vec3 skyColorFast(vec3 d)
{
    float cosTheta = clamp(d.y, 0.0, 1.0);
    float cosGamma = max(dot(d, kSunDir), 0.0);
    float gamma = acos(clamp(cosGamma, 0.0, 1.0));
    const float T = 2.2;
    float Ay = 0.1787 * T - 1.4630;
    float By = -0.3554 * T + 0.4275;
    float Cy = -0.0227 * T + 5.3251;
    float Dy = 0.1206 * T - 2.5771;
    float Ey = -0.0670 * T + 0.3703;
    float cosThetaSafe = max(cosTheta, 0.07);
    float Ftheta = (1.0 + Ay * exp(By / cosThetaSafe)) / (1.0 + Ay * exp(By));
    float Fgamma = 1.0 + Cy * exp(Dy * gamma) + Ey * cosGamma * cosGamma;
    float Y = Ftheta * Fgamma; // luminance distribution, 1 at zenith/sun
    vec3 base = mix(kHorizon * 1.05, kZenith * 0.95, pow(max(cosTheta, 0.0), 0.55));
    vec3 col = base * (0.82 + 0.30 * clamp(Y * 0.08, 0.0, 1.5));
    // sun disc (sharp) and broad glow, both turbidity-scaled
    col += kSunCol * 0.55 * pow(cosGamma, 1150.0) * clamp(Y * 0.15, 0.0, 2.0);
    col += vec3(1.0, 0.85, 0.6) * 0.48 * pow(cosGamma, 6.0) * clamp(Y * 0.06, 0.2, 1.2);
    // ---- golden-hour warmth: low sun tints the horizon amber/pink ----
    float lowSun = 1.0 - smoothstep(0.08, 0.55, kSunDir.y);
    float horizBand = pow(1.0 - cosTheta, 3.0);
    vec3 sunsetTint = mix(vec3(1.0, 0.62, 0.32), vec3(0.95, 0.45, 0.45), clamp(cosGamma, 0.0, 1.0) * 0.5);
    col += sunsetTint * horizBand * lowSun * (0.25 + 0.55 * pow(cosGamma, 2.0));
    col = mix(col, col * vec3(1.06, 0.94, 0.86) + vec3(0.03, 0.01, 0.0), lowSun * 0.45 * horizBand);
    return col;
}

bool rayAABB(vec3 ro, vec3 invRd, out float t0, out float t1)
{
    vec3 half_ = vec3(pc.b.x * 0.5);
    vec3 tv0 = (-half_ - ro) * invRd;
    vec3 tv1 = (half_ - ro) * invRd;
    vec3 tsm = min(tv0, tv1), tbg = max(tv0, tv1);
    t0 = max(max(tsm.x, tsm.y), tsm.z);
    t1 = min(min(tbg.x, tbg.y), tbg.z);
    return t1 >= max(t0, 0.0);
}


vec3 aces(vec3 x){
    return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0);
}

// ---- AgX tonemapping (adapted from vengi _tonemapping.glsl, MIT, B. Wrensch) --
// Scene-referred, filmic, with three looks. Far better hue/contrast handling
// than the old ACES + ad-hoc colour grade, especially for sky and foliage.
const mat3 kAgxMat = mat3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772,  0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
const mat3 kAgxMatInv = mat3(
    1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
   -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
   -0.0990297440797205,-0.0989611768448433,  1.15107367264116);
const float kAgxMinEv = -12.47393;
const float kAgxMaxEv = 4.026069;

vec3 agxContrastApprox(vec3 x){
    vec3 x2 = x*x; vec3 x4 = x2*x2;
    return 15.5*x4*x2 - 40.14*x4*x + 31.96*x4 - 6.868*x2*x + 0.4298*x2 + 0.1191*x - 0.00232;
}
vec3 agxVec(vec3 v){
    v = kAgxMat * v;
    v = clamp(log2(v), kAgxMinEv, kAgxMaxEv);
    v = (v - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
    v = agxContrastApprox(v);
    return v;
}
// looks: 0 = Default, 1 = Golden, 2 = Punchy
vec3 agxLook(vec3 v, int look){
    float luma = dot(v, vec3(0.2126, 0.7152, 0.0722));
    vec3 offset = vec3(0.0), slope = vec3(1.0), power = vec3(1.0);
    float sat = 1.0;
    if (look == 1) { slope = vec3(1.0,0.9,0.5); power = vec3(0.8); sat = 0.8; }
    else if (look == 2) { power = vec3(1.35); sat = 1.4; }
    v = pow(v*slope + offset, power);
    return luma + sat*(v - luma);
}
vec3 tonemapAgX(vec3 c, int look){
    c = agxVec(c);
    c = agxLook(c, look);
    c = kAgxMatInv * c;
    c = pow(max(c, vec3(0.0)), vec3(2.2)); // display-linear (EOTF); final pow(1/2.2) re-encodes
    return c;
}

// ---- PBR shading helpers (shared identical code in both backends) ----------

const float PI = 3.14159265;

vec3 fresnelSchlick(vec3 f0, float vdh)
{
    return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
}

float ggxD(float ndh, float a2)
{
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float smithV(float ndl, float ndv, float a2)
{
    float v = 0.5 / max(ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) +
                        ndv * sqrt(ndl * ndl * (1.0 - a2) + a2), 1e-3);
    return min(v, 6.0);
}

vec3 pbrSpec(vec3 n, vec3 v, vec3 l, vec3 f0, float rough)
{
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 0.0);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);
    float a2 = rough * rough * rough * rough;
    return fresnelSchlick(f0, vdh) * ggxD(ndh, a2) * smithV(ndl, ndv, a2);
}

// 3-tap analytic-sky irradiance: full hemisphere integral approximated by
// tilted taps so the ambient follows the sun direction and sky gradient.
vec3 skyIrradiance(vec3 n)
{
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 a = normalize(n * 0.6 + up * 0.4);
    vec3 b = normalize(n * 0.7 + kSunDir * 0.3);
    vec3 c = normalize(n * 0.15 + up * 0.85);
    return skyColorFast(a) * 0.42 + skyColorFast(b) * 0.28 + skyColorFast(c) * 0.30;
}

// aerial-perspective fog: sky-tinted, sun-warmed, altitude-attenuated
vec3 fogColor(vec3 rd, vec3 p)
{
    vec3 fc = skyColorFast(rd) * 0.88;
    float sunAmt = pow(clamp(dot(rd, kSunDir), 0.0, 1.0), 3.0);
    float lowSun = 1.0 - smoothstep(0.08, 0.55, kSunDir.y);
    fc += mix(vec3(1.0, 0.75, 0.45), vec3(1.0, 0.55, 0.30), lowSun) * (0.10 + 0.12 * lowSun) * sunAmt;
    fc *= clamp(0.60 + 0.40 * clamp((p.y + 1.5) * 0.08, 0.0, 1.0), 0.0, 1.0);
    return fc;
}

// valley/river mist factor: dense near the water table, fading with height.
// Shared by both backends so mist over the stream matches the reference mood.
float mistFactor(vec3 ro, vec3 p, float t)
{
    float h = max(p.y + 0.9, 0.0);
    float lowBand = exp(-h * 0.55);
    float dist = 1.0 - exp(-t * 0.006);
    float drift = 0.75 + 0.25 * vnoise(p.xz * 0.8 + vec2(pc.misc.y * 0.05, 0.0));
    return clamp(lowBand * dist * drift, 0.0, 1.0);
}

// ---- IBL infrastructure (environment cubemap + BRDF LUT) ----------

// IBL: pre-filtered environment cubemap + BRDF LUT
layout(set = 0, binding = 11) uniform samplerCube uEnvCubemap;
layout(set = 0, binding = 12) uniform sampler2D uBRDFLUT;
const float kIBLIntensity = 1.0;

// Pre-filtered mip levels for IBL (0 = rough, 5 = smooth)
const float kEnvMipLevels = 5.0;

vec3 sampleEnv(vec3 d)
{
    return textureLod(uEnvCubemap, d, 0.0).rgb * kIBLIntensity;
}

vec3 iblIrradiance(vec3 n)
{
    vec3 irr = textureLod(uEnvCubemap, n, 1.0).rgb * PI;
    return irr;
}

vec3 iblContribution(vec3 n, vec3 V, vec3 f0, float rough)
{
    vec3 R = reflect(-V, n);
    float vdh = max(dot(R, V), 0.0);
    float envRough = max(rough, 0.05);
    vec3 prefiltered = sampleEnv(R);
    vec2 brdf = texture(uBRDFLUT, vec2(vdh, envRough)).rg;
    vec3 kS = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    vec3 kD = (1.0 - kS) * (1.0 - f0);
    vec3 diffuse = iblIrradiance(n) * kD;
    vec3 specular = prefiltered * (kS * envRough + (1.0 - envRough) * vec3(brdf, 0.0));
    return kD * diffuse + specular;
}

// terrain-only shadow: same march but occluders are the records-derived
// heightfield only (no object volume). Used for the submerged lake bed so its
// reflected appearance stays independent of object add/remove.
float softShadowTerrain(vec3 ro, vec3 rd)
{
    float res = 1.0;
    float t = 0.05;
    for (int i = 0; i < 28; ++i) {
        vec3 sp = ro + rd * t;
        float s = sp.y - heightAt(sp.xz);
        res = min(res, 9.0 * max(s, pc.b.y * 0.45) / t);
        t += clamp(max(s, pc.b.y * 0.35) * 0.85, 0.05, 1.2);
        if (res < 0.004 || t > 40.0)
            break;
    }
    return clamp(res, 0.0, 1.0);
}


// Vegetation + flora micro detail shared by the SVO and splat backends.
// The second shadow evaluation runs after grassDetail perturbs the normal
// (same order as the original inline code). SPLAT_BACKEND selects the march
// (preprocessor: the other branch is removed before compilation, so each
// backend only needs its own shadow function defined).
float softShadow(vec3 ro, vec3 rd);
float softShadowSplat(vec3 ro, vec3 rd);
void applyFlora(vec3 p, vec3 rd, vec3 ro, uint mId, bool useBaked, float shBaked,
                inout vec3 alb, inout vec3 n, inout float sh, inout float ao)
{
    if ((gRenderFlags & 4) != 0 && alb.g > alb.r * 1.15) { // vegetation micro detail
        alb = grassDetail(alb, p, n);
        float shR = 1.0;
        if (useBaked) {
            shR = shBaked;
        } else if ((gRenderFlags & 2) != 0) {
#ifdef SPLAT_BACKEND
            shR = softShadowSplat(p + n * 0.35, kSunDir);
#else
            shR = softShadow(p + n * 0.35, kSunDir);
#endif
        }
        sh = min(sh, shR + 0.15);
    }

    // ---- flora field: grass cards / leaf clusters (near-field) ----
    if ((gRenderFlags & 4) != 0 && (mId <= 1u || mId == 8u)) {
        float lod = 1.0 - smoothstep(12.0, 40.0, length(p - ro));
        if (lod > 0.001) {
            if (mId <= 1u) {
                // vertical blade streaks: anisotropic world-space texture (3-4 cm
                // wide, tall) - reads as blades from every angle, unlike isotropic
                // per-pixel noise. Wind-sheared via pc.misc.y.
                float ghS = heightAt(p.xz);
                float hgt = clamp((p.y - ghS) * 2.2, 0.0, 1.0);
                float sx1 = p.x * 26.0 + p.z * 13.0;
                float streak = vnoise(vec2(sx1 + pc.misc.y * 0.45 + fbm(p.xz * 2.1) * 2.0, p.y * 2.9));
                float blade = 0.55 + 0.45 * fract(hashN(p.xz * 31.7) + streak * 13.1);
                vec3 gshade = mix(vec3(0.78, 0.90, 0.70), vec3(1.16, 1.19, 0.96), blade);
                alb *= mix(vec3(1.0), gshade, lod);
                vec3 bn = normalize(vec3(0.0, 0.85, 0.0)
                     + 0.45 * vec3(fract(streak * 7.31) - 0.5, 0.0, fract(streak * 11.7) - 0.5));
                n = normalize(mix(n, bn, 0.60 * lod * hgt * smoothstep(0.20, 0.85, streak)));
                ao *= mix(0.55, 1.05, hgt);          // dark roots, lit tips
                // silhouette cards (ray-space crossed alpha sprites)
                Card cd = grassCard(ro, rd, length(p - ro), ghS);
                if (cd.hit) {
                    float m = cd.alpha * lod;
                    vec3 bn2 = normalize(cd.n * 0.50 + vec3(0.0, 0.80, 0.0));
                    bn2 = normalize(bn2 + 0.40 * vec3(fract(streak * 7.31) - 0.5, 0.0, fract(streak * 3.17) - 0.5));
                    n = normalize(mix(n, bn2, m * 0.92));
                    alb *= cd.alb * (0.90 + 0.20 * streak);
                    float tg = cd.tip * cd.tip * (3.0 - 2.0 * cd.tip);
                    ao *= mix(0.50, 1.05, tg);
                }
            }
            float gh = (mId == 8u) ? p.y - 0.12 : heightAt(p.xz);
            FloraInfo fi = floraField(p, gh, (mId == 8u) ? 1u : 0u);
            if (fi.mask * lod > 0.02) {
                vec3 bn = fi.n;
                if (mId == 8u) {               // two-sided leaves: face the viewer
                    if (dot(bn, rd) > 0.0) bn = -bn;
                    bn = normalize(bn * 0.75 + vec3(0.0, 0.25, 0.0));
                } else {                       // blades: wall-like, up near the base
                    bn = normalize(mix(vec3(0.0, 0.40, 0.0), bn, 0.35 + fi.tip * 0.65));
                }
                float m = fi.mask * lod;
                n = normalize(mix(n, bn, m * m * (3.0 - 2.0 * m) * 0.9));
                alb *= floraColor(p, mId, fi.tip, fi.col);
                // height-graded self-occlusion: dark roots, lit tips
                float t2 = fi.tip * fi.tip * (3.0 - 2.0 * fi.tip);
                ao *= mix(0.68, 1.08, t2);
            }
        }
    }
}
