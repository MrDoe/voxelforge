// The per-material photo-texture binding table: scan assets/textures/, poll
// every bound file for on-disk changes, and write back only the "textures" key
// of world.json when the GUI picker changes a slot (layers and every unknown key
// are preserved verbatim, so a texture swap can never unbind the atlas or drop
// a layer).
#include "app/app.hpp"
#include "voxel/grass_cards.hpp"

#include <filesystem>

#include "voxel/heightmap.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <spdlog/spdlog.h>
#include <vector>

namespace vf {
namespace app {

bool App::reloadTexAtlas()
{
    if (!m_texAtlas.valid())
        return false;
    // In-flight frames still sample the atlas image: idle before rewriting
    // its pixels + layout. Callers that already idled pay nothing extra.
    vkDeviceWaitIdle(m_ctx.device());
    if (!m_texAtlas.load(m_ctx, m_manifestPath))
        return false;
    // The atlas owns the per-material emissiveScale/meanColor that lights
    // DERIVED from emissive materials read, so a reload can change the light
    // set (picker flag toggle, on-disk image edit, VF_TEXTURES flip). Both
    // callers come through here, so re-deriving once here keeps the rule
    // un-forgettable; the wait above also makes the descriptor write safe.
    uploadLightSources();
    // The foliage-card SSBO is derived from the atlas's material bindings, so
    // rebuild it here: a world.json "foliageCards" edit or a scenery switch
    // then takes effect on the same reload as the textures it binds.
    rebuildGrassCards();
    return true;
}

bool App::rebuildGrassCards()
{
    if ((m_renderFlags & (1 << 13)) == 0) {
        m_furPass.setGrassCards({});
        return true;
    }
    std::vector<voxel::GrassCard> cards = voxel::buildGrassCards(
        [this](int mat) { return m_texAtlas.cardLayer(mat); });
    return m_furPass.setGrassCards(cards);
}

void App::rescanTextureFiles()
{
    m_texFiles.clear();
    const std::string dir = std::string(VOXELFORGE_ASSET_DIR) + "/textures";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file())
            continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            continue;
        m_texFiles.push_back("textures/" + e.path().filename().string());
    }
    std::sort(m_texFiles.begin(), m_texFiles.end());
    m_texThumbsDirty = true;
}

// Resolve a manifest texture path: absolute paths verbatim; anything else
// against the manifest directory first, then the shared asset dir (the
// library lives in assets/ even though the manifest lives in a scenery).
std::string resolveTexPath(const std::string& manifestPath,
                                  const std::string& file)
{
    if (!file.empty() && (file[0] == '/' ||
                          (file.size() > 1 && file[1] == ':')))
        return file;
    std::string dir = manifestPath;
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    return vf::voxel::scenery::resolveShared(dir + "/", file);
}

void App::setTextureBinding(int mat, const std::string& file)
{
    auto it = std::find_if(m_texBindings.begin(), m_texBindings.end(),
                           [&](const vf::voxel::worldfile::TextureBinding& b) {
                               return b.mat == mat;
                           });
    if (file.empty()) {
        if (it != m_texBindings.end())
            m_texBindings.erase(it);
    } else if (it != m_texBindings.end()) {
        it->file = file;
    } else {
        m_texBindings.push_back({ file, mat, 0.5f });
    }
}

void App::pollTextureFiles()
{
    std::vector<std::string> files;
    const std::string texDir = std::string(VOXELFORGE_ASSET_DIR) + "/textures";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(texDir, ec)) {
        if (!e.is_regular_file())
            continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            continue;
        files.push_back("textures/" + e.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    if (files != m_texFiles) {
        m_texFiles = std::move(files);
        m_texThumbsDirty = true;
    }

    std::map<std::string, unsigned long long> sigs;
    for (const auto& b : m_texBindings) {
        const std::string path = resolveTexPath(m_manifestPath, b.file);
        std::error_code fec;
        const auto t = std::filesystem::last_write_time(path, fec);
        const unsigned long long mt = fec
            ? 0ull
            : static_cast<unsigned long long>(t.time_since_epoch().count());
        const unsigned long long sz = static_cast<unsigned long long>(
            fec ? 0ull : std::filesystem::file_size(path, fec));
        sigs[path] = mt ^ (sz * 0x9E3779B97F4A7C15ull);
    }
    for (const auto& kv : sigs) {
        auto it = m_texSig.find(kv.first);
        if (it != m_texSig.end() && it->second != kv.second) {
            spdlog::info("texture hot-swap: '{}' changed on disk, re-uploading",
                         kv.first);
            m_texReloadPending = true;
            break;
        }
    }
    m_texSig = std::move(sigs);
}

void App::applyTextureBindings()
{
    std::sort(m_texBindings.begin(), m_texBindings.end(),
              [](const vf::voxel::worldfile::TextureBinding& a,
                 const vf::voxel::worldfile::TextureBinding& b) {
                  return a.mat < b.mat;
              });
    if (!vf::voxel::worldfile::writeTextureManifest(m_manifestPath, m_texBindings))
        spdlog::warn("texture picker: could not write '{}'", m_manifestPath);
    reloadTexAtlas();
    // the atlas now matches disk: seed the mtime watch so the next poll does
    // not re-upload what the pick itself just uploaded
    pollTextureFiles();
}

void App::refreshTextureThumbnails()
{
    if (!m_texThumbsDirty)
        return;
    destroyTextureThumbnails();
    const int thumb = 64;
    for (const std::string& f : m_texFiles) {
        std::string path = resolveTexPath(m_manifestPath, f);
        std::vector<uint8_t> raw;
        int w = 0, h = 0;
        bool ok = vf::voxel::loadPngRGBA8(path, raw, w, h);
        TexThumb t;
        t.file = f;
        if (ok && w > 0 && h > 0) {
            std::vector<uint8_t> px(size_t(thumb) * thumb * 4);
            for (int y = 0; y < thumb; ++y)
                for (int x = 0; x < thumb; ++x) {
                    int sx = x * w / thumb;
                    int sy = y * h / thumb;
                    if (sx >= w) sx = w - 1;
                    if (sy >= h) sy = h - 1;
                    for (int k = 0; k < 4; ++k)
                        px[size_t((y * thumb + x) * 4 + k)] =
                            raw[size_t((sy * w + sx) * 4 + k)];
                }
            t.img = vf::makeImage2D(m_ctx, uint32_t(thumb), uint32_t(thumb),
                                     VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
            if (t.img.img)
                vf::uploadToImage3D(m_ctx, t.img, px.data(), px.size());
            if (t.img.view)
                t.id = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                    m_uiSampler, t.img.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_texThumbs.push_back(std::move(t));
    }
    m_texThumbsDirty = false;
}

// ---- Foliage-card picker ----------------------------------------------------
void App::rescanCardFiles()
{
    m_cardFiles.clear();
    const std::string cardsJson = std::string(VOXELFORGE_ASSET_DIR) + "/textures/cards/cards.json";
    vf::voxel::worldfile::loadCardLibrary(cardsJson, m_cardFiles);
    // load current bindings
    m_cardBindings.clear();
    vf::voxel::worldfile::loadFoliageCardBindings(m_manifestPath, m_cardBindings);
    m_cardThumbsDirty = true;
}

void App::setCardBinding(int mat, const std::string& card)
{
    auto it = std::find_if(m_cardBindings.begin(), m_cardBindings.end(),
                           [&](const vf::voxel::worldfile::FoliageCardBinding& b) { return b.mat == mat; });
    if (card.empty()) {
        if (it != m_cardBindings.end()) m_cardBindings.erase(it);
    } else if (it != m_cardBindings.end()) {
        it->card = card;
    } else {
        m_cardBindings.push_back({ mat, card });
    }
}

void App::applyCardBindings()
{
    if (!vf::voxel::worldfile::writeFoliageCardManifest(m_manifestPath, m_cardBindings))
        spdlog::warn("card picker: could not write '{}'", m_manifestPath);
    reloadTexAtlas(); // rebuilds grass cards
}

void App::refreshCardThumbnails()
{
    if (!m_cardThumbsDirty) return;
    destroyCardThumbnails();
    const int thumb = 64;
    const std::string cardsDir = std::string(VOXELFORGE_ASSET_DIR) + "/textures/cards";
    for (const auto& cf : m_cardFiles) {
        const std::string& name = cf.first;
        const std::string& file = cf.second;
        std::string path = cardsDir + "/" + file;
        std::vector<uint8_t> raw;
        int w = 0, h = 0;
        bool ok = vf::voxel::loadPngRGBA8(path, raw, w, h);
        CardThumb t;
        t.file = file;
        if (ok && w > 0 && h > 0) {
            std::vector<uint8_t> px(size_t(thumb) * thumb * 4);
            for (int y = 0; y < thumb; ++y)
                for (int x = 0; x < thumb; ++x) {
                    int sx = x * w / thumb;
                    int sy = y * h / thumb;
                    if (sx >= w) sx = w - 1;
                    if (sy >= h) sy = h - 1;
                    for (int k = 0; k < 4; ++k)
                        px[size_t((y * thumb + x) * 4 + k)] =
                            raw[size_t((sy * w + sx) * 4 + k)];
                }
            t.img = vf::makeImage2D(m_ctx, uint32_t(thumb), uint32_t(thumb),
                                     VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
            if (t.img.img)
                vf::uploadToImage3D(m_ctx, t.img, px.data(), px.size());
            if (t.img.view)
                t.id = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                    m_uiSampler, t.img.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        m_cardThumbs.push_back(std::move(t));
    }
    m_cardThumbsDirty = false;
}

void App::destroyCardThumbnails()
{
    if (!m_ctx.device()) return;
    vkDeviceWaitIdle(m_ctx.device());
    for (auto& t : m_cardThumbs) {
        if (t.id) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)t.id);
        t.id = 0;
        vf::destroyImage3D(m_ctx, t.img);
    }
    m_cardThumbs.clear();
}

void App::destroyTextureThumbnails()
{
    if (!m_ctx.device())
        return;
    // avoid destroying while in-flight; idle first
    vkDeviceWaitIdle(m_ctx.device());
    for (auto& t : m_texThumbs) {
        if (t.id)
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)t.id);
        t.id = 0;
        vf::destroyImage3D(m_ctx, t.img);
    }
    m_texThumbs.clear();
}

} // namespace app
} // namespace vf
