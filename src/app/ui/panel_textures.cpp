// The sidebar's textures section: the per-material photo-texture picker.
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

void App::drawPanelTextures()
{
    sectionHeader("Drop files into assets/textures/  ·  changes save to world.json");
    bool textureChanged = false;
    // The loop runs over the WHOLE palette: material 21 is `fleece`, the one
    // that ships with a fur coat, and a material the panel cannot show is a
    // material the user cannot configure.
    for (int material = 0; material < 22; ++material) {
        if (material >= 10 && material <= 15)
            continue;
        auto binding = std::find_if(
            m_texBindings.begin(), m_texBindings.end(),
            [&](const vf::voxel::worldfile::TextureBinding& b) {
                return b.mat == material;
            });
        const std::string current = binding != m_texBindings.end()
            ? binding->file : std::string();
        const bool missing = !current.empty() &&
            std::find(m_texFiles.begin(), m_texFiles.end(), current) ==
                m_texFiles.end();

        // One row per material, collapsed by default: the badge keeps the
        // current binding visible, so a closed row still says which material
        // is textured and with what. The badge is the FILE NAME, not its
        // path - every binding comes from the same folder, and the path
        // would eat the width the material name needs.
        char id[32];
        snprintf(id, sizeof(id), "tex.%d", material);
        std::string badge = current.empty() ? "(palette)"
                                            : current.substr(current.find_last_of('/') + 1);
        const char* badgeText = badge.c_str();
        if (!groupHeader(id, kMatNames[material], false, badgeText)) {
            if (missing)
                ImGui::TextColored(kWarn, "File is not in the scanned folder");
            continue;
        }
        ImGui::SetNextItemWidth(-1.0f);
        std::string label = current.empty() ? "(palette)" : current;
        if (missing)
            label += "  [missing]";
        if (ImGui::BeginCombo("##textureSource", label.c_str())) {
            if (ImGui::Selectable("(palette)", current.empty())) {
                setTextureBinding(material, std::string());
                textureChanged = true;
            }
            for (const std::string& file : m_texFiles)
                if (ImGui::Selectable(file.c_str(), file == current)) {
                    setTextureBinding(material, file);
                    textureChanged = true;
                }
            ImGui::EndCombo();
        }
        // A COAT is a property of the MATERIAL: a material that grows fur
        // gets a coat of hair cards wherever a micro object uses it. The
        // fleece (21) is the one that ships with one.
        bool fur = std::find(m_furMaterials.begin(), m_furMaterials.end(),
                             uint8_t(material)) != m_furMaterials.end();
        if (ImGui::Checkbox("Fur coat", &fur)) {
            if (fur)
                m_furMaterials.push_back(uint8_t(material));
            else
                m_furMaterials.erase(
                    std::remove(m_furMaterials.begin(), m_furMaterials.end(),
                                uint8_t(material)),
                    m_furMaterials.end());
            rebuildFurCards();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grow a hair-card coat on the micro objects "
                              "that use this material. Rebuilds the coat.");
        if (binding != m_texBindings.end()) {
            ImGui::SetNextItemWidth(-60.0f);
            if (ImGui::DragFloat("##scale", &binding->scale, 0.05f, 0.05f,
                                 20.0f, "%.2f m"))
                textureChanged = true;
            ImGui::SameLine();
            ImGui::TextDisabled("m / tile");
            // The material EMITS: its own texels glow (veins, lamp glass
            // instead of a flat colour) and cells of this material become
            // real point lights derived at upload time. Palette materials
            // 9-15 are emissive without a texture; this flag makes any
            // material emissive. Saved to world.json's textures entry.
            if (ImGui::Checkbox("Emissive", &binding->emissive))
                textureChanged = true;
            if (binding->emissive) {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("##emissiveScale", &binding->emissiveScale,
                                     0.05f, 0.1f, 8.0f, "%.2f x strength"))
                    textureChanged = true;
            }
        }
        if (missing)
            ImGui::TextColored(kWarn, "File is not in the scanned folder");
        groupEnd();
    }
    if (textureChanged)
        m_texApplyPending = true;
    ImGui::Separator();
    if (ImGui::Button("Rescan folder##rescanTextures", ImVec2(-1.0f, kUiRowH - 2.0f)))
        rescanTextureFiles();
    if (ImGui::Button("Reload manifest##reloadTextures", ImVec2(-1.0f, kUiRowH - 2.0f))) {
        m_texBindings.clear();
        vf::voxel::worldfile::loadTextureManifest(m_manifestPath, m_texBindings);
        rescanTextureFiles();
        m_texReloadPending = true;
    }
}

} // namespace app
} // namespace vf
