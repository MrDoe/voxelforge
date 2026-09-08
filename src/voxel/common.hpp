#pragma once
// Shared deterministic scene definition used by every voxel backend.
// v3: heightmap-driven river valley - hills + carved stream channel.
// All geometry derives from assets/heightmap.png via sharedHeightmap().
#include "voxel/heightmap.hpp"
#include "voxel/worldfile.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace vf::voxel {

constexpr float WORLD = 102.4f;
constexpr float VOXEL = 0.1f;
constexpr int CHUNK_N = 64;
constexpr float CHUNK_M = float(CHUNK_N) * VOXEL;
constexpr int GRID_N = int(WORLD / CHUNK_M);

inline const std::array<glm::vec3, 17> kPalette {
    glm::vec3 { 0.07f, 0.52f, 0.06f }, // 0 grass dark - vivid
    glm::vec3 { 0.16f, 0.68f, 0.10f }, // 1 grass light - vivid
    glm::vec3 { 0.62f, 0.36f, 0.14f }, // 2 soil - warm brown
    glm::vec3 { 0.84f, 0.72f, 0.38f }, // 3 sand - golden
    glm::vec3 { 0.48f, 0.42f, 0.38f }, // 4 rock
    glm::vec3 { 0.66f, 0.64f, 0.60f }, // 5 light rock
    glm::vec3 { 0.62f, 0.33f, 0.10f }, // 6 wood (logs/trunk) - reddish
    glm::vec3 { 0.42f, 0.22f, 0.10f }, // 7 roof shingles - deep brown
    glm::vec3 { 0.04f, 0.52f, 0.03f }, // 8 foliage - vivid green
    glm::vec3 { 1.00f, 0.35f, 0.06f }, // 9 lava  - emissive
    glm::vec3 { 0.95f, 0.20f, 0.05f }, // 10 ember - emissive
    glm::vec3 { 0.10f, 0.55f, 0.95f }, // 11 glow cyan - emissive
    glm::vec3 { 0.15f, 0.85f, 0.25f }, // 12 glow green - emissive
    glm::vec3 { 0.55f, 0.10f, 0.85f }, // 13 glow purple - emissive
    glm::vec3 { 0.10f, 0.35f, 0.95f }, // 14 glow blue - emissive
    glm::vec3 { 0.95f, 0.90f, 0.85f }, // 15 white-hot - emissive
    glm::vec3 { 0.92f, 0.95f, 0.99f }, // 16 snow - cold white
};

// per-material surface attributes, 0-255: x = reflectivity, y = roughness
inline const std::array<glm::vec2, 17> kMaterialReflection {
    glm::vec2 { 35.f, 235.f },  // 0 grass dark
    glm::vec2 { 40.f, 230.f },  // 1 grass light
    glm::vec2 { 55.f, 225.f },  // 2 soil
    glm::vec2 { 130.f, 190.f }, // 3 sand
    glm::vec2 { 95.f, 150.f },  // 4 rock
    glm::vec2 { 115.f, 135.f }, // 5 light rock
    glm::vec2 { 70.f, 160.f },  // 6 wood
    glm::vec2 { 60.f, 170.f },  // 7 roof
    glm::vec2 { 30.f, 235.f },  // 8 foliage
    glm::vec2 { 45.f, 205.f },  // 9 lava
    glm::vec2 { 45.f, 205.f },  // 10 ember
    glm::vec2 { 40.f, 200.f },  // 11 glow cyan
    glm::vec2 { 40.f, 200.f },  // 12 glow green
    glm::vec2 { 40.f, 200.f },  // 13 glow purple
    glm::vec2 { 40.f, 200.f },  // 14 glow blue
    glm::vec2 { 40.f, 200.f },  // 15 white-hot
    glm::vec2 { 50.f, 200.f },  // 16 snow
};

inline constexpr float WATER_LEVEL = -0.9f;

// Build a single surface VoxelRecord. When r/g/b are absent (negative) the
// colour is taken from kPalette[mat]; when refl/rough are absent (negative) the
// surface response is taken from kMaterialReflection[mat]. Coordinates are
// clamped into the valid 1024^3 lattice. This is the shared record constructor
// used by the MCP object-authoring tools (add_voxels / write_object).
inline VoxelRecord makeVoxelRecord(int x, int y, int z, uint8_t mat, int r = -1,
                                   int g = -1, int b = -1, int refl = -1,
                                   int rough = -1)
{
    auto clamp255 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    auto clampCoord = [](int v) { return v < 0 ? 0 : (v > 1023 ? 1023 : v); };
    VoxelRecord v;
    v.x = uint16_t(clampCoord(x));
    v.y = uint16_t(clampCoord(y));
    v.z = uint16_t(clampCoord(z));
    int mi = mat > 16 ? 16 : int(mat);
    if (r >= 0 && g >= 0 && b >= 0) {
        v.r = uint8_t(clamp255(r));
        v.g = uint8_t(clamp255(g));
        v.b = uint8_t(clamp255(b));
    } else {
        const glm::vec3& col = kPalette[mi];
        v.r = uint8_t(col.r * 255.f);
        v.g = uint8_t(col.g * 255.f);
        v.b = uint8_t(col.b * 255.f);
    }
    v.a = 255;
    if (refl >= 0)
        v.reflectivity = uint8_t(clamp255(refl));
    else
        v.reflectivity = uint8_t(kMaterialReflection[mi].x);
    if (rough >= 0)
        v.roughness = uint8_t(clamp255(rough));
    else
        v.roughness = uint8_t(kMaterialReflection[mi].y);
    v.materialId = uint8_t(mi);
    v.reserved = 0;
    return v;
}

// --- deterministic value noise -------------------------------------------
inline float hash2(float x, float y)
{
    float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453123f;
    return h - floorf(h);
}

inline float valueNoise2(float x, float y)
{
    float xi = floorf(x), yi = floorf(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float a = hash2(xi, yi), b = hash2(xi + 1.0f, yi);
    float c = hash2(xi, yi + 1.0f), d = hash2(xi + 1.0f, yi + 1.0f);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v) * 2.0f - 1.0f;
}

inline float fbm2(float x, float y)
{
    return valueNoise2(x, y) * 0.6f + valueNoise2(x * 2.13f, y * 2.13f) * 0.28f +
           valueNoise2(x * 4.41f, y * 4.41f) * 0.12f;
}

inline float smoothstepf(float e0, float e1, float x)
{
    float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct SceneSample {
    float d;
    uint8_t mat;
};

// material selection shared by scene(), worldgen and the worldfile generator
// terrain material bands shared by worldgen, probes and the worldfile generator.
// wd = WATER_LEVEL - H (>0 submerged); n = per-point variation noise.
inline uint8_t materialFromBands(float wd, float slope, float n)
{
    if (wd > 0.55f)
        return n > 0.0f ? uint8_t(5) : uint8_t(4); // deeper bed: rock
    if (wd > -0.02f)
        return 3;                                  // gravel bed / waterline sand
    if (wd > -0.45f)
        return n > 0.15f ? uint8_t(3) : uint8_t(2); // damp shore: sand & soil
    if (slope > 0.85f)
        return n > 0.0f ? uint8_t(4) : uint8_t(5); // steep slopes: rock
    if (wd > -1.2f)
        return 2;                                  // floodplain soil band
    if (wd < -4.0f && slope < 0.85f)
        return 16;                                 // snow caps on high, gentle peaks
    return n > 0.25f ? uint8_t(1) : uint8_t(0);    // grass
}

inline uint8_t materialAt(float x, float z, float H)
{
    const HeightMap& hm = sharedHeightmap();
    float wd = WATER_LEVEL - H;
    float slope = glm::length(hm.gradient(x, z));
    float n = fbm2(x * 0.35f, z * 0.35f);
    return materialFromBands(wd, slope, n);
}

// --- riverside objects (built layer for layer, bottom to top) ---------------
inline constexpr glm::vec2 kHousePos { 6.5f, 12.5f }; // west bank of the river

inline constexpr float kPadY = -0.40f; // flattened ground level around the house

// wooden footbridge crossing the river at x = 0 (river centre z = riverZ(0) ~ 7.15)
inline constexpr glm::vec2 kBridgePos { 0.0f, 7.15f };
struct ObjHit {
    float d;
    uint8_t mat;
};

inline float sdBoxF(glm::vec3 p, glm::vec3 c, glm::vec3 b)
{
    glm::vec3 q = glm::abs(p - c) - b;
    return glm::length(glm::max(q, glm::vec3(0.0f))) +
           glm::min(glm::max(q.x, glm::max(q.y, q.z)), 0.0f);
}
// horizontal log along X centered at (cx,zOff) at height y
inline float sdLogX(glm::vec3 p, float cx, float halfLen, float y, float zOff, float r)
{
    float dx = std::abs(p.x - cx) - halfLen;
    return glm::length(glm::vec2(glm::max(dx, 0.0f), std::hypot(p.y - y, p.z - zOff))) - r;
}
// horizontal log along Z
inline float sdLogZ(glm::vec3 p, float cz, float halfLen, float y, float xOff, float r)
{
    float dz = std::abs(p.z - cz) - halfLen;
    return glm::length(glm::vec2(std::max(dz, 0.0f), std::hypot(p.y - y, p.x - xOff))) - r;
}
// vertical cylinder between y0..y1
inline float sdCylY(glm::vec3 p, glm::vec2 c, float y0, float y1, float r)
{
    float qr = std::hypot(p.x - c.x, p.z - c.y) - r;
    float qy = glm::max(y0 - p.y, p.y - y1);
    return glm::length(glm::vec2(glm::max(qr, 0.0f), glm::max(qy, 0.0f))) +
           glm::min(glm::max(qr, qy), 0.0f);
}

// --- extra authoring primitives (voxel-object skill) ------------------------
// capsule between two endpoints: beams, branches, ropes
inline float sdCapsule(glm::vec3 p, glm::vec3 a, glm::vec3 b, float r)
{
    glm::vec3 pa = p - a, ba = b - a;
    float h = glm::clamp(glm::dot(pa, ba) / glm::max(glm::dot(ba, ba), 1e-8f), 0.0f, 1.0f);
    return glm::length(pa - ba * h) - r;
}

// approximate ellipsoid (Inigo Quilez): exact for spheres, |d| slightly off on
// extreme radii but the sign is stable - fine for boulders and foliage blobs
inline float sdEllipsoid(glm::vec3 p, glm::vec3 c, glm::vec3 r)
{
    glm::vec3 q = (p - c) / r;
    float k0 = glm::length(q);
    float k1 = glm::length(q / r);
    if (k1 < 1e-6f)
        return -glm::min(glm::min(r.x, r.y), r.z); // centre: exact minor-axis depth
    return k0 * (k0 - 1.0f) / k1;
}

// truncated cone along Y with flat caps, tapering linearly r0 -> r1:
// spires, tent roofs, tapered trunks (approximate side distance)
inline float sdConeY(glm::vec3 p, glm::vec2 c, float y0, float y1, float r0, float r1)
{
    const float hgt = glm::max(y1 - y0, 1e-6f);
    float t = glm::clamp((p.y - y0) / hgt, 0.0f, 1.0f);
    float qr = std::hypot(p.x - c.x, p.z - c.y) - glm::mix(r0, r1, t);
    float qy = glm::max(y0 - p.y, p.y - y1);
    return glm::length(glm::vec2(glm::max(qr, 0.0f), glm::max(qy, 0.0f))) +
           glm::min(glm::max(qr, qy), 0.0f);
}

// smooth union, k in meters. Use sparingly: softens silhouettes and distances.
inline float smin(float a, float b, float k)
{
    float h = glm::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return glm::mix(b, a, h) - k * h * (1.0f - h);
}

// --- voxel stamps: literal layer-by-layer objects built from 0.1 m cells ----
// A stamp is a list of occupied cells relative to an origin point. Cells are
// solid VOXEL-sized cubes, so stamps read as chunky voxel art - ideal for
// signs, mosaics, ruins, or shapes easier to "place" than to formulate as an
// analytic SDF. Keep stamps <= ~1000 cells; every cell survives sdfByte
// quantization by construction (cells are exactly one voxel).
struct StampCell {
    int8_t dx, dy, dz; // cell offsets from origin (units of VOXEL = 0.1 m)
    uint8_t mat;       // kPalette id
};

struct StampHit {
    float d;
    uint8_t mat;
};

// cached dense bucket index over one stamp's AABB, keyed by the cells pointer.
// Single-threaded by design (same contract as sharedHeightmap lazy init).
struct StampIndex {
    glm::vec3 origin {};
    int lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }; // inclusive cell bounds
    std::vector<int32_t> head;                    // bucket heads (-1 = empty)
    std::vector<int32_t> next;                    // per-cell linked list
    bool dense = false;

    int dim(int a) const { return hi[a] - lo[a] + 1; }
    size_t buckets() const { return size_t(dim(0)) * size_t(dim(1)) * size_t(dim(2)); }
    int clampCell(int v, int a) const { return glm::clamp(v, lo[a], hi[a]); }
    size_t bucket(int cx, int cy, int cz) const
    {
        return (size_t(cx - lo[0]) * size_t(dim(1)) + size_t(cy - lo[1])) *
                   size_t(dim(2)) +
               size_t(cz - lo[2]);
    }
};

inline StampIndex& stampIndexFor(const StampCell* cells, size_t n, glm::vec3 origin)
{
    static std::unordered_map<const StampCell*, StampIndex> cache;
    auto it = cache.find(cells);
    if (it != cache.end())
        return it->second;

    StampIndex ix;
    ix.origin = origin;
    if (n == 0) { // degenerate: single empty bucket
        ix.dense = true;
    } else {
        for (int a = 0; a < 3; ++a) {
            ix.lo[a] = INT8_MAX;
            ix.hi[a] = INT8_MIN;
        }
        for (size_t i = 0; i < n; ++i) {
            const StampCell& c = cells[i];
            int v[3] = { c.dx, c.dy, c.dz };
            for (int a = 0; a < 3; ++a) {
                ix.lo[a] = glm::min(ix.lo[a], v[a]);
                ix.hi[a] = glm::max(ix.hi[a], v[a]);
            }
        }
        // cap the dense grid; oversized stamps degrade to scan-all queries
        ix.dense = ix.buckets() <= (size_t(1) << 18);
    }
    ix.head.assign(ix.dense ? ix.buckets() : 1, -1);
    ix.next.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const StampCell& c = cells[i];
        size_t b = ix.dense ? ix.bucket(c.dx, c.dy, c.dz) : size_t(0);
        ix.next[i] = ix.head[b];
        ix.head[b] = int32_t(i);
    }
    return cache.emplace(cells, std::move(ix)).first->second;
}

// Distance to a stamp. Exact cube distance within one cell of any occupied
// cell; outside the stamp AABB returns a conservative underestimate of the
// true distance (never overshoots); inside the AABB in an empty pocket returns
// +VOXEL ("clear here"). CPU-only truth: the GPU sees whatever world.build() /
// heightmap_gen bake from this, never the raw function.
inline StampHit stampAt(glm::vec3 p, glm::vec3 origin, const StampCell* cells, size_t n)
{
    StampHit best { 1e9f, 0u };
    if (n == 0)
        return best;
    StampIndex& ix = stampIndexFor(cells, n, origin);

    glm::vec3 bmin = origin +
                     glm::vec3(float(ix.lo[0]), float(ix.lo[1]), float(ix.lo[2])) * VOXEL -
                     VOXEL * 0.5f;
    glm::vec3 bmax = origin +
                     glm::vec3(float(ix.hi[0]), float(ix.hi[1]), float(ix.hi[2])) * VOXEL +
                     VOXEL * 0.5f;
    glm::vec3 q = glm::max(bmin - p, p - bmax);
    if (q.x > 0.0f || q.y > 0.0f || q.z > 0.0f) {
        best.d = glm::max(glm::length(glm::max(q, glm::vec3(0.0f))) - 0.087f, 0.0f);
        return best;
    }

    int cc[3] = { int(std::floor((p.x - origin.x) / VOXEL)),
                  int(std::floor((p.y - origin.y) / VOXEL)),
                  int(std::floor((p.z - origin.z) / VOXEL)) };
    for (int ox = -1; ox <= 1; ++ox)
        for (int oy = -1; oy <= 1; ++oy)
            for (int oz = -1; oz <= 1; ++oz) {
                size_t b;
                if (ix.dense) {
                    b = ix.bucket(ix.clampCell(cc[0] + ox, 0), ix.clampCell(cc[1] + oy, 1),
                                  ix.clampCell(cc[2] + oz, 2));
                } else {
                    b = 0;
                }
                for (int ci = ix.head[b]; ci >= 0; ci = ix.next[ci]) {
                    const StampCell& c = cells[size_t(ci)];
                    glm::vec3 center(origin.x + float(c.dx) * VOXEL,
                                     origin.y + float(c.dy) * VOXEL,
                                     origin.z + float(c.dz) * VOXEL);
                    float d = sdBoxF(p, center, glm::vec3(VOXEL * 0.5f));
                    if (d < best.d) {
                        best.d = d;
                        best.mat = c.mat;
                    }
                }
            }
    if (best.d > VOXEL) { // empty pocket inside the AABB: bounded clearance claim
        best.d = VOXEL;
        best.mat = 0;
    }
    return best;
}

// log cabin (house.jpeg reference, micro-voxel rebuild): coursed fieldstone
// foundation with per-stone facing -> hand-hewn log walls with chinking ->
// carved openings with log-end reveals -> plank door + emissive lamplit glass
// -> full-width porch with plank deck, balusters, rafters -> stepped shingle
// roof with purlins, bargeboards, moss colonies -> coursed stone chimney ->
// side firewood lean-to with log stacks, stump+axe -> stepping-stone path.
// Footprint ~5.2 x 4.0 m, ridge ~4.3 m above the pad. Probe contract (kept):
//   wall probe (0, 0.565, -hz) solid wood, door probe (-0.7, 1.30, -hz) empty.
inline ObjHit houseAt(glm::vec3 p)
{
    glm::vec3 q(p.x - kHousePos.x, p.y - kPadY, p.z - kHousePos.y);
    const float hx = 2.6f, hz = 2.0f;
    // cheap reject: house + porch + woodpile + stone path bounds
    if (q.x < -4.8f || q.x > 4.8f || q.z < -7.2f || q.z > 3.4f || q.y < -1.2f ||
        q.y > 6.0f)
        return { 1e9f, 6u };
    ObjHit best { 1e9f, 4u };
    auto improve = [&](float d, uint8_t m) {
        if (d < best.d) {
            best.d = d;
            best.mat = m;
        }
    };

    // L0: coursed fieldstone foundation. Backing slab (solid) + two staggered
    // facing courses of individual stones protruding ~6 cm, plus porch piers
    // and a stepping-stone path toward the dock.
    {
        float d = sdBoxF(q, glm::vec3(0.f, 0.05f, 0.f),
                         glm::vec3(hx + 0.28f, 0.30f, hz + 0.28f));
        float h = hash2(std::floor(q.x * 4.3f + 7.1f), std::floor(q.z * 4.3f - 3.7f));
        improve(d, h > 0.5f ? uint8_t(5) : uint8_t(4));
        // facing stones: two courses around all four sides
        for (int course = 0; course < 2; ++course) {
            float y = -0.10f + float(course) * 0.24f;
            // front / back rows (along X)
            for (int i = 0; i < 12; ++i) {
                float fx = -2.70f + (float(i) + 0.5f) * 0.45f +
                           (hash2(float(i) * 3.1f, float(course) * 7.7f) - 0.5f) * 0.10f;
                float fw = 0.19f + hash2(float(i) * 5.3f, float(course) * 1.9f) * 0.05f;
                for (float sz : { -hz - 0.30f, hz + 0.30f }) {
                    float dz = (hash2(fx * 9.1f, sz * 3.3f + float(course)) - 0.5f) * 0.04f;
                    float ds = sdBoxF(q, glm::vec3(fx, y, sz + dz),
                                      glm::vec3(fw, 0.105f, 0.09f));
                    float hm2 = hash2(fx * 12.7f, y * 31.0f + sz);
                    improve(ds, hm2 > 0.45f ? uint8_t(5) : uint8_t(4));
                }
            }
            // side rows (along Z)
            for (int i = 0; i < 9; ++i) {
                float fz = -1.90f + (float(i) + 0.5f) * 0.45f +
                           (hash2(float(i) * 4.7f, float(course) * 3.9f) - 0.5f) * 0.10f;
                float fw = 0.19f + hash2(float(i) * 1.7f, float(course) * 8.3f) * 0.05f;
                for (float sx : { -hx - 0.30f, hx + 0.30f }) {
                    float dx = (hash2(sx * 7.7f, fz * 5.1f + float(course)) - 0.5f) * 0.04f;
                    float ds = sdBoxF(q, glm::vec3(sx + dx, y, fz),
                                      glm::vec3(0.09f, 0.105f, fw));
                    float hm2 = hash2(sx * 17.3f, fz * 11.1f + y * 29.0f);
                    improve(ds, hm2 > 0.45f ? uint8_t(5) : uint8_t(4));
                }
            }
        }
        // stone piers under the porch deck (3) + timber sill beams on top
        for (int i = -1; i <= 1; ++i) {
            float px = float(i) * 1.9f;
            float dp = sdBoxF(q, glm::vec3(px, -0.02f, -hz - 1.0f),
                              glm::vec3(0.20f, 0.24f, 0.20f));
            float hp = hash2(px * 9.1f, -hz * 3.3f);
            improve(dp, hp > 0.5f ? uint8_t(5) : uint8_t(4));
            improve(sdBoxF(q, glm::vec3(px, 0.24f, -hz - 1.0f),
                           glm::vec3(0.10f, 0.08f, 1.05f)),
                    6u);
        }
        // stepping-stone path: porch steps -> dock (3 river-washed slabs)
        {
            const HeightMap& hmH = sharedHeightmap();
            for (int i = 0; i < 3; ++i) {
                float wx = kHousePos.x - 0.7f + (hash2(float(i) * 3.3f, 1.1f) - 0.5f) * 0.3f;
                float wz = kHousePos.y - 5.05f - float(i) * 0.62f;
                float gy = hmH.sample(wx, wz);
                glm::vec3 c(wx - kHousePos.x, gy - kPadY + 0.02f, wz - kHousePos.y);
                float r = 0.30f + hash2(float(i) * 7.7f, 2.2f) * 0.10f;
                improve(sdEllipsoid(q, c, glm::vec3(r, 0.07f, r * 0.8f)),
                        i % 2 ? uint8_t(4) : uint8_t(5));
            }
        }
    }

    // L1: hand-hewn log courses (9), corners crossed like blockwork. Course 0
    // is pinned to the probe contract (y=0.565, r=0.145); upper courses vary
    // in girth/overhang per-course hash + recessed daub chinking between them.
    const float pitch = 0.26f;
    float logY[9], logR[9], logOv[9];
    for (int k = 0; k < 9; ++k) {
        if (k == 0) {
            logR[k] = 0.145f;
            logY[k] = 0.565f;
            logOv[k] = 0.22f;
        } else {
            logR[k] = 0.132f + hash2(float(k) * 7.13f, 3.3f) * 0.022f;
            logY[k] = 0.565f + float(k) * pitch +
                      (hash2(float(k) * 3.71f, 9.2f) - 0.5f) * 0.020f;
            logOv[k] = 0.22f + hash2(float(k) * 5.17f, 1.7f) * 0.05f;
        }
    }
    float ry0 = logY[8] + logR[8] + 0.22f; // roof base (q.y)
    float wall = 1e9f;
    for (int k = 0; k < 9; ++k) {
        float y = logY[k], lr = logR[k], ov = logOv[k];
        if (k % 2 == 0)
            wall = glm::min(wall, glm::min(sdLogX(q, 0.f, hx + ov, y, hz, lr),
                                           sdLogX(q, 0.f, hx + ov, y, -hz, lr)));
        else
            wall = glm::min(wall, glm::min(sdLogZ(q, 0.f, hz + ov, y, hx, lr),
                                           sdLogZ(q, 0.f, hz + ov, y, -hx, lr)));
        // butt-joint shadow rings where crossing logs meet the perpendicular
        // wall (short collar logs, 0.5 m stubs) — reads as saddle notches.
        if (k % 2 == 0) {
            for (float sx : { -hx, hx }) {
                float collar = sdLogZ(q, 0.f, 0.30f, y, sx, lr * 0.92f);
                wall = glm::min(wall, collar);
            }
        } else {
            for (float sz : { -hz, hz }) {
                float collar = sdLogX(q, 0.f, 0.30f, y, sz, lr * 0.92f);
                wall = glm::min(wall, collar);
            }
        }
    }
    // openings first (needed to mask the chinking below): river-side door
    // (deep entry alcove), shallow window reveals, attic vents. Glass sits
    // backed against the reveal back so its baked normal faces outward.
    float door = sdBoxF(q, glm::vec3(-0.7f, 1.30f, -hz), glm::vec3(0.52f, 0.95f, 0.6f));
    float winF1 = sdBoxF(q, glm::vec3(-1.90f, 1.62f, -hz - 0.20f), glm::vec3(0.40f, 0.38f, 0.15f));
    float winF2 = sdBoxF(q, glm::vec3(1.20f, 1.62f, -hz - 0.20f), glm::vec3(0.40f, 0.38f, 0.15f));
    float winA = sdBoxF(q, glm::vec3(-hx - 0.15f, 1.62f, 0.75f), glm::vec3(0.15f, 0.42f, 0.45f));
    float winB = sdBoxF(q, glm::vec3(hx + 0.15f, 1.62f, 0.75f), glm::vec3(0.15f, 0.42f, 0.45f));
    float winC = sdBoxF(q, glm::vec3(-hx - 0.15f, 1.62f, -0.85f), glm::vec3(0.15f, 0.38f, 0.40f));
    float winD = sdBoxF(q, glm::vec3(hx + 0.15f, 1.62f, -0.85f), glm::vec3(0.15f, 0.38f, 0.40f));
    float atticE = sdBoxF(q, glm::vec3(hx + 0.05f, 2.95f, 0.f), glm::vec3(0.35f, 0.30f, 0.32f));
    float atticW = sdBoxF(q, glm::vec3(-hx - 0.05f, 2.95f, 0.f), glm::vec3(0.35f, 0.30f, 0.32f));
    float carve = -door;
    carve = glm::max(carve, -winF1);
    carve = glm::max(carve, -winF2);
    carve = glm::max(carve, -winA);
    carve = glm::max(carve, -winB);
    carve = glm::max(carve, -winC);
    carve = glm::max(carve, -winD);
    carve = glm::max(carve, -atticE);
    carve = glm::max(carve, -atticW);
    // daub chinking: recessed soil lines in the shadow gaps between courses.
    // Masked by the opening carves so doors/windows stay clear.
    for (int k = 0; k < 8; ++k) {
        float ym = 0.5f * (logY[k] + logY[k + 1]);
        float ch = 0.030f;
        improve(glm::max(sdBoxF(q, glm::vec3(0.f, ym, hz - 0.03f),
                                glm::vec3(hx + 0.16f, ch, 0.055f)),
                        carve),
                2u);
        improve(glm::max(sdBoxF(q, glm::vec3(0.f, ym, -hz + 0.03f),
                                glm::vec3(hx + 0.16f, ch, 0.055f)),
                        carve),
                2u);
        improve(glm::max(sdBoxF(q, glm::vec3(hx - 0.03f, ym, 0.f),
                                glm::vec3(0.055f, ch, hz + 0.16f)),
                        carve),
                2u);
        improve(glm::max(sdBoxF(q, glm::vec3(-hx + 0.03f, ym, 0.f),
                                glm::vec3(0.055f, ch, hz + 0.16f)),
                        carve),
                2u);
    }
    wall = glm::max(wall, carve);
    // solid interior (negative inside) so the cabin reads solid at distance.
    // Faces sit just inside the log centre planes so the hand-hewn log
    // surfaces (not this box) always win the bake band at the shell.
    float solidBox = sdBoxF(q, glm::vec3(0.f, (0.35f + ry0) * 0.5f, 0.f),
                            glm::vec3(hx - 0.05f, (ry0 - 0.35f) * 0.5f, hz - 0.05f));
    solidBox = glm::max(solidBox, carve);
    improve(solidBox, 6u);
    improve(wall, 6u);

    // L2: lamplit openings — recessed ember glass, log-end reveals, sills,
    // lintels, trim, plank door with battens + ironwork, twin lanterns.
    auto glassPane = [&](glm::vec3 c, glm::vec3 h) {
        improve(sdBoxF(q, c, h), 10u);
    };
    // panes sit flush with the log crowns (backs on the shallow reveal
    // backs) so wall disks cannot occlude them and normals face outward
    glassPane(glm::vec3(-1.90f, 1.62f, -hz - 0.10f), glm::vec3(0.32f, 0.30f, 0.05f));
    glassPane(glm::vec3(1.20f, 1.62f, -hz - 0.10f), glm::vec3(0.32f, 0.30f, 0.05f));
    glassPane(glm::vec3(-hx - 0.10f, 1.62f, 0.75f), glm::vec3(0.05f, 0.32f, 0.35f));
    glassPane(glm::vec3(hx + 0.10f, 1.62f, 0.75f), glm::vec3(0.05f, 0.32f, 0.35f));
    glassPane(glm::vec3(-hx - 0.10f, 1.62f, -0.85f), glm::vec3(0.05f, 0.30f, 0.32f));
    glassPane(glm::vec3(hx + 0.10f, 1.62f, -0.85f), glm::vec3(0.05f, 0.30f, 0.32f));
    // attic vents read as dark shutters, not lamps + louver slats
    improve(sdBoxF(q, glm::vec3(hx - 0.25f, 2.95f, 0.f),
                   glm::vec3(0.05f, 0.24f, 0.26f)),
            6u);
    improve(sdBoxF(q, glm::vec3(-hx + 0.25f, 2.95f, 0.f),
                   glm::vec3(0.05f, 0.24f, 0.26f)),
            6u);
    for (float ly : { 2.88f, 2.95f, 3.02f }) {
        improve(sdBoxF(q, glm::vec3(hx - 0.28f, ly, 0.f),
                       glm::vec3(0.04f, 0.03f, 0.24f)),
                6u);
        improve(sdBoxF(q, glm::vec3(-hx + 0.28f, ly, 0.f),
                       glm::vec3(0.04f, 0.03f, 0.24f)),
                6u);
    }
    // log-end reveals: short perpendicular stubs framing each front opening
    // (reads as adzed jamb logs) + protruding sill + oversized lintel log
    for (float wx : { -1.90f, 1.20f }) {
        for (float jx : { wx - 0.38f, wx + 0.38f })
            improve(sdBoxF(q, glm::vec3(jx, 1.62f, -hz - 0.05f),
                           glm::vec3(0.07f, 0.40f, 0.22f)),
                    6u);
        improve(sdLogX(q, wx, 0.48f, 2.06f, -hz - 0.02f, 0.10f), 6u); // lintel
        improve(sdBoxF(q, glm::vec3(wx, 1.18f, -hz - 0.06f),
                       glm::vec3(0.46f, 0.07f, 0.20f)),
                6u); // sill
        // trim boards + mullion cross just off the glow
        improve(sdBoxF(q, glm::vec3(wx, 1.62f, -hz - 0.16f),
                       glm::vec3(0.05f, 0.36f, 0.03f)),
                6u);
        improve(sdBoxF(q, glm::vec3(wx, 1.62f, -hz - 0.16f),
                       glm::vec3(0.38f, 0.05f, 0.03f)),
                6u);
        improve(sdBoxF(q, glm::vec3(wx, 2.02f, -hz - 0.08f),
                       glm::vec3(0.50f, 0.07f, 0.14f)),
                6u); // head casing
    }
    // side window dress: sills + lintels + mullions
    for (float sz : { 0.75f, -0.85f }) {
        for (float sx : { -hx - 0.05f, hx + 0.05f }) {
            improve(sdBoxF(q, glm::vec3(sx, 1.18f, sz),
                           glm::vec3(0.20f, 0.07f, 0.46f)),
                    6u);
            improve(sdLogZ(q, 0.f, 0.48f, 2.06f, sx, 0.10f), 6u);
        }
    }
    for (float sz : { 0.75f, -0.85f }) {
        improve(sdBoxF(q, glm::vec3(-hx - 0.16f, 1.62f, sz),
                       glm::vec3(0.03f, 0.36f, 0.05f)),
                6u);
        improve(sdBoxF(q, glm::vec3(hx + 0.16f, 1.62f, sz),
                       glm::vec3(0.03f, 0.36f, 0.05f)),
                6u);
    }
    // plank door: 4 vertical boards with groove shadow gaps + 2 ledges,
    // recessed 0.6 inside the facade so the wall-plane probe stays open.
    for (int b = 0; b < 4; ++b) {
        float bx = -0.7f - 0.33f + float(b) * 0.22f;
        improve(sdBoxF(q, glm::vec3(bx, 1.25f, -hz + 0.55f),
                       glm::vec3(0.095f, 0.88f, 0.07f)),
                6u);
    }
    for (float ly : { 0.78f, 1.72f })
        improve(sdBoxF(q, glm::vec3(-0.7f, ly, -hz + 0.47f),
                       glm::vec3(0.44f, 0.10f, 0.05f)),
                6u);
    // iron strap hinges + ring handle (dark rock) + stone threshold
    for (float hy : { 0.95f, 1.60f })
        improve(sdBoxF(q, glm::vec3(-0.38f, hy, -hz + 0.47f),
                       glm::vec3(0.18f, 0.05f, 0.03f)),
                4u);
    improve(sdBoxF(q, glm::vec3(-0.42f, 1.28f, -hz + 0.44f),
                   glm::vec3(0.04f, 0.10f, 0.05f)),
            4u);
    improve(sdBoxF(q, glm::vec3(-0.7f, 0.36f, -hz + 0.30f),
                   glm::vec3(0.55f, 0.06f, 0.35f)),
            5u);
    improve(sdBoxF(q, glm::vec3(-0.7f, 2.20f, -hz + 0.05f),
                   glm::vec3(0.62f, 0.10f, 0.30f)),
            6u);
    for (float sx : { -1.28f, -0.12f })
        improve(sdBoxF(q, glm::vec3(sx, 1.30f, -hz + 0.05f),
                       glm::vec3(0.08f, 1.00f, 0.28f)),
                6u);
    // twin porch lanterns flanking the door: ember lamp + cap + bracket
    for (float lx : { -1.45f, 0.05f }) {
        improve(sdBoxF(q, glm::vec3(lx, 1.95f, -hz - 0.10f),
                       glm::vec3(0.09f, 0.12f, 0.09f)),
                10u);
        improve(sdBoxF(q, glm::vec3(lx, 2.10f, -hz - 0.10f),
                       glm::vec3(0.14f, 0.05f, 0.14f)),
                6u);
        improve(sdBoxF(q, glm::vec3(lx, 1.82f, -hz - 0.06f),
                       glm::vec3(0.05f, 0.04f, 0.12f)),
                6u);
    }

    // L3: river-side porch — individual deck planks on joists, four steps
    // with stringers + stone cheeks, four posts with brackets, three-rail
    // balustrade with balusters, raftered shed roof, bench + chair.
    const float pz0 = -hz - 1.90f, pz1 = -hz - 0.02f;
    const float pzc = 0.5f * (pz0 + pz1);
    const float pzd = 0.5f * (pz1 - pz0);
    // deck planks (6 along X, 0.28 wide, 0.05 gaps) + rim + joists
    for (int i = 0; i < 6; ++i) {
        float zz = pz0 + 0.16f + float(i) * 0.315f;
        improve(sdBoxF(q, glm::vec3(0.f, 0.30f, zz),
                       glm::vec3(hx + 0.30f, 0.055f, 0.13f)),
                6u);
    }
    improve(sdBoxF(q, glm::vec3(0.f, 0.22f, pzc), glm::vec3(hx + 0.30f, 0.05f, pzd)),
            6u); // rim/bed
    for (float jx : { -hx + 0.3f, 0.f, hx - 0.3f })
        improve(sdLogZ(q, pzc, pzd - 0.05f, 0.16f, jx, 0.06f), 6u); // joists
    // four steps down, centred on the door + stringers + stone cheeks
    auto sHash = [&](float v) { return hash2(v * 9.17f, 4.4f); };
    for (int s = 0; s < 4; ++s) {
        float sy = 0.22f - float(s) * 0.135f;
        float sz = -hz - 2.02f - float(s) * 0.30f;
        improve(sdBoxF(q, glm::vec3(-0.7f, sy, sz),
                       glm::vec3(0.65f, 0.055f, 0.17f)),
                6u);
        if (s == 3)
            improve(sdBoxF(q, glm::vec3(-0.7f, sy - 0.06f, sz),
                           glm::vec3(0.65f, 0.05f, 0.17f)),
                    4u); // worn nosing stone
    }
    for (float sx : { -1.42f, 0.02f })
        improve(sdBoxF(q, glm::vec3(sx, -0.02f, -hz - 2.45f),
                       glm::vec3(0.08f, 0.30f, 1.10f)),
                6u); // stringers
    for (float sx : { -1.55f, 0.15f })
        improve(sdBoxF(q, glm::vec3(sx, -0.10f, -hz - 2.50f),
                       glm::vec3(0.14f, 0.20f, 1.20f)),
                sHash(sx) > 0.5f ? 5u : 4u); // stone cheeks
    // posts (deck -> porch roof) with cap + diagonal brackets
    for (float px : { -hx - 0.15f, -0.95f, 0.95f, hx + 0.15f })
        improve(sdBoxF(q, glm::vec3(px, 1.40f, -hz - 1.72f),
                       glm::vec3(0.075f, 1.02f, 0.075f)),
                6u);
    for (float px : { -hx - 0.15f, -0.95f, 0.95f, hx + 0.15f }) {
        improve(sdBoxF(q, glm::vec3(px, 2.38f, -hz - 1.72f),
                       glm::vec3(0.11f, 0.07f, 0.11f)),
                6u); // caps
        improve(sdCapsule(q, glm::vec3(px, 2.05f, -hz - 1.72f),
                          glm::vec3(px * 0.96f, 2.32f, -hz - 1.15f), 0.045f),
                6u); // brackets to beam
    }
    improve(sdLogX(q, 0.f, hx + 0.30f, 2.38f, -hz - 1.72f, 0.075f), 6u); // beam
    // front rails (3 bars) with a stair gap |x+0.7| < 0.75 + shaped balusters
    const float railZ = -hz - 1.74f;
    for (float seg : { -1.85f, 1.05f }) {
        float cx = seg < 0.f ? -1.85f : 1.30f;
        float hw = seg < 0.f ? 0.95f : 1.50f;
        (void)seg;
        improve(sdBoxF(q, glm::vec3(cx, 1.10f, railZ), glm::vec3(hw, 0.055f, 0.06f)),
                6u); // top rail
        improve(sdBoxF(q, glm::vec3(cx, 0.82f, railZ), glm::vec3(hw, 0.035f, 0.035f)),
                6u); // mid rail
        improve(sdBoxF(q, glm::vec3(cx, 0.52f, railZ), glm::vec3(hw, 0.04f, 0.04f)),
                6u); // bottom rail
    }
    for (float bx = -hx - 0.10f; bx <= hx + 0.11f; bx += 0.24f) {
        if (std::fabs(bx + 0.7f) < 0.78f)
            continue; // stair opening
        improve(sdBoxF(q, glm::vec3(bx, 0.81f, railZ),
                       glm::vec3(0.04f, 0.26f, 0.04f)),
                6u);
        improve(sdBoxF(q, glm::vec3(bx, 0.81f, railZ),
                       glm::vec3(0.07f, 0.06f, 0.07f)),
                6u); // baluster collar
    }
    // side rails (wall -> front posts)
    for (float sx : { -hx - 0.15f, hx + 0.15f }) {
        improve(sdBoxF(q, glm::vec3(sx, 1.10f, pzc), glm::vec3(0.06f, 0.055f, pzd)),
                6u);
        improve(sdBoxF(q, glm::vec3(sx, 0.82f, pzc), glm::vec3(0.035f, 0.035f, pzd)),
                6u);
        improve(sdBoxF(q, glm::vec3(sx, 0.52f, pzc), glm::vec3(0.04f, 0.04f, pzd)),
                6u);
        for (float bz = -hz - 1.55f; bz < -hz - 0.15f; bz += 0.26f) {
            improve(sdBoxF(q, glm::vec3(sx, 0.81f, bz),
                           glm::vec3(0.04f, 0.26f, 0.04f)),
                    6u);
        }
    }
    // porch furniture: bench (left) + chair (right) from slab + legs
    improve(sdBoxF(q, glm::vec3(-1.75f, 0.72f, -hz - 0.55f),
                   glm::vec3(0.55f, 0.06f, 0.22f)),
            6u);
    for (float lx : { -2.20f, -1.30f })
        for (float lz : { -hz - 0.70f, -hz - 0.40f })
            improve(sdBoxF(q, glm::vec3(lx, 0.50f, lz),
                           glm::vec3(0.05f, 0.18f, 0.05f)),
                    6u);
    improve(sdBoxF(q, glm::vec3(1.75f, 0.72f, -hz - 0.55f),
                   glm::vec3(0.45f, 0.06f, 0.40f)),
            6u);
    improve(sdBoxF(q, glm::vec3(1.75f, 1.05f, -hz - 0.20f),
                   glm::vec3(0.45f, 0.45f, 0.06f)),
            6u); // chair back
    // porch shed roof: rafters + 3 shingle courses + moss tufts
    for (int r = 0; r < 7; ++r) {
        float rx = -hx - 0.45f + float(r) * (2.f * (hx + 0.45f) / 6.f);
        improve(sdLogZ(q, pzc, pzd + 0.30f, 2.42f, rx, 0.055f), 6u); // rafters
    }
    for (int c = 0; c < 3; ++c) {
        float cy = 2.52f + float(c) * 0.075f;
        float cz = pzc - float(c) * 0.12f;
        float d = sdBoxF(q, glm::vec3(0.f, cy, cz),
                         glm::vec3(hx + 0.55f, 0.055f, pzd + 0.30f - float(c) * 0.10f));
        float h = hash2(q.x * 2.3f + float(c) * 5.1f, q.z * 2.3f);
        improve(d, h > 0.55f ? (h > 0.8f ? uint8_t(1) : uint8_t(0)) : uint8_t(7));
    }
    for (int m = 0; m < 4; ++m) {
        float mx = -2.0f + hash2(float(m) * 7.3f, 2.1f) * 4.0f;
        float mz = pzc + (hash2(float(m) * 3.9f, 8.8f) - 0.5f) * 1.2f;
        improve(sdEllipsoid(q, glm::vec3(mx, 2.68f, mz),
                            glm::vec3(0.28f, 0.06f, 0.22f)),
                m % 2 ? uint8_t(1) : uint8_t(0));
    }

    // L4: stepped shingle roof (12 fine courses), protruding purlins,
    // bargeboards, plank gables with battens, moss colonies, gable window.
    const int kRoofN = 12;
    for (int i = 0; i < kRoofN; ++i) {
        float t = float(i) / float(kRoofN - 1);
        float rz = glm::mix(hz + 0.70f, 0.13f, t);
        float jit = (hash2(float(i) * 3.7f, 6.1f) - 0.5f) * 0.06f;
        float dslab = sdBoxF(q, glm::vec3(jit * 0.3f, ry0 + float(i) * 0.125f, 0.f),
                             glm::vec3(hx + 0.62f + jit, 0.075f, rz));
        float h = hash2(std::floor(q.x * 2.1f + float(i) * 3.3f),
                        std::floor(q.z * 2.1f - float(i)));
        uint8_t m = (i >= 4 && h > 0.50f) ? (h > 0.76f ? uint8_t(1) : uint8_t(0))
                                          : uint8_t(7);
        improve(dslab, m);
        // exposed shingle butt ends: thin lip under each course front/back
        improve(sdBoxF(q, glm::vec3(0.f, ry0 + float(i) * 0.125f - 0.075f, rz - 0.02f),
                       glm::vec3(hx + 0.60f, 0.03f, 0.05f)),
                7u);
        improve(sdBoxF(q, glm::vec3(0.f, ry0 + float(i) * 0.125f - 0.075f, -rz + 0.02f),
                       glm::vec3(hx + 0.60f, 0.03f, 0.05f)),
                7u);
    }
    float ridgeY = ry0 + float(kRoofN - 1) * 0.125f + 0.14f;
    // ridge beam + cap shingles
    improve(sdLogX(q, 0.f, hx + 0.62f, ridgeY, 0.f, 0.09f), 6u);
    improve(sdBoxF(q, glm::vec3(0.f, ridgeY + 0.10f, 0.f),
                   glm::vec3(hx + 0.60f, 0.06f, 0.20f)),
            7u);
    // purlin logs protruding through both gables (3 per side)
    for (float py : { ry0 + 0.35f, ry0 + 0.80f, ridgeY - 0.12f }) {
        float tt = (py - ry0) / (ridgeY - ry0);
        float pzz = (hz + 0.55f) * (1.f - tt) + 0.10f;
        improve(sdLogX(q, 0.f, hx + 0.78f, py, pzz, 0.07f), 6u);
        improve(sdLogX(q, 0.f, hx + 0.78f, py, -pzz, 0.07f), 6u);
    }
    // bargeboards along the sloped gable edges
    for (float sx : { -hx - 0.62f, hx + 0.62f }) {
        improve(sdCapsule(q, glm::vec3(sx, ry0 - 0.05f, hz + 0.70f),
                          glm::vec3(sx, ridgeY + 0.08f, 0.10f), 0.06f),
                6u);
        improve(sdCapsule(q, glm::vec3(sx, ry0 - 0.05f, -hz - 0.70f),
                          glm::vec3(sx, ridgeY + 0.08f, -0.10f), 0.06f),
                6u);
    }
    // plank gables: vertical boards with battens (front + back)
    for (float gz : { hz - 0.05f, -hz + 0.05f }) {
        for (int b = 0; b < 11; ++b) {
            float bx = -2.5f + float(b) * 0.5f;
            float topY = ridgeY - 0.05f - std::fabs(bx) * 0.18f;
            if (topY < ry0)
                continue;
            float cy = 0.5f * (ry0 + topY);
            improve(sdBoxF(q, glm::vec3(bx, cy, gz),
                           glm::vec3(0.21f, 0.5f * (topY - ry0) + 0.05f, 0.07f)),
                    6u);
        }
        improve(sdBoxF(q, glm::vec3(0.f, 0.5f * (ry0 + ridgeY), gz),
                       glm::vec3(0.09f, 0.5f * (ridgeY - ry0), 0.10f)),
                6u); // centre batten
    }
    // front gable lamplit window (reference peak window) + trim
    {
        float gz = -hz + 0.05f;
        float gy = ridgeY - 0.55f;
        float carveG = sdBoxF(q, glm::vec3(0.f, gy, gz), glm::vec3(0.26f, 0.28f, 0.15f));
        // cut into gable boards: handled by min/max via wall? apply locally:
        // place ember pane + frame (boards behind stay, pane floats off face)
        improve(sdBoxF(q, glm::vec3(0.f, gy, gz - 0.06f),
                       glm::vec3(0.20f, 0.22f, 0.05f)),
                10u);
        improve(sdBoxF(q, glm::vec3(0.f, gy, gz - 0.10f),
                       glm::vec3(0.05f, 0.26f, 0.04f)),
                6u);
        improve(sdBoxF(q, glm::vec3(0.f, gy, gz - 0.10f),
                       glm::vec3(0.26f, 0.05f, 0.04f)),
                6u);
        (void)carveG;
    }
    // moss colonies: flattened blobs on the upper roof slopes (both sides)
    for (int m = 0; m < 14; ++m) {
        float mx = -2.6f + hash2(float(m) * 5.13f, 1.7f) * 5.2f;
        float side = (m % 2 == 0) ? 1.f : -1.f;
        float tt = 0.35f + hash2(float(m) * 9.31f, 4.2f) * 0.55f;
        float my = ry0 + tt * (ridgeY - ry0) + 0.09f;
        float mz = side * ((hz + 0.55f) * (1.f - tt) + 0.05f);
        improve(sdEllipsoid(q, glm::vec3(mx, my, mz),
                            glm::vec3(0.22f + hash2(float(m), 7.7f) * 0.22f, 0.055f,
                                      0.18f + hash2(float(m), 3.1f) * 0.16f)),
                hash2(float(m) * 1.3f, 9.9f) > 0.4f ? uint8_t(0) : uint8_t(1));
    }

    // L5: coursed rubble-stone chimney with shoulders, cap + twin flues.
    {
        // shaft backing (solid) from roof slope through the top
        float chim = sdBoxF(q, glm::vec3(1.55f, ry0 + 0.85f, 0.75f),
                            glm::vec3(0.30f, 1.45f, 0.30f));
        float h = hash2(std::floor(q.y * 5.1f), std::floor((q.x + q.z) * 5.1f));
        improve(chim, h > 0.5f ? uint8_t(5) : uint8_t(4));
        // facing stones: staggered courses up the shaft
        for (int c = 0; c < 9; ++c) {
            float cy = ry0 - 0.30f + float(c) * 0.30f;
            for (int s = 0; s < 2; ++s) {
                float off = (c % 2 ? 0.15f : 0.f) + (hash2(float(c), float(s)) - 0.5f) * 0.06f;
                improve(sdBoxF(q, glm::vec3(1.55f - 0.15f + off, cy, 0.75f + 0.30f),
                               glm::vec3(0.16f, 0.13f, 0.06f)),
                        (c + s) % 2 ? 5u : 4u);
                improve(sdBoxF(q, glm::vec3(1.55f - 0.15f + off, cy, 0.75f - 0.30f),
                               glm::vec3(0.16f, 0.13f, 0.06f)),
                        (c + s + 1) % 2 ? 5u : 4u);
                improve(sdBoxF(q, glm::vec3(1.55f + 0.30f, cy, 0.75f - 0.15f + off),
                               glm::vec3(0.06f, 0.13f, 0.16f)),
                        (c + s) % 2 ? 4u : 5u);
                improve(sdBoxF(q, glm::vec3(1.55f - 0.30f, cy, 0.75f - 0.15f + off),
                               glm::vec3(0.06f, 0.13f, 0.16f)),
                        (c + s + 1) % 2 ? 4u : 5u);
            }
        }
        // shoulders where the stack leaves the roof + cap slab + twin flues
        improve(sdBoxF(q, glm::vec3(1.55f, ry0 + 0.10f, 0.75f),
                       glm::vec3(0.42f, 0.18f, 0.42f)),
                4u);
        improve(sdBoxF(q, glm::vec3(1.55f, ry0 + 2.32f, 0.75f),
                       glm::vec3(0.44f, 0.10f, 0.44f)),
                5u);
        for (float fx : { 1.44f, 1.66f })
            improve(sdBoxF(q, glm::vec3(fx, ry0 + 2.44f, 0.75f),
                           glm::vec3(0.11f, 0.14f, 0.11f)),
                    4u);
        float flue = sdBoxF(q, glm::vec3(1.55f, ry0 + 2.42f, 0.75f),
                            glm::vec3(0.24f, 0.14f, 0.16f));
        // carve the flue mouths out of whatever won (cap/rock) nearby
        if (flue < 0.25f && best.d > -flue)
            best.d = glm::max(best.d, -flue);
    }

    // L6: firewood lean-to on the east wall (reference right-side stack):
    // deck, posts, slanted shingle roof, 4x6 cordwood rows with varied girth,
    // bark-side out, plus chopping stump + axe.
    {
        const float lx = hx + 0.85f;
        improve(sdBoxF(q, glm::vec3(lx, 0.28f, 0.60f), glm::vec3(0.65f, 0.08f, 1.00f)),
                6u);
        for (float px : { lx - 0.55f, lx + 0.55f })
            for (float pz : { -0.30f, 1.50f })
                improve(sdBoxF(q, glm::vec3(px, 0.85f, pz),
                               glm::vec3(0.07f, 0.65f, 0.07f)),
                        6u);
        // slanted roof: 2 courses + edge trim
        improve(sdBoxF(q, glm::vec3(lx, 1.52f, 0.60f), glm::vec3(0.80f, 0.06f, 1.14f)),
                7u);
        improve(sdBoxF(q, glm::vec3(lx, 1.60f, 0.60f), glm::vec3(0.82f, 0.05f, 1.16f)),
                7u);
        improve(sdBoxF(q, glm::vec3(lx, 1.50f, -0.52f), glm::vec3(0.80f, 0.07f, 0.07f)),
                6u);
        // cordwood: 4 rows x 6 sticks, each its own girth/length jitter
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 6; ++k) {
                float y = 0.46f + float(row) * 0.185f;
                float z = -0.12f + float(k) * 0.27f + float(row) * 0.04f;
                float r = 0.080f + hash2(float(k) * 3.3f, float(row) * 7.1f) * 0.025f;
                float hl = 0.40f + (hash2(float(k) * 9.7f, float(row) * 1.3f) - 0.5f) * 0.06f;
                improve(sdLogX(q, lx, hl, y, z, r), 6u);
                // split-face wedge on every other stick (small flat box end)
                if ((k + row) % 2 == 0)
                    improve(sdBoxF(q, glm::vec3(lx + hl - 0.02f, y, z),
                                   glm::vec3(0.03f, r * 0.7f, r * 0.7f)),
                            2u);
            }
        // chopping stump + buried axe (handle capsule + head wedge)
        improve(sdCylY(q, glm::vec2(lx + 0.1f, 2.15f), 0.0f, 0.45f, 0.22f), 6u);
        improve(sdCylY(q, glm::vec2(lx + 0.1f, 2.15f), 0.45f, 0.48f, 0.23f), 2u);
        improve(sdCapsule(q, glm::vec3(lx - 0.05f, 0.48f, 2.10f),
                          glm::vec3(lx + 0.35f, 0.95f, 2.20f), 0.03f),
                6u);
        improve(sdBoxF(q, glm::vec3(lx + 0.02f, 0.55f, 2.12f),
                       glm::vec3(0.05f, 0.09f, 0.03f)),
                4u);
    }
    return best;
}

// broadleaf homestead tree (rewritten): buttress roots -> leaning tapered
// trunk with bark ridges -> 4 limbs -> 9-lobed canopy with light gaps.
// ~4 m scale; crowns stay below the sky band of the fixed house-test cam.
inline ObjHit treeAt(glm::vec3 p, glm::vec2 spot, float groundY)
{
    glm::vec3 q(p.x - spot.x, p.y - groundY, p.z - spot.y);
    constexpr float kS = 0.55f; // orchard scale (crowns clear the sky band)
    glm::vec3 qs = q / kS;
    ObjHit best { 1e9f, 6u };

    auto improve = [&](float d, uint8_t m) {
        d *= kS;
        if (d < best.d) {
            best.d = d;
            best.mat = m;
        }
    };
    float lean = (hash2(spot.x * 3.1f, spot.y * 7.7f) - 0.5f) * 0.5f;
    float lean2 = (hash2(spot.y * 5.3f, spot.x * 1.9f) - 0.5f) * 0.4f;
    // L0: buttress roots (4 toes) + flare
    improve(sdCylY(qs, glm::vec2(0.f), -0.05f, 0.22f, 0.36f), 6u);
    for (int r = 0; r < 4; ++r) {
        float a = float(r) * 1.5707f + hash2(spot.x, float(r)) * 0.5f;
        glm::vec3 tip(std::cos(a) * 0.75f, -0.02f, std::sin(a) * 0.75f);
        improve(sdCapsule(qs, glm::vec3(0.f, 0.18f, 0.f), tip, 0.16f), 6u);
    }
    // L1: leaning tapered trunk (3 segments) + bark ridges
    improve(sdCylY(qs, glm::vec2(0.f), 0.20f, 1.10f, 0.27f), 6u);
    improve(sdCapsule(qs, glm::vec3(0.f, 1.05f, 0.f),
                      glm::vec3(lean * 0.5f, 2.10f, lean2 * 0.5f), 0.225f),
            6u);
    improve(sdCapsule(qs, glm::vec3(lean * 0.5f, 2.05f, lean2 * 0.5f),
                      glm::vec3(lean, 3.10f, lean2), 0.175f),
            6u);
    for (int b = 0; b < 3; ++b) {
        float a = float(b) * 2.094f + lean;
        glm::vec3 base(std::cos(a) * 0.24f, 0.5f, std::sin(a) * 0.24f);
        glm::vec3 top(lean * 0.4f + std::cos(a) * 0.18f, 2.4f,
                      lean2 * 0.4f + std::sin(a) * 0.18f);
        improve(sdCapsule(qs, base, top, 0.055f), 6u); // bark ridges
    }
    // L2: 4 limbs from the crown base outward/up
    glm::vec3 crownBase(lean, 3.05f, lean2);
    const glm::vec3 limbTips[4] = {
        crownBase + glm::vec3(-1.15f, 0.85f, 0.55f),
        crownBase + glm::vec3(1.10f, 0.95f, -0.50f),
        crownBase + glm::vec3(0.15f, 1.25f, 1.05f),
        crownBase + glm::vec3(-0.10f, 1.35f, -1.00f),
    };
    for (int i = 0; i < 4; ++i)
        improve(sdCapsule(qs, crownBase, limbTips[i], 0.11f - float(i) * 0.012f), 6u);

    // L3: 9-lobed canopy — overlapping ellipsoids with gaps for light
    auto blob = [&](glm::vec3 c, glm::vec3 r) {
        float ds = sdEllipsoid(qs, c, r);
        ds *= kS;
        if (ds < best.d) {
            best.d = ds;
            best.mat = 8;
        }
    };
    blob(crownBase + glm::vec3(-0.85f, 0.75f, 0.45f), glm::vec3(1.05f, 0.85f, 0.95f));
    blob(crownBase + glm::vec3(0.90f, 0.90f, -0.35f), glm::vec3(1.10f, 0.90f, 1.00f));
    blob(crownBase + glm::vec3(0.10f, 1.30f, 0.75f), glm::vec3(1.00f, 0.85f, 0.95f));
    blob(crownBase + glm::vec3(-0.05f, 1.40f, -0.75f), glm::vec3(1.05f, 0.90f, 0.95f));
    blob(crownBase + glm::vec3(0.05f, 2.05f, 0.10f), glm::vec3(1.15f, 0.95f, 1.05f));
    blob(crownBase + glm::vec3(-0.70f, 1.90f, -0.40f), glm::vec3(0.80f, 0.70f, 0.75f));
    blob(crownBase + glm::vec3(0.75f, 1.95f, 0.45f), glm::vec3(0.82f, 0.72f, 0.78f));
    blob(crownBase + glm::vec3(0.15f, 2.75f, -0.05f), glm::vec3(0.85f, 0.70f, 0.80f));
    blob(crownBase + glm::vec3(-0.30f, 1.10f, -0.10f), glm::vec3(0.90f, 0.75f, 0.85f));
    return best;
}

// valley-wide scatter: five more trees plus half-buried bank boulders
inline const std::array<glm::vec2, 6> kTreeSpots {
    glm::vec2 { 12.5f, 12.5f }, glm::vec2 { -1.5f, 18.5f }, glm::vec2 { -6.0f, 20.0f },
    glm::vec2 { 14.0f, 4.0f },  glm::vec2 { 18.0f, 16.0f }, glm::vec2 { 15.0f, 22.0f },
};
inline const std::array<glm::vec2, 3> kRockSpots {
    glm::vec2 { 3.0f, 9.0f }, glm::vec2 { 11.5f, 9.0f }, glm::vec2 { -1.0f, 4.5f },
};
inline const std::array<float, 3> kRockRadii { 0.85f, 0.65f, 1.00f };

inline ObjHit treesAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 8u };
    for (size_t i = 0; i < kTreeSpots.size(); ++i) {
        const glm::vec2& s = kTreeSpots[i];
        float dx = p.x - s.x, dz = p.z - s.y;
        if (dx * dx + dz * dz > 16.0f || p.y > hm.sample(s.x, s.y) + 8.4f)
            continue; // cheap reject outside the crown cylinder
        ObjHit t = treeAt(p, s, hm.sample(s.x, s.y));
        if (t.d < best.d)
            best = t;
    }
    return best;
}

inline ObjHit rocksAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 4u };
    for (size_t i = 0; i < kRockSpots.size(); ++i) {
        glm::vec2 s = kRockSpots[i];
        float r = kRockRadii[i];
        float dx = p.x - s.x, dz = p.z - s.y;
        if (dx * dx + dz * dz > (r + 0.4f) * (r + 0.4f))
            continue;
        // half-buried: faceted erratic with moss saddle (same centre/radius)
        glm::vec3 c(s.x, hm.sample(s.x, s.y) + r * 0.30f, s.y);
        float d = sdEllipsoid(p, c, glm::vec3(r, r * 0.78f, r * 0.92f));
        d += fbm2(p.x * 3.7f + float(i) * 5.0f, p.z * 3.7f) * r * 0.10f;
        if (d < best.d) {
            best.d = d;
            best.mat = uint8_t(i == 1 ? 5 : 4);
        }
        // moss saddle on top
        {
            float md = sdEllipsoid(p, c + glm::vec3(0.f, r * 0.72f, 0.f),
                                   glm::vec3(r * 0.55f, r * 0.14f, r * 0.50f));
            if (md < best.d) {
                best.d = md;
                best.mat = 1;
            }
        }
    }
    return best;
}

inline constexpr float kBushCell = 6.0f;

// --- alpaca paddock east of the cabin (constants early: scatter keeps out) --
inline constexpr glm::vec2 kPaddockMin { 8.2f, 14.2f };
inline constexpr glm::vec2 kPaddockMax { 13.8f, 19.8f };
inline constexpr float kGateCenter = 17.0f; // gate gap centre z on the west side
inline constexpr float kGateHalf = 0.75f;

// Scatter (bushes/ferns/pebbles) must never touch solid structures: the
// VoxelField component solver floods each connected shell, and wispy
// fragments grafted onto a solid shell let the exterior-air flood leak in,
// hollowing the whole component. Bulky-on-bulky (tree crowns, boulders) is
// fine; thin scatter near solids is not.
inline bool inPaddock(float x, float z, float margin)
{
    return x > kPaddockMin.x - margin && x < kPaddockMax.x + margin &&
           z > kPaddockMin.y - margin && z < kPaddockMax.y + margin;
}

inline ObjHit bushesAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 8u };
    float cell = kBushCell;
    int ix = int(floor(p.x / cell));
    int iz = int(floor(p.z / cell));
    for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
        int cx = ix + dx, cz = iz + dz;
        float hCell = hash2(float(cx) * 19.1f, float(cz) * 37.7f);
        if (hCell < 0.62f) continue;
        float jx = hash2(float(cx) * 7.3f, float(cz) * 11.1f);
        float jz = hash2(float(cx) * 13.7f, float(cz) * 17.3f);
        float hr = hash2(float(cx) * 23.1f, float(cz) * 29.7f);
        glm::vec2 center((cx + jx) * cell, (cz + jz) * cell);
        if (inPaddock(center.x, center.y, 1.5f))
            continue; // keep the fence/alpaca shells intact (see above)
        {
            float hdx = center.x - kHousePos.x, hdz = center.y - kHousePos.y;
            if (hdx * hdx + hdz * hdz < 25.0f)
                continue; // keep the cabin + porch shells intact
        }
        float H = hm.sample(center.x, center.y);
        uint8_t mat = materialAt(center.x, center.y, H);
        if (mat != 0 && mat != 1) continue;
        float slope = glm::length(hm.gradient(center.x, center.y));
        if (slope > 0.9f) continue;
        float r = 0.35f + hr * 0.45f;
        glm::vec3 c(center.x, H + r * 0.55f, center.y);
        // vertical cull
        if (p.y < H - 0.5f || p.y > c.y + r + 1.0f) continue;
        // three-lobed shrub: main crown + two side puffs + woody stems.
        // Same centre/radius contract as before; lobes stay inside r*1.15.
        float d = glm::length(p - c) - r;
        {
            glm::vec3 l1(center.x - r * 0.55f, H + r * 0.45f, center.y + r * 0.25f);
            glm::vec3 l2(center.x + r * 0.55f, H + r * 0.50f, center.y - r * 0.20f);
            d = glm::min(d, glm::length(p - l1) - r * 0.62f);
            d = glm::min(d, glm::length(p - l2) - r * 0.58f);
            d += fbm2(p.x * 5.0f + float(cx), p.z * 5.0f + float(cz)) * r * 0.12f;
            // stems visible under the crown skirt
            glm::vec3 base(center.x, H - 0.05f, center.y);
            d = glm::min(d, sdCapsule(p, base, c, 0.05f));
            d = glm::min(d, sdCapsule(p, base, l1, 0.035f));
            d = glm::min(d, sdCapsule(p, base, l2, 0.035f));
        }
        if (d < best.d) best.d = d;
    }
    return best;
}

// --- alpaca paddock east of the cabin: post-and-rail fence ------------------
// (kPaddockMin/Max/kGateCenter/kGateHalf live above bushesAt: scatter keeps
// out so wispy fragments never graft onto the fence/alpaca shells.)

inline ObjHit fenceAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 6u };

    // cheap reject: outside the perimeter band or beyond the fence line
    if (p.x < kPaddockMin.x - 0.4f || p.x > kPaddockMax.x + 0.4f ||
        p.z < kPaddockMin.y - 0.4f || p.z > kPaddockMax.y + 0.4f)
        return best;
    float gHere = hm.sample(p.x, p.z);
    if (p.y > gHere + 1.3f || p.y < gHere - 0.8f)
        return best;

    const float postR = 0.055f, railR = 0.042f;

    // L0: posts, each anchored to its own local ground (follows the slope)
    auto post = [&](glm::vec2 xy) {
        float g = hm.sample(xy.x, xy.y);
        return sdBoxF(p, glm::vec3(xy.x, g + 0.365f, xy.y), glm::vec3(postR, 0.485f, postR));
    };
    // L1: rails as capsules between neighbouring post tops (slope-following)
    auto rail = [&](glm::vec2 a, glm::vec2 b, float h) {
        glm::vec3 pa(a.x, hm.sample(a.x, a.y) + h, a.y);
        glm::vec3 pb(b.x, hm.sample(b.x, b.y) + h, b.y);
        return sdCapsule(p, pa, pb, railR);
    };

    auto runSide = [&](glm::vec2 a, glm::vec2 b, bool westSide) {
        glm::vec2 d = b - a;
        float len = glm::length(d);
        int n = glm::max(int(roundf(len / 1.4f)), 1);
        for (int i = 0; i <= n; ++i) {
            glm::vec2 xy = a + d * (float(i) / float(n));
            if (westSide && fabsf(xy.y - kGateCenter) < kGateHalf)
                continue; // keep the gate opening clear
            float dp = post(xy);
            if (dp < best.d) best.d = dp;
            // pointed cap on every post (above rail height; tests untouched)
            float g = hm.sample(xy.x, xy.y);
            float cap = sdConeY(p, xy, g + 0.85f, g + 0.97f, 0.075f, 0.01f);
            if (cap < best.d) best.d = cap;
        }
        for (int i = 0; i < n; ++i) {
            glm::vec2 a0 = a + d * (float(i) / float(n));
            glm::vec2 b0 = a + d * (float(i + 1) / float(n));
            if (westSide &&
                fabsf(0.5f * (a0.y + b0.y) - kGateCenter) < kGateHalf + 0.2f)
                continue; // gate opening toward the cabin
            for (float h : { 0.36f, 0.70f }) {
                float dr = rail(a0, b0, h);
                if (dr < best.d) best.d = dr;
            }
        }
    };
    runSide({ kPaddockMin.x, kPaddockMin.y }, { kPaddockMin.x, kPaddockMax.y }, true);
    runSide({ kPaddockMax.x, kPaddockMin.y }, { kPaddockMax.x, kPaddockMax.y }, false);
    runSide({ kPaddockMin.x, kPaddockMin.y }, { kPaddockMax.x, kPaddockMin.y }, false);
    runSide({ kPaddockMin.x, kPaddockMax.y }, { kPaddockMax.x, kPaddockMax.y }, false);
    return best;
}

// --- the paddock's resident alpaca (faces the cabin, -x) --------------------
inline constexpr glm::vec2 kAlpacaSpot { 11.1f, 16.9f };

inline ObjHit alpacaAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    float g = hm.sample(kAlpacaSpot.x, kAlpacaSpot.y);
    // cheap reject: bounding disc + vertical band around the animal
    float dx = p.x - kAlpacaSpot.x, dz = p.z - kAlpacaSpot.y;
    if (dx * dx + dz * dz > 1.44f || p.y > g + 1.7f || p.y < g - 0.2f)
        return { 1e9f, 5u };

    glm::vec3 q(p.x - kAlpacaSpot.x, p.y - g, p.z - kAlpacaSpot.y);
    ObjHit best { 1e9f, 5u };

    // L0: four thin dark legs, slightly splayed + hooves
    const glm::vec2 legXY[4] = { { -0.30f, 0.150f }, { -0.26f, -0.160f },
                                 { 0.28f, 0.155f },  { 0.32f, -0.150f } };
    for (const glm::vec2& l : legXY) {
        float dl = sdCylY(q, l, 0.0f, 0.50f, 0.052f);
        if (dl < best.d) {
            best.d = dl;
            best.mat = 2;
        }
        float hoof = sdCylY(q, l, 0.0f, 0.09f, 0.062f);
        if (hoof < best.d) {
            best.d = hoof;
            best.mat = 2;
        }
    }

    // L1: woolly body - barrel capsule smoothed into shoulder and rump blobs
    // + staple-length fleece puffs along the back and flanks (mat 5 kept)
    float body = sdCapsule(q, glm::vec3(-0.34f, 0.66f, 0.f),
                           glm::vec3(0.30f, 0.62f, 0.f), 0.295f);
    body = smin(body, sdEllipsoid(q, glm::vec3(-0.24f, 0.62f, 0.f),
                                  glm::vec3(0.26f, 0.28f, 0.25f)), 0.08f);
    body = smin(body, sdEllipsoid(q, glm::vec3(0.30f, 0.68f, 0.f),
                                  glm::vec3(0.27f, 0.31f, 0.27f)), 0.08f);
    if (body < best.d) {
        best.d = body;
        best.mat = 5;
    }
    for (int f = 0; f < 6; ++f) {
        float fx = -0.30f + float(f) * 0.12f;
        float fz = (f % 2 ? 0.20f : -0.20f) + hash2(float(f), 3.3f) * 0.06f;
        float fleece = sdEllipsoid(q, glm::vec3(fx, 0.90f, fz),
                                   glm::vec3(0.10f, 0.09f, 0.09f));
        if (fleece < best.d) {
            best.d = fleece;
            best.mat = 5;
        }
    }

    // L2: neck rising from the chest, then the head
    float neck = sdCapsule(q, glm::vec3(-0.40f, 0.72f, 0.f),
                           glm::vec3(-0.60f, 1.16f, 0.f), 0.125f);
    if (neck < best.d) {
        best.d = neck;
        best.mat = 5;
    }
    float head = sdEllipsoid(q, glm::vec3(-0.68f, 1.24f, 0.f),
                             glm::vec3(0.165f, 0.125f, 0.115f));
    if (head < best.d) {
        best.d = head;
        best.mat = 5;
    }

    // L3: dark muzzle pad and a small tail puff
    float muzzle = sdCapsule(q, glm::vec3(-0.80f, 1.21f, 0.f),
                             glm::vec3(-0.92f, 1.175f, 0.f), 0.07f);
    if (muzzle < best.d) {
        best.d = muzzle;
        best.mat = 2;
    }
    float tail = sdEllipsoid(q, glm::vec3(0.56f, 0.74f, 0.f),
                             glm::vec3(0.09f, 0.12f, 0.10f));
    if (tail < best.d) {
        best.d = tail;
        best.mat = 5;
    }

    // L4: two banana ears
    for (int s = -1; s <= 1; s += 2) {
        float ear = sdCapsule(q, glm::vec3(-0.63f, 1.33f, 0.055f * s),
                              glm::vec3(-0.55f, 1.47f, 0.085f * s), 0.032f);
        if (ear < best.d) {
            best.d = ear;
            best.mat = 5;
        }
    }
    return best;
}

// --- a wooden footbridge arching over the river ----------------------------
// Long axis runs along Z (perpendicular to the river's X-flow). The deck ends
// are anchored to the sampled bank terrain and it bows gently over the water,
// carried on timber trestles whose legs reach down into the banks/riverbed.
inline ObjHit bridgeAt(glm::vec3 p)
{
    const float bx = kBridgePos.x, bz = kBridgePos.y;
    const float halfW = 0.95f;  // half deck width (x)
    const float halfL = 6.0f;   // half span (z)
    const float arch = 0.18f;   // gentle mid-span rise above the chord

    float lx = p.x - bx, lz = p.z - bz;
    if (std::fabs(lx) > halfW + 0.5f || std::fabs(lz) > halfL + 0.5f)
        return { 1e9f, 6u };

    // anchor the deck ends to the bank ground so the bridge meets the terrain
    const HeightMap& hm = sharedHeightmap();
    float gL = hm.sample(bx, bz - halfL);
    float gR = hm.sample(bx, bz + halfL);
    auto deckYAt = [&](float zp) {
        float u = (zp + halfL) / (2.0f * halfL);
        float u01 = glm::clamp(u, 0.0f, 1.0f);
        return glm::mix(gL, gR, u01) + arch * std::sin(3.14159265f * u01);
    };
    ObjHit best { 1e9f, 6u };
    auto improve = [&](float dd, uint8_t m) {
        if (dd < best.d) {
            best.d = dd;
            best.mat = m;
        }
    };

    // stone abutments both ends (coursed blocks)
    for (float ez : { -halfL, halfL }) {
        float gy = hm.sample(bx, bz + ez);
        improve(sdBoxF(p, glm::vec3(bx, gy + 0.05f, bz + ez),
                       glm::vec3(halfW + 0.35f, 0.45f, 0.65f)),
                4u);
        improve(sdBoxF(p, glm::vec3(bx, gy + 0.42f, bz + ez),
                       glm::vec3(halfW + 0.25f, 0.10f, 0.55f)),
                5u); // cap
    }
    // deck: individual cross-planks with gaps + wheel-guard curbs + stringers
    for (float zp = -halfL + 0.18f; zp <= halfL - 0.18f + 1e-3f; zp += 0.32f) {
        float yd = deckYAt(zp);
        float wjit = (hash2(zp * 7.7f, 1.1f) - 0.5f) * 0.02f;
        improve(sdBoxF(p, glm::vec3(bx + wjit, yd - 0.07f, bz + zp),
                       glm::vec3(halfW - 0.02f, 0.06f, 0.12f)),
                6u);
    }
    for (float sx : { -halfW + 0.10f, halfW - 0.10f })
        improve(sdBoxF(p, glm::vec3(bx + sx, deckYAt(0.f) + 0.02f, bz),
                       glm::vec3(0.09f, 0.10f, halfL - 0.1f)),
                6u); // wheel guards (follow mid height; arch is gentle)
    for (float sx : { -halfW + 0.30f, halfW - 0.30f }) {
        // stringers follow the arch in 3 chord segments
        for (int s = 0; s < 3; ++s) {
            float z0 = -halfL + float(s) * (2.f * halfL / 3.f);
            float z1 = -halfL + float(s + 1) * (2.f * halfL / 3.f);
            glm::vec3 a(bx + sx, deckYAt(z0) - 0.18f, bz + z0);
            glm::vec3 b(bx + sx, deckYAt(z1) - 0.18f, bz + z1);
            improve(sdCapsule(p, a, b, 0.075f), 6u);
        }
    }

    // side rails: double top logs + mid rail, uprights, X-braced panels
    for (float sx : { -halfW + 0.06f, halfW - 0.06f }) {
        for (int s = 0; s < 3; ++s) {
            float z0 = -halfL + 0.2f + float(s) * ((2.f * halfL - 0.4f) / 3.f);
            float z1 = -halfL + 0.2f + float(s + 1) * ((2.f * halfL - 0.4f) / 3.f);
            float y0 = deckYAt(z0) + 0.55f, y1 = deckYAt(z1) + 0.55f;
            improve(sdCapsule(p, glm::vec3(bx + sx, y0, bz + z0),
                              glm::vec3(bx + sx, y1, bz + z1), 0.055f),
                    6u); // top rail
            improve(sdCapsule(p, glm::vec3(bx + sx, y0 - 0.28f, bz + z0),
                              glm::vec3(bx + sx, y1 - 0.28f, bz + z1), 0.04f),
                    6u); // mid rail
            // X brace in the panel
            improve(sdCapsule(p, glm::vec3(bx + sx, y0 - 0.28f, bz + z0),
                              glm::vec3(bx + sx, y1, bz + z1), 0.03f),
                    6u);
        }
        for (float zp = -halfL + 0.4f; zp <= halfL - 0.4f + 1e-3f; zp += 1.5f) {
            float yd = deckYAt(zp);
            improve(sdCylY(p, glm::vec2(bx + sx, bz + zp), yd, yd + 0.55f, 0.05f),
                    6u);
        }
    }

    // trestle bents: splayed legs to the ground + cap beam + diagonal + pads
    for (float zp = -halfL + 1.0f; zp <= halfL - 1.0f + 1e-3f; zp += 2.0f) {
        float yd = deckYAt(zp);
        for (int s = -1; s <= 1; s += 2) {
            float xtop = bx + s * (halfW - 0.25f);
            float xbot = bx + s * (halfW + 0.05f); // batter (splay)
            float gLeg = hm.sample(xbot, bz + zp);
            float yBot = glm::min(gLeg, WATER_LEVEL - 2.0f) - 0.4f;
            improve(sdCapsule(p, glm::vec3(xbot, yBot, bz + zp),
                              glm::vec3(xtop, yd - 0.14f, bz + zp), 0.10f),
                    6u);
            improve(sdBoxF(p, glm::vec3(xbot, gLeg + 0.02f, bz + zp),
                           glm::vec3(0.22f, 0.12f, 0.22f)),
                    4u); // stone footing pad
        }
        improve(sdCapsule(p, glm::vec3(bx - halfW + 0.2f, yd - 0.20f, bz + zp),
                          glm::vec3(bx + halfW - 0.2f, yd - 0.20f, bz + zp), 0.07f),
                6u); // cap beam
        improve(sdCapsule(p, glm::vec3(bx - halfW + 0.25f, yd - 0.25f, bz + zp),
                          glm::vec3(bx + halfW - 0.25f, yd - 1.1f, bz + zp), 0.045f),
                6u); // diagonal brace
    }
    return best;
}

// --- conifer forest backdrop (house.jpeg treeline, rewritten) ----------------
// Spruce/fir: buttress roots -> tapered leaning trunk -> 6 whorled branch
// tiers (capsule branches + drooping foliage skirts) -> leader spike.
// Base hugs the heightmap; seed varies height/girth/crown density.
inline ObjHit coniferAt(glm::vec3 p, glm::vec2 spot, float groundY, float seed)
{
    float dx = p.x - spot.x, dz = p.z - spot.y;
    if (dx * dx + dz * dz > 9.0f || p.y < groundY - 0.6f || p.y > groundY + 12.0f)
        return { 1e9f, 8u };
    glm::vec3 q(p.x - spot.x, p.y - groundY, p.z - spot.y);
    float hScale = 0.78f + seed * 0.38f; // 6..9 m tall (sky stays visible)
    float gScale = 0.85f + hash2(seed * 91.7f, seed * 57.3f) * 0.4f;
    float density = 0.75f + hash2(seed * 13.3f, seed * 41.1f) * 0.5f;
    ObjHit best { 1e9f, 6u };
    auto improve = [&](float d, uint8_t m) {
        if (d < best.d) {
            best.d = d;
            best.mat = m;
        }
    };
    // L0: buttress roots + trunk (two segments, slight lean by seed)
    float lean = (hash2(seed * 31.1f, 4.2f) - 0.5f) * 0.3f;
    float lean2 = (hash2(seed * 17.7f, 8.9f) - 0.5f) * 0.25f;
    improve(sdCylY(q, glm::vec2(0.f), -0.05f, 0.32f, 0.30f * gScale), 6u);
    for (int r = 0; r < 5; ++r) {
        float a = float(r) * 1.2566f + seed * 6.28f;
        glm::vec3 tip(std::cos(a) * 0.65f * gScale, 0.0f, std::sin(a) * 0.65f * gScale);
        improve(sdCapsule(q, glm::vec3(0.f, 0.22f, 0.f), tip, 0.10f * gScale), 6u);
    }
    improve(sdCapsule(q, glm::vec3(0.f, 0.30f, 0.f),
                      glm::vec3(lean * 0.5f, 1.60f * hScale, lean2 * 0.5f),
                      0.22f * gScale),
            6u);
    improve(sdCapsule(q, glm::vec3(lean * 0.5f, 1.55f * hScale, lean2 * 0.5f),
                      glm::vec3(lean, 2.90f * hScale, lean2), 0.14f * gScale),
            6u);
    // bark plates: 3 vertical ridges up the bole
    for (int b = 0; b < 3; ++b) {
        float a = seed * 6.28f + float(b) * 2.094f;
        glm::vec3 base(std::cos(a) * 0.20f * gScale, 0.4f, std::sin(a) * 0.20f * gScale);
        glm::vec3 top(lean * 0.4f + std::cos(a) * 0.13f * gScale, 1.9f * hScale,
                      lean2 * 0.4f + std::sin(a) * 0.13f * gScale);
        improve(sdCapsule(q, base, top, 0.05f * gScale), 6u);
    }
    // L1..L6: six whorled tiers — branches first, then drooping skirt cones.
    // Skirt bottoms flare past the branch tips; each tier is jittered so the
    // silhouette reads feathery, not lathed.
    const float baseY = 1.15f * hScale;
    for (int i = 0; i < 6; ++i) {
        float yT = baseY + float(i) * 0.88f * hScale;
        float rT = (1.85f - float(i) * 0.26f) * gScale * density;
        if (rT < 0.18f)
            rT = 0.18f;
        float cx = lean * (0.3f + 0.15f * float(i));
        float cz = lean2 * (0.3f + 0.12f * float(i));
        // 5 branches per whorl, alternating offset per tier
        int nBr = (i < 2) ? 6 : 5;
        for (int b = 0; b < nBr; ++b) {
            float a = (float(b) / float(nBr)) * 6.2831f + float(i) * 0.55f + seed * 3.0f;
            glm::vec3 tip(cx + std::cos(a) * rT * 0.92f, yT + 0.12f * hScale,
                          cz + std::sin(a) * rT * 0.92f);
            improve(sdCapsule(q, glm::vec3(cx, yT + 0.25f * hScale, cz), tip,
                              0.055f * gScale + 0.015f),
                    6u);
            // drooping branchlets near the tip (short down-angled capsules)
            glm::vec3 mid = glm::mix(glm::vec3(cx, yT + 0.25f * hScale, cz), tip, 0.7f);
            improve(sdCapsule(q, mid, mid + glm::vec3(0.f, -0.28f * hScale, 0.f),
                              0.04f * gScale + 0.01f),
                    8u);
        }
        float tierH = (i < 5 ? 1.30f : 1.05f) * hScale;
        float r1 = rT * 0.62f;
        if (r1 < 0.10f)
            r1 = 0.10f;
        float jx = (hash2(seed * 77.7f, float(i) * 3.1f) - 0.5f) * 0.20f;
        float jz = (hash2(seed * 55.5f, float(i) * 7.7f) - 0.5f) * 0.20f;
        improve(sdConeY(q, glm::vec2(cx + jx, cz + jz), yT - 0.15f * hScale,
                        yT + tierH, rT, r1),
                8u);
    }
    // L7: leader spike + top tuft; dead snag stubs low on mature trees
    improve(sdCapsule(q, glm::vec3(lean, baseY + 5.0f * hScale, lean2),
                      glm::vec3(lean * 1.1f, baseY + 5.9f * hScale, lean2), 0.06f),
            6u);
    improve(sdEllipsoid(q, glm::vec3(lean * 1.1f, baseY + 5.85f * hScale, lean2),
                        glm::vec3(0.22f * gScale, 0.35f * hScale, 0.22f * gScale)),
            8u);
    if (seed > 0.45f) {
        for (int s = 0; s < 3; ++s) {
            float a = seed * 9.0f + float(s) * 2.1f;
            float sy = (0.9f + float(s) * 0.5f) * hScale;
            glm::vec3 tip(std::cos(a) * 0.9f * gScale, sy - 0.15f,
                          std::sin(a) * 0.9f * gScale);
            improve(sdCapsule(q, glm::vec3(0.f, sy, 0.f), tip, 0.035f), 6u);
        }
        if (seed > 0.62f)
            improve(sdCylY(q, glm::vec2(lean * 1.1f), baseY + 5.9f * hScale,
                           baseY + 6.4f * hScale, 0.045f),
                    6u);
    }
    return best;
}

// ~21 conifers ringing the cabin: north backdrop with sky slots left open
// above the cabin (the fixed house-test cam needs blue in its top strip),
// east/west ridges and two near framing trees like the reference trunks.
inline const std::array<glm::vec2, 21> kConiferSpots {
    glm::vec2 { -14.f, 24.f }, glm::vec2 { -8.f, 26.f }, glm::vec2 { 16.f, 27.f },
    glm::vec2 { 22.f, 25.f },  glm::vec2 { -18.f, 20.f }, glm::vec2 { -12.f, 21.f },
    glm::vec2 { 12.f, 23.f },  glm::vec2 { 18.f, 22.f }, glm::vec2 { 24.f, 20.f },
    glm::vec2 { 16.f, 8.f },   glm::vec2 { 20.f, 12.f }, glm::vec2 { 23.f, 15.f },
    glm::vec2 { 17.f, 17.f },  glm::vec2 { 14.f, -2.f }, glm::vec2 { 20.f, 2.f },
    glm::vec2 { 24.f, 8.f },   glm::vec2 { -10.f, 8.f }, glm::vec2 { -14.f, 12.f },
    glm::vec2 { -8.f, 14.f },  glm::vec2 { -4.5f, 5.f }, glm::vec2 { 12.f, 4.f },
};

inline ObjHit forestAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 8u };
    for (size_t i = 0; i < kConiferSpots.size(); ++i) {
        const glm::vec2& s = kConiferSpots[i];
        float dx = p.x - s.x, dz = p.z - s.y;
        if (dx * dx + dz * dz > 9.0f)
            continue; // cheap reject outside the crown cylinder
        float g = hm.sample(s.x, s.y);
        if (p.y < g - 0.6f || p.y > g + 12.0f)
            continue;
        float seed = hash2(float(i) * 13.13f + 1.7f, float(i) * 7.71f + 9.2f);
        ObjHit t = coniferAt(p, s, g, seed);
        if (t.d < best.d)
            best = t;
    }
    return best;
}

// --- dock + canoe (house.jpeg left bank) ------------------------------------
// Low wooden jetty reaching south from the north bank into the channel, with
// a beached-style canoe floating alongside. Deck y is fixed near the water;
// posts drop to the riverbed.
inline constexpr glm::vec2 kDockPos { 3.6f, 6.6f }; // deck centre (x,z)
inline constexpr glm::vec2 kCanoePos { 2.25f, 6.3f }; // hull centre (x,z)

inline ObjHit docksideAt(glm::vec3 p)
{
    float dx = p.x - kDockPos.x, dz = p.z - kDockPos.y;
    float cx = p.x - kCanoePos.x, cz = p.z - kCanoePos.y;
    bool nearDock = std::fabs(dx) < 2.2f && std::fabs(dz) < 2.9f;
    bool nearCanoe = cx * cx + cz * cz < 6.25f;
    if (!nearDock && !nearCanoe)
        return { 1e9f, 6u };
    if (p.y < WATER_LEVEL - 3.2f || p.y > 1.2f)
        return { 1e9f, 6u };
    const HeightMap& hm = sharedHeightmap();
    (void)hm;
    ObjHit best { 1e9f, 6u };
    auto improve = [&](float d, uint8_t m) {
        if (d < best.d) {
            best.d = d;
            best.mat = m;
        }
    };
    // jetty: individual cross-planks with gaps on twin stringers, low over
    // the cove. Reaches from the bank (north end) into the water (south).
    const float deckY = -0.35f;
    if (nearDock) {
        // 11 cross-planks (0.24 deep, 0.06 gaps) + rim boards
        for (int i = 0; i < 11; ++i) {
            float zz = kDockPos.y - 1.60f + float(i) * 0.305f;
            float wjit = (hash2(float(i) * 7.7f, 2.2f) - 0.5f) * 0.03f;
            improve(sdBoxF(p, glm::vec3(kDockPos.x + wjit, deckY - 0.045f, zz),
                           glm::vec3(0.78f, 0.055f, 0.12f)),
                    6u);
        }
        for (float sx : { -0.78f, 0.78f })
            improve(sdBoxF(p, glm::vec3(kDockPos.x + sx, deckY - 0.05f, kDockPos.y),
                           glm::vec3(0.06f, 0.09f, 1.72f)),
                    6u); // rim boards
        for (float sx : { -0.55f, 0.55f })
            improve(sdLogZ(p, kDockPos.y, 1.70f, deckY - 0.17f,
                           kDockPos.x + sx, 0.075f),
                    6u); // stringers
        // pile pairs down into the bed with pointed shoes + X cross-bracing
        for (float zp : { -1.45f, -0.30f, 0.85f, 1.60f }) {
            for (float xp : { -0.62f, 0.62f }) {
                float yBot = WATER_LEVEL - 2.6f;
                improve(sdCylY(p, glm::vec2(kDockPos.x + xp, kDockPos.y + zp),
                               yBot, deckY - 0.05f, 0.095f),
                        6u);
                improve(sdConeY(p, glm::vec2(kDockPos.x + xp, kDockPos.y + zp),
                                yBot - 0.25f, yBot + 0.05f, 0.02f, 0.095f),
                        2u); // shoe
            }
            // cross-brace between the pair (below deck, above water)
            glm::vec3 a(kDockPos.x - 0.62f, deckY - 0.55f, kDockPos.y + zp);
            glm::vec3 b(kDockPos.x + 0.62f, deckY - 1.15f, kDockPos.y + zp);
            improve(sdCapsule(p, a, b, 0.045f), 6u);
            improve(sdCapsule(p, glm::vec3(a.x, b.y, a.z), glm::vec3(b.x, a.y, b.z),
                              0.045f),
                    6u);
        }
        // two taller mooring posts + caps + rope to the canoe + cleat
        for (float xp : { -0.62f, 0.62f }) {
            improve(sdCylY(p, glm::vec2(kDockPos.x + xp, kDockPos.y - 1.70f),
                           deckY - 0.05f, deckY + 0.75f, 0.085f),
                    6u);
            improve(sdCylY(p, glm::vec2(kDockPos.x + xp, kDockPos.y - 1.70f),
                           deckY + 0.75f, deckY + 0.83f, 0.105f),
                    6u); // cap
        }
        improve(sdCapsule(p,
                          glm::vec3(kDockPos.x - 0.62f, deckY + 0.55f, kDockPos.y - 1.70f),
                          glm::vec3(kCanoePos.x + 0.25f, WATER_LEVEL + 0.22f,
                                    kCanoePos.y - 1.20f),
                          0.025f),
                2u); // painter rope
        improve(sdBoxF(p, glm::vec3(kDockPos.x, deckY + 0.05f, kDockPos.y + 1.65f),
                       glm::vec3(0.80f, 0.05f, 0.10f)),
                6u); // end cleat board
        // shore ramp: two bank boards from the north end onto the grass
        for (float sx : { -0.40f, 0.40f })
            improve(sdCapsule(p, glm::vec3(kDockPos.x + sx, deckY - 0.05f,
                                           kDockPos.y - 1.70f),
                              glm::vec3(kDockPos.x + sx * 1.3f, deckY + 0.25f,
                                        kDockPos.y - 2.60f),
                              0.06f),
                    6u);
    }
    // canoe: cedar-strip hull (outer minus cockpit), gunwale rails, keel,
    // bow/stern decks, 3 thwarts, ribs. Rides low with a laden waterline.
    if (nearCanoe) {
        glm::vec3 hc(kCanoePos.x, WATER_LEVEL + 0.10f, kCanoePos.y);
        float outer = sdEllipsoid(p, hc, glm::vec3(0.36f, 0.30f, 1.85f));
        float inner = sdEllipsoid(p, hc + glm::vec3(0.f, 0.16f, 0.f),
                                  glm::vec3(0.25f, 0.25f, 1.62f));
        float hull = glm::max(outer, -inner);
        improve(hull, 6u);
        // gunwale rails (port/starboard sheer logs) + keel strip
        for (float gx : { -0.27f, 0.27f })
            improve(sdCapsule(p,
                              glm::vec3(kCanoePos.x + gx, WATER_LEVEL + 0.30f,
                                        kCanoePos.y - 1.60f),
                              glm::vec3(kCanoePos.x + gx * 0.4f, WATER_LEVEL + 0.34f,
                                        kCanoePos.y + 1.60f),
                              0.045f),
                    6u);
        improve(sdCapsule(p,
                          glm::vec3(kCanoePos.x, WATER_LEVEL - 0.18f, kCanoePos.y - 1.5f),
                          glm::vec3(kCanoePos.x, WATER_LEVEL - 0.18f, kCanoePos.y + 1.5f),
                          0.04f),
                2u);
        // bow/stern decks + stem posts
        for (float ez : { -1.62f, 1.62f }) {
            improve(sdBoxF(p, glm::vec3(kCanoePos.x, WATER_LEVEL + 0.22f,
                                        kCanoePos.y + ez),
                           glm::vec3(0.20f, 0.04f, 0.22f)),
                    6u);
            improve(sdCapsule(p,
                              glm::vec3(kCanoePos.x, WATER_LEVEL - 0.05f,
                                        kCanoePos.y + ez * 1.02f),
                              glm::vec3(kCanoePos.x, WATER_LEVEL + 0.32f,
                                        kCanoePos.y + ez * 0.97f),
                              0.04f),
                    6u);
        }
        // floorboards (3 longitudinal strips low in the hull) + thwarts
        for (float fx : { -0.12f, 0.f, 0.12f })
            improve(sdBoxF(p, glm::vec3(kCanoePos.x + fx, WATER_LEVEL - 0.08f,
                                        kCanoePos.y),
                           glm::vec3(0.05f, 0.03f, 1.30f)),
                    2u);
        for (float sz : { -0.60f, 0.05f, 0.65f })
            improve(sdBoxF(p, glm::vec3(kCanoePos.x, WATER_LEVEL + 0.12f,
                                        kCanoePos.y + sz),
                           glm::vec3(0.24f, 0.04f, 0.11f)),
                    6u);
    }
    return best;
}

// --- shoreline boulders + pebbles -------------------------------------------
inline const std::array<glm::vec3, 10> kShoreRocks {
    glm::vec3 { 1.0f, 0.f, 5.4f }, glm::vec3 { 2.0f, 0.f, 4.4f },
    glm::vec3 { 4.6f, 0.f, 4.3f }, glm::vec3 { 6.0f, 0.f, 5.4f },
    glm::vec3 { 6.4f, 0.f, 7.4f }, glm::vec3 { 5.4f, 0.f, 8.8f },
    glm::vec3 { 1.6f, 0.f, 9.2f }, glm::vec3 { 0.8f, 0.f, 7.2f },
    glm::vec3 { -2.0f, 0.f, 6.0f }, glm::vec3 { 7.0f, 0.f, 4.0f },
};
inline const std::array<float, 10> kShoreRadii {
    0.85f, 0.55f, 0.70f, 0.90f, 0.60f, 0.75f, 0.50f, 0.65f, 0.40f, 0.55f,
};

inline ObjHit shoreAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 4u };
    auto improve = [&](float d, uint8_t m) {
        if (d < best.d) {
            best.d = d;
            best.mat = m;
        }
    };
    // half-buried boulders: faceted glacial erratics with moss caps and a
    // dark wet waterline band. Centres ride max(bed, waterline-0.3).
    for (size_t i = 0; i < kShoreRocks.size(); ++i) {
        float sx = kShoreRocks[i].x, sz = kShoreRocks[i].z;
        float r = kShoreRadii[i];
        float dx = p.x - sx, dz = p.z - sz;
        if (dx * dx + dz * dz > (r + 0.6f) * (r + 0.6f))
            continue;
        float g = hm.sample(sx, sz);
        float cy = glm::max(g, WATER_LEVEL - 0.30f) + r * 0.25f;
        float jx = (hash2(float(i) * 3.7f, 1.1f) - 0.5f) * 0.2f;
        float jz = (hash2(float(i) * 8.3f, 4.4f) - 0.5f) * 0.2f;
        glm::vec3 c(sx + jx, cy, sz + jz);
        glm::vec3 rad(r, r * 0.72f, r * 0.88f);
        float d = sdEllipsoid(p, c, rad);
        // facets: two intersecting plane cuts + noise dents (glacial faces)
        float facet = sdBoxF(p, c + glm::vec3(r * 0.35f, r * 0.30f, 0.f),
                             glm::vec3(r * 0.55f, r * 0.45f, r * 0.70f));
        d = glm::max(d, -(facet + r * 0.18f));
        d += fbm2(p.x * 3.1f + float(i) * 7.0f, p.z * 3.1f) * r * 0.10f;
        if (d < 0.35f) {
            uint8_t m = uint8_t(i % 3 == 1 ? 5 : 4);
            // wet band near the waterline reads darker
            if (p.y < WATER_LEVEL + 0.12f)
                m = 4;
            improve(d, m);
            // moss cap on the sheltered top (north-east bias, like the ref)
            glm::vec3 mc(c.x + r * 0.10f, c.y + rad.y * 0.92f, c.z - r * 0.05f);
            float md = sdEllipsoid(p, mc, glm::vec3(r * 0.55f, r * 0.16f, r * 0.50f));
            improve(md, (i % 2) ? uint8_t(0) : uint8_t(1));
            // perched pebble on big boulders
            if (r > 0.65f) {
                glm::vec3 pc(c.x - r * 0.25f, c.y + rad.y + 0.03f, c.z + r * 0.2f);
                improve(glm::length(p - pc) - 0.09f, 5u);
            }
        }
    }
    // rapids boulders mid-channel upstream (whitewater of the reference):
    // break the surface over the riffle bars.
    {
        const glm::vec2 rapids[4] = { { -5.5f, 6.2f }, { -8.0f, 3.4f },
                                      { -11.0f, 5.2f }, { -3.0f, 8.6f } };
        for (int i = 0; i < 4; ++i) {
            float sx = rapids[i].x, sz = rapids[i].y;
            float r = 0.55f + hash2(float(i) * 3.3f, 8.8f) * 0.35f;
            float dx = p.x - sx, dz = p.z - sz;
            if (dx * dx + dz * dz > (r + 0.6f) * (r + 0.6f))
                continue;
            float g = hm.sample(sx, sz);
            float cy = glm::max(g, WATER_LEVEL - 0.55f) + r * 0.45f;
            float d = sdEllipsoid(p, glm::vec3(sx, cy, sz),
                                  glm::vec3(r, r * 0.8f, r * 0.9f));
            d += fbm2(p.x * 4.0f + float(i) * 3.0f, p.z * 4.0f) * r * 0.12f;
            improve(d, i % 2 ? 5u : 4u);
        }
    }
    // pebble + cobble hash grid along the waterline band (5-22 cm stones,
    // two size classes; cobbles cluster at riffle heads like the reference)
    {
        const float cell = 1.25f;
        int ix = int(floor(p.x / cell)), iz = int(floor(p.z / cell));
        for (int oz = -1; oz <= 1; ++oz)
            for (int ox = -1; ox <= 1; ++ox) {
                int cxx = ix + ox, czz = iz + oz;
                float hCell = hash2(float(cxx) * 3.1f + 5.0f, float(czz) * 4.7f);
                if (hCell < 0.38f)
                    continue;
                float jx = hash2(float(cxx) * 7.3f, float(czz) * 1.9f);
                float jz = hash2(float(cxx) * 1.3f, float(czz) * 9.1f);
                float hr = hash2(float(cxx) * 5.9f, float(czz) * 3.3f);
                float hr2 = hash2(float(cxx) * 9.4f + 2.0f, float(czz) * 6.1f);
                glm::vec2 cc((cxx + jx) * cell, (czz + jz) * cell);
                // keep pebbles off the jetty + canoe shells (with air-band
                // margin: a neighbour component's air band must not reach
                // solid cells)
                if (std::fabs(cc.x - kDockPos.x) < 2.2f &&
                    std::fabs(cc.y - kDockPos.y) < 3.2f)
                    continue;
                {
                    float cdx = cc.x - kCanoePos.x, cdz = cc.y - kCanoePos.y;
                    if (cdx * cdx + cdz * cdz < 7.5f)
                        continue;
                }
                // keep clear of the cabin + porch + path (footprint + margin),
                // the paddock, and the bridge trestle strip
                {
                    float hdx = cc.x - kHousePos.x, hdz = cc.y - kHousePos.y;
                    float qx = std::fabs(hdx) - 2.9f; // half footprint x
                    float qz1 = hdz - 2.3f;           // beyond back wall (+z)
                    float qz0 = -hdz - 4.7f;          // beyond porch steps (-z)
                    float ox = glm::max(qx, 0.f);
                    float oz = glm::max(glm::max(qz1, qz0), 0.f);
                    float rectD = std::hypot(ox, oz);
                    if (rectD < 0.9f)
                        continue;
                    if (inPaddock(cc.x, cc.y, 1.5f))
                        continue;
                    if (std::fabs(cc.x - kBridgePos.x) < 2.4f && cc.y > -1.0f &&
                        cc.y < 14.0f)
                        continue;
                }
                float H = hm.sample(cc.x, cc.y);
                float wd = WATER_LEVEL - H;
                if (wd < -0.60f || wd > 0.50f)
                    continue; // waterline band only
                float r = (hr2 > 0.72f ? 0.14f + hr * 0.09f : 0.05f + hr * 0.07f);
                if (p.y < H - 0.3f || p.y > H + r + 0.6f)
                    continue;
                glm::vec3 pc(cc.x, H + r * 0.45f, cc.y);
                float d = sdEllipsoid(p, pc,
                                      glm::vec3(r, r * 0.62f, r * 0.85f));
                if (d < best.d) {
                    best.d = d;
                    // wet stones below the waterline read dark, dry ones pale
                    uint8_t m;
                    if (H < WATER_LEVEL + 0.05f)
                        m = 4;
                    else
                        m = uint8_t(hr > 0.55f ? 5 : 4);
                    best.mat = m;
                }
                // satellite grit beside big cobbles
                if (hr2 > 0.72f) {
                    glm::vec3 gc(cc.x + r * 1.6f, H + 0.02f, cc.y - r * 1.2f);
                    float gd = glm::length(p - gc) - 0.035f;
                    if (gd < best.d) {
                        best.d = gd;
                        best.mat = 5;
                    }
                }
            }
    }
    return best;
}

// --- foreground ferns: low foliage tufts hugging damp grass near the water --
inline constexpr float kFernCell = 3.0f;

inline ObjHit fernsAt(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    ObjHit best { 1e9f, 8u };
    const float cell = kFernCell;
    int ix = int(floor(p.x / cell)), iz = int(floor(p.z / cell));
    for (int oz = -1; oz <= 1; ++oz)
        for (int ox = -1; ox <= 1; ++ox) {
            int cxx = ix + ox, czz = iz + oz;
            float hCell = hash2(float(cxx) * 11.7f + 2.0f, float(czz) * 9.3f);
            if (hCell < 0.55f)
                continue;
            float jx = hash2(float(cxx) * 7.3f, float(czz) * 11.1f);
            float jz = hash2(float(cxx) * 13.7f, float(czz) * 17.3f);
            float hr = hash2(float(cxx) * 23.1f, float(czz) * 29.7f);
            glm::vec2 cc((cxx + jx) * cell, (czz + jz) * cell);
            // keep clear of every solid structure (cabin, jetty, paddock,
            // footbridge) so wispy blades never graft onto solid shells
            {
                float hdx = cc.x - kHousePos.x, hdz = cc.y - kHousePos.y;
                if (hdx * hdx + hdz * hdz < 30.0f)
                    continue;
                float ddx = cc.x - kDockPos.x, ddz = cc.y - kDockPos.y;
                if (ddx * ddx + ddz * ddz < 9.0f)
                    continue;
                if (inPaddock(cc.x, cc.y, 1.5f))
                    continue;
                if (std::fabs(cc.x - kBridgePos.x) < 2.4f && cc.y > -1.0f &&
                    cc.y < 14.0f)
                    continue; // bridge trestle strip
            }
            float H = hm.sample(cc.x, cc.y);
            uint8_t mat = materialAt(cc.x, cc.y, H);
            if (mat != 0 && mat != 1 && mat != 2)
                continue;
            float wd = WATER_LEVEL - H;
            if (wd < -2.2f || wd > 0.6f)
                continue; // damp meadow band
            if (glm::length(hm.gradient(cc.x, cc.y)) > 1.0f)
                continue;
            float r = 0.28f + hr * 0.30f;
            glm::vec3 c(cc.x, H + r * 0.45f, cc.y);
            if (p.y < H - 0.4f || p.y > c.y + r + 0.8f)
                continue;
            // fern rosette: heart bulb + 6 arching fronds (2-segment each)
            // + 2 upright fiddleheads. All blades >= 1 voxel thick.
            float d = glm::length((p - c) / glm::vec3(1.f, 0.62f, 1.f)) * r - r;
            for (int f = 0; f < 6; ++f) {
                float a = (float(f) / 6.f) * 6.2831f +
                          hash2(float(cxx) * 3.7f, float(czz) * 5.9f) * 6.28f;
                float len = r * (0.95f + hash2(float(f) * 7.1f, hr * 9.0f) * 0.5f);
                glm::vec3 elbow(cc.x + std::cos(a) * len * 0.55f, H + r * 1.05f,
                                cc.y + std::sin(a) * len * 0.55f);
                glm::vec3 tip(cc.x + std::cos(a) * len, H + r * 0.75f,
                              cc.y + std::sin(a) * len);
                d = glm::min(d, sdCapsule(p, c, elbow, 0.055f));
                d = glm::min(d, sdCapsule(p, elbow, tip, 0.045f));
            }
            d = glm::min(d, sdCapsule(p, c,
                                      glm::vec3(cc.x, H + r * 1.45f, cc.y), 0.06f));
            for (int f = 0; f < 2; ++f) {
                float a = hash2(float(f) * 11.0f, hr * 7.0f) * 6.28f;
                glm::vec3 tip(cc.x + std::cos(a) * r * 0.2f, H + r * 1.35f,
                              cc.y + std::sin(a) * r * 0.2f);
                d = glm::min(d, sdCapsule(p, c, tip, 0.05f));
            }
            if (d < best.d) {
                best.d = d;
                best.mat = 8;
            }
        }
    return best;
}


} // namespace vf::voxel
