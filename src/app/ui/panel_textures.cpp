// The sidebar's textures section: the per-material photo-texture picker.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::drawPanelTextures()
{
    sectionHeader("Drop files into assets/textures/  ·  changes save to world.json");
    refreshTextureThumbnails();
    bool textureChanged = false;
    // The loop runs over the WHOLE palette: material 21 is `fleece` (fur coat)
    // and 22 is `wet soil`. A material the panel cannot show is a material the
    // user cannot configure — so this bound tracks vf::voxel::kPaletteN.
    for (int material = 0; material < int(vf::voxel::kPaletteN); ++material) {
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
        // Texture picker: a gallery of thumbnails instead of a text dropdown.
        // First tile = palette (no texture); then one tile per file in
        // assets/textures/. Clicking a tile binds it to this material.
        const float tileSize = 44.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        int cols = std::max(1, int(avail / (tileSize + 6.0f)));
        int tileIdx = 0;
        auto afterTile = [&]() {
            ++tileIdx;
            if (tileIdx % cols != 0)
                ImGui::SameLine(0.0f, 6.0f);
        };
        auto tile = [&](const char* id, ImTextureID tex, const char* tooltip,
                        bool selected, const std::function<void()>& onClick) {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(id, ImVec2(tileSize, tileSize)))
                onClick();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tex) {
                dl->AddImage(tex, pos, ImVec2(pos.x + tileSize, pos.y + tileSize));
            } else {
                dl->AddRectFilled(pos, ImVec2(pos.x + tileSize, pos.y + tileSize),
                                  IM_COL32(40, 44, 52, 255));
                dl->AddText(ImVec2(pos.x + 6, pos.y + tileSize * 0.5f - 6),
                            IM_COL32(180, 180, 180, 255), "none");
            }
            ImU32 border = selected ? IM_COL32(50, 200, 230, 255)
                                    : IM_COL32(70, 80, 95, 255);
            dl->AddRect(pos, ImVec2(pos.x + tileSize, pos.y + tileSize), border,
                        0.0f, 0, selected ? 2.0f : 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            afterTile();
        };
        // palette tile
        {
            char pid[32];
            snprintf(pid, sizeof(pid), "pal.%d", material);
            tile(pid, 0, "(palette)", current.empty(), [&]() {
                setTextureBinding(material, std::string());
                textureChanged = true;
            });
        }
        // file tiles
        for (size_t fi = 0; fi < m_texFiles.size(); ++fi) {
            const std::string& file = m_texFiles[fi];
            ImTextureID tid = 0;
            for (const auto& t : m_texThumbs)
                if (t.file == file) { tid = t.id; break; }
            char fid[64];
            snprintf(fid, sizeof(fid), "texfile.%d.%zu", material, fi);
            std::string tip = file.substr(file.find_last_of('/') + 1);
            tile(fid, tid, tip.c_str(), file == current, [&]() {
                setTextureBinding(material, file);
                textureChanged = true;
            });
        }
        ImGui::NewLine();
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
