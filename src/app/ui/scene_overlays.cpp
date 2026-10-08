// The overlays drawn on top of the render: the rotate trackball / move axis
// handles, and the per-voxel hover outline.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::drawSceneOverlays()
{
    ImGuiIO& io = ImGui::GetIO();
    if (m_gizmoValid && m_editBrush == EditBrush::Rotate) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 c(m_gizmoCentre.x, m_gizmoCentre.y);
        const float radius = m_gizmoRadius;
        const ImVec2 mouse = io.MousePos;
        const TrackballHandle hovered = trackballHandleAt(
            glm::vec2(mouse.x, mouse.y), m_gizmoCentre, radius);
        const ImU32 idle = IM_COL32(54, 160, 205, 220);
        const ImU32 hover = IM_COL32(220, 248, 255, 255);
        const ImU32 active = IM_COL32(255, 205, 92, 255);
        auto ringStyle = [&](TrackballHandle handle) {
            const bool isActive = m_rotating && m_rotateHandle == handle;
            const ImU32 color = isActive ? active
                                         : (hovered == handle ? hover : idle);
            return std::pair<ImU32, float>{color, isActive ? 3.5f :
                                           (hovered == handle ? 3.0f : 2.0f)};
        };
        const auto [yawColor, yawThickness] = ringStyle(TrackballHandle::Yaw);
        const auto [pitchColor, pitchThickness] = ringStyle(TrackballHandle::Pitch);
        const auto [rollColor, rollThickness] = ringStyle(TrackballHandle::Roll);
        dl->AddCircle(c, radius, yawColor, 0, yawThickness);
        dl->AddEllipse(c, ImVec2(radius, radius * 0.42f),
                       pitchColor, 0, 48, pitchThickness);
        dl->AddEllipse(c, ImVec2(radius * 0.42f, radius),
                       rollColor, 0, 48, rollThickness);
        // Local-axis labels make the three otherwise-similar rings
        // discoverable: outer/Y, wide/X, tall/Z.
        dl->AddText(ImVec2(c.x + radius + 5.0f, c.y - 7.0f), yawColor, "Y");
        dl->AddText(ImVec2(c.x - 3.0f, c.y - radius * 0.42f - 16.0f),
                    pitchColor, "X");
        dl->AddText(ImVec2(c.x + radius * 0.42f + 5.0f, c.y - radius - 16.0f),
                    rollColor, "Z");
        dl->AddCircleFilled(c, 4.0f, hovered == TrackballHandle::None
                                      ? idle : hover, 20);
    } else if (m_gizmoValid && m_editBrush == EditBrush::Move &&
               !m_moveLayer.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const vf::voxel::WorldAABB b = m_layers.layerBox(m_moveLayer);
        if (b.valid()) {
            const glm::vec3 centre = 0.5f * (b.lo + b.hi);
            const ImVec2 display = io.DisplaySize;
            const glm::ivec2 fb(static_cast<int>(display.x),
                                static_cast<int>(display.y));
            const float tanHalfFov = tanf(glm::radians(60.0f) * 0.5f);
            const ImU32 axisColors[3] = {
                IM_COL32(244, 96, 96, 235),  // X
                IM_COL32(104, 226, 132, 235), // Y
                IM_COL32(104, 176, 255, 235)  // Z
            };
            const MoveAxis axes[3] = { MoveAxis::X, MoveAxis::Y, MoveAxis::Z };
            const glm::vec2 p0 = projectScreen(centre, m_camera, tanHalfFov, fb);
            const ImVec2 centreI(p0.x, p0.y);
            for (int i = 0; i < 3; ++i) {
                const MoveAxis axis = axes[i];
                const MoveAxisLine line = moveAxisScreenLine(
                    centre, axis, m_camera, tanHalfFov, fb, m_gizmoRadius);
                const bool selected = m_moveAxis == axis;
                const ImU32 color = selected ? IM_COL32(255, 205, 92, 255)
                                             : axisColors[i];
                dl->AddLine(ImVec2(line.a.x, line.a.y),
                            ImVec2(line.b.x, line.b.y), color,
                            selected ? 3.5f : 2.0f);
                dl->AddCircleFilled(ImVec2(line.b.x, line.b.y),
                                    selected ? 5.0f : 3.5f, color, 20);
                dl->AddText(ImVec2(line.b.x + 5.0f, line.b.y - 7.0f), color,
                            moveAxisName(axis));
            }
            dl->AddCircleFilled(centreI, 4.0f, IM_COL32(220, 248, 255, 235), 20);
            if (m_moveStaged) {
                const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                                  : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                                  : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
                char text[64];
                snprintf(text, sizeof(text), "Move %s %+.2f m%s",
                         moveAxisName(m_moveAxis), amount,
                         m_moving ? " (dragging)" : " (staged)");
                dl->AddText(ImVec2(centreI.x - 48.0f, centreI.y + 22.0f),
                            IM_COL32(255, 205, 92, 255), text);
            }
        }
    }

    if (m_hasSelection) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const ImVec2 marker(centre.x - 42.0f, centre.y + 25.0f);
        dl->AddCircleFilled(marker, 3.5f, IM_COL32(64, 224, 160, 235), 16);
        dl->AddText(ImVec2(centre.x - 33.0f, centre.y + 20.0f),
                    IM_COL32(160, 245, 210, 245), "Selected voxel");
    }

    drawHotkeyBar();
}

// The bottom-of-screen hotkey reminder.
//
// A/D/S/C/M are recent additions that silently took over three long-standing
// camera keys, so "what does this key do now" is exactly the question a user
// has when they press the wrong thing. The bar is therefore state-dependent
// rather than a static legend: it shows the edit keys when the brush is armed
// and the flight keys when it is not, which is also the honest description of
// the routing in run_hotkeys.cpp / camera.cpp. The mode keys appear only
// while armed - in View mode they do nothing, and Tab is the only key that
// switches the mode.
//
// Drawn on the foreground list like the gizmos (no window, no input), and
// centred on the area RIGHT of the sidebar so it is never hidden underneath it
// - the sidebar is an overlay, so a centred bar would be clipped by it.
void App::drawHotkeyBar()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 420.0f || io.DisplaySize.y < 200.0f)
        return; // too small to place legibly (headless/offscreen sizes)

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // This ImGui version exposes the default font as Fonts[0] / ImFontDefault -
// there is no ImFontAtlas::GetFont. Prefer the bound context font so the bar
// matches whatever the HUD is using this frame.
ImFont* font = io.FontDefault ? io.FontDefault
                              : (io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]
                                                         : nullptr);
    if (!font)
        return;
    const float fontSize = font->FontSize;
    const float pad = 6.0f;
    const float gap = fontSize * 0.55f;

    // Key label -> the action it performs right now.
    struct Entry { const char* key; const char* label; bool active; };
    std::vector<Entry> entries;
    // Movement is listed FIRST and unconditionally: W/A/S/D/Q/E fly in every
    // edit-tool state, and that is the single most surprising-adjacent fact
    // after the mode keys took letters that are also flight keys.
    if (m_editActive) {
        // Mode keys: the armed mode is highlighted, so the bar doubles as the
        // "which tool am I in" readout (the sidebar footer can be collapsed).
        // A/D/S appear once, in the mode row, because the mode is what the
        // press does; the fly row still covers them for the hold.
        entries = {
            { "W/A/S/D", "fly", false },
            { "Q/E", "down/up", false },
            { "C", "Carve", m_editBrush == EditBrush::Carve },
            { "A", "Add", m_editBrush == EditBrush::Add },
            { "D", "Delete", m_editBrush == EditBrush::Delete },
            { "S", "Smooth", m_editBrush == EditBrush::Smooth },
            { "M", "Move", m_editBrush == EditBrush::Move },
            { "+/-", "size", false },
            { "Tab", "View mode", false },
        };
    } else {
        // View mode: only Tab enters Edit mode. The mode keys are
        // deliberately NOT listed - they do nothing until the brush is
        // armed, so advertising them here would promise a mode switch that
        // no longer happens.
        entries = {
            { "W/A/S/D", "fly", false },
            { "Q/E", "down/up", false },
            { "Tab", "Edit mode", false },
            { "Ctrl+B", "sidebar", false },
        };
    }

    // Measure first so the strip can be centred exactly. width is the plain sum
    // of the chip widths: subtracting one pad pair here made the plate 12 px
    // narrower than its last chip, so the final chip's padding hung outside it.
    float width = 0.0f;
    for (const Entry& e : entries) {
        const float kw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, e.key).x;
        const float lw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, e.label).x;
        width += kw + gap + lw + pad * 2.0f;
    }

    // Centre on the region right of the sidebar (it is an overlay, so it
    // covers the left edge rather than shrinking the viewport).
    const float sidebarW =
        m_sidebarCollapsed
            ? std::min(kRailW, io.DisplaySize.x)
            : sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    const float availLeft = sidebarW;
    const float availW = io.DisplaySize.x - availLeft;
    const float platePad = 6.0f;
    // Clamp rather than trusting the centring: with 8 entries at 960x540 and a
    // 300 px sidebar the strip is within ~10 px of the right edge, and a wide
    // user-dragged sidebar pushes it off-screen entirely. Centring alone would
    // draw a plate that runs past the display and gets clipped, which reads as
    // "the bar is broken" rather than "the bar is wide".
    const float minX = platePad;
    const float maxX = std::max(minX, io.DisplaySize.x - width - platePad);
    const float barX = std::clamp(
        availLeft + (availW - width) * 0.5f, minX, maxX);

    // One dark plate behind the whole strip. Without it the bar is unreadable
    // over bright terrain, a pale sky or the water highlight - the text colour
    // alone cannot carry legibility on every background the scene produces.
    // Drawn first so every chip and label composites on top of it.
    //
    // The plate is laid out from the BOTTOM EDGE INWARD and the text is centred
    // inside it. Anchoring the text to a fixed offset from the screen bottom and
    // then padding the plate put the plate's lower edge ~3.5 px OFF-SCREEN
    // (ImGui's AddText y is the text TOP, so the text already ended 10 px above
    // the bottom, and padding was added on top of that).
    const float plateBottom = io.DisplaySize.y - 5.0f;
    const float plateTop = plateBottom - (fontSize + platePad * 2.0f);
    const float y = plateTop + platePad; // text top, inside the plate
    dl->AddRectFilled(ImVec2(barX - platePad, plateTop),
                      ImVec2(barX + width + platePad, plateBottom),
                      IM_COL32(10, 13, 18, 200), 6.0f);

    float x = barX;
    const ImU32 dim = IM_COL32(200, 210, 220, 225);
    const ImU32 keyCol = IM_COL32(160, 205, 255, 245);
    const ImU32 actCol = IM_COL32(255, 205, 92, 255);

    for (const Entry& e : entries) {
        const float kw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, e.key).x;
        const float lw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, e.label).x;
        const float chipW = kw + gap + lw + pad * 2.0f;
        if (e.active) {
            dl->AddRectFilled(ImVec2(x, y - 2.0f),
                              ImVec2(x + chipW, y + fontSize + 2.0f),
                              IM_COL32(70, 54, 18, 235), 4.0f);
        }
        dl->AddText(ImVec2(x + pad, y), e.active ? actCol : keyCol, e.key);
        dl->AddText(ImVec2(x + pad + kw + gap, y), e.active ? actCol : dim,
                    e.label);
        x += chipW;
    }
}

} // namespace app
} // namespace vf
