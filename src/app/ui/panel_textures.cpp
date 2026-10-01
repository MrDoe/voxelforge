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
    sectionHeader("MATERIAL TEXTURES", "Drop files into assets/textures/");
    ImGui::TextWrapped("Changes save to world.json and hot-reload.");
    bool textureChanged = false;
    for (int material = 0; material < 21; ++material) {
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

        ImGui::PushID(material);
        ImGui::SeparatorText(kMatNames[material]);
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
        if (binding != m_texBindings.end()) {
            ImGui::SetNextItemWidth(-60.0f);
            if (ImGui::DragFloat("##scale", &binding->scale, 0.05f, 0.05f,
                                 20.0f, "%.2f m"))
                textureChanged = true;
            ImGui::SameLine();
            ImGui::TextDisabled("m / tile");
        }
        if (missing)
            ImGui::TextColored(kWarn, "File is not in the scanned folder");
        ImGui::PopID();
    }
    if (textureChanged)
        m_texApplyPending = true;
    ImGui::Separator();
    if (ImGui::Button("Rescan folder##rescanTextures", ImVec2(-1.0f, 28.0f)))
        rescanTextureFiles();
    if (ImGui::Button("Reload manifest##reloadTextures", ImVec2(-1.0f, 28.0f))) {
        m_texBindings.clear();
        vf::voxel::worldfile::loadTextureManifest(m_manifestPath, m_texBindings);
        rescanTextureFiles();
        m_texReloadPending = true;
    }
}

} // namespace app
} // namespace vf
