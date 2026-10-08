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
        ImGui::BeginChild("rail", ImVec2(kRailW, topH), ImGuiChildFlags_None);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        for (int i = 0; i < kPanelCount; ++i) {
            const Panel p = Panel(i);
            const bool active = (m_panel == p);
            if (actionButton(kPanels[i].label, active, ImVec2(-1.0f, 30.0f))) {
                if (p == Panel::Mesh)
                    prepareMeshImport();
                m_panel = p;
            }
            // Armed-tool marker as an accent bar on the button's right edge:
            // the brush is armed independently of the section on screen, and a
            // bar costs no vertical row the way a ">" text line did.
            if (p == Panel::Edit && m_editActive) {
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    ImVec2(b.x - 3.0f, a.y + 2.0f), ImVec2(b.x, b.y - 2.0f),
                    IM_COL32(33, 212, 237, 255));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kPanels[i].full);
        }
        ImGui::EndChild();

        // ---- content pane ----------------------------------------------
        if (!m_sidebarCollapsed) {
            ImGui::SameLine();
            if (ImGui::BeginChild("pane", ImVec2(0.0f, topH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollWithMouse)) {
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
    const char* mode = m_renderMode == RenderMode::Splats ? "splat" : "SVO";

    if (m_rotationStaged || m_moveStaged) {
        if (w < 90.0f) {
            ImGui::TextColored(kWarn, "EDIT");
            return;
        }
        ImGui::TextColored(kWarn, "%s staged — not written",
                           m_rotationStaged ? "Rotation" : "Move");
        ImGui::TextDisabled("Ctrl+1 to %s",
                            m_panel == Panel::Edit ? "apply or cancel"
                                                  : "review (Edit section)");
        return;
    }

    if (w < 90.0f) {                       // rail-only sidebar
        ImGui::Text("%.0f", m_lastFrameMs);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tab expands the sidebar\n%.1f ms CPU | geo %.1f ms",
                              m_avgMs, m_profAvg[0]);
        return;
    }
    if (m_editActive)
        ImGui::TextColored(kAccent, "%s  %.1f ms", mode, m_avgMs);
    else
        ImGui::Text("%s  %.1f ms", mode, m_avgMs);
    if (m_editActive) {
        if (m_editBrush == EditBrush::Smooth)
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | smooth %dvox %.0f%% | "
                                        "%d col, %d vox run, +%d up",
                               m_lastFrameMs, m_profAvg[0], brushVoxels(),
                               m_smoothStrength * 100.0f, m_smoothLastColumns,
                               m_smoothLastMaxDelta, m_smoothLastRise);
        else if (m_editBrush == EditBrush::Carve ||
                 m_editBrush == EditBrush::Add)
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s %dvox d%.1f "
                                        "%s",
                               m_lastFrameMs, m_profAvg[0], mode,
                               brushVoxels(), m_editDepth,
                               vf::voxel::EditableWorld::falloffCurveName(
                                   m_editFalloffCurve));
        else if (brushIsPerVoxel() &&
                 (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve))
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s 1 voxel",
                               m_lastFrameMs, m_profAvg[0], brushName(m_editBrush));
        else if (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve)
            // Depth is spelled out here because the 3D depth indicator in the
            // post pass CANNOT saturate-free: it is a screen-space line, so once
            // the volume's far end leaves the viewport it pins to the frame edge
            // and two very large depths look identical. This number is exact at
            // every depth, so the footer is the channel that never lies.
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s %dvox %.1fm",
                               m_lastFrameMs, m_profAvg[0], brushName(m_editBrush),
                               brushVoxels(), m_editDepth);
        else
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s %dvox",
                               m_lastFrameMs, m_profAvg[0], brushName(m_editBrush),
                               brushVoxels());
    } else {
        ImGui::TextDisabled("%.0f CPU  |  geo %.1f", m_lastFrameMs, m_profAvg[0]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ctrl+B collapses the sidebar; Ctrl+1..6 switch section\n"
                          "Tab switches View/Edit mode; in Edit mode A/D/S/C/M pick a mode\n"
                          "The hotkey bar at the bottom lists the keys that "
                          "apply right now");
}

} // namespace app
} // namespace vf
