// Per-frame input: the chat capture latch, the camera update, voxel picking,
// the brush stamp, and the rotate trackball / move axis drags. This is the slice
// that decides what the user is pointing at; what it writes is picked up later
// by the preview tint (run_brush_preview.cpp) and drawn by scene_overlays.cpp,
// which must agree with it about screen geometry.
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

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Camera first: picking, gizmo hit-testing and rendering must all use the same
// pose, otherwise the visible ring trails a moving camera by one frame.
//
// The text-field gate is a MUST-NOT-MOVE, not a "don't touch the keyboard":
// WASD/QE strafe, the RMB look drag and the whole flight path live inside
// Camera::update, so skipping the call freezes the view while the user types a
// layer name or a mesh path. The mouse delta is drained so the first RMB-drag
// after leaving the field does not yank the view by however far the cursor
// travelled while the camera was frozen. This replaces the chat-only gate,
// which left every other InputText (mesh path, layer filter, sun time) flying
// the camera while the user typed into them.
bool App::updateCamera(float dt)
{
    // One flag, one reading. textFieldOwnsKeyboard() covers the chat box as
    // well as every other field, so there is no second condition to keep in
    // sync (the chat's own m_inputFocused flag also stays stale true if the AI
    // section is left, which would freeze the camera for the rest of the run).
    const bool textCaptures = textFieldOwnsKeyboard();
    // One-shot per transition. "A text field owns the keyboard" is otherwise an
    // invisible state, and a gate that silently stops firing - or never
    // releases - is precisely the failure this line exists to expose. It is a
    // member of the frame, not of the flag, so it prints twice at most per
    // focus/unfocus cycle however long the user types.
    {
        static bool wasFocused = false;
        if (textCaptures != wasFocused) {
            wasFocused = textCaptures;
            spdlog::info("keyboard -> {}",
                         textCaptures ? "text field (input frozen)" : "app");
        }
    }
    // A/D/S are also brush-mode shortcuts while the brush is armed, but they
    // remain camera keys: movement is never taken away. The two uses do not
    // conflict because the mode keys are edge-triggered (one action per press)
    // while flying is level-triggered (continuous while held), so holding A to
    // strafe selects Add once and then just strafes.
    if (textCaptures) {
        // block camera move while typing; consume mouse delta to avoid jump
        double _dx, _dy;
        m_window.getMouseDelta(_dx, _dy);
    } else {
        m_camera.update(m_window, dt);
    }
    return textCaptures;
}

// While a text field owns the keyboard, everything below must be inert:
// picking would spend a ray/trace on a frame the user is typing through, the
// click that leaves the field must NOT also carve a voxel, rotate an object or
// drag a trackball ring, and a camera that keeps turning would fight the text
// caret the user is trying to place. Only the button latches are kept current,
// so the frame the user leaves the field gets a clean edge rather than a
// phantom one. The brush preview deliberately survives: it follows
// m_latchedHover, which this path leaves alone, so the tint stays on screen
// while a field is typed into (the same reason resizing the brush from the
// sidebar does not blink it out).
void App::drainInputWhileTextFocused()
{
    GLFWwindow* hw = m_window.handle();
    const bool lmb =
        glfwGetMouseButton(hw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    m_lmbWasDown = lmb;
    m_ctrlWasDown = glfwGetKey(hw, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                    glfwGetKey(hw, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    m_hoverHit.hit = false;
}

// Picking, the brush stamp, and the two gizmo drags. The whole block is one
// scope in the original frame body and the inner scopes interlock (a ring hit
// is tested before ImGui capture, and the sidebar grip yields to both), so it
// is moved as a unit rather than split further.
void App::processInput(bool textCaptures, bool rotateLiveTest)
{
    // A text field owns the keyboard. Drain the buttons, touch nothing else:
    // the latched preview keeps the tint alive, and the click that is about to
    // leave the field is spent on the field, not on the world behind it.
    if (textCaptures) {
        drainInputWhileTextFocused();
        return;
    }
    const float tanHalfFov = tanHalfFov60();
    // Voxel picking: Ctrl+LMB
    {
        ImGuiIO& pickIo = ImGui::GetIO();
        bool ctrl = glfwGetKey(m_window.handle(), GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS ||
                    glfwGetKey(m_window.handle(), GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
        double mx=0,my=0;
        glfwGetCursorPos(m_window.handle(), &mx, &my);
        glm::ivec2 fb = m_window.framebufferSize();
        // GLFW cursor coordinates are logical window pixels, while
        // framebufferSize() is physical pixels. Use ImGui's logical space
        // for picking so high-DPI activation clicks hit the same object as
        // the trackball rings.
        const ImVec2 pickDisplay = pickIo.DisplaySize;
        if (pickDisplay.x > 0.f && pickDisplay.y > 0.f)
            fb = glm::ivec2(int(pickDisplay.x), int(pickDisplay.y));
        const float pickSidebarW = m_sidebarCollapsed
            ? std::min(kRailW, pickDisplay.x)
            : sidebarWidthFor(m_sidebarWidth, pickDisplay.x);
        const bool overSidebarResize =
            !m_sidebarCollapsed && overSidebarResizeGrip(
                pickIo, pickSidebarW, pickDisplay.y);
        const bool sidebarResizeCapturesMouse =
            overSidebarResize || m_sidebarResizing;
        bool wantMouse = pickIo.WantCaptureMouse ||
                        sidebarResizeCapturesMouse;
        bool lmb = glfwGetMouseButton(m_window.handle(), GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
        // hover while Ctrl held (anchor pick) OR while the edit tool is active
        // (so an LMB click can stamp a carve/add at the pointed surface)
        bool computeHover = (ctrl || m_editActive) && !wantMouse && fb.x>0 && fb.y>0;
        if (computeHover) {
            float tanHalf = tanHalfFov;
            float aspect = float(fb.x)/float(fb.y);
            glm::vec3 rd = vf::voxel::screenRayDir(mx,my,fb.x,fb.y,tanHalf,aspect,
                                                   m_camera.forward(), m_camera.right(), m_camera.up());
            m_hoverHit = vf::voxel::rayPickStore(m_layers.store(),
                                                 m_camera.pos, rd);
            // A real pick - hit OR miss - supersedes the latch: it is the
            // pointer's current opinion about the scene. m_latchedHover.hit is
            // the single source of truth for "is there a latched preview";
            // a separate bool would only be a second thing to keep in sync.
            m_latchedHover = m_hoverHit;
        } else {
            m_hoverHit.hit = false;
            // The pointer is over the UI (a sidebar slider, the rail, the
            // resize grip), not off the world: keep the last scene pick so the
            // brush preview stays on screen while the brush is resized. Any
            // other reason for not picking - the pointer left the window, the
            // target collapsed, the tool disarmed - drops the latch, so the
            // preview cannot outlive the interaction that justified it.
            if (!wantMouse)
                m_latchedHover.hit = false;
        }
        // forgiving trigger: fire on whichever edge arrives second, so a
        // few ms between LMB-down and Ctrl-down still picks
        bool lmbEdge = lmb && !m_lmbWasDown;
        bool ctrlEdge = ctrl && !m_ctrlWasDown;
        m_lmbWasDown = lmb;
        m_ctrlWasDown = ctrl;
        bool justPressed =
            ((lmbEdge && ctrl) || (ctrlEdge && lmb)) && !wantMouse;
        if (justPressed && m_hoverHit.hit) {
            m_selectedHit = m_hoverHit;
            m_hasSelection = true;
            const std::string pickedLayer = rotateTargetLayer(m_selectedHit);
            if (!pickedLayer.empty())
                m_selectedLayer = pickedLayer;
            glm::vec3 w = vf::voxel::voxelCenter(m_selectedHit.voxel);
            spdlog::info("pick selected {} {} {} world {:.2f} {:.2f} {:.2f} mat {} layer {}",
                m_selectedHit.voxel.x, m_selectedHit.voxel.y, m_selectedHit.voxel.z,
                w.x,w.y,w.z, int(m_selectedHit.mat),
                pickedLayer.empty() ? "<terrain/unowned>" : pickedLayer);
            // Identify the splats at the picked cell. A splat whose disk does
            // not reach any neighbour is the "floating in space" case; the
            // report prints its stable id so it can be named.
            describeSurfelsAt(m_selectedHit.voxel);
        }
        // Edit tool: plain LMB stamps at the hover point. With "Live patch"
        // on, holding LMB keeps painting (stamp spacing = a quarter brush
        // diameter) so the result appears while drawing; the stroke is
        // saved asynchronously on release.
        const bool editLmb = lmb && !ctrl && !wantMouse && m_editActive &&
                             m_editBrush != EditBrush::Rotate &&
                             m_editBrush != EditBrush::Move;
        bool doStamp = lmbEdge && editLmb;
        if (editLmb && m_hoverHit.hit) {
            // at most one stamp per voxel crossing: a per-voxel brush that
            // re-stamped every 0.08 m would redo the same cell 1.25x
            const float spacing =
                std::max(vf::voxel::VOXEL, m_editDiameter * 0.25f);
            if (!m_hasStamp ||
                glm::distance(vf::voxel::voxelCenter(m_hoverHit.voxel),
                              vf::voxel::voxelCenter(m_lastStampVoxel)) >= spacing) {
                // ...and a click is ONE edit. Suppress (a) the cell the
                // last Add wrote - the stack - and (b) a candidate the
                // pointer has not travelled to since the last stamp, which
                // is jitter. A drag does neither, so it keeps painting.
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float travel = std::hypot(mp.x - m_lastStampMouse.x,
                                                mp.y - m_lastStampMouse.y);
                const bool stack = m_hoverHit.voxel == m_lastStampWroteCell;
                if (!m_hasStamp || (!stack && travel >= kDragTravelPx))
                    doStamp = true;
            }
        }
        if (doStamp && m_hoverHit.hit) {
            m_lastStampVoxel = m_hoverHit.voxel;
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                m_lastStampMouse = glm::vec2(mp.x, mp.y);
            }
            m_hasStamp = true;
            m_dragging = editLmb;
            applyEditLive();
        }
        // Trackball workflow: one plain click on an owned object activates
        // it and shows bounds-sized rings. A later press on the yaw,
        // pitch, or roll ring starts the live drag; release stages it and
        // leaves the same object active for another adjustment or Apply.
        const glm::vec2 inputMouse{float(mx), float(my)};
        const bool overTrackball = !sidebarResizeCapturesMouse &&
            m_gizmoValid &&
            trackballHandleAt(inputMouse, m_gizmoCentre, m_gizmoRadius) !=
                TrackballHandle::None;
        MoveAxis hoveredMoveAxis = MoveAxis::None;
        if (!sidebarResizeCapturesMouse && m_gizmoValid && m_editActive &&
            m_editBrush == EditBrush::Move && !m_moveLayer.empty()) {
            const vf::voxel::WorldAABB moveBox = m_layers.layerBox(m_moveLayer);
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            const glm::ivec2 fb(static_cast<int>(display.x),
                                static_cast<int>(display.y));
            const float fov = tanf(glm::radians(60.0f) * 0.5f);
            hoveredMoveAxis = moveAxisHandleAt(
                inputMouse, 0.5f * (moveBox.lo + moveBox.hi), m_camera,
                fov, fb, m_gizmoRadius);
        }
        const bool overMoveHandle = hoveredMoveAxis != MoveAxis::None;
        const bool rotateMode = m_editActive && !ctrl &&
                                (!wantMouse || overTrackball) &&
                                m_editBrush == EditBrush::Rotate &&
                                !m_rotationPreviewPending;
        const bool rotLmb = lmb && rotateMode;
        if (!rotLmb && !m_rotating && !m_rotationPreviewPending &&
            !m_rotationStaged && !m_moveStaged && !m_moving)
            m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        // m_rotateLayer is activation state, not button state: keep it
        // while Rotate remains selected, including after mouse-up/reload.
        if (m_editBrush != EditBrush::Rotate &&
            !m_rotationPreviewPending) {
            if (m_rotating) {
                m_rotating = false;
                m_rotateHandle = TrackballHandle::None;
                m_rotateLastMouse = {};
                m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
            }
            // A staged pose survives a temporary mode switch so the
            // explicit Apply/Cancel controls remain meaningful. It is
            // cleared only by Apply, Cancel, or target loss.
            if (!m_rotationStaged) {
                if (!m_rotateLayer.empty() || m_previewLightsShifted) {
                    m_rotateLayer.clear();
                    m_rotateHandle = TrackballHandle::None;
                    m_rotateLastMouse = {};
                    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                    // The drag-pushed geometry preview is gone with the
                    // target: restore the base light set to splat (no-op
                    // when the overlay was never pushed).
                    refreshPreviewLights();
                }
            }
        }
        if (m_editBrush != EditBrush::Move && !m_moveStaged) {
            if (m_moving)
                cancelMove();
            m_moveLayer.clear();
        }

        // Ring input takes precedence over the object ray pick: clicking
        // a ring must rotate the already activated object, even when the
        // ring lies over another surface in screen space.
        if (rotLmb && lmbEdge && !m_rotating && m_gizmoValid) {
            m_rotateHandle = trackballHandleAt(
                inputMouse, m_gizmoCentre, m_gizmoRadius);
            if (m_rotateHandle != TrackballHandle::None) {
                m_rotating = true;
                m_rotateLastMouse = inputMouse;
                if (!m_rotationStaged)
                    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                m_taaFirstFrame = true;
                if (getenv("VF_TRACE"))
                    spdlog::info("trackball input: ring={} centre=({:.1f},{:.1f}) radius={:.1f}",
                                 int(m_rotateHandle), m_gizmoCentre.x,
                                 m_gizmoCentre.y, m_gizmoRadius);
            }
        }

        // A plain click activates exactly the object under the cursor.
        // It does not start rotation, so selecting and manipulating are
        // separate, discoverable actions.
        if (rotLmb && lmbEdge && !m_rotating && m_hoverHit.hit) {
            const std::string picked = rotateTargetLayer(m_hoverHit);
            if (!picked.empty() && m_layers.layerId(picked) != 0) {
                if (m_rotationStaged && m_rotateLayer != picked) {
                    spdlog::warn("rotate: apply or cancel the staged pose before selecting another object");
                } else {
                    m_rotateLayer = picked;
                    m_selectedLayer = picked;
                    m_selectedHit = m_hoverHit;
                    m_hasSelection = true;
                    m_rotating = false;
                    m_rotateHandle = TrackballHandle::None;
                    if (!m_rotationStaged)
                        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                    m_taaFirstFrame = true;
                    spdlog::info("rotate: activated {}", m_rotateLayer);
                }
            }
        }

        if (m_rotating && !rotateLiveTest) {
            const glm::vec2 delta = inputMouse - m_rotateLastMouse;
            m_rotateLastMouse = inputMouse;
            const float degreesPerPixel =
                120.f / (glm::pi<float>() * std::max(m_rotateRadius, 48.f));
            float localAngle = 0.0f;
            vf::voxel::worldfile::PlacementAxis localAxis =
                vf::voxel::worldfile::PlacementAxis::Y;
            switch (m_rotateHandle) {
            case TrackballHandle::Yaw:
                localAxis = vf::voxel::worldfile::PlacementAxis::Y;
                localAngle = delta.x * degreesPerPixel;
                break;
            case TrackballHandle::Pitch:
                localAxis = vf::voxel::worldfile::PlacementAxis::X;
                localAngle = -delta.y * degreesPerPixel;
                break;
            case TrackballHandle::Roll:
                localAxis = vf::voxel::worldfile::PlacementAxis::Z;
                localAngle = delta.x * degreesPerPixel;
                break;
            case TrackballHandle::None:
                m_rotating = false;
                break;
            }

            if (m_rotateHandle != TrackballHandle::None) {
                // Compose the drag on the right of the current absolute
                // pose: this is a rotation about the object's own local
                // axis, not a camera/world-axis Euler increment. Convert
                // back to the manifest's Ry*Rx*Rz angles only at the
                // preview/commit boundary.
                const auto layer = std::find_if(
                    m_worldLayers.begin(), m_worldLayers.end(),
                    [&](const vf::voxel::worldfile::WorldLayer& l) {
                        return l.file == m_rotateLayer;
                    });
                glm::vec3 pivot;
                if (layer != m_worldLayers.end() &&
                    m_layers.layerPivot(m_rotateLayer, pivot)) {
                    const glm::mat3 oldR =
                        vf::voxel::worldfile::placementRotation(
                            layer->rotDeg, layer->rotX, layer->rotZ);
                    const glm::mat3 currentR =
                        vf::voxel::worldfile::placementRotation(
                            layer->rotDeg + m_rotateDy,
                            layer->rotX + m_rotateDx,
                            layer->rotZ + m_rotateDz);
                    const glm::mat3 nextR =
                        vf::voxel::worldfile::rotatePlacementLocal(
                            currentR, localAxis, localAngle);
                    const glm::vec3 nextEuler =
                        vf::voxel::worldfile::placementEuler(nextR);
                    m_rotateDy = nextEuler.x - layer->rotDeg;
                    m_rotateDx = nextEuler.y - layer->rotX;
                    m_rotateDz = nextEuler.z - layer->rotZ;
                    const glm::mat3 R = nextR * glm::transpose(oldR);
                    m_splatPass.setRotatePreview(
                        pivot, R, true, m_layers.layerId(m_rotateLayer));
                    refreshPreviewLights(); // carried lamps + derived clusters ride the drag
                }
            }
        }
        // Move mode uses the exact picked owner and one explicit world
        // axis. It stages the delta on release; Apply is the only write.
        const bool moveMode = m_editActive && !ctrl &&
                              (!wantMouse || overMoveHandle) &&
                              m_editBrush == EditBrush::Move &&
                              !m_rotationStaged && !m_rotationPreviewPending;
        const bool moveLmb = lmb && moveMode;
        if (!moveMode && m_moving)
            cancelMove();
        if (moveLmb && lmbEdge && !m_moving &&
            (m_hoverHit.hit || overMoveHandle)) {
            const std::string picked = overMoveHandle
                ? m_moveLayer : rotateTargetLayer(m_hoverHit);
            if (!picked.empty() && m_layers.layerId(picked) != 0) {
                if (m_moveStaged && m_moveLayer != picked) {
                    spdlog::warn("move: apply or cancel the staged move before selecting another object");
                } else {
                    if (m_moveLayer != picked) {
                        m_moveLayer = picked;
                        m_moveDelta = glm::vec3(0.f);
                        m_moveStaged = false;
                    }
                    if (overMoveHandle)
                        m_moveAxis = hoveredMoveAxis;
                    m_selectedLayer = picked;
                    if (m_hoverHit.hit)
                        m_selectedHit = m_hoverHit;
                    m_hasSelection = true;
                    m_moving = true;
                    m_moveLastMouse = inputMouse;
                    m_taaFirstFrame = true;
                    spdlog::info("move: grabbed {} on axis {}",
                                 m_moveLayer, moveAxisName(m_moveAxis));
                }
            }
        }
        if (m_moving && moveLmb) {
            const glm::vec2 delta = inputMouse - m_moveLastMouse;
            m_moveLastMouse = inputMouse;
            constexpr float metresPerPixel = 0.05f;
            switch (m_moveAxis) {
            case MoveAxis::X: m_moveDelta.x += delta.x * metresPerPixel; break;
            case MoveAxis::Y: m_moveDelta.y -= delta.y * metresPerPixel; break;
            case MoveAxis::Z: m_moveDelta.z += delta.x * metresPerPixel; break;
            case MoveAxis::None: break;
            }
        }
        if (m_editBrush == EditBrush::Move && !m_moveLayer.empty() &&
            (m_moving || m_moveStaged)) {
            glm::vec3 pivot;
            if (m_layers.layerPivot(m_moveLayer, pivot)) {
                m_splatPass.setRotatePreview(
                    pivot, glm::mat3(1.f), m_moveDelta, true,
                    m_layers.layerId(m_moveLayer));
                refreshPreviewLights(); // carried lamps + derived clusters ride the drag
            }
        }

        if (!lmb) {
            if (m_rotating && !rotateLiveTest) {
                m_rotating = false;
                m_rotateHandle = TrackballHandle::None;
                m_rotateLastMouse = {};
                if (std::abs(m_rotateDy) >= 1e-4f ||
                    std::abs(m_rotateDx) >= 1e-4f ||
                    std::abs(m_rotateDz) >= 1e-4f) {
                    m_rotationStaged = true;
                    spdlog::info("rotate: staged {} (press Apply to persist)", m_rotateLayer);
                } else if (!m_rotationStaged) {
                    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
                }
            }
            if (m_moving) {
                m_moving = false;
                m_moveLastMouse = {};
                if (glm::length(m_moveDelta) >= 1e-4f) {
                    m_moveStaged = true;
                    spdlog::info("move: staged {} (press Apply to persist)", m_moveLayer);
                }
            }
            if (m_hasStamp && m_liveEditor.attached()) {
                // one undo step per stroke (mouse down..up)
                finishStroke();
                m_overlayWriter.queue(m_layers.store(), overlayPath());
            }
            m_hasStamp = false;
            m_dragging = false;
        }
    }
}

} // namespace app
} // namespace vf
