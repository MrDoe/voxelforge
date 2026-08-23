#pragma once
// Shared deterministic scene definition used by every voxel backend.
// v3: heightmap-driven river valley - hills + carved stream channel.
// All geometry derives from assets/heightmap.png via sharedHeightmap().
#include "voxel/heightmap.hpp"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <cmath>

namespace vf::voxel {

constexpr float WORLD = 102.4f;
constexpr float VOXEL = 0.1f;
constexpr int CHUNK_N = 64;
constexpr float CHUNK_M = float(CHUNK_N) * VOXEL;
constexpr int GRID_N = int(WORLD / CHUNK_M);

inline const std::array<glm::vec3, 9> kPalette {
    glm::vec3 { 0.07f, 0.52f, 0.06f }, // 0 grass dark - vivid
    glm::vec3 { 0.16f, 0.68f, 0.10f }, // 1 grass light - vivid
    glm::vec3 { 0.62f, 0.36f, 0.14f }, // 2 soil - warm brown
    glm::vec3 { 0.84f, 0.72f, 0.38f }, // 3 sand - golden
    glm::vec3 { 0.48f, 0.42f, 0.38f }, // 4 rock
    glm::vec3 { 0.66f, 0.64f, 0.60f }, // 5 light rock
    glm::vec3 { 0.62f, 0.33f, 0.10f }, // 6 wood (logs/trunk) - reddish
    glm::vec3 { 0.42f, 0.22f, 0.10f }, // 7 roof shingles - deep brown
    glm::vec3 { 0.04f, 0.52f, 0.03f }, // 8 foliage - vivid green
};

// per-material surface attributes, 0-255: x = reflectivity, y = roughness
inline const std::array<glm::vec2, 9> kMaterialReflection {
    glm::vec2 { 35.f, 235.f },  // 0 grass dark
    glm::vec2 { 40.f, 230.f },  // 1 grass light
    glm::vec2 { 55.f, 225.f },  // 2 soil
    glm::vec2 { 130.f, 190.f }, // 3 sand
    glm::vec2 { 95.f, 150.f },  // 4 rock
    glm::vec2 { 115.f, 135.f }, // 5 light rock
    glm::vec2 { 70.f, 160.f },  // 6 wood
    glm::vec2 { 60.f, 170.f },  // 7 roof
    glm::vec2 { 30.f, 235.f },  // 8 foliage
};

inline constexpr float WATER_LEVEL = -0.9f;

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
inline uint8_t materialAt(float x, float z, float H)
{
    const HeightMap& hm = sharedHeightmap();
    float wd = WATER_LEVEL - H; // >0: column submerged
    float slope = glm::length(hm.gradient(x, z));
    float n = fbm2(x * 0.35f, z * 0.35f);

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
    return n > 0.25f ? uint8_t(1) : uint8_t(0);    // grass
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
    wall = glm::max(wall, glm::max(-door, glm::max(-winA, -winB)));
    if (wall < best.d) {
        best.d = wall;
        best.mat = 6;
    }

    // L9..: stepped shingle layers from eaves to ridge (gable along X)
    float ry0 = 0.42f + lr + 7.f * pitch + 0.22f;
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
    for (const glm::vec2& s : kTreeSpots) {
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

inline SceneSample scene(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    float H = hm.sample(p.x, p.z);
    float d = p.y - H;
    uint8_t mat = materialAt(p.x, p.z, H);

    ObjHit obj = houseAt(p);
    ObjHit t = treesAt(p);
    if (t.d < obj.d) {
        obj.d = t.d;
        obj.mat = t.mat;
    }
    ObjHit r = rocksAt(p);
    if (r.d < obj.d) {
        obj.d = r.d;
        obj.mat = r.mat;
    }
    if (obj.d < d) {
        d = obj.d;
        mat = obj.mat;
    }
    return { d, mat };
}

// snorm8 helpers shared with GPU encodings
inline uint8_t encodeSnormByte(float v)
{
    auto c = static_cast<int8_t>(lround(glm::clamp(v, -127.0f, 127.0f)));
    return static_cast<uint8_t>(c);
}
inline float decodeSnormByte(uint8_t b) { return float(int8_t(b)); }

} // namespace vf::voxel
