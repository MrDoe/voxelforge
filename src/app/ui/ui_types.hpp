#pragma once

// Editor vocabulary shared by the frame loop and the sidebar: the brush
// modes, the gizmo handles, the sidebar's rail sections, its layout metrics
// and the accent palette. Header-only, so either side may include it.

#include <cstdint>

#include <imgui.h>

namespace vf {
namespace app {

// Edit-tool brush modes. Carve/Add stamp analytic volumes (depth along the
// surface normal); Delete clears the brush ball, Paint recolours it with
// m_editMat, and Smooth relaxes a surface position (terrain column tops, or an
// object surface along its own axis). Every mode patches
// the live ChunkStore; object Rotate/Move remain gizmo operations.
enum class EditBrush : uint8_t { Carve, Add, Delete, Paint, Smooth, Rotate, Move };
enum class TrackballHandle : uint8_t { None, Yaw, Pitch, Roll };
enum class MoveAxis : uint8_t { None, X, Y, Z };

// Click-vs-drag for the edit brush. The stamp trigger is a DISTANCE test (the
// hover must cross a voxel), which has no notion of a click: the same press
// stamps again as soon as the hovered cell differs from the stamped one. Two
// different things must be suppressed and they want DIFFERENT rules.
//
//  1. The stack. An Add changes what the ray hits, so the next pick is the top
//     face of the voxel just created and a held click builds a tower. That has
//     an exact signature - the new pick IS the cell the last Add wrote - so it
//     is suppressed by identity, with no threshold to tune.
//  2. Jitter. A click whose hand wobbles far enough to cross a voxel. Here a
//     screen threshold is the only available signal, measured SINCE THE LAST
//     STAMP rather than from the press point: a small circular drag never gets
//     far from where it started but does keep moving, and a press-relative
//     test silently disables that entirely.
//
// So a held, still click is one edit (both rules), a small drag still paints,
// and the residual is a jittery click landing one neighbour - which the user
// is told about rather than promised away. The gate can only DELAY a stamp;
// lmbEdge still sets the first stamp of a press directly, so no click is lost.
constexpr float kDragTravelPx = 6.0f; // screen px of travel since the last stamp

// The editor has exactly one panel: a docked left sidebar whose icon rail
// picks which of these sections fills the content pane. There are no floating
// windows, so this replaces the old m_showWorldLayers / m_showMeshImport /
// m_showTextures visibility booleans. Order is the rail order and the Ctrl+1..6
// shortcut order.
enum class Panel : int { Edit = 0, World, Render, Textures, Mesh, AI,
                         kCount };
constexpr int kPanelCount = int(Panel::kCount);

// Rail button: the section's name, the icon it is drawn with (a vector glyph
// - the default ImGui font has no icon glyphs, so the rail draws its own) and
// the keyboard shortcut that jumps to it.
struct PanelInfo { const char* label; const char* full; const char* key; };
constexpr PanelInfo kPanels[kPanelCount] = {
    { "Edit",     "Edit tools",        "Ctrl+1" },
    { "World",    "World layers",      "Ctrl+2" },
    { "Render",   "Rendering",         "Ctrl+3" },
    { "Textures", "Material textures", "Ctrl+4" },
    { "Mesh",     "Import mesh",       "Ctrl+5" },
    { "AI",       "AI assistant",      "Ctrl+6" },
};

// Sidebar metrics. The sidebar is an overlay: the render is still full-window

// sidebar only costs screen area, never geometry. It is horizontally
// user-resizable, clamps on small windows, and can collapse to the rail (Ctrl+B).
constexpr float kRailW   = 46.0f;
constexpr float kPaneW   = 300.0f;
// kFooterH covers the separator plus the two status rows and the window
// padding; the footer is fixed-height so switching sections never reflows it.
constexpr float kFooterH = 52.0f;
constexpr float kPaneMin = 150.0f;
constexpr float kSidebarGripW = 8.0f;

// ---- design tokens --------------------------------------------------------
// One accent, four surface levels, three text levels. The rhythm below is the
// whole layout system: every gap, row and band is a multiple of it, so the
// six sections feel like one product instead of six hand-built panels.
//
// The tokens live here (not in theme.cpp) because the frame loop and the
// scene overlays read the accent and the warning colour too, and a second
// copy of a colour is a second thing to keep in sync.
constexpr float kUiGap      = 6.0f;   // base vertical rhythm
constexpr float kUiRowH     = 30.0f;  // the standard control row height
constexpr float kUiBandH    = 38.0f;  // the pane header band
constexpr float kUiGroupH   = 26.0f;  // a group header row
constexpr float kUiIndent   = 10.0f;  // group content indent
constexpr float kUiRadius   = 7.0f;   // control corner radius
constexpr float kUiRadiusLg = 12.0f;  // band / popup corner radius
constexpr float kUiIcon     = 20.0f;  // rail icon box

// The accent: one cyan, used for STATE (what is active, what is armed, what
// is on) and nothing else. Labels are never accent-coloured - when everything
// is highlighted, nothing is.
const ImVec4 kAccent(0.204f, 0.804f, 0.925f, 1.0f);
const ImVec4 kAccentSoft(0.106f, 0.298f, 0.365f, 1.0f);
const ImVec4 kWarn(1.0f, 0.722f, 0.278f, 1.0f);
const ImVec4 kDanger(0.965f, 0.376f, 0.439f, 1.0f);
const ImVec4 kOk(0.361f, 0.831f, 0.573f, 1.0f);

// Text levels: primary (values and labels), secondary (hints and group
// titles), tertiary (units, disabled). The three are a clear step apart at
// 13 px, which is what makes a flat list of controls readable as structure.
const ImVec4 kTextPrimary(0.898f, 0.925f, 0.965f, 1.0f);
const ImVec4 kTextSecondary(0.596f, 0.651f, 0.714f, 1.0f);
const ImVec4 kTextTertiary(0.427f, 0.478f, 0.541f, 1.0f);

// kPalette names (src/voxel/common.hpp) for the material combo. The array
// must cover every palette entry (kPaletteN), or the last materials are
// unreachable from every combo - which is how material 21 (fleece) stayed
// invisible: the array stopped at 20 while the palette had 22 entries.
// kPalette names (src/voxel/common.hpp) for the material combo.
inline constexpr const char* kMatNames[22] = {
    "0 grass dark", "1 grass light", "2 soil", "3 sand", "4 rock",
    "5 light rock", "6 wood", "7 roof", "8 foliage", "9 lava", "10 ember",
    "11 glow cyan", "12 glow green", "13 glow purple", "14 glow blue",
    "15 white-hot", "16 snow", "17 bark", "18 moss", "19 thatch", "20 plaster",
    "21 fleece",
};

} // namespace app
} // namespace vf
