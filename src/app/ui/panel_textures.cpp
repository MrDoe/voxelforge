// The sidebar's textures section: the per-material photo-texture picker.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cctype>
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
    refreshCardThumbnails();
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
        // One button opens the shared modal gallery; the current binding is
        // already visible in the row's badge. The modal holds ALL thumbnails,
        // so expanding a material no longer floods the narrow sidebar.
        char pickId[32];
        snprintf(pickId, sizeof(pickId), "Choose texture##tex.%d", material);
        if (ImGui::Button(pickId, ImVec2(-1.0f, 0.0f))) {
            m_texPickerOpen = true;
            m_texPickerKind = TexturePickerKind::Texture;
            m_texPickerMaterial = material;
            m_texPickerFilter[0] = '\0';
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open the texture gallery. Includes (palette) "
                              "to remove the binding.");
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
    // Foliage cards: bind a ground material to a card from assets/textures/cards/.
    ImGui::Separator();
    ImGui::Text("Foliage cards (grass/leaves on ground materials)");
    bool cardChanged = false;
    for (int material = 0; material < int(vf::voxel::kPaletteN); ++material) {
        if (material >= 10 && material <= 15)
            continue;
        auto bit = std::find_if(
            m_cardBindings.begin(), m_cardBindings.end(),
            [&](const vf::voxel::worldfile::FoliageCardBinding& b) { return b.mat == material; });
        const std::string currentCard = bit != m_cardBindings.end()
            ? bit->card : std::string();
        char id[32];
        snprintf(id, sizeof(id), "cardmat.%d", material);
        std::string badge = currentCard.empty() ? "(none)" : currentCard;
        if (!groupHeader(id, kMatNames[material], false, badge.c_str())) {
            continue;
        }
        // The card binding IS the material's "Has foliage" flag: checked means
        // this ground material grows the photographic plants wherever it is
        // painted. Toggling rebuilds the cards at once, like picking a tile.
        bool hasFoliage = !currentCard.empty();
        if (ImGui::Checkbox("Has foliage", &hasFoliage)) {
            const std::string card = hasFoliage
                ? (currentCard.empty()
                       ? (m_cardFiles.empty() ? std::string("grass")
                                              : m_cardFiles.front().first)
                       : currentCard)
                : std::string();
            setCardBinding(material, card);
            cardChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grow photographic foliage over this material. "
                              "The gallery includes (none) to remove it.");
        char cardPickId[32];
        snprintf(cardPickId, sizeof(cardPickId), "Choose card##card.%d", material);
        if (ImGui::Button(cardPickId, ImVec2(-1.0f, 0.0f))) {
            m_texPickerOpen = true;
            m_texPickerKind = TexturePickerKind::Card;
            m_texPickerMaterial = material;
            m_texPickerFilter[0] = '\0';
        }
        groupEnd();
    }
    // The modal may bind a texture or a card this frame; run it before the
    // pending-apply flags are latched so the change lands in the same frame.
    drawTexturePickerModal(textureChanged, cardChanged);
    if (cardChanged)
        m_cardApplyPending = true;
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

void App::drawTexturePickerModal(bool& textureChanged, bool& cardChanged)
{
    if (m_texPickerOpen) {
        m_texPickerOpen = false;
        ImGui::OpenPopup("Texture picker");
    }
    const bool isCard = m_texPickerKind == TexturePickerKind::Card;
    const int material = m_texPickerMaterial;
    std::string current;
    if (isCard) {
        auto it = std::find_if(m_cardBindings.begin(), m_cardBindings.end(),
                               [&](const vf::voxel::worldfile::FoliageCardBinding& b) {
                                   return b.mat == material;
                               });
        if (it != m_cardBindings.end())
            current = it->card;
    } else {
        auto it = std::find_if(m_texBindings.begin(), m_texBindings.end(),
                               [&](const vf::voxel::worldfile::TextureBinding& b) {
                                   return b.mat == material;
                               });
        if (it != m_texBindings.end())
            current = it->file;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Texture picker", nullptr,
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoCollapse))
        return;
    ImGui::Text("%s  ·  %s", isCard ? "Foliage card" : "Photo texture",
                kMatNames[std::clamp(material, 0, int(vf::voxel::kPaletteN) - 1)]);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##texPickerFilter", "Filter by name…",
                             m_texPickerFilter, sizeof(m_texPickerFilter));
    ImGui::Separator();

    struct Option {
        std::string label;
        std::string value;
        ImTextureID thumb = 0;
    };
    std::vector<Option> options;
    if (isCard) {
        options.push_back({ "(none)", std::string(), 0 });
        for (const auto& c : m_cardFiles) {
            ImTextureID tid = 0;
            for (const auto& t : m_cardThumbs)
                if (t.file == c.second) { tid = t.id; break; }
            options.push_back({ c.first, c.first, tid });
        }
    } else {
        options.push_back({ "(palette)", std::string(), 0 });
        for (const auto& file : m_texFiles) {
            ImTextureID tid = 0;
            for (const auto& t : m_texThumbs)
                if (t.file == file) { tid = t.id; break; }
            const size_t slash = file.find_last_of('/');
            options.push_back({ slash == std::string::npos ? file
                                                           : file.substr(slash + 1),
                                file, tid });
        }
    }

    std::string filter = m_texPickerFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const float tile = 72.0f;
    const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tile + 8.0f)));
    ImGui::BeginChild("##texPickerGrid", ImVec2(0.0f, -34.0f), true);
    int cell = 0;
    for (const auto& opt : options) {
        std::string label = opt.label;
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (!filter.empty() && label.find(filter) == std::string::npos)
            continue;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        char cellId[48];
        snprintf(cellId, sizeof(cellId), "picker.%d", cell);
        const bool selected = opt.value == current;
        if (ImGui::InvisibleButton(cellId, ImVec2(tile, tile))) {
            if (isCard) {
                setCardBinding(material, opt.value);
                cardChanged = true;
            } else {
                setTextureBinding(material, opt.value);
                textureChanged = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (opt.thumb) {
            dl->AddImage(opt.thumb, pos, ImVec2(pos.x + tile, pos.y + tile));
        } else {
            dl->AddRectFilled(pos, ImVec2(pos.x + tile, pos.y + tile),
                              IM_COL32(40, 44, 52, 255));
        }
        dl->AddRect(pos, ImVec2(pos.x + tile, pos.y + tile),
                    selected ? IM_COL32(50, 200, 230, 255) : IM_COL32(70, 80, 95, 255),
                    0.0f, 0, selected ? 2.0f : 1.0f);
        const ImVec2 textPos(pos.x + 2.0f, pos.y + tile - 14.0f);
        const ImVec2 textEnd(textPos.x + tile, textPos.y + 14.0f);
        dl->AddRectFilled(textPos, textEnd, IM_COL32(12, 14, 18, 220));
        dl->AddText(ImVec2(textPos.x + 2.0f, textPos.y + 1.0f),
                    IM_COL32(220, 228, 236, 255), opt.label.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", opt.label.c_str());
        if ((cell + 1) % cols != 0)
            ImGui::SameLine(0.0f, 8.0f);
        else
            ImGui::NewLine();
        ++cell;
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace app
} // namespace vf
