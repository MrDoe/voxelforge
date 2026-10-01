#pragma once

// The sidebar's shared widgets and its layout maths. The metric constants
// live in ui_types.hpp (the frame loop needs them too); only the functions
// are declared here.

#include "app/ui/ui_types.hpp"

#include <imgui.h>

#include <string>

namespace vf {
namespace app {

float clampSidebarWidth(float width, float displayWidth);

float sidebarWidthFor(float requestedWidth, float displayWidth);

bool overSidebarResizeGrip(const ImGuiIO& io, float sidebarWidth,
                           float displayHeight);

// Lowercase a string - the sidebar's case-insensitive layer/mesh filters.

std::string foldCase(std::string s);

// A toggle button that reads as "currently on" instead of a momentary action.

bool actionButton(const char* label, bool active, const ImVec2& size);

// Section title at the top of the content pane, with an optional one-line
// hint underneath. Every section opens with this so the pane has a consistent
// header instead of a floating title bar.

void sectionHeader(const char* title, const char* hint = nullptr);

// The brush mode names, shared by the HUD and the headless test hooks
// ("carve" | "add" | "delete" | "paint" | "smooth" | "rotate" | "move";
// anything else parses as carve).

const char* brushName(EditBrush b);

EditBrush brushFromName(const char* name);

} // namespace app
} // namespace vf
