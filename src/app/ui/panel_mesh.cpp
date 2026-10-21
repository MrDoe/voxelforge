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
    sectionHeader("Voxelizes onto the 10 cm lattice");
    hintLine("Choose a model, place its bottom-center, and write a named .vxw "
             "layer. STL/OBJ files are read from the working directory, "
             "assets/, or assets/models/.");

    if (groupHeader("mesh.source", "SOURCE")) {
        if (ImGui::Button("Rescan model files##meshRescan", ImVec2(-1.0f, kUiRowH - 2.0f)))
            rescanMeshFiles();
        ImGui::TextDisabled("%zu model file%s found", m_meshFiles.size(),
                            m_meshFiles.size() == 1 ? "" : "s");

        const std::string currentPath(m_meshPath);
        controlRow("Mesh file", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##meshFile", currentPath.c_str())) {
                for (const std::string& file : m_meshFiles)
                    if (ImGui::Selectable(file.c_str(), file == currentPath))
                        std::snprintf(m_meshPath, sizeof(m_meshPath), "%s",
                                       file.c_str());
                ImGui::EndCombo();
            }
        });
        controlRow("Path", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##meshPath", m_meshPath, sizeof(m_meshPath));
        });
        controlRow("Layer name", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##meshLayer", m_meshLayerName, sizeof(m_meshLayerName));
        });
        groupEnd();
    }

    if (groupHeader("mesh.placement", "PLACEMENT")) {
        hintLine("Anchor is a lattice cell; the solid AABB is centered on X/Z "
                 "and its lowest cell sits on Y.");
        if (ImGui::BeginTable("MeshAnchor", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("X");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##meshAnchorX", &m_meshAnchor.x);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Y");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##meshAnchorY", &m_meshAnchor.y);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Z");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##meshAnchorZ", &m_meshAnchor.z);
            ImGui::EndTable();
        }
        if (ImGui::Button("Use Ctrl+LMB voxel##meshUsePick", ImVec2(-1.0f, kUiRowH))) {
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
        if (ImGui::Button("Use layer source pivot##meshUseLayer", ImVec2(-1.0f, kUiRowH)))
            useMeshImportLayer(m_selectedLayer);
        ImGui::EndDisabled();
        groupEnd();
    }

    if (groupHeader("mesh.transform", "TRANSFORM")) {
        ImGui::BeginDisabled(!m_meshUseFit);
        controlRow("Fit longest (m)", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat("##meshFit", &m_meshFitMeters, 0.1f, 0.1f, "%.2f");
        });
        ImGui::EndDisabled();
        ImGui::BeginDisabled(m_meshUseFit);
        controlRow("Scale (m/unit)", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat("##meshScale", &m_meshScale, 0.001f, 0.0001f,
                              "%.4f");
        });
        ImGui::EndDisabled();
        controlRow("Yaw (deg)", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputFloat("##meshRotY", &m_meshRotY, 1.0f, 0.1f, "%.1f");
        });
        const char* materialName = kMatNames[std::clamp(m_meshMaterial, 0, 21)];
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("Material##meshMaterial", materialName)) {
            for (int mat = 0; mat < int(vf::voxel::kPaletteN); ++mat)
                if (ImGui::Selectable(kMatNames[std::min(mat, 21)], mat == m_meshMaterial))
                    m_meshMaterial = mat;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Z-up source (swap Y/Z)##meshSwap", &m_meshSwapYz);
        ImGui::Checkbox("Reverse winding##meshFlip", &m_meshFlip);
        ImGui::Checkbox("Solid interior##meshSolid", &m_meshSolid);
        hintLine("Solid fill is safest for VoxelField; shell mode is smaller but "
                 "can render hollow after loading.");
        groupEnd();
    }

    const std::string targetFile = std::string(m_meshLayerName) + ".vxw";
    auto target = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& layer) {
            return layer.file == targetFile;
        });
    if (target != m_worldLayers.end()) {
        if (groupHeader("mesh.existing", "EXISTING LAYER")) {
            keyValue("Position", "%.2f, %.2f, %.2f", target->pos[0], target->pos[1],
                     target->pos[2]);
            keyValue("Rotation", "yaw %.1f · pitch %.1f · roll %.1f", target->rotDeg,
                     target->rotX, target->rotZ);
            ImGui::TextColored(kAccent, "Import replaces records, keeps this pose.");
            groupEnd();
        }
    }

    if (!m_meshStatus.empty()) {
        if (groupHeader("mesh.result", "RESULT")) {
            ImGui::TextWrapped("%s", m_meshStatus.c_str());
            groupEnd();
        }
    }
    if (ImGui::Button("Import / replace layer##meshImportNow", ImVec2(-1.0f, kUiRowH + 6.0f)))
        importMeshFromGui();
}

} // namespace app
} // namespace vf
