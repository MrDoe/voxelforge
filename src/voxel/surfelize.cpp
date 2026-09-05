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

// Binary sun-occlusion march over field.sample (sphere-tracing with tight
// clamps): 0 = a surface comes within tolerance of the ray, 1 = clear.
// Mirrors softShadowSplat's verdicts (same tolerance) but on the exact
// oracle instead of the heightfield+objVol approximation, and runs once per
// surfel at build time instead of once per fragment per frame.
float shadowMarch(const VoxelField& f, glm::vec3 ro, glm::vec3 rd)
{
    float t = 0.05f;
    for (int i = 0; i < 80; ++i) {
        const glm::vec3 sp = ro + rd * t;
        const float d = f.sampleWorld(sp).d;
        if (d < -0.02f)
            return 0.0f;
        t += glm::clamp(std::fabs(d) * 0.7f, 0.04f, 1.0f);
        if (t > 60.0f)
            break;
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

                    if (params.smoothNormals) {
                        glm::vec3 acc = n;
                        float wsum = 1.0f;
                        const int faceDirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 },
                                                     { 0, 1, 0 }, { 0, -1, 0 },
                                                     { 0, 0, 1 }, { 0, 0, -1 } };
                        for (auto& d : faceDirs) {
                            const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                            auto it = std::lower_bound(keys.begin(), keys.end(), nk);
                            if (it != keys.end() && *it == nk) {
                                acc += rawNormals[size_t(it - keys.begin())];
                                ++wsum;
                            }
                        }
                        n = safeNormalize(acc / wsum);
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

                    Surfel sl;
                    sl.pos_rU = glm::vec4(pos, baseR);
                    sl.normal_rV = glm::vec4(n, baseR);
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

    for (int i = 0; i < n; ++i) {
        int x, y, z;
        unpackKey(keys[order[i]], x, y, z);
        ++set.chunkRange[chunkIndex(x, y, z) + 1];
    }
    for (int c = 1; c <= kSurfGridN * kSurfGridN * kSurfGridN; ++c)
        set.chunkRange[c] += set.chunkRange[c - 1];

    set.surfels = std::move(surfels);
    const auto t1 = std::chrono::steady_clock::now();
    auto ms = [](const auto& a, const auto& b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    set.buildMs = float(ms(t0, t1));
    spdlog::info("surfelize: {} surfels ({} terrain, {} object), {} ms "
                 "(enum+surface {} ms, shade+bucket {} ms)",
                 n, set.terrainCount, set.objectCount, set.buildMs, ms(t0, tEnum),
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
