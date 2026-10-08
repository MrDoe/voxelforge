// The headless test hooks that run INSIDE the frame loop, as opposed to the
// startup hooks in run_startup.cpp. They exist so a --shot can verify a
// behaviour that is otherwise only reachable by driving the window.
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"
#include "app/world/store_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The hooks that run before any input: a synthetic layer toggle and a
// synthetic selection, so a headless shot can show a disabled layer or the
// highlight outline.
void App::runPreInputTestHooks()
{
    // GUI test hook: toggle one layer exactly like the checkbox does
    static const char* guiTestName = getenv("VF_GUI_TEST");
    if (guiTestName && *guiTestName && m_frameIdx == 20 && !m_worldLayers.empty()) {
        for (auto& l : m_worldLayers) {
            if (l.role != "landscape" && l.name == guiTestName) {
                l.enabled = !l.enabled;
                if (l.enabled)
                    l.listed = true;
                persistWorldLayers();
                spdlog::info("VF_GUI_TEST: {} -> {}", l.name,
                             l.enabled ? "enabled" : "disabled");
                break;
            }
        }
        m_pendingWorldReload = true;
    }

    // test hook: deterministic selection for headless highlight shots
    static const char* testSel = getenv("VF_TEST_SELECT");
    if (testSel && *testSel && !m_hasSelection && m_layers.loaded()) {
        glm::ivec3 v;
        if (sscanf(testSel, "%d,%d,%d", &v.x, &v.y, &v.z) == 3) {
            m_selectedHit = {};
            m_selectedHit.hit = true;
            m_selectedHit.voxel = v;
            m_selectedHit.mat =
                m_layers.field().sampleWorld(vf::voxel::voxelCenter(v)).mat;
            m_hasSelection = true;
            spdlog::info("VF_TEST_SELECT {} {} {}", v.x, v.y, v.z);
        }
    }

    // VF_TEST_SUN_PHASE_DEFERRED="day"|"night": apply the sun preset AFTER the
    // world has loaded, which is the only way to measure the real sun-change
    // stall. The startup VF_TEST_SUN_PHASE fires inside App::run before
    // initWindow/initVulkan, so it coalesces with the initial load and can only
    // ever report the STARTUP decomposition - never "sun moves on a loaded
    // world". This hook runs in the frame loop (before pollWorldAndTextures, so
    // the reload is consumed the same frame).
    //
    // Gate: the FIRST frame after the first loaded frame. Arm on the first
    // frame where m_layers.loaded() is true, fire on the next one, so exactly
    // one whole frame has elapsed with the world resident - enough for the
    // initial load's applyWorldReload/rebuildSurfels to have completed, and
    // enough for the deferred change to be a genuinely separate reload.
    //
    // This was a magic `m_frameIdx >= 20` before, copied from VF_GUI_TEST
    // without checking it: that threshold is only reachable interactively or
    // under --selftest, because --shot exits at m_frameIdx == 3
    // (record_headless.cpp:175) and m_frameIdx increments at :169, i.e. AFTER
    // this hook runs. A --shot therefore never fires it. Any frame-count gate
    // is the wrong shape here; the condition the hook actually means is "the
    // world is up and a frame has passed", which is what this tests.
    static const char* deferredPhase = getenv("VF_TEST_SUN_PHASE_DEFERRED");
    static bool deferredSunArmed = false;
    static bool deferredSunDone = false;
    if (!deferredSunDone && deferredPhase && *deferredPhase) {
        if (deferredSunArmed) {
            deferredSunDone = true;
            const bool night = strcmp(deferredPhase, "night") == 0;
            // Same entry point as the Render panel's preset buttons and the P
            // hotkey, so the measured path is the shipped one.
            spdlog::info(
                "VF_TEST_SUN_PHASE_DEFERRED {} at frame {} (one frame after load)",
                deferredPhase, m_frameIdx);
            setSunPhase(night);
        } else if (m_layers.loaded()) {
            deferredSunArmed = true;
        }
    }
}

// The hooks that run after input. All three are PREVIEW-ONLY by construction:
// a headless shot must be able to verify the rotate/move lane and the brush
// hover tint without committing the manifest or accumulating cursor deltas.
void App::runPostInputTestHooks(const char* testRotateLive)
{
    // The raw string, not just the flag: the seeding block sscanf's the angles
    // out of it, so a bool would throw away the value being tested.
    const bool rotateLiveTest = testRotateLive && *testRotateLive;
    // deterministic hover injection for headless shots: VF_TEST_HOVER
    // pins the hover point; VF_TEST_BRUSH additionally activates the edit
    // tool with a brush mode (diameter/depth via VF_EDIT_DIAM/DEPTH and
    // Smooth strength via VF_SMOOTH_STRENGTH) so the affected-splat preview
    // renders without any input.
    static const char* testHov = getenv("VF_TEST_HOVER");
    static const char* testBrush = getenv("VF_TEST_BRUSH");
    {
        glm::ivec3 v(0);
        bool have = false;
        if (testBrush && *testBrush) {
            char mode[16] = {};
            if (sscanf(testBrush, "%d,%d,%d,%15s", &v.x, &v.y, &v.z, mode) == 4) {
                m_editActive = true;
                m_editBrush = brushFromName(mode);
                if (const char* d = getenv("VF_EDIT_DIAM"))
                    m_editDiameter = finiteEnv(d, m_editDiameter);
                if (const char* d = getenv("VF_EDIT_DEPTH"))
                    m_editDepth = finiteEnv(d, m_editDepth);
                quantiseBrush(); // snap onto the lattice
                have = true;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    spdlog::info(
                        "VF_TEST_BRUSH {} {} {} {} d={} vox depth={:.1f}",
                        v.x, v.y, v.z, brushName(m_editBrush),
                        brushVoxels(), m_editDepth);
                }
            }
        } else if (testHov && *testHov && !m_hoverHit.hit) {
            if (sscanf(testHov, "%d,%d,%d", &v.x, &v.y, &v.z) == 3) {
                have = true;
                spdlog::info("VF_TEST_HOVER {} {} {}", v.x, v.y, v.z);
            }
        }
        if (have && m_layers.loaded()) {
            m_hoverHit = {};
            m_hoverHit.hit = true;
            m_hoverHit.voxel = v;
            if (m_editActive)
                m_hoverHit.normal = storeNormalAt(v);
            adoptPickOwnership(v);
        }
    }

    // VF_TEST_ROTATE_LIVE="yaw,pitch,roll"[,layer]: seed a synthetic
    // trackball pose (accumulated angles + selected-owner preview armed,
    // NO manifest commit) so a headless shot can verify GPU rotation.
    // VF_TEST_ROTATE commits + rebuilds; this one is preview-only.
    if (rotateLiveTest) {
        float y = 0.f, p = 0.f, r = 0.f;
        if (sscanf(testRotateLive, "%f,%f,%f", &y, &p, &r) == 3) {
            m_editActive = true;
            m_editBrush = EditBrush::Rotate;
            m_panel = Panel::Edit;
            m_rotateLayer.clear();
            if (const char* ln = getenv("VF_ROTATE_LAYER"))
                m_rotateLayer = ln;
            if (m_rotateLayer.empty())
                for (const auto& l : m_layers.layers())
                    if (l.enabled && l.role == "object" &&
                        l.file != vf::voxel::EditableWorld::kFileName) {
                        m_rotateLayer = l.file;
                        break;
                    }
            if (m_rotateLayer.empty()) {
                // Loud, not silent: a hook that arms nothing renders a
                // pixel-identical frame, which reads as "rotation does
                // nothing" instead of "the subject layer is gone". Both
                // failure modes below (stale name, disabled layer) cost a
                // debug session each before this warning existed.
                static bool loggedNoLayer = false;
                if (!loggedNoLayer) {
                    loggedNoLayer = true;
                    spdlog::warn("VF_TEST_ROTATE_LIVE: no enabled object "
                                 "layer to select - preview not armed");
                }
            } else {
                const auto layer = std::find_if(
                    m_worldLayers.begin(), m_worldLayers.end(),
                    [&](const vf::voxel::worldfile::WorldLayer& l) {
                        return l.file == m_rotateLayer;
                    });
                glm::vec3 pivot;
                if (layer == m_worldLayers.end()) {
                    static bool loggedMissing = false;
                    if (!loggedMissing) {
                        loggedMissing = true;
                        spdlog::warn(
                            "VF_TEST_ROTATE_LIVE: layer '{}' not in world "
                            "layers (disabled in world.json or removed?) - "
                            "preview not armed",
                            m_rotateLayer);
                    }
                } else if (!m_layers.layerPivot(m_rotateLayer, pivot)) {
                    static bool loggedNoPivot = false;
                    if (!loggedNoPivot) {
                        loggedNoPivot = true;
                        spdlog::warn(
                            "VF_TEST_ROTATE_LIVE: layer '{}' has no pivot "
                            "(disabled in world.json?) - preview not armed",
                            m_rotateLayer);
                    }
                } else {
                    m_rotating = true;
                    m_rotateDy = y; m_rotateDx = p; m_rotateDz = r;
                    const glm::mat3 R =
                        vf::voxel::worldfile::relativePlacementRotation(
                            layer->rotDeg, layer->rotX, layer->rotZ,
                            layer->rotDeg + y, layer->rotX + p,
                            layer->rotZ + r);
                    m_splatPass.setRotatePreview(
                        pivot, R, true, m_layers.layerId(m_rotateLayer));
                    refreshPreviewLights(); // carried lamps + derived clusters ride the synthetic pose
                }
            }
        }
    }

    // VF_TEST_MOVE_LIVE="dx,dy,dz[,layer]" is a preview-only Move hook.
    // It exercises the selected-owner translation lane without writing
    // world.json; the interactive Apply button remains the commit path.
    if (const char* testMoveLive = getenv("VF_TEST_MOVE_LIVE");
        testMoveLive && *testMoveLive) {
        float dx = 0.f, dy = 0.f, dz = 0.f;
        if (sscanf(testMoveLive, "%f,%f,%f", &dx, &dy, &dz) == 3) {
            m_editActive = true;
            m_editBrush = EditBrush::Move;
            m_panel = Panel::Edit;
            m_moveLayer.clear();
            const char* comma = strchr(testMoveLive, ',');
            if (comma) {
                const char* secondComma = strchr(comma + 1, ',');
                const char* thirdComma = secondComma
                    ? strchr(secondComma + 1, ',') : nullptr;
                if (thirdComma) {
                    std::string layerName(thirdComma + 1);
                    const size_t end = layerName.find(',');
                    if (end != std::string::npos)
                        layerName.resize(end);
                    if (!layerName.empty())
                        m_moveLayer = layerName;
                }
            }
            if (m_moveLayer.empty()) {
                for (const auto& l : m_layers.layers())
                    if (l.enabled && l.role == "object" &&
                        l.file != vf::voxel::EditableWorld::kFileName) {
                        m_moveLayer = l.file;
                        break;
                    }
            }
            glm::vec3 pivot;
            if (m_moveLayer.empty()) {
                static bool loggedMoveNoLayer = false;
                if (!loggedMoveNoLayer) {
                    loggedMoveNoLayer = true;
                    spdlog::warn("VF_TEST_MOVE_LIVE: no enabled object "
                                 "layer to select - preview not armed");
                }
            } else if (!m_layers.layerPivot(m_moveLayer, pivot)) {
                static bool loggedMoveNoPivot = false;
                if (!loggedMoveNoPivot) {
                    loggedMoveNoPivot = true;
                    spdlog::warn("VF_TEST_MOVE_LIVE: layer '{}' not in "
                                 "world layers or has no pivot (disabled or "
                                 "removed?) - preview not armed",
                                 m_moveLayer);
                }
            } else {
                m_moveDelta = glm::vec3(dx, dy, dz);
                m_moveStaged = true;
                m_splatPass.setRotatePreview(
                    pivot, glm::mat3(1.f), m_moveDelta, true,
                    m_layers.layerId(m_moveLayer));
                refreshPreviewLights(); // carried lamps + derived clusters ride the synthetic pose
            }
        }
    }
}

} // namespace app
} // namespace vf
