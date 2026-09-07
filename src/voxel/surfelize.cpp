#include "surfelize.hpp"
#include <numeric>
#include <core/log.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace vf::voxel {

namespace {

constexpr int kSurfGridN = 16;
constexpr int kChunkSize = 64;

// NaN-proof normalize: glm::normalize of a zero vector yields NaN (0/0),
// which then poisons == comparisons, sorting keys derived from it, and the
// GPU. Any degenerate input falls back to +Y.
inline glm::vec3 safeNormalize(glm::vec3 v) {
    const float l2 = glm::dot(v, v);
    if (!(l2 > 1e-12f) || !std::isfinite(l2))
        return glm::vec3(0.0f, 1.0f, 0.0f);
    return v * (1.0f / std::sqrt(l2));
}

inline uint64_t packKey(int x, int y, int z) {
    return (uint64_t(std::uint32_t(x)) << 20) | (uint32_t(y) << 10) | uint32_t(z);
}

// Deterministic 0..1 hash from lattice coords + slot (no RNG state, so two
// builds are bit-identical). Wraps sin-hash like the shader hashN.
inline float microHash(int x, int y, int z, int slot)
{
    float h = sinf(float(x) * 12.9898f + float(y) * 78.233f +
                   float(z) * 37.719f + float(slot) * 11.13f) * 43758.55f;
    return h - floorf(h);
}

inline void unpackKey(uint64_t k, int& x, int& y, int& z) {
    x = int((k >> 20) & 0x3FFu);
    y = int((k >> 10) & 0x3FFu);
    z = int(k & 0x3FFu);
}

inline int chunkIndex(int x, int y, int z) {
    const int cx = x / kChunkSize, cy = y / kChunkSize, cz = z / kChunkSize;
    return (cx * kSurfGridN + cy) * kSurfGridN + cz;
}

bool terrainSurface(const VoxelField& field, int x, int y, int z) {
    const int latN = field.latN();
    if (y < 0 || y >= latN || x < 0 || x >= latN || z < 0 || z >= latN) return false;
    const int16_t colTop = field.colTops()[size_t(z) * latN + size_t(x)];
    if (colTop < 0 || y > colTop) return false;
    if (y == colTop) return true;
    if (x > 0) {
        int16_t n = field.colTops()[size_t(z) * latN + size_t(x - 1)];
        if (n < 0 || n < y) return true;
    }
    if (x < latN - 1) {
        int16_t n = field.colTops()[size_t(z) * latN + size_t(x + 1)];
        if (n < 0 || n < y) return true;
    }
    if (z > 0) {
        int16_t n = field.colTops()[size_t(z - 1) * latN + size_t(x)];
        if (n < 0 || n < y) return true;
    }
    if (z < latN - 1) {
        int16_t n = field.colTops()[size_t(z + 1) * latN + size_t(x)];
        if (n < 0 || n < y) return true;
    }
    return false;
}

float heightAt(const VoxelField& field, float wx, float wz) {
    const auto& ht = field.heightTexture();
    const int sz = field.latN();
    if (sz <= 0 || ht.size() < size_t(sz) * size_t(sz)) return -1e9f;
    const glm::vec2 tc = glm::clamp(glm::vec2(wx, wz) / 102.4f + 0.5f,
                                        glm::vec2(0.0f), glm::vec2(1.0f))
                          * glm::vec2(sz - 1);
    const glm::ivec2 i0(glm::floor(tc)), i1(glm::min(i0 + 1, sz - 1));
    const glm::vec2 f = tc - glm::vec2(i0);
    const float h00 = ht[size_t(i0.y) * sz + size_t(i0.x)].r;
    const float h10 = ht[size_t(i0.y) * sz + size_t(i1.x)].r;
    const float h01 = ht[size_t(i1.y) * sz + size_t(i0.x)].r;
    const float h11 = ht[size_t(i1.y) * sz + size_t(i1.x)].r;
    return glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.y), f.y);
}

glm::vec3 heightfieldNormal(const VoxelField& field, glm::vec3 p) {
    const float e = 0.35f, e2 = 0.10f;
    const glm::vec3 nWide = safeNormalize(glm::vec3(
        heightAt(field, p.x - e, p.z) - heightAt(field, p.x + e, p.z),
        2.0f * e,
        heightAt(field, p.x, p.z - e) - heightAt(field, p.x, p.z + e)));
    const glm::vec3 nFine = safeNormalize(glm::vec3(
        heightAt(field, p.x - e2, p.z) - heightAt(field, p.x + e2, p.z),
        2.0f * e2,
        heightAt(field, p.x, p.z - e2) - heightAt(field, p.x, p.z + e2)));
    return safeNormalize(glm::mix(nFine, nWide, 0.55f));
}

glm::vec3 meanNormal(const VoxelField& field, int x, int y, int z) {    const int latN = field.latN();
    glm::vec3 n(0.0f);
    const int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (auto &d : dirs) {
        const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < 0 || nx >= latN || nz < 0 || nz >= latN || ny < 0 || ny >= latN) {
            n += glm::vec3(float(d[0]), float(d[1]), float(d[2]));
            continue;
        }
        if (field.sample(nx, ny, nz).d > 0.0f)
            n += glm::vec3(float(d[0]), float(d[1]), float(d[2]));
    }
    const float len = glm::length(n);
    return len > 1e-6f ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Binary sun-occlusion march over field.sample: exact Amanatides DDA over
// lattice cells for the first 3 m (visits every cell like the SVO
// reference, so thin eaves/logs are never tunnelled), then sphere-tracing
// with tight clamps to 60 m. 0 = occluded, 1 = clear.
float shadowMarch(const VoxelField& f, glm::vec3 ro, glm::vec3 rd)
{
    // Phase 1: cell-exact DDA to 3 m. sampleWorld floors to the cell, so
    // testing every visited cell matches the voxel truth (SVO parity).
    {
        glm::vec3 cell = glm::floor((ro + 51.2f) / 0.1f);
        const glm::vec3 stp = glm::vec3(rd.x >= 0.0f ? 1.0f : -1.0f,
                                        rd.y >= 0.0f ? 1.0f : -1.0f,
                                        rd.z >= 0.0f ? 1.0f : -1.0f);
        const glm::vec3 rdi = glm::vec3(1.0f) / rd;
        glm::vec3 tMax = ((cell + (stp * 0.5f + 0.5f)) * 0.1f - 51.2f - ro) * rdi;
        const glm::vec3 tDelta = glm::abs(rdi) * 0.1f;
        float t = 0.05f;
        for (int i = 0; i < 64; ++i) {
            const glm::vec3 sp = ro + rd * t;
            if (f.sampleWorld(sp).d < -0.02f)
                return 0.0f;
            // advance to next cell boundary (NaN-safe: rdi inf -> huge tMax)
            float tn = tMax.x;
            int ax = 0;
            if (tMax.y < tn) { tn = tMax.y; ax = 1; }
            if (tMax.z < tn) { tn = tMax.z; ax = 2; }
            if (!(tn > t))
                tn = t + 0.05f;
            t = tn + 1e-4f;
            if (ax == 0) { cell.x += stp.x; tMax.x += tDelta.x; }
            else if (ax == 1) { cell.y += stp.y; tMax.y += tDelta.y; }
            else { cell.z += stp.z; tMax.z += tDelta.z; }
            if (t > 3.0f)
                break;
        }
    }
    // Phase 2: sphere-tracing to 60 m for hills/treelines.
    {
        float t = 3.0f;
        for (int i = 0; i < 80; ++i) {
            const glm::vec3 sp = ro + rd * t;
            const float d = f.sampleWorld(sp).d;
            if (d < -0.02f)
                return 0.0f;
            t += glm::clamp(std::fabs(d) * 0.7f, 0.04f, 1.0f);
            if (t > 60.0f)
                break;
        }
    }
    return 1.0f;
}

// Few-tap bent-normal AO over the exact oracle (mirrors splatAO's rings).
void aoBake(const VoxelField& f, glm::vec3 p, glm::vec3 n, float& ao, glm::vec3& bent)
{
    const float rad[2] = { 0.18f, 0.60f };
    glm::vec3 tv = glm::cross(n, glm::vec3(0.0001f, 1.0f, 0.0001f));
    glm::vec3 tang = safeNormalize(tv + glm::vec3(1e-5f, 0.0f, 0.0f));
    glm::vec3 bitan = safeNormalize(glm::cross(n, tang));
    float occ = 0.0f, wsum = 0.0f;
    glm::vec3 bd = n * 0.5f;
    for (int ring = 0; ring < 2; ++ring) {
        const float r = rad[ring];
        for (int a = 0; a < 4; ++a) {
            const float ang = float(a) * 1.5708f + float(ring) * 0.785f;
            glm::vec3 dir = safeNormalize(n * 0.85f +
                (std::cos(ang) * tang + std::sin(ang) * bitan) * 0.7f);
            const float s = f.sampleWorld(p + dir * r).d;
            float fall = glm::clamp(1.0f - s / r, 0.0f, 1.0f);
            fall = fall * fall * (3.0f - 2.0f * fall);
            const float w = 1.0f / (1.0f + fall * fall * 4.0f);
            occ += w * fall;
            wsum += w;
            bd += dir * w * (1.0f - fall);
        }
    }
    ao = glm::clamp(1.0f - 0.85f * occ / std::max(wsum, 1e-4f), 0.0f, 1.0f);
    bent = safeNormalize(bd + glm::vec3(1e-6f, 0.0f, 0.0f));
}

} // namespace

SurfelSet buildSurfels(const VoxelField& field, const SurfelParams& params) {
    SurfelSet set;
    const auto t0 = std::chrono::steady_clock::now();
    set.chunkRange.assign(size_t(kSurfGridN * kSurfGridN * kSurfGridN) + 1, 0);

    std::vector<uint64_t> keys;
    keys.reserve(1200000);

    const int latN = field.latN();
    for (int z = 0; z < latN; ++z) {
        for (int x = 0; x < latN; ++x) {
            const int16_t colTop = field.colTops()[size_t(z) * latN + size_t(x)];
            if (colTop < 0)
                continue;
            // A cell is laterally exposed only where a neighbour column is
            // lower, so the surface range is [minNbrTop, colTop] (flat
            // ground: just the top cell; canyon walls: the full exposed
            // face). This skips ~550 buried cells per column on average.
            int16_t minNbr = colTop;
            if (x > 0)
                minNbr = std::min(minNbr, field.colTops()[size_t(z) * latN + size_t(x - 1)]);
            if (x < latN - 1)
                minNbr = std::min(minNbr, field.colTops()[size_t(z) * latN + size_t(x + 1)]);
            if (z > 0)
                minNbr = std::min(minNbr, field.colTops()[size_t(z - 1) * latN + size_t(x)]);
            if (z < latN - 1)
                minNbr = std::min(minNbr, field.colTops()[size_t(z + 1) * latN + size_t(x)]);
            if (minNbr < 0)
                minNbr = 0;
            for (int y = minNbr; y <= colTop; ++y) {
                if (terrainSurface(field, x, y, z))
                    keys.push_back(packKey(x, y, z));
            }
        }
    }

    const auto& blk = field.objectBlockMask();
    const int nBlocks = VoxelField::globalBlocks(); // 256
    const int blkSize = VoxelField::blockSize();    // 4
    if (blk.size() >= size_t(nBlocks) * size_t(nBlocks) * size_t(nBlocks)) {
    for (int bz = 0; bz < nBlocks; ++bz)
    for (int by = 0; by < nBlocks; ++by)
    for (int bx = 0; bx < nBlocks; ++bx) {
        if (blk[(size_t(bz) * nBlocks + size_t(by)) * nBlocks + size_t(bx)] == 0) continue;
        const int baseX = bx * blkSize, baseY = by * blkSize, baseZ = bz * blkSize;
        for (int dz = 0; dz < blkSize; ++dz)
        for (int dy = 0; dy < blkSize; ++dy)
        for (int dx = 0; dx < blkSize; ++dx) {
            const int cx = baseX + dx, cy = baseY + dy, cz = baseZ + dz;
            if (cx < 0 || cx >= latN || cy < 0 || cy >= latN || cz < 0 || cz >= latN) continue;
            const auto s = field.sample(cx, cy, cz);
            if (s.d <= 0.0f && s.obj)
                keys.push_back(packKey(cx, cy, cz));
        }
    }
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const auto tEnum = std::chrono::steady_clock::now();

    const float baseR = params.baseRadius;
    const float voxel = 0.1f;
    const int n = int(keys.size());
    set.terrainCount = 0;
    set.objectCount = 0;

    std::vector<glm::vec3> rawNormals(n);
    std::vector<Surfel> surfels(n);

    // Pass 1: raw mean normals (sharded). Smoothing lookups below use
    // binary search over the sorted keys (lock-free, no hash build).
    {
        // meanNormal() is 6 lock-free field samples per cell: shard it
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                for (;;) {
                    const int i = next.fetch_add(1);
                    if (i >= n)
                        return;
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);
                    rawNormals[i] = meanNormal(field, x, y, z);
                }
            });
        for (auto& th : threads)
            th.join();
    }

    // Pass 2: blend, smooth, bake shadow+AO, and emit (also sharded; every
    // index is independent and keys/rawNormals are read-only here).
    const glm::vec3 sunDir = glm::normalize(params.sunDir);
    std::atomic<size_t> terrainCount{ 0 }, objectCount{ 0 };
    {
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                size_t locTerrain = 0, locObject = 0;
                for (;;) {
                    const int i = next.fetch_add(1);
                    if (i >= n)
                        break;
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);

                    const auto s = field.sample(x, y, z);
                    glm::vec3 n = rawNormals[i];

                    if (params.terrainHeightfieldNormals) {
                        const int16_t colTop =
                            field.colTops()[size_t(z) * latN + size_t(x)];
                        if (colTop >= 0 && y == colTop) {
                            const glm::vec3 hn = heightfieldNormal(
                                field,
                                glm::vec3(-51.2f + (x + 0.5f) * voxel,
                                          -51.2f + (y + 0.5f) * voxel,
                                          -51.2f + (z + 0.5f) * voxel));
                            n = safeNormalize(glm::mix(n, hn, params.heightfieldBlend));
                        }
                    }

                    // rChaos grows disks where neighbour normals disagree
                    // (wedges that leak); 1.0 on agreed patches.
                    float rChaos = 1.0f;
                    if (params.smoothNormals) {
                        glm::vec3 acc = n;
                        float wsum = 1.0f;
                        const int faceDirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 },
                                                     { 0, 1, 0 }, { 0, -1, 0 },
                                                     { 0, 0, 1 }, { 0, 0, -1 } };
                        glm::vec3 agr = rawNormals[i];
                        float agrN = 1.0f;
                        for (auto& d : faceDirs) {
                            const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                            auto it = std::lower_bound(keys.begin(), keys.end(), nk);
                            if (it != keys.end() && *it == nk) {
                                const glm::vec3 nn = rawNormals[size_t(it - keys.begin())];
                                acc += nn;
                                ++wsum;
                                agr += nn;
                                ++agrN;
                            }
                        }
                        n = safeNormalize(acc / wsum);
                        // local chaos: agreed flat patches keep tight disks,
                        // disagreeing neighbourhoods (wedges that leak) grow
                        const float agreement =
                            glm::clamp(glm::length(agr) / agrN, 0.0f, 1.0f);
                        rChaos = 1.0f + 0.8f * (1.0f - agreement);
                    }

                    const glm::vec3 cellCentre =
                        glm::vec3(-51.2f + (x + 0.5f) * voxel,
                                  -51.2f + (y + 0.5f) * voxel,
                                  -51.2f + (z + 0.5f) * voxel);
                    const glm::vec3 pos = cellCentre + n * (0.5f * voxel);

                    // baked sun shadow (binary march on the exact oracle) +
                    // bent-normal AO; backfaces skip the march (FS gates too).
                    // Origin rides 0.3 above the surface like the SVO backend
                    // (p + n*0.35): a tight offset buries canopy/thin-object
                    // origins inside neighbouring solid and self-shadows.
                    float shadow = 1.0f;
                    if (glm::dot(n, sunDir) > 0.02f)
                        shadow = shadowMarch(field, pos + n * 0.3f, sunDir);
                    float ao = 1.0f;
                    glm::vec3 bent = n;
                    aoBake(field, pos + n * 0.02f, n, ao, bent);

                    const uint8_t mat = s.mat;
                    const float refl = kMaterialReflection[std::min(int(mat), 16)].x;
                    const float rough = kMaterialReflection[std::min(int(mat), 16)].y;

                    // foliage reads as volume (chaotic normals already grow
                    // it via rChaos); cap the combined multiplier
                    const float rr =
                        baseR * std::min((mat == 8 ? 2.0f : 1.0f) * rChaos, 2.2f);
                    Surfel sl;
                    sl.pos_rU = glm::vec4(pos, rr);
                    sl.normal_rV = glm::vec4(n, rr);
                    sl.bent_sh = glm::vec4(bent, shadow);
                    sl.mat_ao = glm::vec4(float(mat), refl, rough, ao);
                    surfels[i] = sl;
                    if (s.obj)
                        ++locObject;
                    else
                        ++locTerrain;
                }
                terrainCount += locTerrain;
                objectCount += locObject;
            });
        for (auto& th : threads)
            th.join();
    }
    set.terrainCount = terrainCount.load();
    set.objectCount = objectCount.load();

    // Shadow penumbra + AO smoothing: the baked march verdict is binary per
    // surfel (draws shadow boundaries along disk shapes) and the baked AO
    // steps at staircase-tap granularity (mottles flat walls). Average both
    // over face-neighbouring surface surfels so all baked fields vary
    // continuously and adjacent same-color splats shade identically.
    // Reads snapshots (order-free, deterministic).
    {
        std::vector<float> shPrev(n), aoPrev(n);
        for (int i = 0; i < n; ++i) {
            shPrev[i] = surfels[i].bent_sh.w;
            aoPrev[i] = surfels[i].mat_ao.w;
        }
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        const int faceDirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 },
                                     { 0, 1, 0 }, { 0, -1, 0 },
                                     { 0, 0, 1 }, { 0, 0, -1 } };
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                for (;;) {
                    const int i = next.fetch_add(1);
                    if (i >= n)
                        break;
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);
                    float acc = shPrev[i];
                    float aoAcc = aoPrev[i];
                    float wsum = 1.0f;
                    for (auto& d : faceDirs) {
                        const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                        auto it = std::lower_bound(keys.begin(), keys.end(), nk);
                        if (it != keys.end() && *it == nk) {
                            const size_t j = size_t(it - keys.begin());
                            acc += shPrev[j];
                            aoAcc += aoPrev[j];
                            ++wsum;
                        }
                    }
                    surfels[i].bent_sh.w = acc / wsum;
                    surfels[i].mat_ao.w = aoAcc / wsum;
                }
            });
        for (auto& th : threads)
            th.join();
    }

    std::vector<uint64_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    // precompute chunk ids once: the comparator runs O(n log n) times and
    // unpackKey+chunkIndex per comparison dominated the build
    std::vector<uint32_t> chunkOf(n);
    for (int i = 0; i < n; ++i) {
        const uint64_t k = keys[i];
        chunkOf[i] = chunkIndex(int((k >> 20) & 0x3FFu), int((k >> 10) & 0x3FFu),
                                int(k & 0x3FFu));
    }
    // counting sort by chunk (4096 buckets, O(n)) instead of std::sort:
    // surfels stay ordered by key within each chunk (stable)
    constexpr uint32_t kChunks = kSurfGridN * kSurfGridN * kSurfGridN;
    std::vector<uint32_t> counts(kChunks, 0);
    for (uint32_t c : chunkOf)
        ++counts[c];
    std::vector<uint32_t> starts(kChunks + 1, 0);
    for (uint32_t c = 0; c < kChunks; ++c)
        starts[c + 1] = starts[c] + counts[c];
    std::vector<uint32_t> cursor = starts;
    for (uint32_t i = 0; i < uint32_t(n); ++i)
        order[cursor[chunkOf[i]]++] = i;
    std::vector<Surfel> sorted(n);
    for (int i = 0; i < n; ++i) sorted[i] = surfels[order[i]];
    surfels = std::move(sorted);
    // base-only prefix sums over key chunks (rebuilt below if micros added)
    for (int i = 0; i < n; ++i) {
        int x, y, z;
        unpackKey(keys[order[i]], x, y, z);
        ++set.chunkRange[chunkIndex(x, y, z) + 1];
    }
    for (int c = 1; c <= kSurfGridN * kSurfGridN * kSurfGridN; ++c)
        set.chunkRange[c] += set.chunkRange[c - 1];

    // ---- micro-detail: texture texels become real micro-surfel geometry ----
    // Deterministic children of the sorted base set (same material, inherited
    // baked shadow/AO/bent so no extra marches): moss puffs, pebbles, bark
    // relief, leaflets and roof-underside fillers that seal the stepped-slab
    // slits seen from below. Micros inherit the base cell's chunk, and
    // because the base loop below walks chunk by chunk the micro stream is
    // already chunk-grouped for the final interleave. Single-threaded and
    // hash-driven: two builds are bit-identical.
    size_t microTerrain = 0, microObject = 0;
    if (params.microDetail && n > 0) {
        std::vector<Surfel> micros;
        micros.reserve(size_t(n) / 2);
        std::vector<uint32_t> microCounts(kChunks, 0);
        for (int i = 0; i < n; ++i) {
            int x, y, z;
            unpackKey(keys[order[i]], x, y, z);
            const Surfel& b = surfels[i];
            const int mat = int(b.mat_ao.x + 0.5f);
            if (mat < 0 || mat > 16)
                continue;
            if (mat >= 9 && mat <= 15)
                continue; // emissive: keep crisp, no fuzz
            glm::vec3 bn(b.normal_rV);
            bn = safeNormalize(bn);
            glm::vec3 bp(b.pos_rU);
            const uint32_t bc = chunkOf[order[i]];
            const bool isObj = field.sample(x, y, z).obj;
            glm::vec3 up = std::fabs(bn.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                   : glm::vec3(1.0f, 0.0f, 0.0f);
            glm::vec3 t = safeNormalize(glm::cross(bn, up));
            glm::vec3 bb = safeNormalize(glm::cross(bn, t));
            auto emit = [&](float o1, float o2, float lift, float tilt,
                            float rScale, float aoMul, int slot) {
                float j1 = microHash(x, y, z, slot * 2 + 101) - 0.5f;
                float j2 = microHash(x, y, z, slot * 2 + 102) - 0.5f;
                glm::vec3 nn = safeNormalize(bn + (t * j1 + bb * j2) * tilt);
                glm::vec3 pp = bp + (t * o1 + bb * o2) + bn * lift;
                Surfel m;
                const float rr = std::max(b.pos_rU.w * rScale, 1e-4f);
                m.pos_rU = glm::vec4(pp, rr);
                m.normal_rV = glm::vec4(nn, rr);
                m.bent_sh = glm::vec4(safeNormalize(glm::vec3(b.bent_sh) + (nn - bn) * 0.5f),
                                      b.bent_sh.w);
                m.mat_ao = glm::vec4(b.mat_ao.x, b.mat_ao.y, b.mat_ao.z,
                                     glm::clamp(b.mat_ao.w * aoMul, 0.0f, 1.0f));
                micros.push_back(m);
                ++microCounts[bc];
                if (isObj)
                    ++microObject;
                else
                    ++microTerrain;
            };
            const float h0 = microHash(x, y, z, 1);
            const float h1 = microHash(x, y, z, 2);
            const float h2 = microHash(x, y, z, 3);
            const float oA = (h1 - 0.5f) * 0.09f;
            const float oB = (h2 - 0.5f) * 0.09f;
            // micro-grain: small dense children (2-6 cm apparent) so
            // close-ups read as moss grain, sand, bark fibre and leaflets
            // instead of flat 10 cm disks. Spawn rates are high on purpose:
            // the renderer distance-culls micros in far chunks.
            if (mat <= 1) { // meadow blades / soil crumbs
                if (h0 < 0.80f)
                    emit(oA, oB, 0.014f, 0.55f, 0.30f, 0.92f, 1);
                if (h2 < 0.30f)
                    emit(-oA, -oB, 0.020f, 0.70f, 0.24f, 0.88f, 11);
            } else if (mat == 2 || mat == 3) { // pebbles / sand grain
                if (h0 < 0.70f)
                    emit(oA, oB, 0.006f, 0.45f, 0.20f + 0.12f * h1, 0.93f, 2);
                if (h2 < 0.30f)
                    emit(-oA * 0.7f, -oB * 0.7f, 0.004f, 0.60f, 0.16f, 0.90f, 12);
            } else if (mat == 4 || mat == 5 || mat == 16) { // rock strata chips
                if (h0 < 0.65f)
                    emit(oA, oB, 0.008f, 0.60f, 0.34f, 0.88f, 3);
                if (h2 < 0.25f)
                    emit(-oA, -oB, 0.012f, 0.80f, 0.26f, 0.85f, 13);
            } else if (mat == 6) { // bark relief along the tangent
                if (h0 < 0.80f)
                    emit(oA * 1.6f, oB * 0.5f, 0.005f, 0.35f, 0.28f, 0.95f, 4);
                if (h2 < 0.35f)
                    emit(-oA * 1.2f, oB * 0.8f, 0.004f, 0.50f, 0.22f, 0.93f, 14);
            } else if (mat == 7) { // roof: seal undersides, moss the tops
                if (bn.y < -0.2f) {
                    emit(0.0f, 0.0f, -0.005f, 0.0f, 1.15f, 1.0f, 5);
                } else {
                    if (h0 < 0.85f)
                        emit(oA, oB, 0.011f, 0.70f, 0.38f, 0.90f, 6);
                    if (h2 < 0.40f)
                        emit(-oA, -oB, 0.015f, 0.90f, 0.30f, 0.86f, 16);
                }
            } else if (mat == 8) { // canopy leaflets: real volume
                if (h0 < 0.90f)
                    emit(oA * 1.3f, oB * 1.3f, 0.008f, 1.20f, 0.38f + 0.20f * h1, 0.90f, 7);
                if (h2 < 0.55f)
                    emit(-oA, -oB, 0.013f, 1.40f, 0.30f, 0.85f, 8);
                if (microHash(x, y, z, 9) < 0.30f)
                    emit(oB, -oA, 0.018f, 1.10f, 0.26f, 0.88f, 19);
            }
        }
        if (!micros.empty()) {
            // interleave: base chunk range first, then that chunk's micros
            // (micros were emitted in base-sorted order, hence chunk-grouped).
            std::vector<Surfel> combined;
            combined.reserve(size_t(n) + micros.size());
            std::vector<uint32_t> newRange(kChunks + 1, 0);
            std::vector<uint32_t> microStart(kChunks + 1, 0);
            size_t mCur = 0;
            size_t mOff = 0;
            // micro chunk boundaries: recount in emission order per chunk
            std::vector<uint32_t> mStarts(kChunks + 1, 0);
            for (uint32_t c = 0; c < kChunks; ++c)
                mStarts[c + 1] = mStarts[c] + microCounts[c];
            for (uint32_t c = 0; c < kChunks; ++c) {
                const uint32_t b0 = set.chunkRange[c], b1 = set.chunkRange[c + 1];
                for (uint32_t i = b0; i < b1; ++i)
                    combined.push_back(surfels[i]);
                microStart[c] = uint32_t(combined.size()); // base end == micro begin
                const uint32_t m0 = mStarts[c], m1 = mStarts[c + 1];
                for (uint32_t i = m0; i < m1; ++i)
                    combined.push_back(micros[i]);
                newRange[c + 1] = uint32_t(combined.size());
                (void)mCur; (void)mOff;
            }
            microStart[kChunks] = uint32_t(combined.size());
            surfels = std::move(combined);
            set.chunkRange = std::move(newRange);
            set.microStart = std::move(microStart);
            set.terrainCount += microTerrain;
            set.objectCount += microObject;
        }
    }

    set.surfels = std::move(surfels);
    const auto t1 = std::chrono::steady_clock::now();
    auto ms = [](const auto& a, const auto& b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    set.buildMs = float(ms(t0, t1));
    spdlog::info("surfelize: {} surfels ({} terrain, {} object), {} ms "
                 "(enum+surface {} ms, shade+bucket {} ms)",
                 set.surfels.size(), set.terrainCount, set.objectCount, set.buildMs, ms(t0, tEnum),
                 ms(tEnum, t1));
    return set;
}

std::vector<Surfel> buildWaterSurfels(const VoxelField& field, float spacing)
{
    std::vector<Surfel> out;
    const int latN = field.latN();
    if (latN <= 0 || spacing <= 0.0f)
        return out;
    const float half = 0.5f * WORLD;
    const int steps = int(WORLD / spacing);
    out.reserve(8192);
    for (int j = 0; j <= steps; ++j) {
        const float wz = -half + (j + 0.5f) * spacing;
        if (wz < -half || wz > half)
            continue;
        int cz = int((wz + half) / VOXEL);
        if (cz < 0 || cz >= latN)
            continue;
        for (int i = 0; i <= steps; ++i) {
            const float wx = -half + (i + 0.5f) * spacing;
            if (wx < -half || wx > half)
                continue;
            int cx = int((wx + half) / VOXEL);
            if (cx < 0 || cx >= latN)
                continue;
            if (field.terrainTopY(cx, cz) > WATER_LEVEL - 0.02f)
                continue;
            Surfel s;
            const float r = spacing * 0.9f; // overlap for watertight cover
            s.pos_rU = glm::vec4(wx, WATER_LEVEL, wz, r);
            s.normal_rV = glm::vec4(0.0f, 1.0f, 0.0f, r);
            s.bent_sh = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); // unshadowed water
            s.mat_ao = glm::vec4(0.0f, 40.0f, 200.0f, 3.0f); // ao=1 +2 = water
            out.push_back(s);
        }
    }
    return out;
}

} // namespace vf::voxel
