// The per-material photo-texture binding table: scan assets/textures/, poll
// every bound file for on-disk changes, and write back only the "textures" key
// of world.json when the GUI picker changes a slot (layers and every unknown key
// are preserved verbatim, so a texture swap can never unbind the atlas or drop
// a layer).
#include "app/app.hpp"
#include "voxel/grass_cards.hpp"

#include <algorithm>
#include <chrono>
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
    // Coalesce rapid reloads: churning the manifest (GUI swaps, peer writes)
    // can otherwise re-enter this path before the previous GPU work is fully
    // retired, racing the in-flight frames that sample the atlas (SIGSEGV,
    // see quirk 83d662a1). A reload that arrives within the cooldown is
    // deferred to the next poll via m_texReloadPending instead of running now.
    static double lastReload = 0.0;
    const double ts = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count() / 1000.0;
    if (lastReload > 0.0 && (ts - lastReload) < 1.0) {
        m_texReloadPending = true;
        return true;
    }
    lastReload = ts;
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
    // The foliage-card SSBO is derived from the atlas's texture bindings (the
    // "card" field on each row). Rebuild ONLY when the card set actually
    // changed: rebuilding on every texture reload (e.g. a peer churning the
    // textures key) re-uploads a ~16 MB SSBO under rapid cycling and races
    // the in-flight frames that still reference the previous buffer (SIGSEGV,
    // see quirk 83d662a1). A texture-only edit with no card change skips.
    std::vector<std::pair<int, std::string>> now;
    for (const auto& b : m_texBindings)
        if (!b.card.empty())
            now.emplace_back(b.mat, b.card);
    bool changed = now.size() != m_cardBindings.size();
    if (!changed)
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i].first != m_cardBindings[i].mat ||
                now[i].second != m_cardBindings[i].card) { changed = true; break; }
    if (changed) {
        m_cardBindings.clear();
        for (const auto& p : now)
            m_cardBindings.push_back({ p.first, p.second });
        m_cardThumbsDirty = true;
        vkDeviceWaitIdle(m_ctx.device());
        rebuildGrassCards();
    }
    return true;
}

// Read the LIVE height/material texture and store, not the baked field:
// patchHeightTexture() keeps m_heightCpu in step with every paint stamp, so a
// rebuild after painting grows cards on newly-painted grass and drops them
// where the grass was painted away. The old baked-field lookup made foliage
// frozen at the authored ground materials no matter what the brush did.
int App::grassCardLayerAt(float x, float z, int /*hintMat*/,
                          float& outHeight, int& outMaterial)
{
    // Grass is a property of the bound TEXTURE, evaluated against the LIVE
    // surface — exactly like emissive — never a baked grass layer. The height
    // texture is used only as a bounded starting hint for the column scan;
    // the answer comes from the first solid cell seen from above.
    const int latN = m_layers.loaded() ? m_layers.store().latN() : 0;
    if (latN <= 0 || m_heightCpu.size() != size_t(latN) * latN)
        return -1;
    const float half = 0.5f * vf::voxel::WORLD;
    const glm::vec2 tc = glm::clamp(glm::vec2(x, z) / vf::voxel::WORLD + 0.5f,
                                    glm::vec2(0.0f), glm::vec2(1.0f)) *
                         glm::vec2(latN - 1);
    const glm::ivec2 c = glm::clamp(glm::ivec2(tc), glm::ivec2(0),
                                    glm::ivec2(latN - 1));
    const float hint = m_heightCpu[size_t(c.y) * latN + c.x].x;
    if (hint <= -1e29f)
        return -1;
    const int lx = glm::clamp(int(glm::floor((x + half) / vf::voxel::VOXEL)),
                              0, latN - 1);
    const int lz = glm::clamp(int(glm::floor((z + half) / vf::voxel::VOXEL)),
                              0, latN - 1);
    const auto& store = m_layers.store();
    const int top = std::clamp(int(glm::floor((hint + half) / vf::voxel::VOXEL)) - 1,
                               0, latN - 1);
    // The live TERRAIN cell is the source of the material and the root height:
    // paint it grass and grass grows, paint it away and it disappears.
    for (int y = top; y >= 0; --y) {
        const auto cell = store.cellAt(lx, y, lz);
        if (!cell.solid || cell.obj)
            continue;
        outMaterial = cell.mat;
        outHeight = -half + float(y + 1) * vf::voxel::VOXEL;
        const int layer = m_texAtlas.cardLayerForSampled(outMaterial, cell.tags);
        if (layer < 0)
            return -1;
        // A building's FLOOR (the LighthouseIsle barn's `footing`) or a low
        // wall sits just above this terrain cell. Blades rooted on the grass
        // texture below poke through it, which is the "grass inside the barn"
        // bug. Reject only GROUND-LEVEL objects — a tree canopy metres above
        // is not enclosed ground and must not wipe out the meadow under it.
        const int ceiling = std::min(latN - 1, top + 8); // ~0.8 m
        for (int oy = top + 1; oy <= ceiling; ++oy) {
            const auto above = store.cellAt(lx, oy, lz);
            if (above.solid && above.obj)
                return -1;
        }
        return layer;
    }
    return -1;
}

bool App::rebuildGrassCards()
{
    if ((m_renderFlags & (1 << 13)) == 0) {
        m_grassCardsCache.clear();
        m_furPass.setGrassCards({});
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    m_grassCardsCache = voxel::buildGrassCards(
        [this](float x, float z, int mat, float& h, int& m) {
            return grassCardLayerAt(x, z, mat, h, m);
        });
    const float ms = float(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count());
    spdlog::info("foliage cards: full rebuild {} cards ({:.1f} ms) from live ground",
                 m_grassCardsCache.size(), ms);
    return m_furPass.setGrassCards(m_grassCardsCache);
}

bool App::rebuildGrassCardsRegion(float x0, float z0, float x1, float z1)
{
    if ((m_renderFlags & (1 << 13)) == 0)
        return rebuildGrassCards();
    if (x1 < x0 || z1 < z0)
        return true;
    const auto t0 = std::chrono::steady_clock::now();
    // Drop only the cached cards rooted in the painted rectangle. Placement
    // is a hash of the candidate index, so the newly generated subset is
    // exactly what a full rebuild would have produced there — the rest of the
    // ~300k cards are left untouched.
    const size_t before = m_grassCardsCache.size();
    m_grassCardsCache.erase(
        std::remove_if(m_grassCardsCache.begin(), m_grassCardsCache.end(),
                       [x0, z0, x1, z1](const voxel::GrassCard& c) {
                           return c.rootLayer.x >= x0 && c.rootLayer.x <= x1 &&
                                  c.rootLayer.z >= z0 && c.rootLayer.z <= z1;
                       }),
        m_grassCardsCache.end());
    const size_t removed = before - m_grassCardsCache.size();
    const glm::vec4 region(x0, z0, x1, z1);
    std::vector<voxel::GrassCard> added = voxel::buildGrassCards(
        [this](float x, float z, int mat, float& h, int& m) {
            return grassCardLayerAt(x, z, mat, h, m);
        },
        0.15f, 3, &region);
    m_grassCardsCache.insert(m_grassCardsCache.end(), added.begin(), added.end());
    const float ms = float(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count());
    spdlog::info("foliage cards: region ({:.1f},{:.1f})-({:.1f},{:.1f}) "
                 "{} removed, {} added, {} total ({:.2f} ms)",
                 x0, z0, x1, z1, removed, added.size(), m_grassCardsCache.size(), ms);
    return m_furPass.setGrassCards(m_grassCardsCache);
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
        it->card.clear(); // card belonged to the previous texture
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
    // card bindings now live on the textures table; mirror them into
    // m_cardBindings for the picker's current-card lookup.
    m_cardBindings.clear();
    std::vector<vf::voxel::worldfile::TextureBinding> bs;
    vf::voxel::worldfile::loadTextureManifest(m_manifestPath, bs);
    for (const auto& b : bs)
        if (!b.card.empty())
            m_cardBindings.push_back({ b.mat, b.card });
    m_cardThumbsDirty = true;
}

void App::setCardBinding(int mat, const std::string& card)
{
    auto it = std::find_if(m_texBindings.begin(), m_texBindings.end(),
                           [&](const vf::voxel::worldfile::TextureBinding& b) { return b.mat == mat; });
    if (it == m_texBindings.end()) {
        spdlog::warn("card binding: material {} has no texture binding - bind a texture first", mat);
        return;
    }
    it->card = card;
}

void App::applyCardBindings()
{
    // card now lives on the textures binding; the writer preserves it.
    if (!vf::voxel::worldfile::writeTextureManifest(m_manifestPath, m_texBindings))
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
