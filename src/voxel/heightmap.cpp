#include "voxel/heightmap.hpp"
#include "voxel/common.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#ifndef VOXELFORGE_ASSET_DIR
#define VOXELFORGE_ASSET_DIR "assets"
#endif

namespace vf::voxel {

bool HeightMap::loadFromFile(const std::string& path)
{
    int w = 0, h = 0, comp = 0;
    stbi_us* raw = stbi_load_16(path.c_str(), &w, &h, &comp, 1);
    if (!raw) {
        spdlog::critical("failed to load heightmap '{}': {}", path,
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }
    w_ = uint32_t(w);
    h_ = uint32_t(h);
    texels_.resize(size_t(w_) * h_);
    const float span = kHmMaxMeters - kHmMinMeters;
    for (size_t i = 0; i < texels_.size(); ++i)
        texels_[i] = kHmMinMeters + (float(raw[i]) / 65535.0f) * span;
    stbi_image_free(raw);
    spdlog::info("heightmap '{}' loaded: {}x{} texels", path, w_, h_);
    return true;
}

float HeightMap::sample(float x, float z) const
{
    if (!loaded())
        return 0.0f;
    float u = glm::clamp(x / WORLD + 0.5f, 0.0f, 1.0f) * float(w_ - 1);
    float v = glm::clamp(z / WORLD + 0.5f, 0.0f, 1.0f) * float(h_ - 1);
    uint32_t iu = std::min(uint32_t(u), w_ - 2), iv = std::min(uint32_t(v), h_ - 2);
    float fu = u - float(iu), fv = v - float(iv);
    const float* row0 = &texels_[size_t(iv) * w_ + iu];
    const float* row1 = row0 + w_;
    float top = glm::mix(row0[0], row0[1], fu);
    float bot = glm::mix(row1[0], row1[1], fu);
    return glm::mix(top, bot, fv);
}

glm::vec2 HeightMap::gradient(float x, float z) const
{
    const float e = 0.25f;
    return { (sample(x + e, z) - sample(x - e, z)) / (2.0f * e),
             (sample(x, z + e) - sample(x, z - e)) / (2.0f * e) };
}

const HeightMap& sharedHeightmap()
{
    static HeightMap instance = [] {
        HeightMap m;
        if (!m.loadFromFile(std::string(VOXELFORGE_ASSET_DIR) + "/heightmap.png"))
            spdlog::critical("terrain heightmap missing - run heightmap_gen");
        return m;
    }();
    return instance;
}

} // namespace vf::voxel
