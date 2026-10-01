// The keyboard: render-flag bits, the browser-style tool shortcuts, the edit
// hotkeys, and the sidebar chrome (Tab collapse, Ctrl+1..6 section jumps).
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Edge-triggered, and interactive only: a headless run must stay deterministic,
// because key state on a hidden window can phantom-trigger toggles. The bare
// 1..5 render-flag keys and the Ctrl path share a latch, so Ctrl gives the edge
// on exactly the frames the bare loop must ignore.
void App::handleHotkeys(bool chatCaptures, bool headlessRun)
{
    // ---- render-option hotkeys (edge-triggered, interactive only:
    // headless runs must stay deterministic - key state on a hidden
    // window can phantom-trigger toggles) ----
    if (!chatCaptures && !headlessRun) {
        auto edge = [](int k, GLFWwindow* w) {
            static std::vector<uint8_t> prev(1024, 0);
            bool now = glfwGetKey(w, k) == GLFW_PRESS;
            bool e = now && !prev[k];
            prev[k] = now ? 1 : 0;
            return e;
        };
        GLFWwindow* hw = m_window.handle();
        bool shift = glfwGetKey(hw, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                     glfwGetKey(hw, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        bool ctrl = glfwGetKey(hw, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                    glfwGetKey(hw, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        // Ctrl+Z: undo the last stroke (works with the edit tool on/off,
        // but only when the chat input does not capture the keyboard)
        if (ctrl && edge(GLFW_KEY_Z, hw))
            undoEdit();
        if (edge(GLFW_KEY_T, hw)) {
            m_tonemapLook = (m_tonemapLook + 1) % 3;
            spdlog::info("tonemap look -> {}", m_tonemapLook);
        }
        if (edge(GLFW_KEY_F, hw)) {
            m_renderMode = (m_renderMode == RenderMode::Splats) ? RenderMode::Svo
                                                                : RenderMode::Splats;
            spdlog::info("render mode -> {}",
                         m_renderMode == RenderMode::Splats ? "splats" : "svo");
        }
        if (edge(GLFW_KEY_N, hw)) {
            m_taaEnabled = !m_taaEnabled;
            m_taaFirstFrame = true; // never blend stale history on re-enable
            spdlog::info("TAA -> {}", m_taaEnabled ? "on" : "off");
        }
        // splat disk size ([ shrink / ] grow), live for the splat backend
        if (edge(GLFW_KEY_LEFT_BRACKET, hw)) {
            m_splatPass.setRadiusScale(m_splatPass.radiusScale() / 1.12f);
            spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
        }
        if (edge(GLFW_KEY_RIGHT_BRACKET, hw)) {
            m_splatPass.setRadiusScale(m_splatPass.radiusScale() * 1.12f);
            spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
        }
        // toggle the carve / add edit tool
        if (edge(GLFW_KEY_C, hw)) {
            m_editActive = !m_editActive;
            // Arming also reveals the brush controls, so the key that
            // starts an edit is never a two-step hunt through the rail.
            if (m_editActive)
                m_panel = Panel::Edit;
            spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");
        }
        // Sidebar chrome. Both are suppressed while a text field owns the
        // keyboard (the chat input is a multiline InputText, and Tab is a
        // normal character there).
        if (!ImGui::GetIO().WantTextInput) {
            if (edge(GLFW_KEY_TAB, hw)) {
                m_sidebarCollapsed = !m_sidebarCollapsed;
                spdlog::info("sidebar -> {}",
                             m_sidebarCollapsed ? "collapsed" : "expanded");
            }
            if (ctrl) {
                for (int i = 0; i < kPanelCount; ++i) {
                    if (!edge(GLFW_KEY_1 + i, hw))
                        continue;
                    if (Panel(i) == Panel::Mesh)
                        prepareMeshImport();
                    m_panel = Panel(i);
                    spdlog::info("sidebar section -> {}",
                                 kPanels[i].label);
                }
            }
        }
        if (m_editActive) {
            // + / - change the width by one voxel; Shift + / - change the
            // depth (or Smooth strength when the relaxation brush is
            // selected). The floor is one voxel = per-voxel mode.
            if (edge(GLFW_KEY_EQUAL, hw) || edge(GLFW_KEY_KP_ADD, hw)) {
                if (shift) {
                    if (m_editBrush == EditBrush::Smooth)
                        m_smoothStrength = std::min(m_smoothStrength + 0.05f, 1.0f);
                    else
                        setBrushDepthVoxels(brushDepthVoxels() + 2);
                } else {
                    setBrushVoxels(brushVoxels() + 1);
                }
            }
            if (edge(GLFW_KEY_MINUS, hw) || edge(GLFW_KEY_KP_SUBTRACT, hw)) {
                if (shift) {
                    if (m_editBrush == EditBrush::Smooth)
                        m_smoothStrength = std::max(m_smoothStrength - 0.05f, 0.0f);
                    else
                        setBrushDepthVoxels(brushDepthVoxels() - 2);
                } else {
                    setBrushVoxels(brushVoxels() - 1);
                }
            }
        } else {
            if (edge(GLFW_KEY_EQUAL, hw) || edge(GLFW_KEY_KP_ADD, hw)) {
                m_exposure = m_exposure * 1.1f < 4.0f ? m_exposure * 1.1f : 4.0f;
                spdlog::info("exposure -> {:.2f}", m_exposure);
            }
            if (edge(GLFW_KEY_MINUS, hw) || edge(GLFW_KEY_KP_SUBTRACT, hw)) {
                m_exposure = m_exposure / 1.1f > 0.1f ? m_exposure / 1.1f : 0.1f;
                spdlog::info("exposure -> {:.2f}", m_exposure);
            }
        }
        // Bare 1..5 toggle render flags. edge() is a per-key-code latch,
        // not a consume: the Ctrl+1..6 sidebar switch above polls these
        // same codes first, but only while Ctrl is held, so it takes the
        // edge on exactly the frames this loop must not act on.
        for (int i = 0; i < 5; ++i) {
            if (edge(GLFW_KEY_1 + i, hw)) {
                m_renderFlags ^= (1 << i);
                spdlog::info("render flag {} -> {}", i, (m_renderFlags >> i) & 1);
            }
        }
        if (edge(GLFW_KEY_0, hw)) {
            m_renderFlags = 31;
            spdlog::info("render flags reset -> 31");
        }
        // photorealism feature toggles
        if (edge(GLFW_KEY_G, hw)) {
            m_renderFlags ^= (1 << 5); // SSR
            spdlog::info("SSR -> {}", (m_renderFlags >> 5) & 1);
        }
        if (edge(GLFW_KEY_H, hw)) {
            m_renderFlags ^= (1 << 6); // SSAO
            spdlog::info("SSAO -> {}", (m_renderFlags >> 6) & 1);
        }
        if (edge(GLFW_KEY_B, hw)) {
            m_renderFlags ^= (1 << 7); // texture detail normals
            spdlog::info("detail normals -> {}", (m_renderFlags >> 7) & 1);
        }
        if (edge(GLFW_KEY_J, hw)) {
            m_volFogEnabled = !m_volFogEnabled;
            spdlog::info("volumetric fog -> {}", m_volFogEnabled);
        }
        if (edge(GLFW_KEY_K, hw)) {
            m_motionBlurEnabled = !m_motionBlurEnabled;
            spdlog::info("motion blur -> {}", m_motionBlurEnabled);
        }
        if (edge(GLFW_KEY_L, hw)) {
            m_dofEnabled = !m_dofEnabled;
            spdlog::info("depth of field -> {}", m_dofEnabled);
        }
        if (edge(GLFW_KEY_M, hw)) {
            // Micro detail is baked into the surfel stream, so flipping it
            // has to re-run the surfelizer - the same path a layer toggle
            // uses (stalls the device, so it is a deliberate action).
            m_microDetail = !m_microDetail;
            requestWorldReload();
            spdlog::info("micro detail -> {} (rebuilding surfels)", m_microDetail);
        }
    }
}

} // namespace app
} // namespace vf
