// Per-frame polling of the things that change outside the process: the layer
// files (an MCP/chat edit, a layer toggle) and the bound texture images (a
// folder drop-in, an edit on disk). Both land HERE, between frames, so no
// recorded command buffer is ever left sampling a world or an atlas that is
// being rewritten underneath it.
#include "app/app.hpp"

#include "app/frame/frame.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Reloads are camera-distance-priority and run off the render thread (see
// LayeredWorld), so an in-progress rebuild never blocks a frame; the finished
// world is swapped in here on the next consumeRebuild(). The texture apply
// wins over the reload, because a picker edit is a binding change that also
// needs the atlas re-uploaded.
void App::pollWorldAndTextures(float dt)
{
    // live world reload: MCP/chat edits and layer toggles land in the
    // layer files; poll for changes and swap the SVO in-place. Reloads are
    // camera-distance-priority and run off the render thread (see
    // LayeredWorld), so an in-progress rebuild never blocks a frame; the
    // finished world is swapped in here on the next consumeRebuild().
    m_layerPollT += dt;
    if (m_layers.loaded() && m_layerPollT >= 0.5f) {
        m_layerPollT = 0.f;
        if (m_layers.reloadIfChanged(m_camera.pos) == vf::voxel::LayeredWorld::kSyncDone)
            applyWorldReload();
    }
    if (m_layers.consumeRebuild())
        applyWorldReload();
    if (m_pendingWorldReload) {
        m_pendingWorldReload = false;
        m_layers.requestReload(m_camera.pos, true);
    }

    // texture hot-swap: a picker edit (write world.json + re-upload) or an
    // on-disk change of a bound image (re-upload only) lands here, between
    // frames, so no recorded command buffer is ever left sampling the
    // atlas while it is rewritten. The apply wins over the reload.
    if (m_texApplyPending) {
        m_texApplyPending = false;
        m_texReloadPending = false;
        applyTextureBindings();
    } else if (m_texReloadPending) {
        m_texReloadPending = false;
        reloadTexAtlas();
    }
    m_texPollT += dt;
    if (m_texPollT >= 1.0f) {
        m_texPollT = 0.f;
        pollTextureFiles();
    }
}

} // namespace app
} // namespace vf
