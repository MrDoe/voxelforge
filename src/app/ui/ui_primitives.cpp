#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vf {
namespace app {

float clampSidebarWidth(float width, float displayWidth)
{
    if (displayWidth <= 0.0f)
        return width;
    // Keep the rail plus a usable content pane on desktop-sized windows. On a
    // tiny window, fitting the display wins so the docked edge never leaves a
    // scene-coloured strip at the top or bottom.
    const float minW = std::min(displayWidth, kRailW + kPaneMin);
    const float maxW = std::max(minW, displayWidth - kSidebarGripW);
    return std::clamp(width, minW, maxW);
}

float sidebarWidthFor(float requestedWidth, float displayWidth)
{
    if (requestedWidth > 0.0f)
        return clampSidebarWidth(requestedWidth, displayWidth);
    const float paneW = std::max(
        kPaneMin, std::min(kPaneW, displayWidth * 0.30f));
    return clampSidebarWidth(kRailW + paneW, displayWidth);
}

bool overSidebarResizeGrip(const ImGuiIO& io, float sidebarWidth,
                           float displayHeight)
{
    if (sidebarWidth <= 0.0f || displayHeight <= 0.0f)
        return false;
    const float half = kSidebarGripW * 0.5f;
    return io.MousePos.x >= sidebarWidth - half &&
           io.MousePos.x <= sidebarWidth + half &&
           io.MousePos.y >= 0.0f && io.MousePos.y <= displayHeight;
}

std::string foldCase(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool actionButton(const char* label, bool active, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? kAccentSoft
                                 : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    if (active && ImGui::IsItemHovered())
        ImGui::SetTooltip("Active");
    return clicked;
}

// Section title at the top of the content pane, with an optional one-line
// hint underneath. Every section opens with this so the pane has a consistent
// header instead of a floating title bar.
void sectionHeader(const char* title, const char* hint)
{
    ImGui::TextColored(kAccent, "%s", title);
    if (hint)
        ImGui::TextDisabled("%s", hint);
    ImGui::Separator();
}

const char* brushName(EditBrush b)
{
    switch (b) {
    case EditBrush::Carve:  return "carve";
    case EditBrush::Add:    return "add";
    case EditBrush::Delete: return "delete";
    case EditBrush::Paint:  return "paint";
    case EditBrush::Smooth: return "smooth";
    case EditBrush::Rotate: return "rotate";
    case EditBrush::Move:   return "move";
    }
    return "carve";
}

EditBrush brushFromName(const char* name)
{
    if (!name)
        return EditBrush::Carve;
    if (strcmp(name, "add") == 0 || strcmp(name, "raise") == 0) return EditBrush::Add;
    if (strcmp(name, "delete") == 0 || strcmp(name, "clear") == 0) return EditBrush::Delete;
    if (strcmp(name, "paint") == 0) return EditBrush::Paint;
    if (strcmp(name, "smooth") == 0 || strcmp(name, "relax") == 0) return EditBrush::Smooth;
    if (strcmp(name, "rotate") == 0) return EditBrush::Rotate;
    if (strcmp(name, "move") == 0) return EditBrush::Move;
    return EditBrush::Carve;
}

} // namespace app
} // namespace vf
