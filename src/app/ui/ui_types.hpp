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

// Rail button: 2-letter label (the default ImGui font has no icon glyphs),
// tooltip, and a marker for the active entry / the armed edit tool.
struct PanelInfo { const char* label; const char* full; };
constexpr PanelInfo kPanels[kPanelCount] = {
    { "ED", "Edit tools  (Ctrl+1)" },
    { "WL", "World layers  (Ctrl+2)" },
    { "RN", "Rendering  (Ctrl+3)" },
    { "TX", "Material textures  (Ctrl+4)" },
    { "IM", "Import mesh  (Ctrl+5)" },
    { "AI", "AI assistant  (Ctrl+6)" },
};

// Sidebar metrics. The sidebar is an overlay: the render is still full-window

// sidebar only costs screen area, never geometry. It is horizontally
// user-resizable, clamps on small windows, and can collapse to the rail (Tab).
constexpr float kRailW   = 46.0f;
constexpr float kPaneW   = 300.0f;
// kFooterH covers the separator plus the two status rows and the window
// padding; the footer is fixed-height so switching sections never reflows it.
constexpr float kFooterH = 52.0f;
constexpr float kPaneMin = 150.0f;
constexpr float kSidebarGripW = 8.0f;

const ImVec4 kAccent(0.13f, 0.83f, 0.93f, 1.0f);
const ImVec4 kAccentSoft(0.08f, 0.32f, 0.40f, 1.0f);
const ImVec4 kWarn(1.0f, 0.70f, 0.25f, 1.0f);
const ImVec4 kDanger(0.96f, 0.33f, 0.42f, 1.0f);

// kPalette names (src/voxel/common.hpp) for the material combo.
inline constexpr const char* kMatNames[21] = {
    "0 grass dark", "1 grass light", "2 soil", "3 sand", "4 rock",
    "5 light rock", "6 wood", "7 roof", "8 foliage", "9 lava", "10 ember",
    "11 glow cyan", "12 glow green", "13 glow purple", "14 glow blue",
    "15 white-hot", "16 snow", "17 bark", "18 moss", "19 thatch", "20 plaster",
};

} // namespace app
} // namespace vf
