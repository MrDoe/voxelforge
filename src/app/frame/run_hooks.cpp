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
#include <map>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The hooks that run before any input: a synthetic layer toggle and a
// synthetic selection, so a headless shot can show a disabled layer or the
// highlight outline.
void App::runPreInputTestHooks()
{
    // VF_TEST_BEHAVIOUR_CMD="name:command[:speed]": tell a micro object what
    // to do, the way the AI tool does ("let the alpaca run"). It runs in the
    // PRE-input hook so the command is in force for the frame.
    if (const char* testCmd = getenv("VF_TEST_BEHAVIOUR_CMD");
        testCmd && *testCmd) {
        static bool done = false;
        if (!done) {
            done = true;
            std::string spec(testCmd);
            std::string who = spec, cmd, speedStr;
            const size_t c1 = spec.find(':');
            if (c1 != std::string::npos) {
                who = spec.substr(0, c1);
                const size_t c2 = spec.find(':', c1 + 1);
                if (c2 != std::string::npos) {
                    cmd = spec.substr(c1 + 1, c2 - c1 - 1);
                    speedStr = spec.substr(c2 + 1);
                } else {
                    cmd = spec.substr(c1 + 1);
                }
            }
            const float speed = speedStr.empty() ? 0.f : float(atof(speedStr.c_str()));
            const bool ok = setMicroBehaviour(who, cmd, speed);
            spdlog::info("VF_TEST_BEHAVIOUR_CMD: '{}' -> '{}' ({})", who, cmd,
                         ok ? "ok" : "NO SUCH ANIMAL");
        }
    }

    // VF_TEST_MICRO_BEHAVIOUR="seconds": fast-forward the autonomous
    // behaviours, so a headless render shows the animal at that point in its
    // walk. The behaviour is advanced in the same 0.1 s steps the frame loop
    // uses (the step is clamped there, so a long frame cannot teleport it).
    //
    // It runs HERE, in the PRE-input hook, and not after: the behaviour is
    // advanced (and pushed to the micro pass) during the frame, so a
    // fast-forward that lands after the frame would only show up in the
    // NEXT one - and a one-frame shot would never see it at all.
    if (const char* testBehaviour = getenv("VF_TEST_MICRO_BEHAVIOUR");
        testBehaviour && *testBehaviour) {
        static bool done = false;
        if (!done) {
            done = true;
            const float seconds = float(atof(testBehaviour));
            const int steps = int(seconds / 0.1f);
            for (auto& b : m_microBehaviours) {
                if (!b.active)
                    continue;
                for (int i = 0; i < steps; ++i) {
                    b.update(0.1f);
                    // Every tenth step, so a gate can measure how FAR the
                    // animal moves per step: a teleport would be metres, a
                    // walk is centimetres.
                    if (i % 10 == 9)
                        spdlog::info("VF_TEST_MICRO_BEHAVIOUR: t={:.1f} "
                                     "offset=({:+.3f},{:+.3f},{:+.3f}) state={}",
                                     double(i + 1) * 0.1, double(b.offset.x),
                                     double(b.offset.y), double(b.offset.z),
                                     int(b.state));
                }
                spdlog::info("VF_TEST_MICRO_BEHAVIOUR: fast-forwarded {:.1f} s "
                             "-> offset ({:+.3f},{:+.3f},{:+.3f}) state {}",
                             seconds, double(b.offset.x), double(b.offset.y),
                             double(b.offset.z), int(b.state));
            }
        }
    }

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
                } else if (!layerPivotWorld(m_rotateLayer, pivot)) {
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
                    // A MICRO object has no world owner id, so the splat
                    // pass's preview cannot show it - the micro pass holds
                    // its own live rotation.
                    if (isMicroLayer(m_rotateLayer)) {
                        const int mi = microWorldIndex(m_rotateLayer);
                        if (mi >= 0)
                            m_microSvoPass.setWorldRotation(size_t(mi), R,
                                                            pivot, true);
                    }
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
            } else if (!layerPivotWorld(m_moveLayer, pivot)) {
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

    // VF_TEST_MOVE_DRAG: drive a full Move DRAG on a micro object through the
    // app's pointer seam - the grab, the axis handle, the delta and the
    // preview - and report the state after each step. The Move lane is four
    // steps and each can fail on its own, so a report that says WHICH one
    // failed is worth more than a screenshot.
    if (const char* testMoveDrag = getenv("VF_TEST_MOVE_DRAG");
        testMoveDrag && *testMoveDrag) {
        static int phase = 0;
        static glm::vec2 grabPt { 0.f, 0.f };
        static glm::vec2 handlePt { 0.f, 0.f };
        switch (phase) {
        case 0: {
            // Arm the tool and pick the Move mode.
            m_editActive = true;
            m_editBrush = EditBrush::Move;
            m_panel = Panel::Edit;
            // Find a pixel where a micro object wins the real pick.
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const int W = int(disp.x > 0.f ? disp.x : m_window.framebufferSize().x);
            const int H = int(disp.y > 0.f ? disp.y : m_window.framebufferSize().y);
            bool found = false;
            for (int y = 0; y < H && !found; y += 3) {
                for (int x = 0; x < W && !found; x += 3) {
                    const vf::voxel::PickHit h = pickAtWindowPos(double(x), double(y));
                    if (h.hit && !h.microFile.empty()) {
                        grabPt = glm::vec2(float(x), float(y));
                        found = true;
                    }
                }
            }
            spdlog::info("VF_TEST_MOVE_DRAG: the micro object is at ({:.0f},{:.0f})",
                         double(grabPt.x), double(grabPt.y));
            phase = 1;
            break;
        }
        case 1:
            // The GRAB: a plain click on the object.
            m_testPointer.valid = true;
            m_testPointer.x = grabPt.x;
            m_testPointer.y = grabPt.y;
            m_testPointer.ctrl = false;
            m_testPointer.lmb = true;
            phase = 2;
            break;
        case 2:
            m_testPointer.lmb = false;   // release
            phase = 3;
            break;
        case 3: {
            // The AXIS: the handle sits at the end of the axis line, which
            // is the gizmo's radius away from the centre.
            m_testPointer.lmb = false;
            const float r = m_gizmoRadius;
            handlePt = m_gizmoCentre + glm::vec2(r, 0.f);
            spdlog::info("VF_TEST_MOVE_DRAG: the gizmo centre is ({:.1f},{:.1f}) "
                         "radius {:.1f}; the X handle at ({:.1f},{:.1f})",
                         double(m_gizmoCentre.x), double(m_gizmoCentre.y),
                         double(r), double(handlePt.x), double(handlePt.y));
            m_testPointer.x = handlePt.x;
            m_testPointer.y = handlePt.y;
            m_testPointer.lmb = true;
            phase = 4;
            break;
        }
        case 4:
            // The DRAG: 40 px to the right.
            m_testPointer.x = handlePt.x + 40.0f;
            m_testPointer.y = handlePt.y;
            m_testPointer.lmb = true;
            phase = 5;
            break;
        case 5:
            m_testPointer.lmb = false;   // release: the delta is staged
            spdlog::info("VF_TEST_MOVE_DRAG: the drag ended with delta "
                         "({:+.3f},{:+.3f},{:+.3f}) staged={} layer='{}'",
                         double(m_moveDelta.x), double(m_moveDelta.y),
                         double(m_moveDelta.z), int(m_moveStaged), m_moveLayer);
            phase = 6;
            break;
        default:
            m_testPointer.valid = false;
            break;
        }
    }

    // VF_TEST_CLICK_SCAN: drive the REAL interactive input path - the gates
    // (text field, mouse capture, ctrl/armed) and the selection itself -
    // without a mouse, by injecting a pointer position and a button state
    // into processInput (App::m_testPointer).
    //
    // The positions come from the SAME pick the pointer uses
    // (pickAtWindowPos), so the scan finds the pixels where a micro object
    // actually is and clicks one of them. This is the end-to-end proof that
    // a micro object can be SELECTED: the earlier hooks only proved the
    // micro march in isolation, and the gates around it went unmeasured.
    if (const char* testClickScan = getenv("VF_TEST_CLICK_SCAN");
        testClickScan && *testClickScan) {
        static int phase = 0;          // 0 = find, 1 = press, 2 = release
        static bool plain = false;     // the plain click (no ctrl): the
                                       // activation gesture of the Rotate mode
        static std::vector<glm::vec2> spots;
        static size_t next = 0;
        if (phase == 0) {
            // Find the pixels where a micro object wins the real pick.
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const int W = int(disp.x > 0.f ? disp.x : m_window.framebufferSize().x);
            const int H = int(disp.y > 0.f ? disp.y : m_window.framebufferSize().y);
            for (int y = 0; y < H && spots.size() < 12; y += 4) {
                for (int x = 0; x < W && spots.size() < 12; x += 4) {
                    const vf::voxel::PickHit h = pickAtWindowPos(double(x), double(y));
                    if (h.hit && !h.microFile.empty())
                        spots.push_back(glm::vec2(float(x), float(y)));
                }
            }
            spdlog::info("VF_TEST_CLICK_SCAN: {} micro pixels found", spots.size());
            phase = 1;
        } else if (phase == 1 && next < spots.size()) {
            m_testPointer.valid = true;
            m_testPointer.x = spots[next].x;
            m_testPointer.y = spots[next].y;
            m_testPointer.ctrl = !plain;
            m_testPointer.lmb = true;      // the press edge
            phase = 2;
        } else if (phase == 2) {
            m_testPointer.lmb = false;     // release: the edges clear
            m_testPointer.ctrl = false;
            phase = 3;
        } else if (phase == 3) {
            ++next;
            if (next >= spots.size()) {
                if (plain) {
                    m_testPointer.valid = false;
                    spdlog::info("VF_TEST_CLICK_SCAN: done, {} ctrl-clicks and "
                                 "{} plain-clicks", spots.size(), spots.size());
                    phase = 4;
                } else {
                    // The SECOND gesture: a plain click in the Rotate mode
                    // with the tool ARMED activates the object under the
                    // pointer (the trackball appears). It is a different
                    // code path from the Ctrl+LMB pick - and it is the one
                    // a user reaches for when they say "select this".
                    plain = true;
                    next = 0;
                    m_editActive = true;
                    m_editBrush = EditBrush::Rotate;
                    m_panel = Panel::Edit;
                    phase = 1;
                }
            } else {
                phase = 1;
            }
        }
    }

    // VF_TEST_PICK_SCAN: pick over a GRID OF WINDOW POSITIONS through the
    // app's REAL pick path (pickAtWindowPos - the same screen->ray
    // conversion, the same store march, the same store-vs-micro competition
    // the pointer uses) and report what won where.
    //
    // Why a grid and not a single point: the question is "can the pointer
    // select a micro object at all", and the answer is a SET of pixels, not
    // a coordinate someone typed. The earlier VF_TEST_MICRO_PICK hook only
    // proved that pickMicroObjects() finds an object in isolation - it never
    // went through the screen->ray conversion or the competition with the
    // world's store, so it could pass while the app's selection did not.
    if (const char* testPickScan = getenv("VF_TEST_PICK_SCAN");
        testPickScan && *testPickScan) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const int W = int(disp.x > 0.f ? disp.x : m_window.framebufferSize().x);
            const int H = int(disp.y > 0.f ? disp.y : m_window.framebufferSize().y);
            const int step = std::max(2, (testPickScan[0] == 'f') ? 4 : 12);
            int microHits = 0, storeHits = 0, misses = 0;
            std::map<std::string, int> perFile;
            int firstX = -1, firstY = -1;
            for (int y = 0; y < H; y += step) {
                for (int x = 0; x < W; x += step) {
                    const vf::voxel::PickHit h = pickAtWindowPos(double(x), double(y));
                    if (!h.hit) {
                        ++misses;
                    } else if (!h.microFile.empty()) {
                        ++microHits;
                        ++perFile[h.microFile];
                        if (firstX < 0) { firstX = x; firstY = y; }
                    } else {
                        ++storeHits;
                    }
                }
            }
            spdlog::info("VF_TEST_PICK_SCAN: {}x{} step {} -> {} micro hits, "
                         "{} store hits, {} misses; first micro hit at ({}, {})",
                         W, H, step, microHits, storeHits, misses, firstX, firstY);
            for (const auto& kv : perFile)
                spdlog::info("VF_TEST_PICK_SCAN: '{}' selected at {} pixels",
                             kv.first, kv.second);
            if (microHits == 0)
                spdlog::warn("VF_TEST_PICK_SCAN: NO micro object is selectable "
                             "through the real pick path");
        }
    }

    // VF_TEST_MICRO_PICK: exercise the app's OWN micro pick path - the same
    // pickMicroObjects() the pointer uses - from the camera toward every
    // micro object, and report what it found.
    //
    // A micro object is NOT in the world's store (it is a separate SVO world
    // on its own fine lattice), so this is the only way to verify that one is
    // SELECTABLE without a mouse. It is worth a hook because the bug it pins
    // is invisible in a screenshot: the field's CPU sampler returned 0
    // outside the field's grid, so the pick's sphere-trace crawled at its
    // minimum step and never reached an object 15 m away - the alpaca
    // rendered perfectly and could not be clicked.
    if (const char* testMicroPick = getenv("VF_TEST_MICRO_PICK");
        testMicroPick && *testMicroPick) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            int aimed = 0, hit = 0;
            for (const auto& o : m_microOccluders) {
                if (!o.field)
                    continue;
                ++aimed;
                // Aim at the object's own box centre, from the camera.
                const glm::vec3 centre = 0.5f * (o.aabbMin + o.aabbMax);
                const glm::vec3 toCentre = centre - m_camera.pos;
                if (glm::length(toCentre) < 1e-4f)
                    continue;
                const glm::vec3 rd = glm::normalize(toCentre);
                const MicroObjectPick mp = pickMicroObjects(m_microOccluders,
                                                            m_camera.pos, rd);
                const bool own = mp.hit && mp.file == o.layerFile;
                if (own)
                    ++hit;
                spdlog::info("VF_TEST_MICRO_PICK '{}' -> {}", o.layerFile,
                             mp.hit ? ("hit '" + mp.file + "' at " +
                                       std::to_string(mp.dist) + " m")
                                    : std::string("NO HIT"));
            }
            spdlog::info("VF_TEST_MICRO_PICK: {}/{} micro objects selectable",
                         hit, aimed);
        }
    }
}

} // namespace app
} // namespace vf
