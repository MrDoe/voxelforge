// The sidebar's mesh section: STL/OBJ import: file, fit, source axes, material, solid/shell, anchor.
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

void App::drawPanelMesh()
{
    sectionHeader("MESH IMPORT", "voxelizes onto the 10 cm lattice");
    ImGui::TextWrapped(
        "Choose a model, place its bottom-center, and write a named .vxw layer. "
        "STL/OBJ files are read from the working directory, assets/, or assets/models/.");

    if (ImGui::Button("Rescan model files##meshRescan", ImVec2(-1.0f, 26.0f)))
        rescanMeshFiles();
    ImGui::TextDisabled("%zu model file%s found", m_meshFiles.size(),
                        m_meshFiles.size() == 1 ? "" : "s");

    const std::string currentPath(m_meshPath);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Mesh file##meshFile", currentPath.c_str())) {
        for (const std::string& file : m_meshFiles)
            if (ImGui::Selectable(file.c_str(), file == currentPath))
                std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", file.c_str());
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Path##meshPath", m_meshPath, sizeof(m_meshPath));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Layer name##meshLayer", m_meshLayerName, sizeof(m_meshLayerName));

    ImGui::SeparatorText("Placement");
    ImGui::TextDisabled("Anchor is a lattice cell; the solid AABB is centered on "
                        "X/Z and its lowest cell sits on Y.");
    if (ImGui::BeginTable("MeshAnchor", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("X##meshAnchorX", &m_meshAnchor.x);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Y##meshAnchorY", &m_meshAnchor.y);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Z##meshAnchorZ", &m_meshAnchor.z);
        ImGui::EndTable();
    }
    if (ImGui::Button("Use Ctrl+LMB voxel##meshUsePick", ImVec2(-1.0f, 28.0f))) {
        if (m_hasSelection) {
            m_meshAnchor = m_selectedHit.voxel;
            m_meshStatus = "Anchor set to the selected voxel";
            m_meshStatusError = false;
        } else {
            m_meshStatus = "Pick a voxel with Ctrl+LMB first";
            m_meshStatusError = true;
        }
    }
    ImGui::BeginDisabled(m_selectedLayer.empty() ||
                         m_selectedLayer == "landscape.vxw" ||
                         m_selectedLayer == vf::voxel::EditableWorld::kFileName);
    if (ImGui::Button("Use layer source pivot##meshUseLayer", ImVec2(-1.0f, 28.0f)))
        useMeshImportLayer(m_selectedLayer);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Transform");
    ImGui::BeginDisabled(!m_meshUseFit);
    ImGui::InputFloat("Fit longest side (m)##meshFit", &m_meshFitMeters, 0.1f,
                      0.1f, "%.2f");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(m_meshUseFit);
    ImGui::InputFloat("Scale (m per unit)##meshScale", &m_meshScale, 0.001f,
                      0.0001f, "%.4f");
    ImGui::EndDisabled();
    ImGui::InputFloat("Mesh-local yaw (deg)##meshRotY", &m_meshRotY, 1.0f, 0.1f,
                      "%.1f");
    const char* materialName = kMatNames[std::clamp(m_meshMaterial, 0, 20)];
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Material##meshMaterial", materialName)) {
        for (int mat = 0; mat < int(vf::voxel::kPaletteN); ++mat)
            if (ImGui::Selectable(kMatNames[std::min(mat, 20)], mat == m_meshMaterial))
                m_meshMaterial = mat;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Z-up source (swap Y/Z)##meshSwap", &m_meshSwapYz);
    ImGui::Checkbox("Reverse winding##meshFlip", &m_meshFlip);
    ImGui::Checkbox("Solid interior##meshSolid", &m_meshSolid);
    ImGui::TextDisabled("Solid fill is safest for VoxelField; shell mode is smaller "
                        "but can render hollow after loading.");

    const std::string targetFile = std::string(m_meshLayerName) + ".vxw";
    auto target = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& layer) {
            return layer.file == targetFile;
        });
    if (target != m_worldLayers.end()) {
        ImGui::SeparatorText("Existing layer");
        ImGui::TextDisabled("pos %.2f, %.2f, %.2f", target->pos[0], target->pos[1],
                            target->pos[2]);
        ImGui::TextDisabled("yaw %.1f  pitch %.1f  roll %.1f", target->rotDeg,
                            target->rotX, target->rotZ);
        ImGui::TextColored(kAccent, "Import replaces records, keeps this pose.");
    }

    if (!m_meshStatus.empty()) {
        ImGui::SeparatorText("Result");
        ImGui::TextWrapped("%s", m_meshStatus.c_str());
    }
    if (ImGui::Button("Import / replace layer##meshImportNow", ImVec2(-1.0f, 36.0f)))
        importMeshFromGui();
}

} // namespace app
} // namespace vf
