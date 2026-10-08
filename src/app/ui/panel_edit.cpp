// The sidebar's edit section: the seven brush modes, the lattice size/material controls, and the perf readout.
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

// The single mode-switch implementation, shared with the A/D/S/C/M hotkeys in
// run_hotkeys.cpp. The guards matter: switching modes while a rotation/move is
// staged (or a preview is committing) would either orphan the pending
// transform or apply it to the wrong mode, so both callers must refuse the
// same way. Declared in app.hpp.
void App::chooseEditMode(EditBrush mode)
{
    if (m_rotationPreviewPending ||
        (mode == EditBrush::Move && m_rotationStaged) ||
        (mode == EditBrush::Rotate && m_moveStaged))
        return;
    m_editBrush = mode;
    if (mode == EditBrush::Rotate) {
        if (!m_rotationStaged || m_rotateLayer.empty())
            m_rotateLayer = m_selectedLayer;
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        if (!m_rotationStaged)
            m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        m_renderMode = RenderMode::Splats;
        m_taaFirstFrame = true;
    } else if (mode == EditBrush::Move) {
        m_moveLayer = m_selectedLayer;
        m_moving = false;
        if (!m_moveStaged)
            m_moveDelta = glm::vec3(0.f);
    } else if (!m_rotationStaged) {
        m_rotateLayer.clear();
        m_rotateHandle = TrackballHandle::None;
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
    }
    if (mode != EditBrush::Move && !m_moveStaged) {
        m_moving = false;
        m_moveLayer.clear();
    }
    spdlog::info("edit mode -> {} ({})", brushName(mode),
                 m_editActive ? "armed" : "disarmed");
}

void App::drawPanelEdit()
{
    sectionHeader("EDIT TOOLBOX",
                  "Tab: View/Edit  |  A/D/S/C/M pick a mode  |  +/- size");

    if (m_rotationPreviewPending) {
        ImGui::SeparatorText("Pending");
        ImGui::TextColored(kWarn, "Applying staged transform...");
    }
    if (m_rotationStaged || m_moveStaged) {
        ImGui::TextColored(kWarn, "%s staged — not written yet",
                           m_rotationStaged ? "Rotation" : "Move");
        if (m_rotationStaged) {
            ImGui::Text("Yaw %+.1f  Pitch %+.1f  Roll %+.1f", m_rotateDy,
                        m_rotateDx, m_rotateDz);
            if (ImGui::Button("Apply rotation##applyRotation", ImVec2(-1.0f, 30.0f)))
                commitRotation();
            if (ImGui::Button("Cancel rotation##cancelRotation", ImVec2(-1.0f, 30.0f)))
                cancelRotation();
        } else {
            const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                              : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                              : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
            ImGui::Text("Delta %+.2f m along %s", amount, moveAxisName(m_moveAxis));
            if (ImGui::Button("Apply move##applyMove", ImVec2(-1.0f, 30.0f)))
                commitMove();
            if (ImGui::Button("Cancel move##cancelMove", ImVec2(-1.0f, 30.0f)))
                cancelMove();
        }
    }

    // Arm/disarm is a mode switch, not a window toggle: disarming keeps the
    // section (and its settings) visible so it can be re-armed in one press.
    if (ImGui::Checkbox("Brush armed##editArmed", &m_editActive))
        spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");

    // shared with the mode hotkeys - see chooseEditMode above
    auto chooseMode = [&](EditBrush mode) { chooseEditMode(mode); };

    ImGui::SeparatorText("Mode");
    if (ImGui::BeginTable("BrushModes", 2, ImGuiTableFlags_SizingStretchSame)) {
        const struct { const char* label; EditBrush mode; } modes[] = {
            { "Carve", EditBrush::Carve }, { "Add", EditBrush::Add },
            { "Delete", EditBrush::Delete }, { "Paint", EditBrush::Paint },
            { "Smooth", EditBrush::Smooth }, { "Rotate", EditBrush::Rotate },
            { "Move", EditBrush::Move }
        };
        for (int i = 0; i < int(sizeof(modes) / sizeof(modes[0])); ++i) {
            if (i % 2 == 0)
                ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(i % 2);
            if (actionButton(modes[i].label, m_editBrush == modes[i].mode,
                             ImVec2(-1.0f, 32.0f)))
                chooseMode(modes[i].mode);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Brush settings");
    if (m_editBrush != EditBrush::Rotate &&
        m_editBrush != EditBrush::Move) {
        // The brush lives on the 0.1 m lattice, so it is sized in voxels: the
        // minimum is one voxel, which is the per-voxel Add/Carve mode.
        int vox = brushVoxels();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragInt("Width##brushSize", &vox, 1, kBrushMinVox,
                           kBrushMaxVox, "%d vox")) {
            setBrushVoxels(vox);
            if (m_editDepth < vf::voxel::VOXEL)
                m_editDepth = vf::voxel::VOXEL;
        }
        ImGui::TextDisabled("%.2f m across%s", m_editDiameter,
                            brushIsPerVoxel() ? "  (per-voxel: one cell)" : "");
    }
    if (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Add) {
        ImGui::BeginDisabled(brushIsPerVoxel());
        if (ImGui::DragFloat("Depth / height##brushDepth", &m_editDepth, 0.1f,
                             0.1f, 12.0f, "%.1f m"))
            setBrushDepthVoxels(brushDepthVoxels());
        if (brushIsPerVoxel())
            ImGui::TextDisabled("Depth is ignored at 1 voxel.");
        ImGui::EndDisabled();
    }
    if (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Add ||
        m_editBrush == EditBrush::Delete || m_editBrush == EditBrush::Paint) {
        // Falloff is a NAMED CURVE, not a strength: it decides how influence
        // decays from the cursor to the rim of Width. Constant is the original
        // hard-edged footprint (and what makes a flat-bottomed dig); the rest
        // taper. The curve is plotted underneath, because choosing a falloff by
        // name alone is exactly what made the old scalar feel arbitrary.
        using C = vf::voxel::EditableWorld::FalloffCurve;
        const char* const names[] = { "Constant", "Sphere", "Root",
                                      "Smooth",   "Linear", "Sharp" };
        int cur = int(m_editFalloffCurve);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Falloff##editFalloff", &cur, names,
                         IM_ARRAYSIZE(names))) {
            m_editFalloffCurve = C(std::clamp(cur, 0, int(C::Count) - 1));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Influence from cursor (centre) to the rim of Width.\n"
                "Constant  flat - the whole footprint at full depth\n"
                "Sphere    round dome    Root  broad shoulder\n"
                "Smooth    default S     Linear cone\n"
                "Sharp     tight, for creases");

        // Profile plot: f(q) over the footprint. Cheap (ImGui draws it), and it
        // is the difference between picking a name and seeing the shape.
        {
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = 44.0f;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##falloffPlot", ImVec2(std::max(40.0f, w), h));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
            const ImU32 fg = ImGui::GetColorU32(ImGuiCol_PlotLines);
            const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg, 3.0f);
            // Axis guides: q = 0.5 and f = 0.5, so the half-radius value that
            // actually distinguishes the curves is readable.
            dl->AddLine(ImVec2(p0.x + w * 0.5f, p0.y),
                        ImVec2(p0.x + w * 0.5f, p0.y + h), dim, 1.0f);
            dl->AddLine(ImVec2(p0.x, p0.y + h * 0.5f),
                        ImVec2(p0.x + w, p0.y + h * 0.5f), dim, 1.0f);
            constexpr int kSteps = 64;
            ImVec2 prev(p0.x, p0.y + h);
            for (int i = 1; i <= kSteps; ++i) {
                const float q = float(i) / float(kSteps);
                const float f =
                    vf::voxel::EditableWorld::falloffCurveAt(m_editFalloffCurve, q);
                const ImVec2 pt(p0.x + w * q, p0.y + h * (1.0f - f));
                dl->AddLine(prev, pt, fg, 2.0f);
                prev = pt;
            }
        }

        // The visible mark is SMALLER than Width whenever the curve tapers,
        // because the curve reaches zero at the rim. Report the reach at which
        // a column still receives half a voxel, so the number on the Width
        // slider is not silently contradicted by what appears in the world.
        if (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Add) {
            const float r = m_editDiameter * 0.5f;
            // longest depth this stamp can place at the rim is m_editDepth, so
            // find the largest q whose column still clears half a voxel
            float q = 0.0f;
            for (int i = 100; i >= 0; --i) {
                const float t = float(i) / 100.0f;
                if (m_editDepth *
                        vf::voxel::EditableWorld::falloffCurveAt(
                            m_editFalloffCurve, t) >=
                    vf::voxel::VOXEL * 0.5f) {
                    q = t;
                    break;
                }
            }
            ImGui::TextDisabled("reach %.2f m (%.2f m footprint)",
                                r * q, r);
        }
    }
    if (m_editBrush == EditBrush::Smooth) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("Strength##smoothStrength", &m_smoothStrength, 0.01f,
                         0.0f, 1.0f, "%.2f");
        // The dead zone, stated rather than papered over. A stamp moves
        // lround(|deviation| * strength) cells, so a feature only moves at all
        // when strength >= 0.5 / |deviation|. Measured deviations: a 1-cell
        // step reads ~0.5, a 2-cell step ~1.0, a 3-cell step ~1.5. So the
        // default 0.65 CANNOT touch 1-cell noise no matter how many clicks -
        // that is inherent to integer cells with one stamp per click, and a
        // "floor" that turned a 0.1-cell intent into a whole cell would be a
        // worse lie than the dead zone.
        if (m_smoothStrength < 0.01f) {
            ImGui::TextDisabled("strength 0: no effect");
        } else {
            const float need = 0.5f / m_smoothStrength;
            ImGui::TextDisabled("moves features >= %.1f cells (%.2f m)", need,
                                need * vf::voxel::VOXEL);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "A stamp moves round(deviation x strength) cells, so small\n"
                    "features need more strength before they move AT ALL:\n"
                    "one 0.1 m step needs strength 1.0, a 0.2 m step 0.5.\n"
                    "Raise Strength (or repeat) to reach smaller detail.");
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Checkbox("Preserve volume##smoothPreserve",
                        &m_smoothPreserveVolume);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Softens each stamp so a held brush shrinks relief less.\n"
                "Off by default: it also makes the brush ~47%% weaker.");
    }
    if (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Paint) {
        int mat = int(m_editMat);
        ImGui::SetNextItemWidth(-60.0f);
        if (ImGui::BeginCombo("Material", kMatNames[std::min(mat, 20)])) {
            for (int i = 0; i < 21; ++i)
                if (ImGui::Selectable(kMatNames[i], mat == i))
                    m_editMat = uint8_t(i);
            ImGui::EndCombo();
        }
        const glm::vec3 c = vf::voxel::kPalette[std::min(int(m_editMat), 16)];
        ImGui::SameLine();
        ImGui::ColorButton("##materialSwatch", ImVec4(c.r, c.g, c.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(24.0f, 24.0f));
    }

    ImGui::SeparatorText("Action");
    if (m_editBrush == EditBrush::Rotate) {
        if (m_selectedLayer.empty()) {
            ImGui::TextColored(kWarn, "No object layer selected");
            ImGui::TextWrapped("Pick one in World layers, activate its trackball "
                               "there, or click an object in the scene.");
        } else {
            ImGui::Text("Target  %s", m_selectedLayer.c_str());
            const std::string hoverLayer = m_hoverHit.hit
                ? m_layers.layerFile(m_hoverHit.layer) : std::string();
            if (!hoverLayer.empty())
                ImGui::TextDisabled("Hover resolves to: %s", hoverLayer.c_str());
            ImGui::TextWrapped(
                "Click the object to activate it. Drag outer/local-Y, wide/local-X, "
                "or tall/local-Z ring; release stages it, then press Apply.");
            if (m_rotating)
                ImGui::Text("Yaw %+.1f  Pitch %+.1f  Roll %+.1f", m_rotateDy,
                            m_rotateDx, m_rotateDz);
        }
        if (m_renderMode != RenderMode::Splats)
            ImGui::TextDisabled("Live preview requires Gaussian surfels.");
    } else if (m_editBrush == EditBrush::Move) {
        if (m_moveLayer.empty()) {
            ImGui::TextColored(kWarn, "No object layer grabbed");
            ImGui::TextWrapped("Click an owned object, then use the colored X/Y/Z handles.");
        } else {
            ImGui::Text("Target  %s", m_moveLayer.c_str());
            ImGui::TextWrapped("Click the object to grab it, or click an X/Y/Z "
                               "handle. Drag to stage; press Apply to persist.");
            ImGui::SeparatorText("Move axis");
            ImGui::BeginDisabled(m_moveStaged || m_moving);
            if (actionButton("X##moveX", m_moveAxis == MoveAxis::X, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::X;
            ImGui::SameLine();
            if (actionButton("Y##moveY", m_moveAxis == MoveAxis::Y, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::Y;
            ImGui::SameLine();
            if (actionButton("Z##moveZ", m_moveAxis == MoveAxis::Z, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::Z;
            ImGui::EndDisabled();
            const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                          : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                          : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
            ImGui::Text("Delta %+.2f m%s", amount,
                        m_moveStaged ? "  (staged)" :
                        m_moving ? "  (dragging)" : "");
        }
        if (m_renderMode != RenderMode::Splats)
            ImGui::TextDisabled("Live move preview needs Gaussian surfels; "
                                "Apply still rebuilds the SVO.");
    } else {
        switch (m_editBrush) {
        case EditBrush::Carve:
            ImGui::TextWrapped("LMB drag: scoop along the surface normal.");
            break;
        case EditBrush::Add:
            ImGui::TextWrapped("LMB drag: grow the surface along the picked normal.");
            break;
        case EditBrush::Delete:
            ImGui::TextWrapped("LMB drag: clear every voxel in the ball.");
            break;
        case EditBrush::Paint:
            ImGui::TextWrapped("LMB drag: recolor solid cells in the ball.");
            break;
        case EditBrush::Smooth:
            ImGui::TextWrapped("LMB drag: relax the picked surface toward its "
                               "local average — terrain heights on ground, an "
                               "object's own surface on objects; terrain and "
                               "objects never convert into each other.");
            break;
        case EditBrush::Rotate:
        case EditBrush::Move:
            break;
        }
        ImGui::TextDisabled(m_editBrush == EditBrush::Smooth
                            ? "Blue preview shows the terrain footprint."
                            : "Preview tints exactly the affected surfels.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo (Ctrl+Z)", ImVec2(-1.0f, 30.0f)))
        undoEdit();
    ImGui::EndDisabled();
    ImGui::TextDisabled("%zu undo step%s", m_undo.size(),
                        m_undo.size() == 1 ? "" : "s");
    const double now = glfwGetTime();
    const bool confirmingClear = now < m_clearConfirmUntil;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          confirmingClear ? kDanger : ImVec4(0.24f, 0.08f, 0.11f, 1.0f));
    if (ImGui::Button(confirmingClear ? "Click again to clear all"
                                      : "Clear live edits",
                       ImVec2(-1.0f, 30.0f))) {
        if (confirmingClear) {
            clearLiveEdits();
            m_clearConfirmUntil = 0.0;
        } else {
            m_clearConfirmUntil = now + 3.0;
        }
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Last stamp: %.1f ms  |  %zu surfels", m_lastEditMs,
                        m_lastEditSurfels);
}

} // namespace app
} // namespace vf
