#pragma once
// Terrain source of truth: a 16-bit grayscale PNG covering the whole world.
// Row-major texels, row v corresponds to z, column u to x:
//   u = x / WORLD + 0.5 , v = z / WORLD + 0.5   (both clamped to [0,1])
// Meters decode: h = HM_MIN + (texel / 65535) * (HM_MAX - HM_MIN)
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace vf::voxel {

constexpr uint32_t kHmSize = 2048;      // texels per side
constexpr float kHmMinMeters = -8.0f;   // meters at texel 0
constexpr float kHmMaxMeters = 24.0f;   // meters at texel 65535

class HeightMap {
public:
    bool loadFromFile(const std::string& path);

    float sample(float x, float z) const;        // bilinear, edge-clamped, meters
    glm::vec2 gradient(float x, float z) const;  // finite difference, meters/meter

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    const float* data() const { return texels_.data(); }
    size_t bytes() const { return texels_.size() * sizeof(float); }
    bool loaded() const { return !texels_.empty(); }

private:
    std::vector<float> texels_; // meters, row-major [v][u]
    uint32_t w_ = 0, h_ = 0;
};

// Lazily loads VOXELFORGE_ASSET_DIR "/heightmap.png" on first use.
const HeightMap& sharedHeightmap();

} // namespace vf::voxel
