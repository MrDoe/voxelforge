// Object placement. The rotate/move PREVIEW is recorded in
// frame/run_input.cpp; these are the only writers of world.json's
// pos/rot/rotX/rotZ, reached from the sidebar's Apply buttons.
#include "app/app.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

std::string App::rotateTargetLayer(const vf::voxel::PickHit& hit) const
{
    if (!m_layers.loaded() || !hit.hit || !hit.object || hit.layer == 0)
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
    // Keep the final GPU preview active until the rebuilt world is swapped in;
    // disabling it here would flash the old pose during the async rebuild.
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
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
    m_rotating = false;
    m_rotationStaged = false;
    m_rotateHandle = TrackballHandle::None;
    m_rotateLastMouse = {};
    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
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
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
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
}

} // namespace app
} // namespace vf
