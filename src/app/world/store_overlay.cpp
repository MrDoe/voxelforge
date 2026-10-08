// The runtime-edit overlay (assets/runtime_edits.vxw): adopt it at startup and
// after every world reload, patching the chunks it names.
#include "app/app.hpp"

#include "app/world/store_overlay.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Live-edit store overlay path: assets/runtime_edits.vxw, overridable so
// headless checks can keep a session's painting untouched (VF_OVERLAY_PATH).
std::string overlayPath()
{
    if (const char* p = getenv("VF_OVERLAY_PATH"); p && *p)
        return p;
    return std::string(VOXELFORGE_ASSET_DIR) + "/runtime_edits.vxw";
}

void App::loadStoreOverlay()
{
    if (m_overlayLoaded)
        return;
    m_overlayLoaded = true;
    // Test/debug switch: ignore a saved live-edit overlay so gates run against
    // the baked world (a session's painted edits would otherwise change every
    // shot). The app still saves new strokes normally.
    if (getenv("VF_NO_OVERLAY")) {
        spdlog::info("live overlay: skipped (VF_NO_OVERLAY)");
        return;
    }
    const std::string path = overlayPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return;
    auto& store = m_layers.store();
    if (!store.loadOverlay(path)) {
        spdlog::warn("live overlay '{}' could not be applied", path);
        return;
    }
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    const std::vector<int> chunks = store.editedChunks();
    size_t n = 0;
    for (int ci : chunks) {
        // Start from what the GPU already renders (base surfels), then refresh
        // exactly the region the edit touched — the same splice a live stamp
        // would have produced, so the restored frame matches the session.
        // (skipped in --mode svo: the splat backend has no surfel data)
        if (m_renderMode == RenderMode::Splats) {
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
    spdlog::info("live overlay: restored {} chunks ({} run surfels) from {}", chunks.size(),
                 n, path);
}

} // namespace app
} // namespace vf
