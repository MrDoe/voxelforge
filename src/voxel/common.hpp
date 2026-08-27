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

// log cabin: stone foundation -> alternating log courses -> carved openings ->
// stepped shingle layers -> chimney. ~5.2 x 4.5 m footprint, ridge ~3.9 m.
inline ObjHit houseAt(glm::vec3 p)
{
    glm::vec3 q(p.x - kHousePos.x, p.y - kPadY, p.z - kHousePos.y);
    const float hx = 2.6f, hz = 2.0f;
    ObjHit best { 1e9f, 4u };

    // L0: stone foundation slab, proud of the walls by a hand's width
    best.d = sdBoxF(q, glm::vec3(0.f, 0.05f, 0.f),
                    glm::vec3(hx + 0.28f, 0.30f, hz + 0.28f));
    best.mat = 4;

    // L1..L8: stacked log courses, orientation alternates like real blockwork
    const float lr = 0.145f, pitch = 0.27f;
    float ry0 = 0.42f + lr + 7.f * pitch + 0.22f; // roof base (q.y)
    float wall = 1e9f;
    for (int k = 0; k < 8; ++k) {
        float y = 0.42f + lr + float(k) * pitch;
        if (k % 2 == 0)
            wall = glm::min(wall, glm::min(sdLogX(q, 0.f, hx, y, hz, lr),
                                           sdLogX(q, 0.f, hx, y, -hz, lr)));
        else
            wall = glm::min(wall, glm::min(sdLogZ(q, 0.f, hz, y, hx, lr),
                                           sdLogZ(q, 0.f, hz, y, -hx, lr)));
    }
    // door opening on the river side (-z), two windows on the long walls
    float door = sdBoxF(q, glm::vec3(-0.7f, 1.30f, -hz), glm::vec3(0.52f, 0.95f, 0.6f));
    float winA = sdBoxF(q, glm::vec3(-hx, 1.62f, 0.75f), glm::vec3(0.6f, 0.42f, 0.45f));
    float winB = sdBoxF(q, glm::vec3(hx, 1.62f, 0.75f), glm::vec3(0.6f, 0.42f, 0.45f));
    float carve = glm::max(-door, glm::max(-winA, -winB));
    wall = glm::max(wall, carve);
    // Enclose the interior as a true solid SDF (negative inside) so the house
    // reads solid at every distance instead of a hollow log cage. The same
    // door/window carve opens it.
    float solidBox = sdBoxF(q, glm::vec3(0.f, (0.35f + ry0) * 0.5f, 0.f),
                            glm::vec3(hx + 0.20f, (ry0 - 0.35f) * 0.5f, hz + 0.20f));
    solidBox = glm::max(solidBox, carve);
    if (solidBox < best.d) {
        best.d = solidBox;
        best.mat = 6;
    }
    if (wall < best.d) {
        best.d = wall;
        best.mat = 6;
    }

    // L9..: stepped shingle layers from eaves to ridge (gable along X)
    ry0 = 0.42f + lr + 7.f * pitch + 0.22f;
    for (int i = 0; i < 10; ++i) {
        float t = float(i) / 9.0f;
        float rz = glm::mix(hz + 0.66f, 0.14f, t);
        float dslab = sdBoxF(q, glm::vec3(0.f, ry0 + float(i) * 0.145f, 0.f),
                             glm::vec3(hx + 0.58f, 0.09f, rz));
        if (dslab < best.d) {
            best.d = dslab;
            best.mat = 7;
        }
    }

    // chimney through the roof on the east side
    float chim = sdBoxF(q, glm::vec3(1.55f, ry0 + 0.85f, 0.75f),
                        glm::vec3(0.30f, 1.45f, 0.30f));
    if (chim < best.d) {
        best.d = chim;
        best.mat = 4;
    }
    return best;
}

// broadleaf tree: root flare -> tapered trunk -> four foliage tiers (~7.5 m)
// base y comes from the heightmap so trees hug whatever ground they stand on
inline ObjHit treeAt(glm::vec3 p, glm::vec2 spot, float groundY)
{
    glm::vec3 q(p.x - spot.x, p.y - groundY, p.z - spot.y);
    ObjHit best { 1e9f, 6u };

    best.d = sdCylY(q, glm::vec2(0.f), -0.05f, 0.20f, 0.37f);      // root flare
    float d = sdCylY(q, glm::vec2(0.f), 0.20f, 0.45f, 0.28f);
    if (d < best.d) best.d = d;
    d = sdCylY(q, glm::vec2(0.f), 0.45f, 1.70f, 0.235f);
    if (d < best.d) best.d = d;
    d = sdCylY(q, glm::vec2(0.f), 1.70f, 2.70f, 0.195f);
    if (d < best.d) best.d = d;
    d = sdCylY(q, glm::vec2(0.f), 2.70f, 3.25f, 0.165f);
    if (d < best.d) best.d = d;

    auto tier = [&](glm::vec3 c, float r) {
        float ds = glm::length(q - c) - r;
        if (ds < best.d) {
            best.d = ds;
            best.mat = 8;
        }
    };
    tier(glm::vec3(-0.45f, 3.75f, 0.30f), 1.50f);
    tier(glm::vec3(0.50f, 4.35f, -0.25f), 1.70f);
    tier(glm::vec3(0.00f, 5.25f, 0.10f), 1.50f);
    tier(glm::vec3(0.10f, 6.05f, -0.05f), 1.05f);
    return best;
}

// valley-wide scatter: five more trees plus half-buried bank boulders
inline const std::array<glm::vec2, 6> kTreeSpots {
    glm::vec2 { 10.5f, 11.5f }, glm::vec2 { 2.0f, 17.0f },  glm::vec2 { -6.0f, 20.0f },
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
        // half-buried: center sits below local ground
        glm::vec3 c(s.x, hm.sample(s.x, s.y) + r * 0.30f, s.y);
        float d = glm::length(p - c) - r;
        if (d < best.d) {
            best.d = d;
            best.mat = uint8_t(i == 1 ? 5 : 4);
        }
    }
    return best;
}

inline constexpr float kBushCell = 6.0f;

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
        float H = hm.sample(center.x, center.y);
        uint8_t mat = materialAt(center.x, center.y, H);
        if (mat != 0 && mat != 1) continue;
        float slope = glm::length(hm.gradient(center.x, center.y));
        if (slope > 0.9f) continue;
        float r = 0.35f + hr * 0.45f;
        glm::vec3 c(center.x, H + r * 0.55f, center.y);
        // vertical cull
        if (p.y < H - 0.5f || p.y > c.y + r + 1.0f) continue;
        float d = glm::length(p - c) - r;
        if (d < best.d) best.d = d;
    }
    return best;
}

// --- alpaca paddock east of the cabin: post-and-rail fence ------------------
inline constexpr glm::vec2 kPaddockMin { 8.2f, 14.2f };
inline constexpr glm::vec2 kPaddockMax { 13.8f, 19.8f };
inline constexpr float kGateCenter = 17.0f; // gate gap centre z on the west side
inline constexpr float kGateHalf = 0.75f;

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

    // L0: four thin dark legs, slightly splayed
    const glm::vec2 legXY[4] = { { -0.30f, 0.150f }, { -0.26f, -0.160f },
                                 { 0.28f, 0.155f },  { 0.32f, -0.150f } };
    for (const glm::vec2& l : legXY) {
        float dl = sdCylY(q, l, 0.0f, 0.50f, 0.052f);
        if (dl < best.d) {
            best.d = dl;
            best.mat = 2;
        }
    }

    // L1: woolly body - barrel capsule smoothed into shoulder and rump blobs
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


} // namespace vf::voxel
