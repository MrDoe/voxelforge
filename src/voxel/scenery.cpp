// Scenery resolution - see scenery.hpp for the contract.
#include "voxel/scenery.hpp"

#ifndef VOXELFORGE_ASSET_DIR
#define VOXELFORGE_ASSET_DIR "assets"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <spdlog/spdlog.h>

namespace vf {
namespace voxel {
namespace scenery {

namespace fs = std::filesystem;

std::string base()
{
    return (fs::path(std::string(VOXELFORGE_ASSET_DIR)).parent_path() / "scenery")
        .string();
}

namespace {

std::mutex g_mtx;
std::string g_override; // explicit selection (CLI / setCurrent)
std::string g_memo;     // resolved once, read by everyone else

bool usableDir(const fs::path& dir)
{
    std::error_code ec;
    return fs::is_regular_file(dir / "world.json", ec);
}

// The pointer file is one tiny JSON object written by setCurrent(); parsed
// with a hand-rolled scan (no JSON dependency in vf_core): the first
// quoted string after the "scenery" key.
std::string readPointer()
{
    std::ifstream f(fs::path(base()) / "last.json");
    if (!f)
        return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    const size_t k = s.find("\"scenery\"");
    if (k == std::string::npos)
        return {};
    const size_t q1 = s.find('"', s.find(':', k));
    if (q1 == std::string::npos)
        return {};
    const size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return {};
    return s.substr(q1 + 1, q2 - q1 - 1);
}

// temp+rename, the same shape OverlayWriter uses: a torn pointer file must
// never read as a truncated name.
bool writePointer(const std::string& name)
{
    const fs::path dir(base());
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path tmp = dir / "last.json.tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o)
            return false;
        o << "{\"scenery\": \"" << name << "\"}\n";
    }
    std::filesystem::rename(tmp, dir / "last.json", ec);
    return !ec;
}

std::string resolve(const std::string& sel)
{
    if (sel.empty())
        return {};
    // A selection containing a path separator (or absolute) is a direct path;
    // a bare token is a folder name under the scenery base.
    const bool likePath = fs::path(sel).is_absolute() ||
                          sel.find('/') != std::string::npos ||
                          sel.find('\\') != std::string::npos;
    const fs::path dir = likePath ? fs::path(sel) : fs::path(base()) / sel;
    return usableDir(dir) ? dir.string() : std::string();
}

std::string doResolve()
{
    if (!g_override.empty()) {
        static bool logged = false;
        std::string r = resolve(g_override);
        if (!r.empty()) {
            if (!logged) {
                spdlog::info("scenery: '{}' (selected)", r);
                logged = true;
            }
            return r;
        }
        spdlog::warn("scenery: '{}' has no world.json - falling back", g_override);
    }
    if (const char* e = getenv("VF_SCENERY"); e && *e) {
        std::string r = resolve(e);
        if (!r.empty()) {
            spdlog::info("scenery: '{}' (VF_SCENERY)", r);
            return r;
        }
        spdlog::warn("scenery: VF_SCENERY='{}' has no world.json - falling back", e);
    }
    {
        std::string r = resolve(readPointer());
        if (!r.empty()) {
            spdlog::info("scenery: '{}' (last used)", r);
            return r;
        }
    }
    {
        std::string r = resolve(kDefaultScenery);
        if (!r.empty()) {
            spdlog::info("scenery: '{}' (default)", r);
            return r;
        }
    }
    // first discovered folder, deterministically by name
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(base(), ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_directory() && usableDir(it->path()))
            names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    if (!names.empty()) {
        spdlog::info("scenery: '{}' (first discovered)", names.front());
        return (fs::path(base()) / names.front()).string();
    }
    // No scenery tree at all (pre-migration checkout, or assets not baked):
    // return the legacy shared dir and let the caller's world.json existence
    // check produce its clear "run 'ninja -C build world'" message.
    return std::string(VOXELFORGE_ASSET_DIR);
}

std::string resolved()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_override.empty())
        return doResolve();
    if (g_memo.empty())
        g_memo = doResolve();
    return g_memo;
}

} // namespace

std::string root() { return resolved(); }

std::string current()
{
    return fs::path(root()).filename().string();
}

std::vector<std::string> list()
{
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(base(), ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_directory() && usableDir(it->path()))
            names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

bool setCurrent(const std::string& name)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const fs::path dir = fs::path(base()) / name;
    if (!usableDir(dir)) {
        spdlog::warn("scenery: cannot switch to '{}': no world.json under {}",
                     name, dir.string());
        return false;
    }
    g_override = dir.string();
    g_memo = g_override;
    writePointer(name);
    spdlog::info("scenery: switched to '{}'", dir.string());
    return true;
}

void setCliScenery(const std::string& name)
{
    if (name.empty())
        return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_override = name;
    g_memo.clear();
}

std::string resolveShared(const std::string& manifestDir, const std::string& file)
{
    const std::string inScene = manifestDir + file;
    std::error_code ec;
    if (fs::is_regular_file(inScene, ec))
        return inScene;
    const std::string shared =
        (fs::path(std::string(VOXELFORGE_ASSET_DIR)) / file).string();
    if (fs::is_regular_file(shared, ec))
        return shared;
    return inScene;
}

} // namespace scenery
} // namespace voxel
} // namespace vf
