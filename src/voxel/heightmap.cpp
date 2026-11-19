#include "voxel/heightmap.hpp"
#include "voxel/common.hpp"
#include "voxel/scenery.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // the texture picker takes dropped .jpg too
#define STBI_ONLY_HDR
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

bool loadPngRGBA8(const std::string& path, std::vector<uint8_t>& out,
                  int& w, int& h)
{
    int comp = 0;
    // The 8-bit path accepts PNG and JPEG (both are enabled above); the
    // texture picker lists either, so a dropped .jpg works as-is.
    stbi_uc* raw = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!raw) {
        spdlog::warn("failed to load texture '{}': {}", path,
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }
    out.assign(raw, raw + size_t(w) * h * 4);
    stbi_image_free(raw);
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

namespace {
const HeightMap* g_override = nullptr;
// The shared instance lives at file scope (not in the accessor's local
// static) so a scenery switch can re-load it in place; first_use guards the
// lazy-load semantics sharedHeightmap() has always had.
HeightMap g_instance;
bool g_first_use = true;
std::mutex g_hm_mtx;
} // namespace

const HeightMap& sharedHeightmap()
{
    if (g_override)
        return *g_override;
    std::lock_guard<std::mutex> lk(g_hm_mtx);
    if (g_first_use)
    {
        g_first_use = false;
        // the terrain belongs to the CURRENT scenery
        if (!g_instance.loadFromFile(scenery::root() + "/heightmap.png"))
            spdlog::critical("terrain heightmap missing - run heightmap_gen");
    }
    return g_instance;
}

bool reloadSharedHeightmap()
{
    if (g_override)
        return true; // a tool injected its own instance; nothing to reload
    std::lock_guard<std::mutex> lk(g_hm_mtx);
    HeightMap next;
    if (!next.loadFromFile(scenery::root() + "/heightmap.png"))
        return false;
    g_instance = std::move(next);
    g_first_use = false;
    return true;
}

void setSharedHeightmap(const HeightMap* m) { g_override = m; }

} // namespace vf::voxel
