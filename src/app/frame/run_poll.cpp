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
    // A GUI scenery switch lands HERE, between frames — never from the
    // sidebar's draw pass, where the swapchain image is already acquired and
    // the command buffer is mid-recording (applyWorldReload waits for idle
    // and rebuilds buffers a recording still references; that deadlocked a
    // live window on 2026-11-19). Consumed BEFORE this frame's poll so no
    // reload is in flight against the switch's own full reload.
    if (!m_pendingScenerySwitch.empty())
    {
        const std::string target = m_pendingScenerySwitch;
        m_pendingScenerySwitch.clear();
        switchScenery(target);
    }
    // live world reload: MCP/chat edits and layer toggles land in the
    // layer files; poll for changes and swap the SVO in-place. Reloads are
    // camera-distance-priority and run off the render thread (see
    // LayeredWorld), so an in-progress rebuild never blocks a frame; the
    // finished world is swapped in here on the next consumeRebuild().
    m_layerPollT += dt;
    if (!m_studio.active && m_layers.loaded() && m_layerPollT >= 0.5f) {
        m_layerPollT = 0.f;
        // snapshot manifest mtime/size before the reload consumes it
        std::error_code ec;
        unsigned long long mt = 0, sz = 0;
        auto t = std::filesystem::last_write_time(m_manifestPath, ec);
        if (!ec) mt = (unsigned long long)t.time_since_epoch().count();
        sz = (unsigned long long)std::filesystem::file_size(m_manifestPath, ec);
        const bool manifestChanged =
            m_microManifestSeeded && (mt != m_microManifestMtime || sz != m_microManifestSize);
        auto res = m_layers.reloadIfChanged(m_camera.pos);
        const bool worldReloaded = (res == vf::voxel::LayeredWorld::kSyncDone);
        if (worldReloaded)
            applyWorldReload();
        // micro objects are separate SVO worlds and are skipped by the world
        // bake; a manifest-only edit (e.g. MCP set_layer_transform on a micro
        // layer) never triggers applyWorldReload, so reload them here.
        if (manifestChanged && !worldReloaded) {
            loadMicroObjects();
            m_taaFirstFrame = true;
        }
        // A manifest edit may carry UI settings (vf_mcp set_ui/set_sun/
        // set_scenery). Apply between frames; field-level guards keep a GUI
        // drag untouched (see world/ui_settings.cpp).
        if (manifestChanged) {
            applyUiSettingsFromManifest(true);
            applyManifestSun();
        }
        m_microManifestMtime = mt;
        m_microManifestSize = sz;
        m_microManifestSeeded = true;
    }
    if (m_layers.consumeRebuild())
        applyWorldReload();
    if (m_pendingWorldReload) {
        m_pendingWorldReload = false;
        m_layers.requestReload(m_camera.pos, true);
    }
    // vf_mcp live-lane intents run AFTER any reload so a stamp always edits
    // the freshly adopted store, never a stale one. They go through
    // applyEditLive — the GUI brush's own implementation.
    if (m_layers.loaded())
        consumeMcpIntents();

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
    if (m_cardApplyPending) {
        m_cardApplyPending = false;
        applyCardBindings();
    }
    m_texPollT += dt;
    if (m_texPollT >= 1.0f) {
        m_texPollT = 0.f;
        pollTextureFiles();
    }
}

} // namespace app
} // namespace vf
