// The sidebar's world section: the layer list, its enable switches, and the per-layer placement fields.
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

void App::drawPanelWorld()
{
    sectionHeader("WORLD LAYERS", "Picking resolves cell provenance, not AABBs");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##layerFilter", "Filter .vxw files", m_layerFilter,
                             sizeof(m_layerFilter));
    const std::string filter = foldCase(m_layerFilter);
    size_t enabledCount = 0;
    for (const auto& l : m_worldLayers)
        enabledCount += l.enabled ? 1 : 0;
    ImGui::TextDisabled("%zu files  |  %zu enabled  |  %zu records live",
                        m_worldLayers.size(), enabledCount, m_layers.stats().records);

    // Bounded list height: the inspector below must stay reachable without a
    // long scroll even in a short window.
    const float listH = std::max(110.0f, ImGui::GetContentRegionAvail().y * 0.30f);
    if (ImGui::BeginTable("LayerInventory", 2,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, listH))) {
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableHeadersRow();
        for (auto& l : m_worldLayers) {
            if (!filter.empty() && foldCase(l.file).find(filter) == std::string::npos)
                continue;
            ImGui::PushID(l.file.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // One row = the enable checkbox plus the selection highlight, so
            // the inventory stays two columns wide in a 300 px pane.
            bool enabled = l.enabled;
            if (l.role == "landscape")
                ImGui::BeginDisabled(true);
            if (ImGui::Checkbox("##enabled", &enabled)) {
                l.enabled = enabled;
                l.listed = true;
                persistWorldLayers();
                m_pendingWorldReload = true;
            }
            if (l.role == "landscape")
                ImGui::EndDisabled();
            ImGui::SameLine();
            const bool selected = m_selectedLayer == l.file;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, kAccentSoft);
            // The label doubles as the row select target: a leading bullet
            // marks the enabled layers without spending a column on it.
            const std::string label = l.enabled ? "* " + l.file : "  " + l.file;
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedLayer = l.file;
            }
            if (selected)
                ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            if (l.enabled)
                ImGui::TextColored(kAccent, "LIVE");
            else
                ImGui::TextDisabled("OFF");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("* = merged into the world");

    auto selected = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) {
            return l.file == m_selectedLayer;
        });
    if (selected == m_worldLayers.end()) {
        if (!m_selectedLayer.empty())
            m_selectedLayer.clear();
        ImGui::SeparatorText("Inspector");
        ImGui::TextWrapped("Select a layer to inspect, place, import, or rotate it.");
    } else {
        ImGui::SeparatorText("Inspector");
        ImGui::TextColored(kAccent, "%s", selected->file.c_str());
        const uint8_t ownerId = m_layers.layerId(selected->file);
        ImGui::TextDisabled("role: %s  |  owner id: %u", selected->role.c_str(),
                            unsigned(ownerId));
        glm::vec3 pivot;
        if (m_layers.layerPivot(selected->file, pivot))
            ImGui::TextDisabled("pivot: %.2f, %.2f, %.2f", pivot.x, pivot.y, pivot.z);

        const bool canImport = m_hasSelection && selected->role != "landscape";
        ImGui::BeginDisabled(!canImport);
        if (ImGui::Button("Import copy at anchor##import", ImVec2(-1.0f, 30.0f))) {
            const std::string path = std::string(VOXELFORGE_ASSET_DIR) + "/" +
                                     selected->file;
            if (m_editable.importLayer(path, m_selectedHit.voxel) > 0)
                requestWorldReload();
        }
        ImGui::EndDisabled();
        if (!m_hasSelection)
            ImGui::TextDisabled("Import needs a Ctrl+LMB anchor.");

        const bool rotatable = ownerId != 0 && selected->enabled &&
                               selected->file != vf::voxel::EditableWorld::kFileName;
        const bool canActivate = !m_rotationStaged ||
                                  m_rotateLayer == selected->file;
        ImGui::BeginDisabled(!rotatable || !canActivate || m_rotationPreviewPending);
        if (ImGui::Button("Activate trackball##rotate", ImVec2(-1.0f, 30.0f))) {
            m_editActive = true;
            m_editBrush = EditBrush::Rotate;
            m_panel = Panel::Edit;
            m_rotateLayer = selected->file;
            m_rotating = false;
            m_rotateHandle = TrackballHandle::None;
            if (!m_rotationStaged)
                m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
            m_taaFirstFrame = true;
            m_renderMode = RenderMode::Splats; // live preview backend
        }
        ImGui::EndDisabled();
        if (selected->file == vf::voxel::EditableWorld::kFileName)
            ImGui::TextWrapped("AI edits are world-space, not one rotatable object.");

        // Placement: 3 columns, one row per axis group. The old 2-column
        // label/field table needed ~250 px of content width; three compact
        // drag fields per row fit the pane and are read at a glance.
        glm::vec3 posDraft(selected->pos[0], selected->pos[1], selected->pos[2]);
        float angleDraft[3] = { selected->rotDeg, selected->rotX, selected->rotZ };
        bool transformChanged = false;
        static const char* kPosIds[3] = { "##posX", "##posY", "##posZ" };
        static const char* kAngIds[3] = { "##yaw", "##pitch", "##roll" };
        ImGui::TextDisabled("Position (m)");
        if (ImGui::BeginTable("TransformPos", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (int c = 0; c < 3; ++c) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(c);
                ImGui::TextDisabled("%c", "XYZ"[c]);
                ImGui::SetNextItemWidth(-1.0f);
                transformChanged |= ImGui::DragFloat(kPosIds[c], &posDraft[c], 0.1f,
                                                    -100.0f, 100.0f, "%.1f");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Rotation (deg)");
        if (ImGui::BeginTable("TransformRot", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (int c = 0; c < 3; ++c) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(c);
                ImGui::TextDisabled("%s", c == 0 ? "Yaw" : c == 1 ? "Pitch" : "Roll");
                ImGui::SetNextItemWidth(-1.0f);
                transformChanged |= ImGui::DragFloat(kAngIds[c], &angleDraft[c], 1.0f,
                                                    -360.0f, 360.0f, "%.0f");
            }
            ImGui::EndTable();
        }
        if (transformChanged)
            ImGui::TextColored(kWarn, "Unsaved transform");
        if (ImGui::Button("Apply transform##apply", ImVec2(-1.0f, 30.0f))) {
            selected->pos[0] = posDraft.x;
            selected->pos[1] = posDraft.y;
            selected->pos[2] = posDraft.z;
            selected->rotDeg = angleDraft[0];
            selected->rotX = angleDraft[1];
            selected->rotZ = angleDraft[2];
            selected->listed = true;
            persistWorldLayers();
            m_pendingWorldReload = true;
            m_taaFirstFrame = true;
        }
        ImGui::BeginDisabled(!transformChanged);
        if (ImGui::Button("Reset transform##resetTransform", ImVec2(-1.0f, 30.0f))) {
            selected->pos[0] = selected->pos[1] = selected->pos[2] = 0.0f;
            selected->rotDeg = selected->rotX = selected->rotZ = 0.0f;
            selected->listed = true;
            persistWorldLayers();
            m_pendingWorldReload = true;
            m_taaFirstFrame = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Written to world.json on Apply.");
    }

    ImGui::Separator();
    if (ImGui::Button("Rescan assets##rescan", ImVec2(-1.0f, 28.0f)))
        rescanWorldLayers();
}

} // namespace app
} // namespace vf
