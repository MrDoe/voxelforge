#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

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
    // An active button is a filled accent chip, an inactive one is a sunken
    // control: the state is legible without reading the label, which is what
    // a mode grid (seven brushes, two backends, three axes) needs.
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? kAccentSoft
                                 : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          active ? kAccent : kTextPrimary);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    if (active && ImGui::IsItemHovered())
        ImGui::SetTooltip("Active");
    return clicked;
}

// The one-line hint under the pane's header band. The band's title comes from
// the rail, so the section body opens with a sentence rather than a second
// heading that would compete with it.
void sectionHeader(const char* hint)
{
    if (hint && *hint) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSecondary);
        ImGui::TextWrapped("%s", hint);
        ImGui::PopStyleColor();
    }
}

// The pane's header band. A title bar makes the section unmistakable after
// a keyboard jump, and the shortcut chip on the right tells the user how it
// got there - the rail is the same information, but only once the eye has
// travelled back to the left edge.
void paneBand(const char* title, const char* shortcut)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = kUiBandH;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 band = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    band.w = 0.70f;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(band),
                      kUiRadius);
    // the accent tab: a 3 px bar, the one place the accent says "here"
    dl->AddRectFilled(ImVec2(pos.x + 2.0f, pos.y + 9.0f),
                      ImVec2(pos.x + 5.0f, pos.y + h - 9.0f),
                      ImGui::GetColorU32(kAccent), 2.0f);

    // the shortcut chip, right-aligned
    if (shortcut && *shortcut) {
        const ImVec2 ts = ImGui::CalcTextSize(shortcut);
        const float cw = ts.x + 12.0f, ch = ts.y + 5.0f;
        const ImVec2 c0(pos.x + w - cw - 8.0f, pos.y + (h - ch) * 0.5f);
        const ImVec2 c1(c0.x + cw, c0.y + ch);
        ImVec4 fill = kAccent;
        fill.w = 0.14f;
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(fill), 5.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(kAccent), 5.0f, 0, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(c0.x + 6.0f, c0.y + 2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(shortcut);
        ImGui::PopStyleColor();
    }

    // the title
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 14.0f,
                                     pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    // consume the band
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
}

// Elide a string to a pixel width, with a trailing "...". The default ImGui
// font is ASCII-only, so the ellipsis is three dots, not U+2026.
static std::string elideToWidth(const char* text, float maxWidth)
{
    const std::string s = text ? text : "";
    if (maxWidth <= 0.0f)
        return std::string();
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth)
        return s;
    const float budget = maxWidth - 12.0f;   // room for the "..."
    int lo = 0, hi = int(s.size());
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (ImGui::CalcTextSize(s.substr(0, mid).c_str()).x <= budget)
            lo = mid;
        else
            hi = mid - 1;
    }
    return s.substr(0, lo) + "...";
}

// A collapsible group. The id is the persistent key (the state survives a
// section switch), the title is drawn in the SECONDARY text level so the
// group's contents stay the loudest thing inside it, and the row itself is a
// band with a chevron - the three cues that turn a flat list of controls
// into something you can navigate.
bool groupHeader(const char* id, const char* title, bool defaultOpen,
                 const char* badge)
{
    static std::unordered_map<std::string, bool> open;
    auto it = open.find(id);
    if (it == open.end())
        it = open.emplace(id, defaultOpen).first;
    bool isOpen = it->second;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = kUiGroupH;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    if (ImGui::IsItemClicked())
        isOpen = !isOpen;
    it->second = isOpen;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + w, pos.y + h),
                      ImGui::GetColorU32(hovered
                          ? ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered)
                          : ImGui::GetStyleColorVec4(ImGuiCol_Header)),
                      kUiRadius);

    const ImVec2 c(pos.x + 14.0f, pos.y + h * 0.5f);
    ImVec2 tri[3];
    if (isOpen) {
        tri[0] = ImVec2(c.x - 4.0f, c.y - 2.5f);
        tri[1] = ImVec2(c.x + 4.0f, c.y - 2.5f);
        tri[2] = ImVec2(c.x, c.y + 3.0f);
    } else {
        tri[0] = ImVec2(c.x - 2.5f, c.y - 4.0f);
        tri[1] = ImVec2(c.x - 2.5f, c.y + 4.0f);
        tri[2] = ImVec2(c.x + 3.0f, c.y);
    }
    dl->AddConvexPolyFilled(tri, 3, ImGui::GetColorU32(kTextSecondary));

    ImGui::SetCursorScreenPos(ImVec2(pos.x + 26.0f,
                                     pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSecondary);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    // The badge: the group's current value, right-aligned. A closed group
    // then still says what it holds, which is what makes collapsing a group
    // a way of tidying the panel rather than a way of hiding information.
    // It is ELIDED to whatever the title leaves over - a long file path must
    // not run back into the name it belongs to.
    if (badge && *badge) {
        const float titleW = ImGui::CalcTextSize(title).x;
        // 22 px of slack: the badge is right-aligned, the title is not, and
        // the two must not touch.
        const float badgeMax = w - (26.0f + titleW + 22.0f);
        const std::string shown = elideToWidth(badge, badgeMax);
        if (!shown.empty()) {
            const ImVec2 bs = ImGui::CalcTextSize(shown.c_str());
            ImGui::SetCursorScreenPos(ImVec2(pos.x + w - bs.x - 10.0f,
                                             pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, kTextTertiary);
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopStyleColor();
        }
    }
    // Continue BELOW the band: the group's content belongs under it.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    return isOpen;
}

void groupEnd()
{
    ImGui::Spacing();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
                ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

static void formatValue(char* buf, size_t n, const char* fmt, va_list ap)
{
    vsnprintf(buf, n, fmt, ap);
}

// A label on the left, a value on the right: the readouts become DATA
// instead of prose, and the value lands where the eye already looks for a
// number (the right edge, like every other editor).
void keyValueColored(const char* label, const ImVec4& color, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    formatValue(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const float w = ImGui::GetContentRegionAvail().x;
    const float vw = ImGui::CalcTextSize(buf).x;
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSecondary);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const float lw = ImGui::CalcTextSize(label).x;
    ImGui::SameLine(0.0f, 0.0f);
    // Never let the value run back over the label in a narrow pane.
    const float x = std::max(lw + 10.0f, w - vw);
    ImGui::SetCursorPosX(x);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void keyValue(const char* label, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    formatValue(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    keyValueColored(label, kTextPrimary, "%s", buf);
}

void hintLine(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    formatValue(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextTertiary);
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
}

void statusPill(const char* text, const ImVec4& color)
{
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float lh = ImGui::GetTextLineHeight();
    const float padX = 7.0f, padY = 3.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + ts.x + padX * 2.0f, pos.y + lh + padY * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 fill = color;
    fill.w = 0.16f;
    dl->AddRectFilled(pos, end, ImGui::GetColorU32(fill), 5.0f);
    dl->AddRect(pos, end, ImGui::GetColorU32(color), 5.0f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + padX, pos.y + padY));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y + 1.0f));
}

// ---- rail icons ----------------------------------------------------------
// Vector glyphs, drawn from primitives: the default ImGui font has no icon
// codepoints, and a 2-letter abbreviation ("ED", "WL") reads as a code
// rather than a destination. These are drawn at whatever size the rail
// asks for, so they stay sharp.

static void iconPencil(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // A chisel pointing up-right: a triangular tip, a body quad and a
    // ferrule band separated by a gap, so the three parts read as one tool
    // without needing a background-coloured cut-out.
    const float w = 0.24f * s;
    const ImVec2 dir(0.7071f, -0.7071f);
    const ImVec2 perp(-0.7071f, -0.7071f);
    const ImVec2 base(c.x + 0.08f * s, c.y - 0.08f * s);
    const ImVec2 tip(c.x + 0.38f * s, c.y - 0.38f * s);
    const ImVec2 p0(base.x + perp.x * w * 0.5f, base.y + perp.y * w * 0.5f);
    const ImVec2 p1(base.x - perp.x * w * 0.5f, base.y - perp.y * w * 0.5f);
    const ImVec2 tri[3] = { p0, p1, tip };
    dl->AddConvexPolyFilled(tri, 3, col);

    // body
    const ImVec2 bEnd(base.x - dir.x * 0.26f * s, base.y - dir.y * 0.26f * s);
    const ImVec2 q0(p0.x - dir.x * 0.26f * s, p0.y - dir.y * 0.26f * s);
    const ImVec2 q1(p1.x - dir.x * 0.26f * s, p1.y - dir.y * 0.26f * s);
    const ImVec2 body[4] = { p0, p1, q1, q0 };
    dl->AddConvexPolyFilled(body, 4, col);

    // ferrule, one step further out with a gap
    const ImVec2 f0(q0.x - dir.x * 0.05f * s, q0.y - dir.y * 0.05f * s);
    const ImVec2 f1(q1.x - dir.x * 0.05f * s, q1.y - dir.y * 0.05f * s);
    const ImVec2 f2(bEnd.x - dir.x * 0.16f * s, bEnd.y - dir.y * 0.16f * s);
    const ImVec2 f3(f0.x - dir.x * 0.16f * s, f0.y - dir.y * 0.16f * s);
    const ImVec2 ferrule[4] = { f0, f1, f2, f3 };
    dl->AddConvexPolyFilled(ferrule, 4, col);
}

static void iconLayers(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // Three stacked plates: the top one filled, the two below it outlined, so
    // the stack reads as depth rather than as a grid.
    for (int i = 0; i < 3; ++i) {
        const float y = c.y + (0.20f * float(i) - 0.20f) * s;
        const float w = 0.36f * s, h = 0.16f * s;
        const ImVec2 p[4] = { ImVec2(c.x, y - h), ImVec2(c.x + w, y),
                              ImVec2(c.x, y + h), ImVec2(c.x - w, y) };
        if (i == 0) {
            dl->AddConvexPolyFilled(p, 4, col);
        } else {
            dl->AddPolyline(p, 4, col, 0, 1.6f);
        }
    }
}

static void iconSun(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    dl->AddCircleFilled(c, 0.17f * s, col);
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.7853981634f;
        const float ca = std::cos(a), sa = std::sin(a);
        dl->AddLine(ImVec2(c.x + ca * 0.26f * s, c.y + sa * 0.26f * s),
                    ImVec2(c.x + ca * 0.40f * s, c.y + sa * 0.40f * s), col, 1.6f);
    }
}

static void iconSwatch(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    // A 2x2 checker: the classic "material" swatch. Two cells are filled and
    // two are outlined, so the glyph reads as a swatch at 20 px.
    const float q = 0.19f * s, gap = 0.035f * s;
    const float x0 = c.x - q - gap * 0.5f, x1 = c.x + gap * 0.5f;
    const float y0 = c.y - q - gap * 0.5f, y1 = c.y + gap * 0.5f;
    const ImVec2 cells[4][2] = {
        { ImVec2(x0, y0), ImVec2(x0 + q, y0 + q) },
        { ImVec2(x1, y0), ImVec2(x1 + q, y0 + q) },
        { ImVec2(x0, y1), ImVec2(x0 + q, y1 + q) },
        { ImVec2(x1, y1), ImVec2(x1 + q, y1 + q) },
    };
    for (int i = 0; i < 4; ++i) {
        if (i == 0 || i == 3)
            dl->AddRectFilled(cells[i][0], cells[i][1], col, 2.0f);
        else
            dl->AddRect(cells[i][0], cells[i][1], col, 2.0f, 0, 1.5f);
    }
}

static void iconCube(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float w = 0.34f * s, h = 0.38f * s, q = 0.19f * s;
    const ImVec2 top(c.x, c.y - h);
    const ImVec2 ur(c.x + w, c.y - q);
    const ImVec2 lr(c.x + w, c.y + q);
    const ImVec2 bot(c.x, c.y + h);
    const ImVec2 ll(c.x - w, c.y + q);
    const ImVec2 ul(c.x - w, c.y - q);
    const ImVec2 hex[6] = { top, ur, lr, bot, ll, ul };
    dl->AddPolyline(hex, 6, col, 0, 1.6f);
    dl->AddLine(top, c, col, 1.4f);
    dl->AddLine(lr, c, col, 1.4f);
    dl->AddLine(ll, c, col, 1.4f);
}

static void iconBubble(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float w = 0.34f * s, h = 0.26f * s;
    const ImVec2 p0(c.x - w, c.y - h);
    const ImVec2 p1(c.x + w, c.y + h * 0.35f);
    dl->AddRectFilled(p0, p1, col, 0.09f * s);
    // the tail
    const ImVec2 tri[3] = { ImVec2(c.x - w * 0.55f, c.y + h * 0.35f),
                            ImVec2(c.x - w * 0.10f, c.y + h * 0.35f),
                            ImVec2(c.x - w * 0.62f, c.y + h * 0.95f) };
    dl->AddConvexPolyFilled(tri, 3, col);
}

void drawPanelIcon(Panel panel, const ImVec2& center, float size, ImU32 color)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    switch (panel) {
    case Panel::Edit:      iconPencil(dl, center, size, color); break;
    case Panel::World:     iconLayers(dl, center, size, color); break;
    case Panel::Render:    iconSun(dl, center, size, color); break;
    case Panel::Textures:  iconSwatch(dl, center, size, color); break;
    case Panel::Mesh:      iconCube(dl, center, size, color); break;
    case Panel::AI:        iconBubble(dl, center, size, color); break;
    case Panel::kCount:    break;
    }
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
