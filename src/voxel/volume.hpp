#pragma once
// Dense debug/reference volume - thin wrapper over the shared scene function.
#include "voxel/common.hpp"
#include <vector>

namespace vf::voxel {

struct DenseVolume {
    int n = 256;
    float worldSize = 25.6f;              // edge length in meters
    glm::vec3 originOffset { 0.0f, 0.0f, 0.0f }; // region center in world space

    std::vector<float> sdf;      // meters, n^3
    std::vector<uint8_t> albedo; // RGB8, n^3 * 3
    std::vector<uint8_t> matId;  // palette index, n^3

    void generate();
    float sampleTrilinear(glm::vec3 localP) const; // local coords centered at origin
};

constexpr float MAX_ENCODED_DIST = 1.27f; // snorm8 clamp range for dense path (meters)

inline uint8_t encodeSnorm8(float v)
{
    float x = glm::clamp(v / MAX_ENCODED_DIST, -1.0f, 1.0f);
    auto c = static_cast<int8_t>(lround(x * 127.0f));
    return static_cast<uint8_t>(c);
}
inline float decodeSnorm8(uint8_t b)
{
    float c = static_cast<float>(static_cast<int8_t>(b));
    return glm::max(c / 127.0f, -1.0f) * MAX_ENCODED_DIST;
}

} // namespace vf::voxel
