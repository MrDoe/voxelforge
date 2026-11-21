// The AI section body: hands the panel its scene context, renders the chat,
// and turns agent screenshots (PPM/PNG on disk) into ImGui textures using the
// same Vulkan upload path the texture picker uses.
#include "app/app.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/scenery.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>

namespace vf {
namespace app {

namespace {

bool decodePPM(const std::string& path, std::vector<uint8_t>& px, int& w, int& h)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string magic;
    in >> magic;
    if (magic != "P6") return false;
    int maxv = 0;
    in >> w >> h >> maxv;
    if (!in || w <= 0 || h <= 0 || maxv <= 0 || maxv > 255) return false;
    in.get(); // exactly one whitespace follows maxval
    px.resize(size_t(w) * size_t(h) * 4);
    for (int i = 0; i < w * h; ++i) {
        unsigned char rgb[3];
        if (!in.read(reinterpret_cast<char*>(rgb), 3)) return false;
        px[size_t(i) * 4 + 0] = rgb[0];
        px[size_t(i) * 4 + 1] = rgb[1];
        px[size_t(i) * 4 + 2] = rgb[2];
        px[size_t(i) * 4 + 3] = 255;
    }
    return true;
}

// A 720p screenshot is ~3.5 MB as RGBA; the tool card shows it at ~300 px.
// Downsample once at load so many screenshots do not eat VRAM or stall a
// frame with a multi-megabyte upload.
void downscaleRGBA(const std::vector<uint8_t>& src, int sw, int sh, int maxW,
                   std::vector<uint8_t>& dst, int& dw, int& dh)
{
    const double scale = std::min(1.0, double(maxW) / double(sw));
    dw = std::max(1, int(double(sw) * scale));
    dh = std::max(1, int(double(sh) * scale));
    dst.resize(size_t(dw) * size_t(dh) * 4);
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(sh - 1, y * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(sw - 1, x * sw / dw);
            const size_t s = (size_t(sy) * size_t(sw) + size_t(sx)) * 4;
            const size_t d = (size_t(y) * size_t(dw) + size_t(x)) * 4;
            for (int k = 0; k < 4; ++k) dst[d + size_t(k)] = src[s + size_t(k)];
        }
    }
}

} // namespace

ImTextureID App::aiScreenshotTexture(const std::string& path, int* w, int* h)
{
    if (m_aiShots.capacity() == 0)
        m_aiShots.reserve(32); // avoid re-copying Vulkan handles on growth
    for (auto& s : m_aiShots)
        if (s.path == path) {
            if (w) *w = s.w;
            if (h) *h = s.h;
            return s.id;
        }
    std::vector<uint8_t> px;
    int iw = 0, ih = 0;
    bool ok = false;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ppm") == 0)
        ok = decodePPM(path, px, iw, ih);
    else
        ok = vf::voxel::loadPngRGBA8(path, px, iw, ih);
    if (!ok || iw <= 0 || ih <= 0) {
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    std::vector<uint8_t> small;
    int dw = 0, dh = 0;
    downscaleRGBA(px, iw, ih, 360, small, dw, dh);
    px.swap(small);
    iw = dw;
    ih = dh;
    AiShot shot;
    shot.path = path;
    shot.w = iw;
    shot.h = ih;
    shot.img = vf::makeImage2D(m_ctx, uint32_t(iw), uint32_t(ih),
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT);
    if (shot.img.img) {
        vf::uploadToImage3D(m_ctx, shot.img, px.data(), px.size());
        if (shot.img.view)
            shot.id = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                m_uiSampler, shot.img.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (w) *w = shot.w;
    if (h) *h = shot.h;
    ImTextureID id = shot.id;
    m_aiShots.push_back(std::move(shot));
    // Cards are capped at 24 in ChatUi, so 32 downsampled shots (~0.3 MB
    // each) never evict a texture a visible card still references.
    while (m_aiShots.size() > 32) {
        auto& old = m_aiShots.front();
        if (old.id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)old.id);
        vf::destroyImage3D(m_ctx, old.img);
        m_aiShots.erase(m_aiShots.begin());
    }
    return id;
}

void App::drawPanelAI()
{
    ai::ChatUi::Context ctx;
    ctx.sceneryDir = vf::voxel::scenery::root();
    ctx.sessionFile = ".opencode/voxelforge_sessions.json";
    ctx.reloadFn = [this] { requestWorldReload(); };
    ctx.imageFn = [this](const std::string& path, int* w, int* h) {
        return aiScreenshotTexture(path, w, h);
    };
    m_chatUi.setContext(std::move(ctx));
    m_chatUi.drawPanel(m_hoverHit.hit ? &m_hoverHit : nullptr,
                       m_hasSelection ? &m_selectedHit : nullptr, m_hasSelection);
}

} // namespace app
} // namespace vf
