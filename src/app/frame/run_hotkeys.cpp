// The keyboard: render-flag bits, the browser-style tool shortcuts, the edit
// hotkeys, and the sidebar chrome (Tab collapse, Ctrl+1..6 section jumps).
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/sun_angles.hpp"
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
namespace {

// Does this physical key show `want` on the CURRENT layout?
//
// GLFW reports physical positions, so the key that types '+' is layout
// dependent: on a US layout it is Shift+EQUAL, on a German one it is the AD12
// position that GLFW calls RIGHT_BRACKET (and the German '-' is the AB10
// position it calls SLASH). Binding positions therefore silently does nothing
// for the keycap the user is actually reading. glfwGetKeyName asks the layout.
//
// Deliberately NOT done via io.InputQueueCharacters: ImGui fills that from its
// event queue during NewFrame(), which this file runs BEFORE (the frame calls
// handleHotkeys, then record_interactive calls NewFrame), so the queue is
// always empty here.
bool keyShowsChar(int key, char want)
{
    const int sc = glfwGetKeyScancode(key);
    if (sc < 0)
        return false;
    const char* nm = glfwGetKeyName(key, sc);
    return nm != nullptr && nm[0] == want && nm[1] == '\0';
}

} // namespace

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
        // splat disk size ([ shrink / ] grow), live for the splat backend.
        // On a German layout the '+' keycap sits at the AD12 position, which
        // GLFW reports as RIGHT_BRACKET - so while editing, a '+' press must
        // NOT also grow the disks, or one press would change brush size and
        // splat size together. Suppressed only while editing and only when the
        // layout really puts '+' on that key, so US ']' keeps working.
        const bool leftBracketEdge = edge(GLFW_KEY_LEFT_BRACKET, hw);
        const bool rightBracketEdge = edge(GLFW_KEY_RIGHT_BRACKET, hw);
        const bool rightBracketIsPlus = keyShowsChar(GLFW_KEY_RIGHT_BRACKET, '+');
        if (leftBracketEdge) {
            m_splatPass.setRadiusScale(m_splatPass.radiusScale() / 1.12f);
            spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
        }
        if (rightBracketEdge && !(m_editActive && rightBracketIsPlus)) {
            m_splatPass.setRadiusScale(m_splatPass.radiusScale() * 1.12f);
            spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
        }
        // ---- edit brush modes ------------------------------------------------
        // The mode keys are ALSO camera keys (A/D/S = strafe/back) and they
        // keep flying - movement is never yielded to the brush, because flying
        // is how you aim the brush. The two uses do not conflict: a mode is
        // picked on the EDGE (one action per press, matching the rest of this
        // function) while flying is level-triggered, so holding A strafes
        // continuously and selects Add once.
        //
        // The mode keys act ONLY while the brush is armed. Tab is the single
        // View/Edit switch, both directions: a stray A/D/S/C/M while flying
        // must never drag the user into Edit mode, and no non-Tab key may
        // leave it either. This REVERSED the earlier design where the mode
        // keys armed the brush themselves and C disarmed on a second press -
        // do not restore either.
        //
        // Rotate stays unbound: R would collide with nothing today but it needs
        // a selected layer first, and a hotkey that silently no-ops is worse
        // than no hotkey. It is one click in the Edit section.
        //
        // All five edges are read unconditionally, even in View mode: edge()
        // is a stateful latch, so short-circuiting a key while disarmed would
        // leave its latch stale and it would phantom-fire on the frame the
        // brush arms.
        const bool cEdge = edge(GLFW_KEY_C, hw);
        const bool aEdge = edge(GLFW_KEY_A, hw);
        const bool dEdge = edge(GLFW_KEY_D, hw);
        const bool sEdge = edge(GLFW_KEY_S, hw);
        const bool mEdge = edge(GLFW_KEY_M, hw);
        if (m_editActive) {
            struct ModeKey { bool pressed; EditBrush mode; };
            const ModeKey kModeKeys[] = {
                { cEdge, EditBrush::Carve }, { aEdge, EditBrush::Add },
                { dEdge, EditBrush::Delete }, { sEdge, EditBrush::Smooth },
                { mEdge, EditBrush::Move }
            };
            for (const ModeKey& mk : kModeKeys)
                if (mk.pressed)
                    chooseEditMode(mk.mode);
        }
        // Sidebar chrome. Both are suppressed while a text field owns the
        // keyboard (the chat input is a multiline InputText, and Tab is a
        // normal character there).
        if (!ImGui::GetIO().WantTextInput) {
            // Tab is the ONLY View/Edit switch, both directions. The mode
            // keys do not arm the brush and C does not disarm (see above):
            // one key owns the mode, so a press can never switch it by
            // accident. Arming reveals the brush controls so the switch into
            // Edit mode is never a two-step hunt through the rail.
            if (edge(GLFW_KEY_TAB, hw)) {
                m_editActive = !m_editActive;
                if (m_editActive)
                    m_panel = Panel::Edit;
                spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");
            }
            // Sidebar collapse moved off Tab to make room for the edit toggle.
            // Ctrl+B, which is free and the conventional "toggle the panel"
            // chord. Note B is NOT unbound - bare B toggles detail normals
            // below - so this relies on the same latch discipline as
            // Ctrl+1..6 vs bare 1..5: this block runs FIRST, so while Ctrl is
            // held it takes B's edge and the bare toggle below sees none. That
            // is the intended reading of the latch, not an accident.
            if (ctrl && edge(GLFW_KEY_B, hw)) {
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
            //
            // Keycaps, not positions. GLFW reports physical X positions, so on a
            // German layout '+' is the AD12 position (which GLFW calls
            // RIGHT_BRACKET and which also drives splat size) and '-' is AB10
            // (GLFW_KEY_SLASH, bound to nothing) - the position-based test
            // silently did nothing for the key the user was actually pressing.
            // keyShowsChar asks the layout which keycap is which.
            //
            // The numpad stays positional (KP_ADD/KP_SUBTRACT are unambiguous
            // in every layout), and the US '='/'-' keys are matched only when
            // the layout really shows '='/'-' there, so a German dead key in
            // the '=' position cannot spuriously grow the brush.
            //
            // Every edge() call is made unconditionally: they are stateful
            // latches, so short-circuiting one leaves its latch stale and it
            // phantom-fires on a later frame.
            const bool kpAddEdge = edge(GLFW_KEY_KP_ADD, hw);
            const bool kpSubEdge = edge(GLFW_KEY_KP_SUBTRACT, hw);
            const bool eqEdge = edge(GLFW_KEY_EQUAL, hw);
            const bool minusEdge = edge(GLFW_KEY_MINUS, hw);
            const bool rbEdge = edge(GLFW_KEY_RIGHT_BRACKET, hw);
            const bool slashEdge = edge(GLFW_KEY_SLASH, hw);
            const bool inc = kpAddEdge || (eqEdge && keyShowsChar(GLFW_KEY_EQUAL, '=')) ||
                             (rbEdge && keyShowsChar(GLFW_KEY_RIGHT_BRACKET, '+'));
            const bool dec = kpSubEdge ||
                             (minusEdge && keyShowsChar(GLFW_KEY_MINUS, '-')) ||
                             (slashEdge && keyShowsChar(GLFW_KEY_SLASH, '-'));
            if (inc) {
                if (shift) {
                    if (m_editBrush == EditBrush::Smooth) {
                        m_smoothStrength = std::min(m_smoothStrength + 0.05f, 1.0f);
                        spdlog::info("smooth strength -> {:.0f}%",
                                     m_smoothStrength * 100.0f);
                    } else {
                        setBrushDepthVoxels(brushDepthVoxels() + 2);
                        spdlog::info("brush depth -> {} vox", brushDepthVoxels());
                    }
                } else {
                    setBrushVoxels(brushVoxels() + 1);
                    spdlog::info("brush size -> {} vox", brushVoxels());
                }
            }
            if (dec) {
                if (shift) {
                    if (m_editBrush == EditBrush::Smooth) {
                        m_smoothStrength = std::max(m_smoothStrength - 0.05f, 0.0f);
                        spdlog::info("smooth strength -> {:.0f}%",
                                     m_smoothStrength * 100.0f);
                    } else {
                        setBrushDepthVoxels(brushDepthVoxels() - 2);
                        spdlog::info("brush depth -> {} vox", brushDepthVoxels());
                    }
                } else {
                    setBrushVoxels(brushVoxels() - 1);
                    spdlog::info("brush size -> {} vox", brushVoxels());
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
        // P toggles the coarse day/night switch (Render panel Sun section).
        // Same sunIsNight() the Night button's highlight uses, read back off
            // m_sunDir rather than a parallel bool, so it agrees with whatever
            // the sliders last set. It rebuilds: the per-surfel sun
            // shadow is CPU-baked.
        if (edge(GLFW_KEY_P, hw)) {
            const float curElev =
                glm::degrees(std::asin(glm::clamp(m_sunDir.y, -1.0f, 1.0f)));
            setSunPhase(!sunIsNight(curElev)); // was day -> go night, else -> go day
        }
    }
}

} // namespace app
} // namespace vf
