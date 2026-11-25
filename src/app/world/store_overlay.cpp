// The runtime-edit overlay (scenery/<name>/runtime_edits.vxw): adopt it at startup and
// after every world reload, patching the chunks it names.
#include "app/app.hpp"

#include "app/world/store_overlay.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>

namespace vf {
namespace app {

// Live-edit store overlay path: assets/runtime_edits.vxw, overridable so
// headless checks can keep a session's painting untouched (VF_OVERLAY_PATH).
std::string overlayPath()
{
    if (const char* p = getenv("VF_OVERLAY_PATH"); p && *p)
        return p;
    // the live-edit session belongs to the CURRENT scenery
    return vf::voxel::scenery::root() + "/runtime_edits.vxw";
}

// Move/copy `path` to a timestamped sibling (`<path>.<tag>-<unix-seconds>`)
// so a destructive overlay operation never loses a session for good. Returns
// the backup path, or an empty string when nothing was backed up.
std::string backupOverlayFile(const std::string& path, const char* tag, bool move)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return {};
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::string dst = path + "." + tag + "-" + std::to_string(secs);
    for (int i = 1; std::filesystem::exists(dst, ec); ++i)
        dst = path + "." + tag + "-" + std::to_string(secs) + "-" + std::to_string(i);
    if (move)
        std::filesystem::rename(path, dst, ec);
    else
        std::filesystem::copy_file(path, dst, ec);
    if (ec) {
        spdlog::warn("live overlay: cannot back up '{}' to '{}': {}", path, dst,
                     ec.message());
        return {};
    }
    return dst;
}

void App::queueOverlaySave()
{
    if (m_overlaySaveBlocked) {
        if (!m_overlayBlockWarned)
            spdlog::warn("live overlay: NOT saved - VF_NO_OVERLAY skipped loading '{}', "
                         "so saving would replace that session with this run's "
                         "edits (set VF_OVERLAY_PATH to persist them elsewhere)",
                         overlayPath());
        m_overlayBlockWarned = true;
        return;
    }
    auto& store = m_layers.store();
    const std::string path = overlayPath();
    // The writer serializes the EDITED chunks only, so a process whose edited
    // set is smaller than the file it loaded (a re-adopted store, a reload
    // race) would persist a shrunken session. Keep one copy of the file as it
    // was before the first shrinking save of this load.
    if (!m_overlayShrinkBackedUp && m_overlayDiskChunks > 0) {
        const size_t n = store.editedChunks().size();
        if (n < m_overlayDiskChunks) {
            m_overlayWriter.flush();
            const std::string bak = backupOverlayFile(path, "bak-shrink", false);
            spdlog::warn("live overlay: save shrinks {} -> {} chunks; previous file "
                         "kept as '{}'",
                         m_overlayDiskChunks, n, bak);
            m_overlayShrinkBackedUp = true;
        }
    }
    m_overlayWriter.queue(store, path);
}

void App::loadStoreOverlay()
{
    if (m_overlayLoaded)
        return;
    m_overlayLoaded = true;
    m_overlayDiskChunks = 0;
    m_overlayShrinkBackedUp = false;
    m_overlaySaveBlocked = false;
    const std::string path = overlayPath();
    std::error_code ec;
    // Test/debug switch: ignore a saved live-edit overlay so gates run against
    // the baked world (a session's painted edits would otherwise change every
    // shot). Saving is then BLOCKED when the skipped file is the default one:
    // the writer persists only this run's edits, so the first stroke used to
    // replace the whole unloaded session. VF_OVERLAY_PATH re-enables saving.
    if (getenv("VF_NO_OVERLAY")) {
        const char* ov = getenv("VF_OVERLAY_PATH");
        if (!(ov && *ov) && std::filesystem::exists(path, ec))
            m_overlaySaveBlocked = true;
        spdlog::info("live overlay: skipped (VF_NO_OVERLAY){}",
                     m_overlaySaveBlocked ? ", saving disabled" : "");
        return;
    }
    if (!std::filesystem::exists(path, ec))
        return;
    auto& store = m_layers.store();
    if (!store.loadOverlay(path)) {
        // Unreadable (corrupt, or an older schema): move it aside instead of
        // letting the next stroke overwrite it with this run's edits only.
        const std::string bak = backupOverlayFile(path, "unreadable", true);
        spdlog::warn("live overlay '{}' could not be applied; moved to '{}'", path, bak);
        return;
    }
    m_overlayDiskChunks = store.editedChunks().size();
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);
    const vf::voxel::SurfelParams sp = surfelParams(false);
    const std::vector<int> chunks = store.editedChunks();
    size_t n = 0;
    for (int ci : chunks) {
        // Start from what the GPU already renders (base surfels), then refresh
        // exactly the region the edit touched — the same splice a live stamp
        // would have produced, so the restored frame matches the session.
        // (skipped in --mode svo: the splat backend has no surfel data)
        if (m_renderMode == RenderMode::Splats || m_renderMode == RenderMode::Dgs) {
            m_liveEditor.chunkSurfels(ci, sp); // seeds (GPU source or store fallback)
            int lo[3], hi[3];
            if (store.chunkEditBounds(ci, lo, hi)) {
                // Exact margin: the persisted stroke solved the same store
                // band a live stamp did, so the restored run matches the
                // session (Smooth especially).
                const int m = vf::voxel::LiveEditor::kExactStampMargin;
                m_liveEditor.refreshRegion(ci, glm::ivec3(lo[0] - m, lo[1] - m, lo[2] - m),
                                           glm::ivec3(hi[0] + m + 1, hi[1] + m + 1,
                                                      hi[2] + m + 1), sp);
            } else {
                m_liveEditor.seedFromStore(ci, sp);
            }
            const std::vector<vf::voxel::Surfel>& surfels =
                m_liveEditor.chunkRun(ci, sp);
            n += surfels.size();
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.edgeCountOf(ci));
        }
        // the restored edit also changed the terrain surface the water
        // shading reads: re-derive its height-texture window
        {
            int lo[3], hi[3];
            if (store.chunkEditBounds(ci, lo, hi))
                // Restoration may contain a Smooth raise larger than the
                // current cached top; scan from the world top for correctness.
                patchHeightTexture(lo[0] - 3, lo[2] - 3, hi[0] + 3, hi[2] + 3,
                                   store.latN());
        }
        const auto& pool = store.pool(ci);
        if (pool)
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    // The restored overlay carries painted materials: regrow foliage from the
    // same live height texture the strokes patched.
    rebuildGrassCards();
    spdlog::info("live overlay: restored {} chunks ({} run surfels) from {}", chunks.size(),
                 n, path);
}

} // namespace app
} // namespace vf
