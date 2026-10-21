// The one docked left sidebar: the icon rail, the active section's content
// pane, the fixed status footer, and the HUD overlay above them.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include <spdlog/spdlog.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace vf {
namespace app {

void App::drawHud()
{
    drawSidebar();
    drawSceneOverlays();
}

void App::drawSidebar()
{
    ImGuiIO& io = ImGui::GetIO();
    const float height = io.DisplaySize.y;

    // The width is user-owned once the right-edge grip is dragged, but stays
    // clamped to the current display. Width zero means "use the responsive
    // default"; double-clicking the grip restores that default.
    float totalW = m_sidebarCollapsed
        ? std::min(kRailW, io.DisplaySize.x)
        : sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    bool overResizeGrip = !m_sidebarCollapsed &&
        overSidebarResizeGrip(io, totalW, height);
    if (m_sidebarCollapsed) {
        m_sidebarResizing = false;
    } else if (io.MouseDoubleClicked[0] && overResizeGrip) {
        m_sidebarWidth = 0.0f;
        m_sidebarResizing = false;
        totalW = sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    } else if (!m_sidebarResizing && io.MouseClicked[0] &&
               overResizeGrip) {
        m_sidebarResizing = true;
    }
    if (m_sidebarResizing) {
        if (io.MouseDown[0]) {
            m_sidebarWidth = clampSidebarWidth(io.MousePos.x,
                                                io.DisplaySize.x);
        } else {
            m_sidebarResizing = false;
            spdlog::info("sidebar width -> {:.0f} px", m_sidebarWidth);
        }
        totalW = sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    }
    const bool resizeCapturesMouse = overResizeGrip || m_sidebarResizing;

    // A trackball ring is drawn over the scene, so it can land on top of the
    // sidebar. Ring hits are tested BEFORE ImGui mouse capture, and while the
    // pointer is on a ring the whole sidebar stops accepting input: otherwise
    // the panel would either steal the drag or an unrelated button underneath
    // the ring would toggle. An in-progress drag also fades the sidebar so the
    // object being rotated or moved stays visible. The docked resize edge wins
    // a tie so dragging it can never rotate a ring underneath the grip.
    const TrackballHandle hoveredHandle =
        m_gizmoValid && !resizeCapturesMouse
        ? trackballHandleAt(glm::vec2(io.MousePos.x, io.MousePos.y),
                            m_gizmoCentre, m_gizmoRadius)
        : TrackballHandle::None;
    const bool overRing = hoveredHandle != TrackballHandle::None;
    const bool dragging = m_rotating || m_moving;

    // Docked, not floating: the opaque panel occupies the whole left edge,
    // top to bottom, with no inset margin, title bar, rounding, or border.

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    // Child surfaces normally have their own translucent colour, leaving
    // darker top/bottom padding bands that make a flush window look inset.
    // Match the dock background exactly while idle. During a trackball drag
    // the parent supplies the 0.30 fade, so children must contribute no second
    // translucent layer of their own.
    ImVec4 dockBackground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    dockBackground.w = dragging ? 0.0f : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, dockBackground);
    // NoSavedSettings + Cond_Always: a stale imgui.ini saved by the old
    // floating layout can never drag this panel off-screen or resize it.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(totalW, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(dragging ? 0.30f : 1.0f);
    // NoDecoration = NoTitleBar | NoResize | NoScrollbar | NoCollapse
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollWithMouse;
    if (overRing)
        flags |= ImGuiWindowFlags_NoInputs;

    const bool open = ImGui::Begin("Voxelforge##Sidebar", nullptr, flags);
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor();
    if (open) {
        const float topH = std::max(0.0f, height - kFooterH - 2.0f * ImGui::GetStyle().WindowPadding.y);

        // ---- icon rail -------------------------------------------------
        // The rail is a column of vector icons, not of two-letter codes: the
        // abbreviation ("ED", "WL") was a code to be decoded, and a drawing
        // is read at a glance. The active entry is marked three ways - a
        // filled pill, an accent-coloured glyph and an accent underline -
        // because the rail is the one thing that must stay legible while the
        // eye is on the scene.
        ImGui::BeginChild("rail", ImVec2(kRailW, topH), ImGuiChildFlags_None);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        for (int i = 0; i < kPanelCount; ++i) {
            const Panel p = Panel(i);
            const bool active = (m_panel == p);
            const float cw = ImGui::GetContentRegionAvail().x;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float bs = 36.0f;
            ImGui::InvisibleButton(kPanels[i].label, ImVec2(cw, bs));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                if (p == Panel::Mesh)
                    prepareMeshImport();
                m_panel = p;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 r0(pos.x, pos.y);
            const ImVec2 r1(pos.x + cw, pos.y + bs);
            if (active) {
                dl->AddRectFilled(r0, r1, ImGui::GetColorU32(kAccentSoft), kUiRadius);
                dl->AddRectFilled(ImVec2(r0.x + 6.0f, r1.y - 2.0f),
                                  ImVec2(r1.x - 6.0f, r1.y), ImGui::GetColorU32(kAccent),
                                  2.0f);
            } else if (hovered) {
                dl->AddRectFilled(r0, r1,
                                  ImGui::GetColorU32(ImGui::GetStyleColorVec4(
                                      ImGuiCol_HeaderHovered)),
                                  kUiRadius);
            }
            const ImU32 iconCol = ImGui::GetColorU32(
                active ? kAccent : (hovered ? kTextPrimary : kTextSecondary));
            drawPanelIcon(p, ImVec2(r0.x + cw * 0.5f, r0.y + bs * 0.5f), kUiIcon,
                          iconCol);
            // The armed brush is tool state, not section state: a dot on the
            // Edit glyph says "the tool is live" without stealing the rail's
            // active-section marker.
            if (p == Panel::Edit && m_editActive)
                dl->AddCircleFilled(ImVec2(r1.x - 6.0f, r0.y + 6.0f), 3.0f,
                                    ImGui::GetColorU32(kAccent));
            if (hovered)
                ImGui::SetTooltip("%s  (%s)", kPanels[i].full, kPanels[i].key);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
        ImGui::EndChild();

        // ---- content pane ----------------------------------------------
        if (!m_sidebarCollapsed) {
            ImGui::SameLine();
            // The pane SCROLLS with the wheel. It used to carry
            // NoScrollWithMouse, which was protecting the camera's wheel
            // binding - a scroll over the sidebar scaled the movement speed
            // with nothing on screen saying so. That binding is gone (the
            // speed is the Render panel's VIEW slider now), so the wheel can
            // do the obvious thing here. The parent window keeps the flag,
            // so the rail does not scroll anything.
            if (ImGui::BeginChild("pane", ImVec2(0.0f, topH), ImGuiChildFlags_None)) {
                // The band names the section: switching panels is a jump to a
                // different place, and the place has to say what it is.
                const int pi = int(m_panel);
                paneBand(kPanels[pi].full, kPanels[pi].key);
                switch (m_panel) {
                case Panel::Edit:      drawPanelEdit(); break;
                case Panel::World:     drawPanelWorld(); break;
                case Panel::Render:    drawPanelRender(); break;
                case Panel::Textures:  drawPanelTextures(); break;
                case Panel::Mesh:      drawPanelMesh(); break;
                case Panel::AI:        drawPanelAI(); break;
                case Panel::kCount:    break;
                }
            }
            ImGui::EndChild();
        }
        drawSidebarFooter();
    }
    ImGui::End();

    // Custom horizontal splitter. It lives entirely inside the opaque panel,
    // so the resize affordance cannot introduce a transparent seam at the
    // dock edge. The input loop yields to the same 8 px hit zone before scene
    // picking/stamping; double-click resets to the responsive default.
    if (!m_sidebarCollapsed && totalW > 0.0f && height > 0.0f) {
        const float gripLeft = std::max(0.0f, totalW - kSidebarGripW);
        const ImU32 tint = m_sidebarResizing
            ? IM_COL32(33, 212, 237, 210)
            : (overResizeGrip ? IM_COL32(33, 212, 237, 150)
                              : IM_COL32(120, 145, 170, 105));
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        if (m_sidebarResizing || overResizeGrip) {
            draw->AddRectFilled(
                ImVec2(gripLeft, 0.0f), ImVec2(totalW, height),
                IM_COL32(33, 212, 237, m_sidebarResizing ? 34 : 18));
        }
        const float centreY = height * 0.5f;
        for (float y = std::max(6.0f, centreY - 18.0f);
             y <= std::min(height - 6.0f, centreY + 18.0f); y += 6.0f) {
            draw->AddLine(ImVec2(gripLeft + 2.0f, y),
                          ImVec2(totalW - 2.0f, y), tint, 1.0f);
        }
    }
}

void App::drawSidebarFooter()
{
    ImGui::Separator();
    const float w = ImGui::GetContentRegionAvail().x;
    const char* mode = m_renderMode == RenderMode::Splats ? "SPLAT" : "SVO";

    if (w < 90.0f) {                       // rail-only sidebar
        ImGui::Text("%.0f", m_lastFrameMs);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tab expands the sidebar\n%.1f ms CPU | geo %.1f ms",
                              m_avgMs, m_profAvg[0]);
        return;
    }

    if (m_rotationStaged || m_moveStaged) {
        // A staged transform is a PENDING WRITE. It is the one thing in the
        // footer that must never be missed, so it takes the whole bar: an
        // amber chip, then where the Apply button is.
        statusPill(m_rotationStaged ? "ROTATION STAGED" : "MOVE STAGED", kWarn);
        ImGui::TextDisabled("Not written yet — Ctrl+1 to %s",
                            m_panel == Panel::Edit ? "apply or cancel"
                                                  : "review (Edit section)");
        return;
    }

    // Two aligned columns: WHAT the editor is doing on the left, HOW FAST it
    // is on the right. The old footer was one run-on line that had to be
    // parsed; a chip, a value and a right-aligned number can be read.
    if (ImGui::BeginTable("footerStatus", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableSetupColumn("timing", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        statusPill(mode, m_editActive ? kAccent : kTextSecondary);
        if (m_editActive) {
            ImGui::SameLine();
            statusPill(brushName(m_editBrush), kAccent);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
        ImGui::Text("%.1f ms", m_avgMs);
        ImGui::PopStyleColor();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (m_editActive) {
            ImGui::PushStyleColor(ImGuiCol_Text, kTextSecondary);
            if (m_editBrush == EditBrush::Smooth)
                ImGui::Text("%d vox · %.0f%% · %d col · +%d up", brushVoxels(),
                            m_smoothStrength * 100.0f, m_smoothLastColumns,
                            m_smoothLastRise);
            else if (brushIsPerVoxel() &&
                     (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve))
                ImGui::Text("%s · 1 voxel", brushName(m_editBrush));
            else if (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve)
                // Depth is spelled out here because the 3D depth indicator in
                // the post pass CANNOT saturate-free: it is a screen-space
                // line, so once the volume's far end leaves the viewport it
                // pins to the frame edge and two very large depths look
                // identical. This number is exact at every depth, so the
                // footer is the channel that never lies.
                ImGui::Text("%d vox · %.1f m", brushVoxels(), m_editDepth);
            else
                ImGui::Text("%d vox", brushVoxels());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kTextTertiary);
            ImGui::Text("%s", m_renderMode == RenderMode::Splats
                                  ? "Gaussian surfels" : "SVO reference");
            ImGui::PopStyleColor();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextTertiary);
        ImGui::Text("CPU %.0f · geo %.1f", m_lastFrameMs, m_profAvg[0]);
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ctrl+B collapses the sidebar; Ctrl+1..6 switch section\n"
                          "Tab switches View/Edit mode; in Edit mode A/D/S/C/M pick a mode\n"
                          "The hotkey bar at the bottom lists the keys that "
                          "apply right now");
}

} // namespace app
} // namespace vf
