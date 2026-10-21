#pragma once

// The sidebar's shared widgets and its layout maths. The metric constants
// live in ui_types.hpp (the frame loop needs them too); only the functions
// are declared here.
//
// The widgets below are the sidebar's vocabulary: every section builds its
// layout out of them, so a change to one of them moves all six sections at
// once. They are deliberately plain ImGui - no custom draw lists beyond the
// few that need a shape ImGui cannot draw (the rail icons, the group chevron,
// the status chip).

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

// The one-line hint under the pane's header band. The band's title comes
// from the rail, so the section body opens with a sentence, not a second
// heading.

void sectionHeader(const char* hint);

// The pane's header band: the section's name, an accent tab and the
// keyboard shortcut that jumps to it. It is the answer to "where am I?"
// after a Ctrl+1..6 jump.

void paneBand(const char* title, const char* shortcut);

// A collapsible group. The id is the persistent key (the state survives a
// section switch); the title is drawn in the SECONDARY text level, so the
// group's contents stay the loudest thing in it. An optional badge is
// drawn right-aligned in the header row (the current value of whatever the
// group edits), so a closed group still says what it holds. Returns true
// when open.

bool groupHeader(const char* id, const char* title, bool defaultOpen = true,
                 const char* badge = nullptr);

// Closes a group: a little air, and a hairline that separates the group
// from the next one.

void groupEnd();

// A label on the left, a value on the right. The readouts become DATA
// instead of prose, and the value is the part the eye is looking for.

void keyValue(const char* label, const char* fmt, ...);

void keyValueColored(const char* label, const ImVec4& color, const char* fmt, ...);

// A muted note, wrapped. Used for the one line that says what a control
// actually does.

void hintLine(const char* fmt, ...);

// A labelled control row: the label on the LEFT, the control filling the
// rest. ImGui draws a widget's label to the widget's RIGHT, so a full-width
// widget with a visible label pushes that label off the pane - "Depth /
// height" used to be cut in half at the pane's edge.
//
// The control is a callable, so any widget can be used:
//
//   controlRow("Depth / height", [&] {
//       ImGui::SetNextItemWidth(-1.0f);
//       ImGui::DragFloat("##depth", &depth, 0.1f);
//   });

template <typename Fn>
void controlRow(const char* label, Fn&& control)
{
    ImGui::PushID(label);
    if (ImGui::BeginTable("##controlRow", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch, 0.60f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSecondary);
        // WRAPPED: a label longer than the label column must wrap, not be
        // clipped by the cell (TextUnformatted would run into the control).
        ImGui::TextWrapped("%s", label);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        control();
        ImGui::EndTable();
    }
    ImGui::PopID();
}

// A small filled chip: a status word (LIVE, OFF, STAGED) that carries state
// without shouting.

void statusPill(const char* text, const ImVec4& color);

// The rail's vector icons. The default ImGui font has no icon glyphs, so
// each section draws its own - sharp at any size and themeable.

void drawPanelIcon(Panel panel, const ImVec2& center, float size, ImU32 color);

// The brush mode names, shared by the HUD and the headless test hooks
// ("carve" | "add" | "delete" | "paint" | "smooth" | "rotate" | "move";
// anything else parses as carve).

const char* brushName(EditBrush b);

EditBrush brushFromName(const char* name);

} // namespace app
} // namespace vf
