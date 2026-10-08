// The per-material photo-texture binding table: scan assets/textures/, poll
// every bound file for on-disk changes, and write back only the "textures" key
// of world.json when the GUI picker changes a slot (layers and every unknown key
// are preserved verbatim, so a texture swap can never unbind the atlas or drop
// a layer).
#include "app/app.hpp"

#include <filesystem>

#include <spdlog/spdlog.h>

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
    return true;
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
}

// Resolve a manifest texture path the same way TexAtlas::load does: absolute
// paths verbatim, anything else against the manifest directory (assets/).
std::string resolveTexPath(const std::string& manifestPath,
                                  const std::string& file)
{
    if (!file.empty() && (file[0] == '/' ||
                          (file.size() > 1 && file[1] == ':')))
        return file;
    std::string dir = manifestPath;
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    return dir + file;
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
    if (files != m_texFiles)
        m_texFiles = std::move(files);

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

} // namespace app
} // namespace vf
