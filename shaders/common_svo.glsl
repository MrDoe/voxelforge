float sdBox(vec3 p, vec3 bmin, vec3 bmax)
{
    vec3 q = max(bmin - p, p - bmax);
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}


float brickVoxelSdf(uint bi, ivec3 c)
{
    c = clamp(c, ivec3(0), ivec3(BRICK_N - 1));
    uint v = uBricks[bi * BRICK_WORDS + (uint(c.z) * BRICK_N * BRICK_N +
                                         uint(c.y) * BRICK_N + uint(c.x)) *
                                            2];
    int raw = int((v >> 24) & 255u); // int8 sdf, encoded as sdf / VOXEL
    if (raw >= 128)
        raw -= 256;
    return float(raw) * pc.b.y; // decode: meters = (sdf / VOXEL) * VOXEL
}

float brickSample(uint bi, vec3 p, vec3 mn, vec3 sz)
{
    float cellSz = sz.x / float(BRICK_N);
    vec3 g = clamp((p - mn) / cellSz - 0.5, vec3(0.0), vec3(float(BRICK_N) - 1.001));
    ivec3 i0 = ivec3(g);
    vec3 f = g - vec3(i0);
    float s000 = brickVoxelSdf(bi, i0 + ivec3(0, 0, 0));
    float s100 = brickVoxelSdf(bi, i0 + ivec3(1, 0, 0));
    float s010 = brickVoxelSdf(bi, i0 + ivec3(0, 1, 0));
    float s110 = brickVoxelSdf(bi, i0 + ivec3(1, 1, 0));
    float s001 = brickVoxelSdf(bi, i0 + ivec3(0, 0, 1));
    float s101 = brickVoxelSdf(bi, i0 + ivec3(1, 0, 1));
    float s011 = brickVoxelSdf(bi, i0 + ivec3(0, 1, 1));
    float s111 = brickVoxelSdf(bi, i0 + ivec3(1, 1, 1));
    float c00 = mix(s000, s100, f.x), c10 = mix(s010, s110, f.x);
    float c01 = mix(s001, s101, f.x), c11 = mix(s011, s111, f.x);
    return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z) - 0.025;
}

float map(vec3 p)
{
    vec3 wmin = vec3(-pc.b.x * 0.5);
    vec3 rp = p - wmin;
    float span = pc.b.x;
    if (any(lessThan(rp, vec3(0.0))) || any(greaterThanEqual(rp, vec3(span))))
        return 6.0;
    float chunkM = span / pc.b.z;
    ivec3 cc = ivec3(floor(rp / chunkM));
    int root = uGrid[(cc.z * int(pc.b.z) + cc.y) * int(pc.b.z) + cc.x];
    if (root < 0) {
        vec3 cmin = wmin + vec3(cc) * chunkM;
        float d = sdBox(p, cmin, cmin + vec3(chunkM));
        return max(-d, pc.b.y * 0.5);
    }
    vec3 mn = wmin + vec3(cc) * chunkM;
    vec3 sz = vec3(chunkM);
    uint h = uint(root);
    for (int guard = 0; guard < 16; ++guard) {
        uint ty = h & 3u;
        if (ty == 1u) {               // brick -> sampled SDF
            return brickSample(h >> 2u, p, mn, sz);
        }
        if (ty == 2u)                 // 0xFFFFFFFE solid terminal
            return -pc.b.y;
        if (ty == 3u) {               // 0xFFFFFFFF empty terminal - conservative sdBox, not 6.0 (voxel-only fix for thin-log tunneling)
            float d = sdBox(p, mn, mn + sz);
            return max(-d, pc.b.y * 0.5);
        }
        // node
        uint ni = h >> 2u;
        uint pl = uPayload[ni];
        vec3 halfS = sz * 0.5;
        vec3 r2 = p - mn;
        int oct = (r2.x >= halfS.x ? 1 : 0) | (r2.y >= halfS.y ? 2 : 0) |
                  (r2.z >= halfS.z ? 4 : 0);
        if (((pl >> (8u + uint(oct))) & 1u) != 0u)
            return -pc.b.y;           // fully-solid child octant
        uint ch = uHandles[uChildBase[ni] + uint(oct)];
        if (ch == 0xFFFFFFFFu) {
            vec3 cmin = mn + vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
            float d = sdBox(p, cmin, cmin + halfS);
            return max(-d, pc.b.y * 0.5);
        }
        if (ch == 0xFFFFFFFEu)
            return -pc.b.y;
        h = ch;
        mn += vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
        sz = halfS;
    }
    return 6.0;
}


// full material byte of the brick cell at p (bit 7 = object surface).
// Returns 0xFF when p does not resolve to a brick (air/solid terminal).
uint brickMatByte(vec3 p)
{
    vec3 wmin = vec3(-pc.b.x * 0.5);
    vec3 rel = p - wmin;
    float chunkM = pc.b.x / pc.b.z;
    ivec3 cc = ivec3(floor(rel / chunkM));
    cc = clamp(cc, ivec3(0), ivec3(int(pc.b.z) - 1));
    int root = uGrid[(cc.z * int(pc.b.z) + cc.y) * int(pc.b.z) + cc.x];
    if (root == -1)
        return 0xFFu;
    uint h = uint(root);
    vec3 mn = wmin + vec3(cc) * chunkM;
    vec3 sz = vec3(chunkM);
    for (int guard = 0; guard < 12; ++guard) {
        if ((h & 3u) == 1u) {
            uint bi = h >> 2;
            float cellSz = sz.x / float(BRICK_N);
            vec3 g = clamp((p - mn) / cellSz - 0.5, vec3(0.0), vec3(BRICK_N - 1.001));
            ivec3 i0 = ivec3(g);
            uint vi = bi * BRICK_WORDS + (uint(i0.z) * BRICK_N * BRICK_N +
                                          uint(i0.y) * BRICK_N + uint(i0.x)) *
                                             2 + 1;
            return (uBricks[vi] >> 24) & 255u;
        }
        if ((h & 3u) != 0u)
            break; // solid/empty terminal
        uint ni = h >> 2;
        vec3 halfS = sz * 0.5;
        vec3 r2 = p - mn;
        int oct = (r2.x >= halfS.x ? 1 : 0) | (r2.y >= halfS.y ? 2 : 0) |
                  (r2.z >= halfS.z ? 4 : 0);
        h = uHandles[uChildBase[ni] + oct];
        mn += vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
        sz = halfS;
    }
    return 0xFFu;
}

bool isObjectSurface(vec3 p)
{
    return ((brickMatByte(p) & 0x80u) != 0u) && map(p) < p.y - heightAt(p.xz) + 0.02;
}

uint getMaterialId(vec3 p)
{
    // objects: material baked into each brick cell by the CPU bake
    if (isObjectSurface(p))
        return brickMatByte(p) & 0x7Fu;
    // terrain: material straight from the records-derived height texture
    return heightMatNearest(p.xz);
}

vec3 calcNormal(vec3 p)
{
    // objects get SVO gradient normals; terrain keeps the smooth heightfield normal
    if (isObjectSurface(p)) {
        float eo = 0.03;
        vec3 g = vec3(map(p + vec3(eo, 0, 0)) - map(p - vec3(eo, 0, 0)),
                      map(p + vec3(0, eo, 0)) - map(p - vec3(0, eo, 0)),
                      map(p + vec3(0, 0, eo)) - map(p - vec3(0, 0, eo)));
        // Guard against a degenerate gradient (deep inside a solid terminal
        // where the SDF is flat). Without this, normalize() yields NaN and the
        // face shades black - the "hollow" artefact. Fall back to the dominant
        // world axis so the surface still reads as solid, never as a hole.
        float gl = length(g);
        if (gl < 1e-4)
            return vec3(0.0, 1.0, 0.0);
        return normalize(g);
    }
    // analytic heightfield normal - identical in both backends so shading
    // stays independent of each field's sampling resolution.
    // Two scales: fine detail (grass bumps) blended with broad form.
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

// ---------------------------------------------------------------------------
// Exact SVO / DDA traversal
// The previous sphere-trace stepped on an interpolated SDF which over-estimates
// distance in concave / corner regions, so thin voxels were tunnelled and cube
// corners were rounded ("cut off"). This traversal visits every voxel in strict
// ray order (3D-DDA over the chunk grid, then the octree, then the brick's
// voxels) and returns the *first* solid voxel - nothing is skipped, and corners
// are sharp because the test is a per-voxel sign check, not an interpolated SDF.
// ---------------------------------------------------------------------------
void boxTSlab(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tEnter, out float tExit)
{
    vec3 t0 = vec3(-1e30), t1 = vec3(1e30);
    for (int a = 0; a < 3; ++a) {
        if (abs(rd[a]) < 1e-9) {
            if (ro[a] < bmin[a] || ro[a] > bmax[a]) { tEnter = 1e30; tExit = -1e30; return; }
        } else {
            float ta = (bmin[a] - ro[a]) / rd[a];
            float tb = (bmax[a] - ro[a]) / rd[a];
            t0[a] = min(ta, tb);
            t1[a] = max(ta, tb);
        }
    }
    tEnter = max(max(t0.x, t0.y), t0.z);
    tExit  = min(min(t1.x, t1.y), t1.z);
}

bool brickDDAB(vec3 ro, vec3 rd, vec3 rdi, uint bi, vec3 bmin, float bsz,
               float tA, float tB, out float tHit)
{
    float vsz = bsz / float(BRICK_N);
    float t = max(tA, 0.0);
    vec3 p = ro + rd * t;
    vec3 vcell = floor((p - bmin) / vsz);
    vec3 stp = vec3(rd.x >= 0.0 ? 1.0 : -1.0, rd.y >= 0.0 ? 1.0 : -1.0, rd.z >= 0.0 ? 1.0 : -1.0);
    vec3 tDelta = abs(rdi) * vsz;
    vec3 nextB;
    nextB.x = (stp.x > 0.0) ? (bmin.x + (vcell.x + 1.0) * vsz) : (bmin.x + vcell.x * vsz);
    nextB.y = (stp.y > 0.0) ? (bmin.y + (vcell.y + 1.0) * vsz) : (bmin.y + vcell.y * vsz);
    nextB.z = (stp.z > 0.0) ? (bmin.z + (vcell.z + 1.0) * vsz) : (bmin.z + vcell.z * vsz);
    vec3 tMax = (nextB - ro) * rdi;
    for (int i = 0; i < BRICK_N * BRICK_N * BRICK_N + 16; ++i) {
        if (t > tB) return false;
        ivec3 vc = ivec3(clamp(vcell, vec3(0.0), vec3(float(BRICK_N) - 1.0)));
        float sdf = brickVoxelSdf(bi, vc);   // signed metres, < 0 inside solid
        if (sdf <= 0.0) { tHit = t; return true; }
        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) { t = tMax.x; tMax.x += tDelta.x; vcell.x += stp.x; }
            else { t = tMax.z; tMax.z += tDelta.z; vcell.z += stp.z; }
        } else {
            if (tMax.y < tMax.z) { t = tMax.y; tMax.y += tDelta.y; vcell.y += stp.y; }
            else { t = tMax.z; tMax.z += tDelta.z; vcell.z += stp.z; }
        }
    }
    return false;
}

void traverseSVONode(vec3 ro, vec3 rd, vec3 rdi, uint h, vec3 nmin, float sz,
                     float tEnter, float tExit, inout float bestT, inout bool found)
{
    const int MAXS = 256;
    uint  sh[MAXS]; vec3 smin[MAXS]; float ssz[MAXS]; float se[MAXS]; float sx[MAXS];
    int sp = 0;
    if (sp < MAXS) { sh[sp] = h; smin[sp] = nmin; ssz[sp] = sz; se[sp] = tEnter; sx[sp] = tExit; sp++; }

    while (sp > 0) {
        sp--;
        uint hh = sh[sp];
        vec3 curMin = smin[sp];
        float curSz = ssz[sp];
        float curEn = se[sp];
        float curEx = sx[sp];
        if (found && curEn >= bestT) continue;   // cannot improve best
        if (curEn > curEx) continue;

        uint ty = hh & 3u;
        if (ty == 3u) continue;                  // empty terminal
        if (ty == 2u) {                          // solid terminal
            if (curEn < bestT) { bestT = curEn; found = true; }
            continue;
        }
        if (ty == 1u) {                          // brick -> per-voxel DDA
            float th;
            if (brickDDAB(ro, rd, rdi, hh >> 2u, curMin, curSz, curEn, curEx, th)) {
                if (th < bestT) { bestT = th; found = true; }
            }
            continue;
        }
        // node
        uint ni = hh >> 2u;
        uint pl = uPayload[ni];
        uint validMask = pl & 0xFFu;
        uint solidMask = (pl >> 8) & 0xFFu;
        uint cbase = uChildBase[ni];
        // distance LOD: a node that projects to <= ~1px can be collapsed to its
        // solid content with no visible change. This is octree LOD in the
        // raymarch - distant solid subtrees stop descending, so far geometry
        // (terrain masses, big objects) costs a fraction of the steps.
        float pixAng = (2.0f * pc.a.x) / max(pc.a.w, 1.0f);
        if (curSz / max(curEn, 1e-3f) < 0.9f * pixAng) {
            if (((solidMask | validMask) != 0u) && curEn < bestT) {
                bestT = curEn; found = true;
            }
            continue;
        }
        float hsz = curSz * 0.5;
        float ten[8]; uint ctgt[8]; vec3 cmin_t[8]; float tex_t[8]; int cnt = 0;
        for (int o = 0; o < 8; ++o) {
            uint om = uint(1 << o);
            bool solid = ((solidMask & om) != 0u);
            bool valid = ((validMask & om) != 0u);
            if (!solid && !valid) continue;
            vec3 cmin_o = curMin + vec3((o & 1) != 0 ? hsz : 0.0,
                                        (o & 2) != 0 ? hsz : 0.0,
                                        (o & 4) != 0 ? hsz : 0.0);
            vec3 cmax_o = cmin_o + vec3(hsz);
            float te, tx;
            boxTSlab(ro, rd, cmin_o, cmax_o, te, tx);
            float tce = max(curEn, te);
            float tcx = min(curEx, tx);
            if (tce > tcx) continue;
            uint chh;
            if (solid) chh = 0xFFFFFFFEu;
            else {
                uint ch = uHandles[cbase + uint(o)];
                if (ch == 0xFFFFFFFFu) continue;
                chh = (ch == 0xFFFFFFFEu) ? 0xFFFFFFFEu : ch;
            }
            ten[cnt] = tce; ctgt[cnt] = chh; cmin_t[cnt] = cmin_o; tex_t[cnt] = tcx; cnt++;
        }
        // sort descending by entry t so the LIFO stack pops nearest-first
        for (int a = 0; a < cnt; ++a)
            for (int b = a + 1; b < cnt; ++b)
                if (ten[b] > ten[a]) {
                    float ft = ten[a]; ten[a] = ten[b]; ten[b] = ft;
                    uint ut = ctgt[a]; ctgt[a] = ctgt[b]; ctgt[b] = ut;
                    vec3 vt = cmin_t[a]; cmin_t[a] = cmin_t[b]; cmin_t[b] = vt;
                    float xt = tex_t[a]; tex_t[a] = tex_t[b]; tex_t[b] = xt;
                }
        for (int a = 0; a < cnt; ++a) {
            if (sp < MAXS) {
                sh[sp] = ctgt[a]; smin[sp] = cmin_t[a]; ssz[sp] = hsz;
                se[sp] = ten[a]; sx[sp] = tex_t[a]; sp++;
            }
        }
    }
}

bool exactSVOHit(vec3 ro, vec3 rd, float tStart, float tEnd, out float tHit)
{
    vec3 rdi = 1.0 / rd;
    float W = pc.b.x;
    vec3 wmin = vec3(-W * 0.5);
    float chunkM = W / pc.b.z;
    int GN = int(pc.b.z);
    float t = tStart;
    vec3 p = ro + rd * t;
    vec3 cell = floor((p - wmin) / chunkM);
    vec3 stp = vec3(rd.x >= 0.0 ? 1.0 : -1.0, rd.y >= 0.0 ? 1.0 : -1.0, rd.z >= 0.0 ? 1.0 : -1.0);
    vec3 tDelta = abs(rdi) * chunkM;
    vec3 nextB;
    nextB.x = (stp.x > 0.0) ? (wmin.x + (cell.x + 1.0) * chunkM) : (wmin.x + cell.x * chunkM);
    nextB.y = (stp.y > 0.0) ? (wmin.y + (cell.y + 1.0) * chunkM) : (wmin.y + cell.y * chunkM);
    nextB.z = (stp.z > 0.0) ? (wmin.z + (cell.z + 1.0) * chunkM) : (wmin.z + cell.z * chunkM);
    vec3 tMax = (nextB - ro) * rdi;

    float bestT = 1e30f;
    bool found = false;
    for (int citer = 0; citer < 4096; ++citer) {
        if (t > tEnd) { tHit = bestT; return found; }
        if (found && t >= bestT) { tHit = bestT; return found; }
        ivec3 ci = ivec3(cell);
        if (ci.x >= 0 && ci.y >= 0 && ci.z >= 0 && ci.x < GN && ci.y < GN && ci.z < GN) {
            int idx = ci.z * GN * GN + ci.y * GN + ci.x;
            int root = uGrid[idx];
            if (root >= 0) {
                vec3 cmin = wmin + vec3(ci) * chunkM;
                float tce, tcx;
                boxTSlab(ro, rd, cmin, cmin + vec3(chunkM), tce, tcx);
                float tEn = max(t, tce);
                float tEx = min(tEnd, tcx);
                if (tEn <= tEx)
                    traverseSVONode(ro, rd, rdi, uint(root), cmin, chunkM, tEn, tEx, bestT, found);
            }
        }
        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) { t = tMax.x; tMax.x += tDelta.x; cell.x += stp.x; }
            else { t = tMax.z; tMax.z += tDelta.z; cell.z += stp.z; }
        } else {
            if (tMax.y < tMax.z) { t = tMax.y; tMax.y += tDelta.y; cell.y += stp.y; }
            else { t = tMax.z; tMax.z += tDelta.z; cell.z += stp.z; }
        }
    }
    tHit = bestT;
    return found;
}

float softShadow(vec3 ro, vec3 rd)
{
    // Exact SVO/DDA occlusion: any solid voxel between the surface and the sun
    // fully occludes. Replaces the conservative sphere-trace, which could
    // over-step and let thin occluders leak light (no voxel is ever missed now).
    float th;
    if (exactSVOHit(ro, rd, 0.05, 60.0, th))
        return 0.0;
    return 1.0;
}

vec3 brickAlbedo(vec3 p)
{
    vec3 wmin = vec3(-pc.b.x * 0.5);
    vec3 rel = p - wmin;
    float chunkM = pc.b.x / pc.b.z;
    ivec3 cc = ivec3(floor(rel / chunkM));
    cc = clamp(cc, ivec3(0), ivec3(int(pc.b.z) - 1));
    int root = uGrid[(cc.z * int(pc.b.z) + cc.y) * int(pc.b.z) + cc.x];
    if (root == -1)
        return vec3(0.5);
    uint h = uint(root);
    vec3 mn = wmin + vec3(cc) * chunkM;
    vec3 sz = vec3(chunkM);
    for (int guard = 0; guard < 12; ++guard) {
        if ((h & 3u) == 1u) {
            uint bi = h >> 2;
            float cellSz = sz.x / float(BRICK_N);
            vec3 g = clamp((p - mn) / cellSz - 0.5, vec3(0.0), vec3(BRICK_N - 1.001));
            ivec3 i0 = ivec3(g);
            uint vi = bi * BRICK_WORDS + (uint(i0.z) * BRICK_N * BRICK_N +
                                           uint(i0.y) * BRICK_N + uint(i0.x)) *
                                              2;
            uint v = uBricks[vi];
            return vec3(float(v & 255u), float((v >> 8) & 255u), float((v >> 16) & 255u)) / 255.0;
        }
        if ((h & 3u) != 0u)
            break; // solid/empty terminal
        uint ni = h >> 2;
        vec3 halfS = sz * 0.5;
        vec3 r2 = p - mn;
        int oct = (r2.x >= halfS.x ? 1 : 0) | (r2.y >= halfS.y ? 2 : 0) |
                  (r2.z >= halfS.z ? 4 : 0);
        h = uHandles[uChildBase[ni] + oct];
        mn += vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
        sz = halfS;
    }
    return vec3(0.5);
}

vec2 brickReflectivity(vec3 p)
{
    vec3 wmin = vec3(-pc.b.x * 0.5);
    vec3 rel = p - wmin;
    float chunkM = pc.b.x / pc.b.z;
    ivec3 cc = ivec3(floor(rel / chunkM));
    cc = clamp(cc, ivec3(0), ivec3(int(pc.b.z) - 1));
    int root = uGrid[(cc.z * int(pc.b.z) + cc.y) * int(pc.b.z) + cc.x];
    if (root == -1)
        return vec2(0.15, 0.9);
    uint h = uint(root);
    vec3 mn = wmin + vec3(cc) * chunkM;
    vec3 sz = vec3(chunkM);
    for (int guard = 0; guard < 12; ++guard) {
        if ((h & 3u) == 1u) {
            uint bi = h >> 2;
            float cellSz = sz.x / float(BRICK_N);
            vec3 g = clamp((p - mn) / cellSz - 0.5, vec3(0.0), vec3(BRICK_N - 1.001));
            ivec3 i0 = ivec3(g);
            uint vi = bi * BRICK_WORDS + (uint(i0.z) * BRICK_N * BRICK_N +
                                           uint(i0.y) * BRICK_N + uint(i0.x)) *
                                              2 + 1;
            uint v = uBricks[vi];
            float refl = float((v >> 8) & 255u) / 255.0;
            float rough = float((v >> 16) & 255u) / 255.0;
            return vec2(refl, rough);
        }
        if ((h & 3u) != 0u)
            break;
        uint ni = h >> 2;
        vec3 halfS = sz * 0.5;
        vec3 r2 = p - mn;
        int oct = (r2.x >= halfS.x ? 1 : 0) | (r2.y >= halfS.y ? 2 : 0) |
                  (r2.z >= halfS.z ? 4 : 0);
        h = uHandles[uChildBase[ni] + oct];
        mn += vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
        sz = halfS;
    }
    return vec2(0.15, 0.9);
}

float sceneMap(vec3 p) { return map(p); }
// multi-scale SDF ambient occlusion with bent normal
void sdfAO(vec3 p, vec3 n, out float ao, out vec3 bent)
{
    const float rad[3] = float[3](0.14, 0.45, 1.25);
    vec3 tv = cross(n, vec3(0.0001, 1.0, 0.0001));
    vec3 tang = normalize(tv);
    vec3 bitan = normalize(cross(n, tang));
    float occ = 0.0, wsum = 0.0;
    vec3 bd = n * 0.5;
    for (int ring = 0; ring < 3; ++ring) {
        float r = rad[ring];
        for (int a = 0; a < 4; ++a) {
            float ang = float(a) * 1.5708 + float(ring) * 0.785;
            vec3 dir = normalize(n * 0.85 + (cos(ang) * tang + sin(ang) * bitan) * 0.7);
            float s = sceneMap(p + dir * r);
            float fall = clamp(1.0 - s / r, 0.0, 1.0);
            fall = fall * fall * (3.0 - 2.0 * fall);
            float w = 1.0 / (1.0 + fall * fall * 4.0);
            occ += w * fall;
            wsum += w;
            bd += dir * w * (1.0 - fall);
        }
    }
    ao = clamp(1.0 - 0.85 * occ / max(wsum, 1e-4), 0.0, 1.0);
    bent = normalize(bd);
}

// shaded submerged terrain floor, reflected by the water surface. Purely
// terrain-derived (heightfield normal, palette material, terrain-only shadow)
// so the water's base look never changes when objects are added or removed.
vec3 shadeFloor(vec3 q, vec3 r)
{
    uint mId = heightMatNearest(q.xz);
    vec3 n = calcNormal(q);
    float sh = ((gRenderFlags & 2) != 0) ? softShadowTerrain(q + n * 0.3, kSunDir) : 1.0;
    float ndl = max(dot(n, kSunDir), 0.0);
    vec3 alb = kPalette[mId];
    vec2 rr = kMatRefl[mId];
    vec3 V = -r;
    vec3 h = normalize(kSunDir + V);
    float vdh = max(dot(V, h), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    vec3 f0 = vec3(0.04) + vec3(rr.x) * 0.70;
    vec3 F = fresnelSchlick(f0, vdh);
    float fAvg = (F.r + F.g + F.b) * 0.3333;
    vec3 spec = pbrSpec(n, V, kSunDir, f0, rough);
    vec3 amb = skyIrradiance(n) * 0.5;
    vec3 col = alb * (1.0 - fAvg) * (kSunCol * ndl * sh + amb)
         + spec * kSunCol * ndl * sh;
    col += (mId >= 9u && mId <= 15u) ? kEmissive[mId] : vec3(0.0);
    return col;
}
vec3 shadeTerrain(vec3 p, vec3 rd, vec3 alb, vec2 rr, vec3 ro)
{
    uint mId = getMaterialId(p);
    vec3 n = calcNormal(p);
    float sh = ((gRenderFlags & 2) != 0) ? softShadow(p + n * 0.35, kSunDir) : 1.0;

    float ao = 1.0;
    vec3 bent = n;
    if ((gRenderFlags & 1) != 0)
        sdfAO(p, n, ao, bent);

    applyFlora(p, rd, ro, mId, false, 0.0, alb, n, sh, ao);

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

    // backlit foliage translucency (canopy scatters light through leaves)
    if ((gRenderFlags & 4) != 0 && mId == 8u) {
        float back = pow(1.0 - max(dot(n, kSunDir), 0.0), 2.0);
        float thick = clamp(0.5 - sceneMap(p + n * 0.3) * 2.0, 0.0, 1.0);
        col += alb * kSunCol * back * (1.0 - thick * 0.85) * 0.55;
    }

    if (p.y < kWaterLevel && underWater(p.xz) && !gUnderwater) {
        float depth = kWaterLevel - p.y;
        col *= exp(-depth * vec3(0.35, 0.18, 0.12) * 3.0);
        col = mix(col, vec3(0.05, 0.14, 0.13), clamp(depth * 0.8, 0.0, 0.85));
    }
    col += (mId >= 9u && mId <= 15u) ? kEmissive[mId] : vec3(0.0);
    return col;
}