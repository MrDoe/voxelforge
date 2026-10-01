// The overlays drawn on top of the render: the rotate trackball / move axis
// handles, and the per-voxel hover outline.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
}

} // namespace app
} // namespace vf
