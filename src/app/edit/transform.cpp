// Object placement. The rotate/move PREVIEW is recorded in
// frame/run_input.cpp; these are the only writers of world.json's
// pos/rot/rotX/rotZ, reached from the sidebar's Apply buttons.
#include "app/app.hpp"

#include <limits>
#include <spdlog/spdlog.h>

namespace vf {
namespace app {

std::string App::rotateTargetLayer(const vf::voxel::PickHit& hit) const
{
    if (!hit.hit || !hit.object)
        return {};
    // A MICRO object has no world owner id (it is a separate SVO world), so it
    // is named by its manifest file instead.
    if (!hit.microFile.empty())
        return hit.microFile;
    if (!m_layers.loaded() || hit.layer == 0)
        return {};
    return m_layers.layerFile(hit.layer);
}

void App::commitRotation()
{
    if (m_rotateLayer.empty())
        return;
    m_rotationStaged = false;
    if (std::abs(m_rotateDy) < 1e-4f && std::abs(m_rotateDx) < 1e-4f &&
        std::abs(m_rotateDz) < 1e-4f) {
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        clearMicroRotatePreview();
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        m_rotateLastMouse = {};
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        return;
    }
    const auto it = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) { return l.file == m_rotateLayer; });
    if (it == m_worldLayers.end()) {
        spdlog::warn("rotate: layer '{}' no longer in the manifest", m_rotateLayer);
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        m_rotateLayer.clear();
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        m_rotateLastMouse = {};
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        return;
    }
    // add to whatever placement the layer already had (drag is a delta)
    it->rotDeg += m_rotateDy;
    it->rotX   += m_rotateDx;
    it->rotZ   += m_rotateDz;
    it->listed = true;
    persistWorldLayers();
    spdlog::info("rotate: {} -> yaw {:+.1f} pitch {:+.1f} roll {:+.1f}",
                 m_rotateLayer, it->rotDeg, it->rotX, it->rotZ);
    if (isMicroLayer(m_rotateLayer)) {
        // A MICRO object is its own world: re-bake the micro objects
        // (milliseconds) instead of the whole world (seconds). The rotation
        // is applied to the RECORDS at load time, so the re-bake turns it -
        // the same destination-driven map the panel's inspector uses, so the
        // shape keeps its solidity.
        loadMicroObjects();
        m_rotationStaged = false;
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        m_taaFirstFrame = true;
        return;
    }
    // Keep the final GPU preview active until the rebuilt world is swapped in;
    // disabling it here would flash the old pose during the async rebuild.
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
    // Push the final overlay BEFORE zeroing the staged angles (the reload
    // window shows the kept preview, so the lights stay shifted with it;
    // the hold blocks an early restore, the reload upload clears it).
    refreshPreviewLights();
    m_previewLightsHold = true;
    m_rotationPreviewPending = true;
    m_taaFirstFrame = true;
    m_rotating = false;
    m_rotateHandle = TrackballHandle::None;
    m_rotateLastMouse = {};
    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
}

void App::cancelRotation()
{
    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
    clearMicroRotatePreview();
    m_rotating = false;
    m_rotationStaged = false;
    m_rotateHandle = TrackballHandle::None;
    m_rotateLastMouse = {};
    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
    refreshPreviewLights(); // restores the base set to splat when one was shifted
}

void App::commitMove()
{
    if (m_moveLayer.empty())
        return;
    if (glm::length(m_moveDelta) < 1e-4f) {
        m_moveStaged = false;
        return;
    }
    const auto it = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) {
            return l.file == m_moveLayer;
        });
    if (it == m_worldLayers.end()) {
        spdlog::warn("move: layer '{}' no longer in the manifest", m_moveLayer);
        cancelMove();
        m_moveLayer.clear();
        return;
    }
    it->pos[0] += m_moveDelta.x;
    it->pos[1] += m_moveDelta.y;
    it->pos[2] += m_moveDelta.z;
    it->listed = true;
    persistWorldLayers();
    spdlog::info("move: {} -> pos ({:+.2f}, {:+.2f}, {:+.2f})",
                 m_moveLayer, it->pos[0], it->pos[1], it->pos[2]);
    if (isMicroLayer(m_moveLayer)) {
        // A MICRO object is its own world: re-bake ONLY it (milliseconds)
        // instead of the whole world (seconds), and clear the preview - the
        // object is now where the manifest says it is.
        updateMicroMovePreview();
        loadMicroObjects();
        m_moveStaged = false;
        m_moving = false;
        m_moveDelta = glm::vec3(0.f);
        m_moveLastMouse = {};
        m_taaFirstFrame = true;
        return;
    }
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
    // Same reload-window cover as commitRotation: final overlay first (the
    // staged delta is still live here), then the hold until the upload lands.
    refreshPreviewLights();
    m_previewLightsHold = true;
    m_rotationPreviewPending = true; // keep the final translation visible
    m_moveStaged = false;
    m_moving = false;
    m_moveDelta = glm::vec3(0.f);
    m_moveLastMouse = {};
    m_taaFirstFrame = true;
}

void App::cancelMove()
{
    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
    m_moving = false;
    m_moveStaged = false;
    m_moveDelta = glm::vec3(0.f);
    m_moveLastMouse = {};
    // A micro object's preview lives in the micro pass's own table, and it
    // must be cleared AFTER the delta is zeroed - the preview IS the delta.
    if (!m_moveLayer.empty() && isMicroLayer(m_moveLayer))
        updateMicroMovePreview();
    refreshPreviewLights(); // restores the base set to splat when one was shifted
}

// Bake-seeded splice base shared by the trigger and the preview overlay:
// exactly the store-enumerated set the bake upload derives. Idempotent (a
// populated base is kept verbatim, so repeated strokes/previews never reseed
// mid-session).
void App::ensureDerivedBase()
{
    if (!m_derivedClusters.empty() || !m_layers.loaded())
        return;
    const std::vector<glm::vec3> emission =
        buildEmissionTable(m_texBindings, m_texAtlas);
    std::vector<vf::voxel::VoxelField::EmissiveCluster> baked;
    m_layers.store().collectEmissive(emission, baked,
                                     std::numeric_limits<int>::max());
    const int budget = std::clamp(m_lightBudget, 1, vf::voxel::kMaxLights);
    for (const auto& c : baked) {
        if (int(m_derivedClusters.size()) >= budget)
            break;
        m_derivedClusters.push_back(c);
    }
}

void App::refreshPreviewLights()
{
    // Active preview layer + rigid delta, mirroring the geometry preview in
    // run_input.cpp (rotate = relativePlacementRotation about the pivot,
    // move = pure translation). The commit hold keeps the shifted set while
    // the reload lands (the final GPU preview stays visible through the
    // swap, so the lights must stay shifted with it).
    std::string layer;
    glm::vec3 pivot(0.f), dT(0.f);
    glm::mat3 dR(1.f);
    bool haveDelta = false;
    const float rotMag = std::abs(m_rotateDy) + std::abs(m_rotateDx) +
                         std::abs(m_rotateDz);
    if (!m_rotateLayer.empty() && (m_rotating || m_rotationStaged) &&
        rotMag >= 1e-4f && layerPivotWorld(m_rotateLayer, pivot)) {
        const auto it = std::find_if(
            m_worldLayers.begin(), m_worldLayers.end(),
            [&](const vf::voxel::worldfile::WorldLayer& w) {
                return w.file == m_rotateLayer;
            });
        if (it != m_worldLayers.end()) {
            dR = vf::voxel::worldfile::relativePlacementRotation(
                it->rotDeg, it->rotX, it->rotZ,
                it->rotDeg + m_rotateDy, it->rotX + m_rotateDx,
                it->rotZ + m_rotateDz);
            layer = m_rotateLayer;
            haveDelta = true;
        }
    } else if (!m_moveLayer.empty() && (m_moving || m_moveStaged) &&
               glm::length(m_moveDelta) >= 1e-4f &&
               layerPivotWorld(m_moveLayer, pivot)) {
        dT = m_moveDelta;
        layer = m_moveLayer;
        haveDelta = true;
    }
    if (!haveDelta && m_previewLightsHold)
        return; // commit window: reload upload clears the hold on landing
    if (!haveDelta) {
        if (!m_previewLightsShifted)
            return;
        // Preview ended: splat rejoins the base set the SVO pass never left.
        ensureDerivedBase();
        const int budget = std::clamp(m_lightBudget, 1, vf::voxel::kMaxLights);
        vf::voxel::LightUBO ubo{};
        const std::vector<vf::voxel::LightSource> noAuthored;
        vf::voxel::fillLightUBO(noAuthored, m_derivedClusters, budget, ubo,
                                false);
        ubo.perPixelK =
            std::clamp(m_lightK, 1, vf::voxel::worldfile::kLightKMax);
        m_splatPass.setLights(ubo);
        m_microSvoPass.setLights(ubo);
        m_furPass.setLights(ubo);
        m_previewLightsShifted = false;
        return;
    }

    ensureDerivedBase();
    // Derived clusters inside the layer's PRE-preview record box ride the
    // same delta (rigid shift: count/order preserved, so no slot flicker).
    // The box is grown 0.6 m: lifted centroids hover just outside the raw
    // records. Commit-reload re-enumerates exactly, so a shifted centroid
    // that lands inside geometry is preview-only transient.
    auto shifted = m_derivedClusters;
    const vf::voxel::WorldAABB box = layerBounds(layer);
    int shiftedDerived = 0;
    if (box.valid()) {
        for (auto& c : shifted) {
            if (c.pos.x >= box.lo.x - 0.6f && c.pos.x <= box.hi.x + 0.6f &&
                c.pos.y >= box.lo.y - 0.6f && c.pos.y <= box.hi.y + 0.6f &&
                c.pos.z >= box.lo.z - 0.6f && c.pos.z <= box.hi.z + 0.6f) {
                c.pos = pivot + dR * (c.pos - pivot) + dT;
                ++shiftedDerived;
            }
        }
    }
    const int budget = std::clamp(m_lightBudget, 1, vf::voxel::kMaxLights);
    vf::voxel::LightUBO ubo{};
    const std::vector<vf::voxel::LightSource> noAuthored;
    vf::voxel::fillLightUBO(noAuthored, shifted, budget, ubo, false);
    ubo.perPixelK = std::clamp(m_lightK, 1, vf::voxel::worldfile::kLightKMax);
    // The world SVO backend ignores the world geometry preview, so it keeps
    // the base set. Splat, micro and fur all can show the preview and receive
    // the shifted set. Shadows follow for free: the direct term marches
    // visibility per frame from this UBO. The indirect volume is not rebaked
    // per drag (bake cost); commit/upload refreshes it.
    m_splatPass.setLights(ubo);
    m_microSvoPass.setLights(ubo);
    m_furPass.setLights(ubo);
    m_previewLightsShifted = true;
    if (getenv("VF_TRACE"))
        spdlog::info("preview lights: layer {} shifted {} derived clusters",
                     layer, shiftedDerived);
}

} // namespace app
} // namespace vf
