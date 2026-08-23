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

inline const std::array<glm::vec3, 6> kPalette {
    glm::vec3 { 0.32f, 0.46f, 0.22f }, // 0 grass dark
    glm::vec3 { 0.45f, 0.58f, 0.26f }, // 1 grass light
    glm::vec3 { 0.52f, 0.44f, 0.30f }, // 2 soil
    glm::vec3 { 0.62f, 0.55f, 0.42f }, // 3 sand
    glm::vec3 { 0.42f, 0.41f, 0.40f }, // 4 rock
    glm::vec3 { 0.50f, 0.48f, 0.46f }, // 5 light rock
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

inline SceneSample scene(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    float H = hm.sample(p.x, p.z);
    float d = p.y - H;

    float wd = WATER_LEVEL - H; // >0: column submerged
    float slope = glm::length(hm.gradient(p.x, p.z));
    float n = fbm2(p.x * 0.35f, p.z * 0.35f);

    uint8_t mat;
    if (wd > 0.55f)
        mat = n > 0.0f ? uint8_t(5) : uint8_t(4); // deeper bed: rock
    else if (wd > -0.02f)
        mat = 3;                                  // gravel bed / waterline sand
    else if (wd > -0.45f)
        mat = n > 0.15f ? uint8_t(3) : uint8_t(2); // damp shore: sand & soil
    else if (slope > 0.85f)
        mat = n > 0.0f ? uint8_t(4) : uint8_t(5); // steep slopes: rock
    else if (wd > -1.2f)
        mat = 2;                                  // floodplain soil band
    else
        mat = n > 0.25f ? uint8_t(1) : uint8_t(0); // grass

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
