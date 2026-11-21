// The world.json layer list: scan it, persist the GUI's enable/pose edits, and
// swap a rebuilt world into both backends in place.
#include "app/app.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::syncWorldLayerList()
{
    if (!m_layers.loaded())
        return;
    // every manifest layer shows up in the GUI, plus previously discovered
    // folder entries that are still unlisted (shown as "(new)")
    std::vector<vf::voxel::worldfile::WorldLayer> l = m_layers.layers();
    for (const auto& u : m_worldLayers)
        if (!u.listed && std::none_of(l.begin(), l.end(),
                                      [&](const vf::voxel::worldfile::WorldLayer& e) {
                                          return e.file == u.file;
                                      }))
            l.push_back(u);
    // enforce unique ids: one row per file
    auto dupEnd = std::unique(l.begin(), l.end(),
                              [](const vf::voxel::worldfile::WorldLayer& a,
                                 const vf::voxel::worldfile::WorldLayer& b) {
                                  return a.file == b.file;
                              });
    l.erase(dupEnd, l.end());
    m_worldLayers = std::move(l);
}

void App::persistWorldLayers()
{
    // The studio loads NO world layers (it renders the micro object on a
    // flat backdrop), so its m_worldLayers is empty. Persisting that would
    // rewrite the shipped manifest with zero layers and destroy the scene -
    // the exact failure mode AGENTS.md warns about. The studio never owns
    // world.json; every manifest edit belongs to the main app.
    if (m_studio.active)
        return;
    std::vector<vf::voxel::worldfile::WorldLayer> out;
    out.reserve(m_worldLayers.size());
    for (const auto& l : m_worldLayers)
        if (l.listed &&
            std::none_of(out.begin(), out.end(),
                         [&](const vf::voxel::worldfile::WorldLayer& e) {
                             return e.file == l.file;
                         }))
            out.push_back(l);
    vf::voxel::worldfile::writeManifest(vf::voxel::scenery::root() + "/world.json",
                                        out);
}

void App::rescanWorldLayers()
{
    namespace fs = std::filesystem;
    const fs::path dir = vf::voxel::scenery::root();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string f = it->path().filename().string();
        if (it->path().extension() != ".vxw" || f == "world.vxw")
            continue;
        bool known = false;
        for (const auto& l : m_worldLayers)
            known |= (l.file == f);
        if (known)
            continue;
        vf::voxel::worldfile::WorldLayer nl;
        nl.file = f;
        nl.name = it->path().stem().string();
        nl.role = "object";
        nl.listed = false;
        nl.enabled = false;
        m_worldLayers.push_back(nl);
    }
}

// Switch the live world to another scenery folder, in session.
//
// Ordering rules baked into this sequence:
//  - the pending overlay stroke is flushed while the OLD root still resolves
//    (overlayPath() follows the resolver, so flushing after the switch would
//    write the old scene's session into the NEW scene's file);
//  - the heightmap re-loads first: if the target has no terrain PNG the
//    switch is aborted before anything else moved;
//  - applyWorldReload() at the end is the same full-reload path layer
//    toggles use (SVO + terrain/objvol textures + atlas + lights + micro +
//    surfels + store re-adoption + NEW overlay file + re-listed layers), so
//    the switch reuses verified machinery instead of duplicating it.
bool App::switchScenery(const std::string& name)
{
    if (m_studio.active)
        return false; // the studio never owns world.json
    // Forensic start line: flushed immediately, so a kill mid-switch still
    // shows whether the switch began (info lines can die in stdout buffers).
    spdlog::info("scenery: switch begin '{}'", name);
    spdlog::default_logger()->flush();
    const std::string oldRoot = vf::voxel::scenery::root();
    if (!vf::voxel::scenery::setCurrent(name))
        return false;
    const std::string newRoot = vf::voxel::scenery::root();
    if (newRoot == oldRoot)
        return true; // already there
    if (!vf::voxel::reloadSharedHeightmap()) {
        spdlog::warn("scenery: '{}' has no loadable heightmap.png - switch aborted",
                     name);
        vf::voxel::scenery::setCurrent(oldRoot);
        vf::voxel::reloadSharedHeightmap();
        return false;
    }
    // Old scene: persist its session before the paths move under it.
    m_overlayWriter.flush();
    if (m_editable.size() > 0)
        m_editable.save(); // ai_edits belongs to the scene being left
    m_manifestPath = newRoot + "/world.json";
    if (!m_layers.load(m_manifestPath)) {
        spdlog::warn("scenery: '{}' manifest would not load - switch aborted", name);
        m_manifestPath = oldRoot + "/world.json";
        m_layers.load(m_manifestPath);
        vf::voxel::scenery::setCurrent(oldRoot); // restore the root FIRST,
        vf::voxel::reloadSharedHeightmap();      // then the terrain follows it
        return false;
    }
    // The AI-edits layer follows the scene (setAssetDir alone never re-reads,
    // so old-scene records can never save into the new scene's file).
    m_editable.setAssetDir(newRoot);
    m_editable.load();
    m_editable.ensureManifest();
    m_carve.setAssetDir(newRoot);
    m_add.setAssetDir(newRoot);
    // Selection/undo state is scene-scoped: drop it before the reload.
    m_selectedLayer.clear();
    m_rotateLayer.clear();
    m_moveLayer.clear();
    m_undo.clear();
    m_undoPending.clear();
    m_hasStamp = false;
    m_worldLayers.clear(); // rescan below rebuilds from the new folder
    // An authored sun belongs to its scene: re-read it (a --sun flag still wins).
    {
        float e = 0.f, a = 0.f;
        if (!m_args.sunSet &&
            vf::voxel::worldfile::loadSunManifest(m_manifestPath, e, a))
            setSunAngles(e, a);
    }
    applyWorldReload();
    // The seen-snapshots belonged to the scene just left; the new scene's
    // "ui"/"sun" blocks must be judged against this switch's applied state.
    resetUiSettingsState();
    spdlog::info("scenery: switched live to '{}'", newRoot);
    return true;
}

void App::applyWorldReload()
{
    if (!m_layers.loaded())
        return;
    // A reload replaces the runtime store generation. Cell-state inverse
    // edits from the previous generation must never be replayed against the
    // new world, and an in-flight drag cannot be completed across that swap.
    // Flush first so loadStoreOverlay() below sees the latest queued stroke
    // rather than an older snapshot still being written by the worker.
    if (m_hasStamp || !m_undoPending.empty())
        spdlog::warn("undo: aborting active stroke across world reload");
    m_overlayWriter.flush();
    m_undo.clear();
    m_undoPending.clear();
    m_undoOverflow = false;
    m_strokeRiseCells = 2;
    m_hasStamp = false;
    m_dragging = false;
    // Preserve the current physical button state so a reload during a held
    // click cannot manufacture a fresh stamp edge on the next frame.
    m_lmbWasDown =
        glfwGetMouseButton(m_window.handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    m_ctrlWasDown =
        glfwGetKey(m_window.handle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(m_window.handle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    // swap the freshly synthesized SVO buffers under an idle device
    vkDeviceWaitIdle(m_ctx.device());
    const auto& g = m_layers.gpu();
    m_svoPass.setWorld(g);
    uploadTerrainTexture(); // layer toggles can change materials too
    uploadObjVolTexture();  // keep AI/object shadows in sync with the SVO
    reloadTexAtlas();       // world.json "textures" edits ride the same poll
    // an external manifest edit must not leave a stale picker table behind
    // (a pending GUI pick is applied after this, so it still wins the frame)
    vf::voxel::worldfile::loadTextureManifest(m_manifestPath, m_texBindings);
    // explicit light edits ride the same poll; they are tiny, so reload them
    // with the manifest rather than waiting for a restart.
    uploadLightSources();
    // micro objects are SEPARATE SVO worlds, so a manifest edit that adds,
    // removes or repositions one only re-uploads its own buffers - it never
    // invalidates the world bake. They load FIRST because the surfel bake
    // below consults their fields for the cast-shadow half of the two-way
    // shadow contract.
    loadMicroObjects();
    rebuildSurfels();       // splat backend follows the same live field
    // the reload re-adopted the store: re-apply the persisted live overlay
    m_liveEditor.clear();
    m_overlayLoaded = false;
    loadStoreOverlay();
    syncWorldLayerList();
    rescanWorldLayers(); // layers dropped into the scenery while running show up too
    // Keep m_rotateLayer alive across the rebuild: it is the selected
    // interaction target, not a transient mouse-down state.
    if (m_rotationPreviewPending) {
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        m_rotationPreviewPending = false;
        m_taaFirstFrame = true;
    } else {
        // An unrelated layer/manifest reload invalidates an uncommitted draft;
        // never silently apply a stale staged delta to the new world.
        if (m_rotationStaged)
            cancelRotation();
        if (m_moveStaged)
            cancelMove();
    }
}

} // namespace app
} // namespace vf
