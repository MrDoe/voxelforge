#pragma once
// Shared deterministic scene definition used by every voxel backend.
// v2: river-valley landscape - meandering stream channel, layered materials,
// boulder accents. The log hut is placed by worldgen (see hut.hpp).
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

inline const std::array<glm::vec3, 8> kPalette {
    glm::vec3 { 0.32f, 0.46f, 0.22f }, // 0 grass dark
    glm::vec3 { 0.45f, 0.58f, 0.26f }, // 1 grass light
    glm::vec3 { 0.52f, 0.44f, 0.30f }, // 2 soil
    glm::vec3 { 0.62f, 0.55f, 0.42f }, // 3 sand
    glm::vec3 { 0.42f, 0.41f, 0.40f }, // 4 rock
    glm::vec3 { 0.50f, 0.48f, 0.46f }, // 5 light rock
    glm::vec3 { 0.36f, 0.28f, 0.20f }, // 6 wood (logs/hut)
    glm::vec3 { 0.14f, 0.30f, 0.15f }, // 7 pine foliage
};

inline constexpr float WATER_LEVEL = -0.9f;
inline constexpr glm::vec2 kHutPad { -14.0f, -15.5f };
inline constexpr float HUT_PAD_H = 0.35f;

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

inline float sdBox(glm::vec3 p, glm::vec3 c, glm::vec3 b)
{
    glm::vec3 q = abs(p - c) - b;
    return length(glm::max(q, 0.0f)) + glm::min(glm::max(q.x, glm::max(q.y, q.z)), 0.0f);
}

// --- landscape shape functions --------------------------------------------
// stream centerline z as function of x
inline float streamCenterZ(float x) { return 16.0f * sinf(x * 0.05f); }

// normalized cross-stream coordinate (0 = center, 1 = bank edge)
inline float streamCross(float x, float z)
{
    return std::abs(z - streamCenterZ(x)) / (2.1f + 0.6f * sinf(x * 0.11f + 2.0f));
}

// base meadow height (before channel carving)
inline float baseHeight(float x, float z)
{
    return 1.25f + fbm2(x * 0.03f, z * 0.03f) * 1.5f + 0.4f * sinf(z * 0.07f + x * 0.02f);
}

// final terrain height including carved channel
inline float terrainHeight(float x, float z)
{
    float t = streamCross(x, z);
    float carve = 1.8f * (1.0f - smoothstepf(0.15f, 1.55f, t));
    // hut pad flattening
    glm::vec2 hp = kHutPad;
    float pd = length(glm::vec2(x, z) - hp);
    float pad = smoothstepf(4.2f, 6.5f, pd); // 0 inside pad, 1 outside
    float padH = HUT_PAD_H;
    float h = baseHeight(x, z) - carve;
    return glm::mix(padH, h, pad);
}

struct SceneSample {
    float d;
    uint8_t mat;
};

inline SceneSample scene(glm::vec3 p)
{
    float H = terrainHeight(p.x, p.z);
    float d = p.y - H;

    // material selection -------------------------------------------------
    float t = streamCross(p.x, p.z);
    float n = fbm2(p.x * 0.35f, p.z * 0.35f);
    uint8_t mat = n > 0.25f ? 1 : 0;                 // grass variation
    if (d > -0.06f && d < 0.05f && n > 0.0f)
        mat = 1;

    float wetness = 1.0f - smoothstepf(WATER_LEVEL + 0.05f, WATER_LEVEL + 0.55f, p.y);
    if (t < 1.15f) {
        // channel: rocky bed, sandy margins
        if (p.y < WATER_LEVEL + 0.25f)
            mat = 4;                                   // submerged bed rock
        else if (t > 0.75f)
            mat = 3;                                   // gravel/sand shore
    } else if (wetness > 0.55f && t < 1.8f) {
        mat = 3;                                       // damp sand strip
    } else if (wetness > 0.25f) {
        mat = 2;                                       // dark soil band
    }

    // boulders -------------------------------------------------------------
    auto sdSph = [](glm::vec3 q, glm::vec3 c, float r) { return length(q - c) - r; };
    float bA = sdSph(p, { 13.f, 2.2f, -13.f }, 2.4f);
    float bB = sdSph(p, { -26.f, 1.6f, 18.f }, 3.0f);
    float bC = sdSph(p, { 8.f, 2.0f, 26.f }, 1.7f);
    float boulders = glm::min(bA, glm::min(bB, bC));
    if (boulders < d) {
        d = glm::min(d, boulders);
        if (boulders < 0.3f)
            mat = n > 0.0f ? 4 : 5;
    }


    // --- log cabin ---------------------------------------------------------
    {
        glm::vec3 q = p - glm::vec3(kHutPad.x, HUT_PAD_H, kHutPad.y);
        // walls: stacked horizontal logs, footprint 4.4 x 3.4 m, height 2.1 m
        float wallD = 1e9f;
        const float lr = 0.155f;
        const int courses = 7;
        for (int c = 0; c < courses; ++c) {
            float ly = 0.075f + c * (2.0f / float(courses));
            // long walls (along x) at z = +/-1.7
            glm::vec3 lp = glm::vec3(q.x, q.y - ly, std::abs(q.z) - 1.7f);
            float segZ = lp.z;
            float dLong = length(glm::max(glm::vec2(std::abs(lp.x) - 2.2f, segZ),
                                          glm::vec2(0.0f))) - lr;
            dLong = length(glm::vec2(glm::max(std::abs(lp.x) - 2.2f, 0.0f), lp.z)) - lr;
            // short walls (along z) at x = +/-2.2
            glm::vec3 sp = glm::vec3(std::abs(q.x) - 2.2f, q.y - ly, q.z);
            float dShort = length(glm::vec2(glm::max(sp.x, 0.0f), sp.z)) - lr;
            wallD = glm::min(wallD, glm::min(dLong, dShort));
        }
        // gable roof: two tilted slabs meeting at ridge y=3.3
        auto sdRoof = [&](float side) {
            float a = glm::radians(38.0f);
            glm::vec3 rp = q - glm::vec3(0.0f, 3.05f, 0.0f);
            float cs = cosf(a * side), sn = sinf(a * side);
            glm::vec3 rr(rp.x, rp.y * cs - rp.z * sn, rp.y * sn + rp.z * cs);
            return length(glm::vec2(glm::max(std::abs(rr.x) - 2.7f, 0.0f),
                                    glm::max(rr.y, 0.0f))) +
                   glm::min(glm::max(rr.y, std::abs(rr.z) - 0.09f), 0.0f) - 0.075f;
        };
        float roof = glm::min(sdRoof(1.0f), sdRoof(-1.0f));
        float hut = glm::min(wallD, roof);
        // door opening: south wall (+z), 0.9 wide x 1.6 high
        float door = sdBox(q, glm::vec3(0.55f, 1.05f, 1.85f),
                           glm::vec3(0.45f, 0.8f, 0.4f));
        hut = glm::max(hut, -door);

        if (hut < d) {
            d = glm::min(d, hut);
            if (hut < 0.25f)
                mat = roof < wallD ? 4 : 6;
        }
    }

    // --- pines ---------------------------------------------------------------
    {
        auto pine = [&](glm::vec2 base, float hgt) {
            float gy = terrainHeight(base.x, base.y);
            glm::vec3 q = p - glm::vec3(base.x, gy, base.y);
            float trunk = length(glm::vec2(q.x, q.z)) - 0.22f;
            trunk = glm::max(trunk, -(q.y - 0.0f));
            trunk = glm::max(trunk, q.y - hgt * 0.45f);
            float canopy = 1e9f;
            for (int k = 0; k < 3; ++k) {
                float cy = hgt * (0.32f + 0.21f * k);
                float cr = hgt * (0.30f - 0.065f * k);
                float dc = length(glm::vec2(q.x, q.z)) - cr * (1.0f - (q.y - cy) / (hgt*0.30f));
                dc = glm::max(dc, q.y - cy - hgt * 0.22f);
                dc = glm::max(dc, cy - q.y);
                canopy = glm::min(canopy, dc);
            }
            return glm::vec2(glm::min(trunk, canopy), canopy < trunk ? 7.0f : 6.0f);
        };
        glm::vec2 spots[4] = { {-4.f, 12.f}, {22.f, 0.f}, {-27.f, -4.f}, {12.f, 20.f} };
        for (int k = 0; k < 4; ++k) {
            glm::vec2 dr = pine(spots[k], 6.0f + 1.5f * hash2(spots[k].x, spots[k].y));
            if (dr.x < d) {
                d = glm::min(d, dr.x);
                if (dr.x < 0.2f)
                    mat = uint8_t(dr.y);
            }
        }
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
