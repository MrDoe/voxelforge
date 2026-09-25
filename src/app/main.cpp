// Voxelforge - window, chunked-SVO ray marcher, HUD.
#include "core/camera.hpp"
#include "core/log.hpp"
#include "platform/window.hpp"
#include "rhi/swapchain.hpp"
#include "render/svo_pass.hpp"
#include "render/splat_pass.hpp"
#include "render/texture_atlas.hpp"
#include "render/post_pass.hpp"
#include "render/taa_pass.hpp"
#include "render/ssr_pass.hpp"
#include "render/ssao_pass.hpp"
#include "render/volumetric_fog_pass.hpp"
#include "render/motion_blur_pass.hpp"
#include "render/dof_pass.hpp"
#include "render/environment_pass.hpp"
#include "voxel/surfelize.hpp"
#include "voxel/chunk_index.hpp"
#include "voxel/worldfile.hpp"
#include <algorithm>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "ai/ollama_client.hpp"
#include "app/chat_ui.hpp"
#include "voxel/picking.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/live_editor.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// One camera view of a headless render. A --shotlist file holds one per line
// ("path x y z tx ty tz"); the app loads the world once and walks the list,
// so a test with N views pays the ~13 s world load a single time.
struct ShotSpec {
    std::string path;
    float camx=0,camy=0,camz=0,tx=0,ty=0,tz=0;
};

struct Args {
    bool selftest = false;
    int smokeFrames = 0;
    int width = 1600, height = 900;
    std::string shot;    // dump one frame to PPM and exit
    std::vector<ShotSpec> shots; // --shotlist: one load, N cameras
    float camx=0,camy=0,camz=0,tx=0,ty=0,tz=0;
    bool camSet=false;
    float sunElev=34.0f, sunAzim=238.0f; // golden-hour: long visible shadows
    bool sunSet=false;
    float animTime=0.0f;
    int tonemap = 2;    // AgX look: 0=Default 1=Golden 2=Punchy
    std::string mode = "splat"; // primary renderer: "splat" | "svo" (reference)
    bool probeSet=false;
    glm::vec3 probe { 0.f };
    std::string llmUrl = "http://127.0.0.1:11434";
    std::string llmModel = "gemma4:12b";
};

Args parseArgs(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](int def) -> int {
            return i + 1 < argc ? atoi(argv[++i]) : def;
        };
        if (s == "--selftest")
            a.selftest = true;
        else if (s == "--smoke")
            a.smokeFrames = next(240);
        else if (s == "--width")
            a.width = next(a.width);
        else if (s == "--height")
            a.height = next(a.height);
        else if (s == "--shot" && i + 1 < argc)
            a.shot = argv[++i];
        else if (s == "--shotlist" && i + 1 < argc) {
            const std::string listFile = argv[++i];
            std::ifstream lf(listFile);
            if (!lf) {
                spdlog::warn("--shotlist '{}' cannot be opened", listFile);
            } else {
                std::string line;
                while (std::getline(lf, line)) {
                    if (!line.empty() && line[0] == '#')
                        continue;
                    std::istringstream ls(line);
                    ShotSpec sh;
                    if (ls >> sh.path >> sh.camx >> sh.camy >> sh.camz >>
                            sh.tx >> sh.ty >> sh.tz)
                        a.shots.push_back(sh);
                }
                if (a.shots.empty())
                    spdlog::warn("--shotlist '{}' has no valid lines", listFile);
            }
        }
        else if (s == "--cam" && i + 1 < argc) {
            // accept either six argv tokens or one comma-separated token
            // ("x,y,z,tx,ty,tz") - the documented single-token form silently
            // no-ops under a bare argc check
            float* dst[6] = { &a.camx, &a.camy, &a.camz, &a.tx, &a.ty, &a.tz };
            std::string tok = argv[i + 1];
            const bool comma = tok.find(',') != std::string::npos;
            if (comma) {
                std::istringstream ls(tok);
                std::string val;
                for (int c = 0; c < 6 && std::getline(ls, val, ','); ++c)
                    *dst[c] = float(atof(val.c_str()));
                ++i;
            } else if (i + 6 < argc) {
                for (int c = 0; c < 6; ++c)
                    *dst[c] = float(atof(argv[++i]));
            } else {
                spdlog::warn("--cam needs 6 values (x y z tx ty tz)");
                break;
            }
            a.camSet = true;
        } else if (s == "--sun" && i + 2 < argc) {
            a.sunElev = atof(argv[++i]); a.sunAzim = atof(argv[++i]);
            a.sunSet = true;
        }         else if (s == "--animtime" && i + 1 < argc) {
            a.animTime = atof(argv[++i]);
        } else if (s == "--tonemap" && i + 1 < argc) {
            a.tonemap = atoi(argv[++i]);
        } else if (s == "--mode" && i + 1 < argc) {
            a.mode = argv[++i];
        } else if (s == "--probe" && i + 3 < argc) {
            a.probe = { float(atof(argv[i + 1])), float(atof(argv[i + 2])),
                        float(atof(argv[i + 3])) };
            i += 3;
            a.probeSet = true;
        } else if ((s=="--llm-url" || s=="--ollama-url") && i+1<argc) a.llmUrl = argv[++i];
        else if ((s=="--llm-model" || s=="--ollama-model") && i+1<argc) a.llmModel = argv[++i];
    }
    if (const char* e = getenv("VF_LLM_URL")) a.llmUrl = e;
    if (const char* e = getenv("VF_LLM_MODEL")) a.llmModel = e;
    return a;
}

constexpr uint32_t kMaxFramesInFlight = 3;

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderDone = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

// GPU timestamp profiling: kProfMarks timestamps per frame slot; the
// consecutive deltas give per-pass GPU ms (geo = splat raster | SVO march,
// post, fx = photorealism chain, taa, tail = blit+UI+transitions). Slot
// results are read right after that slot's fence wait (its submission from
// kMaxFramesInFlight frames ago is then complete) and reset inside the next
// recording of the same slot.
constexpr uint32_t kProfMarks = 6;

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
// stamps again as soon as the hovered cell differs from the stamped one. A
// click can produce that on its own - ordinary cursor jitter, or, worse, the
// Add changing what the ray hits, so the next pick is the top face of the
// voxel just created and a held click stacks one on top of another. A second
// stamp therefore requires the pointer to have moved NET DISPLACEMENT since
// the PRESS, not since the last stamp: a hand that wobbles while held returns
// near its press point and never crosses the threshold, while a real drag
// only increases its distance from it. The gate can only delay a stamp, never
// add one, and lmbEdge still sets the first stamp directly, so no click is
// ever lost.
constexpr float kDragTravelPx = 14.0f; // net screen px from the press point

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
// and picking still runs from the camera through the cursor, so a wider
// sidebar only costs screen area, never geometry. It is horizontally
// user-resizable, clamps on small windows, and can collapse to the rail (Tab).
constexpr float kRailW   = 46.0f;
constexpr float kPaneW   = 300.0f;
// kFooterH covers the separator plus the two status rows and the window
// padding; the footer is fixed-height so switching sections never reflows it.
constexpr float kFooterH = 52.0f;
constexpr float kPaneMin = 150.0f;
constexpr float kSidebarGripW = 8.0f;

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

const ImVec4 kAccent(0.13f, 0.83f, 0.93f, 1.0f);
const ImVec4 kAccentSoft(0.08f, 0.32f, 0.40f, 1.0f);
const ImVec4 kWarn(1.0f, 0.70f, 0.25f, 1.0f);
const ImVec4 kDanger(0.96f, 0.33f, 0.42f, 1.0f);

std::string foldCase(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A toggle button that reads as "currently on" instead of a momentary action.
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
void sectionHeader(const char* title, const char* hint = nullptr)
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

// "carve" | "add" | "delete" | "paint" | "smooth" | "rotate" | "move"
// (anything else = carve); shared by the HUD and headless test hooks.
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

// Project a world point to viewport pixels (perspective, same rule the splat
// VS uses). Callers only feed objects the camera is looking at and skip
// behind-camera AABB corners.
glm::vec2 projectScreen(const glm::vec3& w, const vf::Camera& cam,
                        float tanHalfFov, const glm::ivec2& fb)
{
    // Match splat.vert's view-space projection exactly. Using a normalized
    // world direction here (the old implementation) changes the horizontal
    // scale with depth and put the trackball outside the viewport.
    const glm::vec3 rel = w - cam.pos;
    const float depth = std::max(glm::dot(rel, cam.forward()), 1e-3f);
    const float aspect = float(fb.x) / float(fb.y);
    const float ndcX = glm::dot(rel, cam.right()) /
                       (depth * tanHalfFov * aspect);
    const float ndcY = -glm::dot(rel, cam.up()) / (depth * tanHalfFov);
    return { 0.5f * float(fb.x) * (1.f + ndcX),
             0.5f * float(fb.y) * (1.f + ndcY) };
}

float trackballRingDistance(const glm::vec2& p, const glm::vec2& centre,
                            float radiusX, float radiusY)
{
    if (radiusX < 1e-3f || radiusY < 1e-3f)
        return 1e30f;
    const glm::vec2 d = (p - centre) / glm::vec2(radiusX, radiusY);
    // Use the mean pixel scale rather than the smaller radius. The previous
    // min-radius bias made one inner ellipse swallow clicks intended for the
    // other handles, especially near their intersections.
    return std::abs(glm::length(d) - 1.f) *
           0.5f * (radiusX + radiusY);
}

TrackballHandle trackballHandleAt(const glm::vec2& p,
                                  const glm::vec2& centre, float radius,
                                  float slop = 10.f)
{
    if (radius < 1e-3f)
        return TrackballHandle::None;
    float best = slop;
    TrackballHandle hit = TrackballHandle::None;
    auto consider = [&](TrackballHandle handle, float rx, float ry) {
        const float d = trackballRingDistance(p, centre, rx, ry);
        if (d < best) {
            best = d;
            hit = handle;
        }
    };
    consider(TrackballHandle::Yaw, radius, radius);
    consider(TrackballHandle::Pitch, radius, radius * 0.42f);
    consider(TrackballHandle::Roll, radius * 0.42f, radius);
    return hit;
}

glm::vec3 moveAxisVector(MoveAxis axis)
{
    switch (axis) {
    case MoveAxis::Y: return glm::vec3(0.f, 1.f, 0.f);
    case MoveAxis::Z: return glm::vec3(0.f, 0.f, 1.f);
    case MoveAxis::None:
    case MoveAxis::X: break;
    }
    return glm::vec3(1.f, 0.f, 0.f);
}

const char* moveAxisName(MoveAxis axis)
{
    switch (axis) {
    case MoveAxis::X: return "X";
    case MoveAxis::Y: return "Y";
    case MoveAxis::Z: return "Z";
    case MoveAxis::None: break;
    }
    return "";
}

struct MoveAxisLine {
    glm::vec2 a;
    glm::vec2 b;
};

MoveAxisLine moveAxisScreenLine(const glm::vec3& worldCentre, MoveAxis axis,
                                const vf::Camera& cam, float tanHalfFov,
                                const glm::ivec2& fb, float radius)
{
    const glm::vec2 p0 = projectScreen(worldCentre, cam, tanHalfFov, fb);
    const glm::vec3 v = moveAxisVector(axis);
    glm::vec2 dir = projectScreen(worldCentre + v * 0.75f, cam, tanHalfFov, fb) - p0;
    if (glm::length(dir) < 1e-3f) {
        // A view-parallel axis has no useful projection; keep a visible,
        // deterministic screen direction instead of dropping the handle.
        dir = axis == MoveAxis::Y ? glm::vec2(0.f, -1.f)
            : axis == MoveAxis::Z ? glm::vec2(0.7071f, 0.7071f)
                                  : glm::vec2(1.f, 0.f);
    }
    const float pixelLength = std::max(radius * 0.9f, 28.f);
    dir = glm::normalize(dir) * pixelLength;
    return { p0 - dir * 0.35f, p0 + dir };
}

float screenSegmentDistance(const glm::vec2& p, const glm::vec2& a,
                            const glm::vec2& b)
{
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-6f)
        return glm::distance(p, a);
    const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::distance(p, a + ab * t);
}

MoveAxis moveAxisHandleAt(const glm::vec2& p, const glm::vec3& worldCentre,
                          const vf::Camera& cam, float tanHalfFov,
                          const glm::ivec2& fb, float radius,
                          float slop = 12.f)
{
    if (radius < 1e-3f)
        return MoveAxis::None;
    float best = slop;
    MoveAxis hit = MoveAxis::None;
    for (MoveAxis axis : { MoveAxis::X, MoveAxis::Y, MoveAxis::Z }) {
        const MoveAxisLine line = moveAxisScreenLine(
            worldCentre, axis, cam, tanHalfFov, fb, radius);
        const float d = screenSegmentDistance(p, line.a, line.b);
        if (d < best) {
            best = d;
            hit = axis;
        }
    }
    return hit;
}

// The visible trackball is centered on the placed object's bounds and sized
// from those bounds. It is an interaction surface only; object rotation still
// uses transformRecords' exact bottom-center pivot.
bool trackballScreenForBox(const vf::voxel::WorldAABB& b,
                           const vf::Camera& cam, float tanHalfFov,
                           const glm::ivec2& fb, glm::vec2& centre,
                           float& radius)
{
    if (!b.valid())
        return false;
    const glm::vec3 worldCentre = 0.5f * (b.lo + b.hi);
    if (glm::dot(worldCentre - cam.pos, cam.forward()) <= 0.05f)
        return false;
    centre = projectScreen(worldCentre, cam, tanHalfFov, fb);
    float maxR = 0.f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(
            b.lo.x + (i & 1 ? b.hi.x - b.lo.x : 0.f),
            b.lo.y + (i & 2 ? b.hi.y - b.lo.y : 0.f),
            b.lo.z + (i & 4 ? b.hi.z - b.lo.z : 0.f));
        if (glm::dot(corner - cam.pos, cam.forward()) <= 0.05f)
            continue;
        maxR = std::max(maxR, glm::distance(
            centre, projectScreen(corner, cam, tanHalfFov, fb)));
    }
    // Small objects still need a usable hit target; very large bounds stay
    // inside the viewport so all three rings remain reachable.
    radius = std::clamp(maxR, 24.f, 0.46f * float(std::min(fb.x, fb.y)));
    return true;
}

// kPalette names (src/voxel/common.hpp) for the material combo.
const char* kMatNames[21] = {
    "0 grass dark", "1 grass light", "2 soil", "3 sand", "4 rock",
    "5 light rock", "6 wood", "7 roof", "8 foliage", "9 lava", "10 ember",
    "11 glow cyan", "12 glow green", "13 glow purple", "14 glow blue",
    "15 white-hot", "16 snow", "17 bark", "18 moss", "19 thatch", "20 plaster",
};

class App {
public:
    int run(const Args& args);

private:
    bool initWindow(const Args& args);
    bool initVulkan();
    void destroy();
    bool createOffscreen(uint32_t w, uint32_t h);
    void ensureAcquireSemaphores();
    void handleResize();
    void drawHud();
    // The single editor panel: one docked left sidebar (icon rail + content
    // pane + status footer). Each section is its own method so the sidebar
    // stays a thin dispatcher and every section reads on its own.
    void drawSidebar();
    void drawPanelEdit();
    void drawPanelWorld();
    void drawPanelRender();
    void drawPanelTextures();
    void drawPanelMesh();
    void drawPanelAI();
    void drawSidebarFooter();
    void drawSceneOverlays();
    bool runSelftest();
    void syncWorldLayerList();
    bool uploadTerrainTexture();
    bool uploadObjVolTexture();
    bool reloadTexAtlas();
    void persistWorldLayers();
    void rescanWorldLayers();
    void rescanMeshFiles();
    void prepareMeshImport();
    void useMeshImportLayer(const std::string& file);
    bool importMeshFromGui();
    // material texture picker (assets/textures/* -> world.json "textures")
    void rescanTextureFiles();
    void setTextureBinding(int mat, const std::string& file);
    void applyTextureBindings();
    // hot-swap watch: folder drop-ins + mtime of every bound image file
    void pollTextureFiles();
    void applyWorldReload();
    void rebuildSurfels(); // (re)build the surfel set from the live field
    void cancelRotation();
    void commitMove();
    void cancelMove();
    // GPU timestamp profiling (no-op until m_profPool exists)
    void accumulateProf(uint32_t slot, uint64_t frameIdx)
    {
        if (m_profPool == VK_NULL_HANDLE || frameIdx < kMaxFramesInFlight)
            return;
        uint64_t t[kProfMarks] {};
        if (vkGetQueryPoolResults(m_ctx.device(), m_profPool, slot * kProfMarks,
                                  kProfMarks, sizeof(t), t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        auto ms = [&](uint32_t a, uint32_t b) {
            return double(t[b] - t[a]) * m_profPeriodNs * 1e-6;
        };
        constexpr double w = 0.05;
        m_profAvg[0] += (ms(0, 1) - m_profAvg[0]) * w; // splat | svo
        m_profAvg[1] += (ms(1, 2) - m_profAvg[1]) * w; // post
        m_profAvg[2] += (ms(2, 3) - m_profAvg[2]) * w; // photorealism fx
        m_profAvg[3] += (ms(3, 4) - m_profAvg[3]) * w; // taa
        m_profAvg[4] += (ms(4, 5) - m_profAvg[4]) * w; // blit + UI + transitions
        if (getenv("VF_TRACE") && frameIdx % 120 == 0)
            spdlog::info("gpu[{}]: geo {:.2f} post {:.2f} fx {:.2f} taa {:.2f} "
                         "tail {:.2f} ms",
                         frameIdx, m_profAvg[0], m_profAvg[1],
                         m_profAvg[2], m_profAvg[3], m_profAvg[4]);
    }
    // Per-frame CPU probe: camera embedded in solid => two-sided shells.
    void updateBuriedProbe()
    {
        m_splatPass.setBuried(m_layers.loaded() &&
                              m_layers.field().sampleWorld(m_camera.pos).d < 0.0f);
    }
    // In-place photorealism chain on m_offscreen (post-tonemap LDR):
    // SSR/SSAO/volumetric-fog/motion-blur/DoF per the G/H/J/K/L toggles.
    // Shared by the headless and interactive frame paths so they can't drift.
    void recordPhotorealism(VkCommandBuffer cmd, const vf::RaymarchPush& push);

    Args m_args;
    vf::Window m_window;
    vf::Context m_ctx;
    vf::Swapchain m_swapchain;

    vf::Image3D m_offscreen;
    vf::Image3D m_hdr;     // ray-march linear HDR output
    vf::Image3D m_gpos;    // ray-march G-buffer: world pos + hit type
    vf::Image3D m_gnorm;   // G-buffer: shading normal (xyz), 0 on sky
    vf::Image3D m_ssaoAo;  // SSAO scratch: raw AO term (ssao.comp -> ssao_apply.comp)
    vf::Image3D m_heightImg;
    vf::Image3D m_objVolImg;
    vf::SvoPass m_svoPass;
    vf::SplatPass m_splatPass;
    vf::TaaPass m_taaPass;
    vf::PostPass m_postPass;
    // photorealism passes
    vf::SSRPass m_ssrPass;
    vf::SSAOPass m_ssaoPass;
    vf::VolumetricFogPass m_volFogPass;
    vf::MotionBlurPass m_motionBlurPass;
    vf::DepthOfFieldPass m_dofPass;
    vf::EnvironmentPass m_envPass;
    vf::TexAtlas m_texAtlas;   // optional PNG overrides (world.json "textures")
    std::string m_manifestPath; // world.json path (for atlas reloads)
    // primary visibility renderer; SVO kept as the pixel reference
    enum class RenderMode { Splats, Svo };
    RenderMode m_renderMode = RenderMode::Splats;
    vf::Image3D m_taaHistory[2];
    vf::Image3D m_taaResolved;
    bool m_taaEnabled = true;
    float m_taaBlend = 0.92f; // base history blend (VF_TAA_BLEND overrides)
    bool m_taaFirstFrame = true;
    int m_taaHistoryIdx = 0;
    vf::TaaPrevCam m_prevCam{};
    glm::vec4 m_pushB { 1.0f };
    // layered world state (GUI)
    std::vector<vf::voxel::worldfile::WorldLayer> m_worldLayers;
    std::string m_selectedLayer; // active inspector row / exact rotate target
    char m_layerFilter[64] = {};
    // STL/OBJ authoring section.  The importer writes a named record layer;
    // an existing layer's manifest pose is intentionally left untouched, so
    // replacing a placed object never changes its authored orientation.
    bool m_meshImportPrepared = false;
    char m_meshPath[512] = {};
    char m_meshLayerName[64] = {};
    std::vector<std::string> m_meshFiles;
    glm::ivec3 m_meshAnchor { 512, 512, 512 };
    bool m_meshUseFit = true;
    float m_meshFitMeters = 5.0f;
    float m_meshScale = 1.0f;
    float m_meshRotY = 0.0f;
    int m_meshMaterial = 6;
    bool m_meshSwapYz = false;
    bool m_meshFlip = false;
    bool m_meshSolid = true;
    std::string m_meshStatus;
    bool m_meshStatusError = false;
    vf::voxel::LayeredWorld m_layers;
    float m_layerPollT = 0.f;
    bool m_pendingWorldReload = false;
    // texture picker state: the live binding table + the files found in
    // assets/textures/ (paths relative to the manifest dir, e.g. "textures/x.png")
    std::vector<vf::voxel::worldfile::TextureBinding> m_texBindings;
    std::vector<std::string> m_texFiles;
    // hot-swap: a GUI edit (write manifest + re-upload) and a bound file's
    // on-disk change (re-upload only) both land between frames, never inside
    // a recorded command buffer. m_texSig = resolved path -> mtime+size.
    bool m_texApplyPending = false;
    bool m_texReloadPending = false;
    float m_texPollT = 0.f;
    std::map<std::string, unsigned long long> m_texSig;

    // Direction TOWARD the sun, derived from --sun elevation/azimuth (degrees).
    glm::vec4 m_sunDir { 0.449f, 0.8338f, 0.3207f, 0.0f };

    VkCommandPool m_framePool = VK_NULL_HANDLE;
    std::vector<FrameSync> m_frames;
    std::vector<VkSemaphore> m_acquireSems; // one per swapchain image

    vf::Camera m_camera;
    float m_lastFrameMs = 16.7f;
    double m_avgMs = 16.7f;
    float m_minMs = 1e9f, m_maxMs = 0.0f;
    uint64_t m_frameIdx = 0;
    size_t m_shotIdx = 0; // --shotlist: which view the headless loop is on
    // GPU timestamp profiling state (see kProfMarks)
    VkQueryPool m_profPool = VK_NULL_HANDLE;
    double m_profPeriodNs = 1.0;
    double m_profAvg[5] = { 0, 0, 0, 0, 0 };
    VkSampler m_uiSampler = VK_NULL_HANDLE;
    ImTextureID m_sceneTexId = 0;
    bool m_scenePreview = false;
    float m_animTime = 0.0f;
    int m_tonemapLook = 2;
    // bit0 AO,bit1 shadows,bit2 flora,bit3 water,bit4 outline,bit5 SSR,bit6 SSAO
    // SSR+SSAO on by default (measured: +0.65 ms @720p / +1.34 ms @1080p hero;
    // visual_check black-in-silhouette 0.15-0.32 % vs the 5 % gate). Bit 7 =
    // texture detail normals (B): a Sobel of the material albedo perturbs the
    // shading normal, so photo textures read as surfaces. It is a no-op for
    // untextured materials, so VF_TEXTURES=0 stays bit-exact. VolFog /
    // motion blur / DoF stay opt-in (separate toggles; volfog is WIP - see
    // shaders/volumetric_fog.comp).
    int m_renderFlags = 255;
    float m_exposure = 1.15f;
    // SSAO tuning (VF_SSAO_*): world-scale two-band AO, opt-in via H / bit 6.
    float m_ssaoStrength = 0.6f;
    float m_ssaoRadius = 0.8f; // far-band world radius (m)
    int m_ssaoDebug = 0;       // 1 = raw AO, 2 = G-buffer normal
    bool m_ssaoBlur = true;    // cross-bilateral denoise before applying
    bool m_volFogEnabled = false;
    // Micro-surfel detail (M key). Changing it changes the SURFEL STREAM, not
    // just a draw-time flag, so a toggle re-runs the surfelizer via the world
    // reload path (same as a layer toggle). Initial value from VF_MICRO.
    bool m_microDetail = true;
    // Tighten only genuine hard-edge object parents; small tangent-aligned
    // bridge splats preserve crease coverage. Both are baked, so changing
    // either setting requests a world reload.
    float m_edgeShrink = 0.35f;
    bool m_edgeFill = true;
    bool m_motionBlurEnabled = false;
    bool m_dofEnabled = false;
    float m_dofFocusDist = 10.0f;
    float m_dofFocalLength = 50.0f;
    uint32_t m_nextAcquire = 0;

    // AI chat + picking + editable world
    vf::voxel::EditableWorld m_editable { std::string(VOXELFORGE_ASSET_DIR) };
    // Carve/raise rasterizer providers: the edit tool only calls their
    // makeXxx() shape builders (the carving/raising layers themselves are
    // legacy - every brush stamp now patches the runtime store instead).
    vf::voxel::EditableWorld m_carve { std::string(VOXELFORGE_ASSET_DIR),
                                       std::string(vf::voxel::EditableWorld::kCarveFileName),
                                       std::string(vf::voxel::EditableWorld::kCarveLayerName),
                                       std::string("carve") };
    vf::voxel::EditableWorld m_add { std::string(VOXELFORGE_ASSET_DIR),
                                     std::string(vf::voxel::EditableWorld::kRaiseFileName),
                                     std::string(vf::voxel::EditableWorld::kRaiseLayerName),
                                     std::string("raise") };
    vf::ai::ChatUi m_chatUi;
    vf::voxel::PickHit m_hoverHit;
    vf::voxel::PickHit m_selectedHit;
    bool m_hasSelection = false;
    bool m_lmbWasDown = false;
    bool m_ctrlWasDown = false;
    bool m_chatInitialized = false;

    // The single docked left sidebar. m_panel is which rail section fills the
    // content pane; m_sidebarCollapsed shrinks the sidebar to the icon rail
    // alone so the scene is unobstructed. m_sidebarWidth is the user's
    // horizontal size (0 keeps the responsive default); the grip state lets
    // frame-loop input yield to the UI before picking/stamping. m_editActive
    // is deliberately NOT folded into m_panel: it is tool-armed state that
    // gates LMB stamping, independent of which section is on screen.
    Panel m_panel = Panel::World;
    bool m_sidebarCollapsed = false;
    float m_sidebarWidth = 0.0f;
    bool m_sidebarResizing = false;

    // carve / add / delete / paint / smooth edit tool: stamps a brush volume
    // at the hovered surface point along its normal (Carve = depth-limited
    // cylinder scoop, Add = dome, Delete = clear the ball, Paint = recolour
    // the ball, Smooth = relax terrain columns or an object surface along its
    // own axis). Diameter/depth are
    // adjustable via +/-; mode is selected in the sidebar's Edit section.
    bool m_editActive = false;
    EditBrush m_editBrush = EditBrush::Carve;
    float m_editDiameter = 2.0f; // meters (brush ball/cylinder/smooth footprint)
    // The brush is a lattice tool, so its width is a whole number of voxels
    // (VOXEL = 0.1 m) and the slider speaks voxels. 1 voxel is the per-voxel
    // mode: Add/Carve then touch exactly the cell under the cursor.
    static constexpr int kBrushMinVox = 1;
    static constexpr int kBrushMaxVox = 120; // 12 m, the old slider ceiling
    int brushVoxels() const
    {
        const float d = std::isfinite(m_editDiameter) ? m_editDiameter : 2.0f;
        return std::clamp(int(std::lround(d / vf::voxel::VOXEL)), kBrushMinVox,
                          kBrushMaxVox);
    }
    void setBrushVoxels(int v)
    {
        v = std::clamp(v, kBrushMinVox, kBrushMaxVox);
        m_editDiameter = float(v) * vf::voxel::VOXEL;
    }
    // Snap size and depth onto the lattice after an env override, so "1 voxel"
    // is reachable and exact (0.13 m would otherwise be one voxel wide but
    // still reach 0.13 m along the normal).
    int brushDepthVoxels() const
    {
        const float d = std::isfinite(m_editDepth) ? m_editDepth : 1.5f;
        return std::clamp(int(std::lround(d / vf::voxel::VOXEL)), 1, kBrushMaxVox);
    }
    void setBrushDepthVoxels(int v)
    {
        m_editDepth = float(std::clamp(v, 1, kBrushMaxVox)) * vf::voxel::VOXEL;
    }
    void quantiseBrush()
    {
        if (!std::isfinite(m_editDiameter))
            m_editDiameter = 2.0f;
        if (!std::isfinite(m_editDepth))
            m_editDepth = 1.5f;
        setBrushVoxels(brushVoxels());
        setBrushDepthVoxels(brushDepthVoxels());
    }
    bool brushIsPerVoxel() const { return brushVoxels() <= 1; }
    // The cell a 1-voxel Add/Carve would touch: the pick itself for Carve, or
    // the neighbour one cell out along the normal for Add. Both the stamp and
    // its hover preview go through makeSingleVoxel so the step (and the lattice
    // clamp) has exactly one implementation.
    glm::ivec3 perVoxelTarget(const glm::vec3& n) const
    {
        if (m_editBrush != EditBrush::Add)
            return m_hoverHit.voxel;
        const auto recs = m_add.makeSingleVoxel(m_hoverHit.voxel, n, m_editMat, true);
        if (recs.empty())
            return m_hoverHit.voxel; // stepped out of the lattice
        return glm::ivec3(recs[0].x, recs[0].y, recs[0].z);
    }
    float m_editDepth = 1.5f;    // meters (carve depth / add length)
    float m_smoothStrength = 0.65f; // 0..1 surface relaxation per stamp
    uint8_t m_editMat = 6;       // palette id for Add and Paint
    // Rotate trackball: m_rotateLayer is the object activated by one click.
    // A subsequent press on the yaw/pitch/roll ring starts the live drag;
    // release stages the pose, and Apply is the only path that writes it.
    std::string m_rotateLayer;
    float m_rotateRadius = 0.f;
    glm::vec2 m_rotateLastMouse { 0.f };
    TrackballHandle m_rotateHandle = TrackballHandle::None;
    bool m_rotating = false;
    bool m_rotationStaged = false;       // uncommitted GPU pose
    bool m_rotationPreviewPending = false; // keep final pose until rebuilt
    // Trackball drawn over the active object; its bounds are refreshed by the
    // frame input/brush block and ring picking uses the same screen geometry.
    glm::vec2 m_gizmoCentre { 0.f };
    float m_gizmoRadius = 0.f;
    bool m_gizmoValid = false;
    // live yaw/pitch/roll deltas in degrees, shown in the panel while dragging
    float m_rotateDy = 0.f, m_rotateDx = 0.f, m_rotateDz = 0.f;
    // Move mode: target owner, constrained world axis, and an uncommitted
    // translation draft. The manifest is changed only by Apply move.
    std::string m_moveLayer;
    MoveAxis m_moveAxis = MoveAxis::X;
    glm::vec3 m_moveDelta { 0.f };
    glm::vec2 m_moveLastMouse { 0.f };
    bool m_moving = false;
    bool m_moveStaged = false;
    // Every stamp patches the runtime ChunkStore and the GPU buffers per dirty
    // chunk (live bake, no world reload); strokes persist asynchronously to
    // assets/runtime_edits.vxw on mouse release.
    float m_lastEditMs = 0.0f;
    size_t m_lastEditSurfels = 0;
    vf::voxel::LiveEditor m_liveEditor;
    vf::voxel::OverlayWriter m_overlayWriter;
    // Undo: the state each touched cell held before the current stroke (keyed
    // by packed cell; the first occurrence wins) plus the finished strokes as
    // inverse edits + the height-texture rise the replay needs. A stroke too
    // large to record is dropped wholesale instead of becoming a partial undo.
    std::unordered_map<uint64_t, vf::voxel::StoreEdit> m_undoPending;
    std::vector<std::pair<std::vector<vf::voxel::StoreEdit>, int>> m_undo;
    int m_strokeRiseCells = 2; // forward height-texture headroom; finishStroke
                                // derives the inverse/undo headroom separately
    bool m_undoOverflow = false;
    double m_clearConfirmUntil = 0.0; // two-click destructive action
    static constexpr size_t kUndoMaxCells = 250000;
    static constexpr size_t kUndoDepth = 32;
    // Water-plane splats: the world-wide coverage grid as one run, bucketed
    // per chunk for culling (the shader intersects the analytic plane).
    std::vector<vf::voxel::Surfel> m_waterSurfels;
    std::vector<uint32_t> m_waterRel;
    // CPU mirror of the height texture so live edits can patch the edited
    // columns in place (the water shading and the splat shadow/AO marches
    // read it; a carve must not leave it stale).
    std::vector<glm::vec2> m_heightCpu;
    bool m_overlayLoaded = false;
    bool m_hasStamp = false;
    bool m_dragging = false;
    glm::ivec3 m_lastStampVoxel { 0 };
    // Where the pointer was when the current stroke STARTED, so a held click
    // (which wanders but does not go anywhere) is told from a drag (which
    // keeps increasing its net distance from this point).
    glm::vec2 m_stampPressMouse { 0.f };

    void applyEditLive();
    // Trackball rotate: activate the picked voxel's exact owner, then map a
    // yaw/pitch/roll ring drag to its manifest placement.
    std::string rotateTargetLayer(const vf::voxel::PickHit& hit) const;
    void commitRotation();
    // Shared tail of every store mutation (brush stamp and undo): apply the
    // edits, rebuild the dirty store regions, re-derive the affected chunks'
    // surfels/octree pools, patch the GPU buffers and the edited
    // height-texture columns, update the HUD/log.
    void commitStoreEdits(std::vector<vf::voxel::StoreEdit>& edits, int riseCells,
                          const char* what,
                          int margin = vf::voxel::LiveEditor::kStampMargin);
    // Stroke end (mouse release / lone headless stamp): make the accumulated
    // pre-edit cells one undo step, so undo reverts a whole stroke.
    void finishStroke();
    void undoEdit();
    // Drop every runtime edit + the persisted overlay, re-patching the touched
    // chunks from the baked pools (no world reload).
    void clearLiveEdits();
    // (Re)bucket the water-plane run per chunk (deterministic order) and
    // return the per-chunk offsets relative to the run start.
    std::vector<uint32_t> updateWaterBuckets();
    // Re-derive the height texture for a column window from the store and
    // re-upload that sub-rect (see the definition).
    void patchHeightTexture(int x0, int z0, int x1, int z1, int riseCells);
    void loadStoreOverlay();
    // Store-gradient surface normal at a lattice cell (test hooks / injected
    // hovers); falls back to +Y when the field around the cell is flat.
    glm::vec3 storeNormalAt(const glm::ivec3& v);
public:
    void requestWorldReload() { m_pendingWorldReload = true; }
    };

bool App::initWindow(const Args& args)
{
    return m_window.init(args.width, args.height, "Voxelforge");
}

void App::ensureAcquireSemaphores()
{
    for (VkSemaphore s : m_acquireSems)
        vkDestroySemaphore(m_ctx.device(), s, nullptr);
    m_acquireSems.assign(m_swapchain.imageCount(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (VkSemaphore& s : m_acquireSems)
        vkCreateSemaphore(m_ctx.device(), &si, nullptr, &s);
}

bool App::createOffscreen(uint32_t w, uint32_t h)
{
    vf::destroyImage3D(m_ctx, m_offscreen);
    vf::destroyImage3D(m_ctx, m_taaHistory[0]);
    vf::destroyImage3D(m_ctx, m_taaHistory[1]);
    vf::destroyImage3D(m_ctx, m_taaResolved);
    if (m_scenePreview && m_sceneTexId) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_sceneTexId);
        m_sceneTexId = 0;
    }
    m_offscreen = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_taaHistory[0] = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_taaHistory[1] = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    m_taaResolved = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vf::destroyImage3D(m_ctx, m_hdr);
    vf::destroyImage3D(m_ctx, m_gpos);
    m_hdr = vf::makeImage2D(
        m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    m_gpos = vf::makeImage2D(
        m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    vf::destroyImage3D(m_ctx, m_gnorm);
    m_gnorm = vf::makeImage2D(
        m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    vf::destroyImage3D(m_ctx, m_ssaoAo);
    m_ssaoAo = vf::makeImage2D(
        m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!m_splatPass.recreateDepth(w, h))
        return false;
    if (!m_offscreen.img || !m_taaHistory[0].img || !m_taaHistory[1].img ||
        !m_taaResolved.img || !m_hdr.img || !m_gpos.img || !m_gnorm.img ||
        !m_ssaoAo.img)
        return false;
    m_svoPass.updateDescriptors(m_hdr, m_gpos, m_gnorm);
    m_splatPass.updateDescriptors(m_hdr.view, m_gpos.view, m_gnorm.view,
                                  m_heightImg.view, m_objVolImg.view);
    m_postPass.updateDescriptors(m_hdr.view, m_gpos.view, m_offscreen.view);
    m_taaFirstFrame = true;
    m_taaHistoryIdx = 0;
    return true;
}

// Diagnostic aid for a windowed render that goes dark or stops presenting:
// a Vulkan-level failure here is otherwise INVISIBLE. handleResize() covers
// only OUT_OF_DATE/SUBOPTIMAL and logs nothing, and every other VkResult from
// acquire/present falls through unlogged - including VK_ERROR_DEVICE_LOST and
// VK_ERROR_OUT_OF_HOST_MEMORY. A persistent failure at acquire makes the frame
// loop skip its tail every iteration (it never draws, never crashes, never
// says so), which is indistinguishable from "the app hung" when all you have
// is pixels.
//
// Log-once, NOT per frame: the failure modes above are exactly the ones that
// spin, and a per-frame error would emit thousands of lines a second and
// destroy the instrument. Gated on VF_TRACE, so a default run is unchanged.
// This deliberately does NOT make device-lost exit: that is a behaviour
// decision, not a logging side effect.
namespace {
const char* vkResultName(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default: return "other";
    }
}

// true the first time this result is seen, so a spinning failure logs once.
bool firstSight(VkResult r, const char* what)
{
    if (!getenv("VF_TRACE"))
        return false;
    static int seen[2] = { 0, 0 };
    const int slot = what[0] == 'a' ? 0 : 1;
    if (seen[slot] == int(r))
        return false;
    seen[slot] = int(r);
    return true;
}
} // namespace

void App::handleResize()
{
    vkDeviceWaitIdle(m_ctx.device());
    glm::ivec2 fbs = m_window.framebufferSize();
    if (!m_swapchain.recreate(uint32_t(fbs.x), uint32_t(fbs.y)) ||
        !createOffscreen(m_swapchain.extent().width, m_swapchain.extent().height))
        spdlog::error("resize failed");
    else if (getenv("VF_TRACE"))
        spdlog::info("swapchain recreate: {}x{} framebuffer, {} swapchain images",
                      m_swapchain.extent().width, m_swapchain.extent().height,
                      m_swapchain.imageCount());
    ensureAcquireSemaphores();
}

bool App::initVulkan()
{
    if (!m_ctx.init(m_window.handle(), true))
        return false;

    glm::ivec2 fb = m_window.framebufferSize();
    // IMMEDIATE is the default: MAILBOX deadlocks after a few thousand frames
    // on NVIDIA 580 + X11 (xcb present wakeup loss) - see swapchain.cpp.
    vf::PresentPolicy policy = vf::PresentPolicy::Immediate;
    if (const char* pm = getenv("VF_PRESENT")) {
        // manual override for testing WSI paths
        if (strcmp(pm, "immediate") == 0)
            policy = vf::PresentPolicy::Immediate;
        else if (strcmp(pm, "mailbox") == 0)
            policy = vf::PresentPolicy::PreferMailbox;
        else
            spdlog::warn("VF_PRESENT='{}' ignored (use immediate|mailbox)", pm);
    }
    if (!m_swapchain.init(m_ctx, uint32_t(fb.x), uint32_t(fb.y), policy))
        return false;

    // chunked-SVO world synthesis (single render path) ---------------------
    // layered world is the single source: the SVO is synthesized directly
    // from the enabled layer files (no merged cache, no procedural fallback)
    {
        m_manifestPath = std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
        const std::string& manifestPath = m_manifestPath;
        if (!m_layers.load(manifestPath)) {
            spdlog::critical("cannot load {} - run 'ninja -C build world' to bake assets",
                             manifestPath);
            return false;
        }
        const auto& st = m_layers.stats();
        spdlog::info("SVO world synthesized from layers: {} nodes, {} bricks,"
                     " {}/{} chunks, {:.1f} MB",
                     st.nodes, st.bricks, st.activeChunks,
                     vf::voxel::GRID_N * vf::voxel::GRID_N * vf::voxel::GRID_N,
                     double(st.memoryBytes) / (1024.0 * 1024.0));
        {
            if (!m_svoPass.init(m_ctx))
                return false;
            const auto& g = m_layers.gpu();
            m_svoPass.setWorld(g);
            // every layer (incl. MCP-added ai_edits) is listed in the GUI
            syncWorldLayerList();
            rescanWorldLayers();
        }

        m_pushB = glm::vec4(vf::voxel::WORLD, vf::voxel::VOXEL, float(vf::voxel::GRID_N), 0);
    }

    // field-derived GPU textures (terrain heights/materials + object shadows)
    if (!uploadTerrainTexture() || !uploadObjVolTexture())
        return false;

    if (!m_taaPass.init(m_ctx))
        return false;
    if (!m_postPass.init(m_ctx))
        return false;
    if (!m_splatPass.init(m_ctx))
        return false;

    // photorealism passes
    if (!m_ssrPass.init(m_ctx)) return false;
    if (!m_ssaoPass.init(m_ctx)) return false;
    if (!m_volFogPass.init(m_ctx)) return false;
    if (!m_motionBlurPass.init(m_ctx)) return false;
    if (!m_dofPass.init(m_ctx)) return false;
    if (!m_envPass.init(m_ctx)) return false;

    // optional PNG texture atlas (world.json "textures" table; a missing
    // table is not an error - the palette path stays bit-exact). Both
    // pipelines must be initialised before the descriptors are written.
    if (!m_texAtlas.load(m_ctx, m_manifestPath))
        spdlog::warn("texture atlas: unavailable, using the palette path");
    m_splatPass.setTexAtlas(m_texAtlas.view(), m_texAtlas.sampler(),
                            m_texAtlas.tableUbo());
    m_svoPass.setTexAtlas(m_texAtlas.view(), m_texAtlas.sampler(),
                          m_texAtlas.tableUbo());
    // seed the GUI picker from the manifest + whatever is on disk
    vf::voxel::worldfile::loadTextureManifest(m_manifestPath, m_texBindings);
    rescanTextureFiles();
    // headless hook: stage one GUI pick (VF_TEST_TEX_SWAP="mat,file", empty
    // file = palette) so a --shot run exercises the exact pending-apply path
    // the picker uses: write world.json + re-upload the atlas on frame 1.
    if (const char* ts = getenv("VF_TEST_TEX_SWAP"); ts && *ts) {
        char* endp = nullptr;
        const long mat = strtol(ts, &endp, 10);
        if (endp && *endp == ',' && mat >= 0 && mat < 21) {
            const std::string file = endp + 1;
            setTextureBinding(int(mat), file);
            m_texApplyPending = true;
            spdlog::info("VF_TEST_TEX_SWAP: material {} -> '{}'", mat,
                         file.empty() ? "(palette)" : file);
        } else {
            spdlog::warn("VF_TEST_TEX_SWAP: expected \"mat,file\" (got '{}')", ts);
        }
    }

    if (!createOffscreen(m_swapchain.extent().width, m_swapchain.extent().height))
        return false;

    // Micro-surfel detail is baked into the surfel stream, so its launch-time
    // override has to be read BEFORE the first bake (the M key flips the same
    // member at runtime and re-runs the surfelizer).
    if (const char* e = getenv("VF_MICRO"))
        m_microDetail = atoi(e) != 0;
    // Hard-edge fit and crease bridges are baked into the stream. Read launch
    // overrides before the first surfel pass; the GUI uses the same members
    // and requests a reload after a committed edit.
    if (const char* e = getenv("VF_EDGE_SHRINK"))
        m_edgeShrink = std::clamp(float(atof(e)), 0.0f, 1.0f);
    if (const char* e = getenv("VF_EDGE_FILL"))
        m_edgeFill = atoi(e) != 0;

    // splat backend owns the primary view: build surfels from the live field
    m_renderMode = (m_args.mode == "svo") ? RenderMode::Svo : RenderMode::Splats;
    if (m_args.mode != "splat" && m_args.mode != "svo")
        spdlog::warn("--mode '{}' unknown (use splat|svo), defaulting to splat", m_args.mode);
    // A headless --mode svo run never rasterizes surfels, so the ~2 s bake is
    // skipped (interactive SVO still builds them: the F flip to splats needs
    // a valid buffer to draw).
    const bool headlessAtInit = m_args.selftest || m_args.smokeFrames > 0 ||
                                !m_args.shot.empty() || !m_args.shots.empty();
    if (m_renderMode == RenderMode::Splats || !headlessAtInit)
        rebuildSurfels();

    // LiveEditor first-touch seeding: splice from the GPU's current chunk run
    // (base surfels) instead of re-baking the whole chunk, so painting starts
    // instantly even in chunks the stroke enters for the first time.
    m_liveEditor.setSeedSource([this](int ci) {
        std::vector<uint8_t> bytes;
        uint32_t edgeCount = 0;
        const uint32_t n = m_splatPass.readChunkSurfels(
            uint32_t(ci), bytes, &edgeCount);
        vf::voxel::SurfelRange range;
        const uint32_t parentCount = n - std::min(n, edgeCount);
        range.surfels.resize(parentCount);
        range.edgeSurfels.resize(n - parentCount);
        if (parentCount)
            std::memcpy(range.surfels.data(), bytes.data(),
                        size_t(parentCount) * sizeof(vf::voxel::Surfel));
        if (n > parentCount)
            std::memcpy(range.edgeSurfels.data(),
                        bytes.data() + size_t(parentCount) * sizeof(vf::voxel::Surfel),
                        size_t(n - parentCount) * sizeof(vf::voxel::Surfel));
        return range;
    });
    loadStoreOverlay(); // restore the async-saved live-edit overlay, if any

    // VF_DUMP_CHUNK=N: read one chunk's base surfels back from the GPU and
    // report the tan_aspect.w (texture override) distribution - a quick
    // sanity check that the phase-2 byte actually arrives on the GPU
    if (const char* dc = getenv("VF_DUMP_CHUNK")) {
        const int ci = atoi(dc);
        std::vector<uint8_t> bytes;
        const uint32_t n = m_splatPass.readChunkSurfels(uint32_t(ci), bytes);
        std::vector<vf::voxel::Surfel> sf(n);
        if (n)
            std::memcpy(sf.data(), bytes.data(), size_t(n) * sizeof(*sf.data()));
        int nonzero = 0;
        for (const auto& s : sf)
            if (s.tan_aspect.w > 0.5f)
                ++nonzero;
        spdlog::info("VF_DUMP_CHUNK {}: {} surfels, tan_aspect.w>0.5 on {}",
                     ci, n, nonzero);
    }

    // frame sync ---------------------------------------------------------
    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_ctx.graphicsFamily();
    if (vkCreateCommandPool(m_ctx.device(), &pci, nullptr, &m_framePool) != VK_SUCCESS)
        return false;

    ensureAcquireSemaphores();

    m_frames.resize(kMaxFramesInFlight);
    for (auto& f : m_frames) {
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateSemaphore(m_ctx.device(), &si, nullptr, &f.imageAvailable);
        vkCreateSemaphore(m_ctx.device(), &si, nullptr, &f.renderDone);
        vkCreateFence(m_ctx.device(), &fi, nullptr, &f.inFlight);

        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_framePool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_ctx.device(), &ai, &f.cmd);
    }

    // GPU timestamp profiling pool: kProfMarks per frame slot
    VkPhysicalDeviceProperties pp {};
    vkGetPhysicalDeviceProperties(m_ctx.physicalDevice(), &pp);
    m_profPeriodNs = double(pp.limits.timestampPeriod);
    VkQueryPoolCreateInfo qpi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = kProfMarks * kMaxFramesInFlight;
    if (vkCreateQueryPool(m_ctx.device(), &qpi, nullptr, &m_profPool) != VK_SUCCESS) {
        m_profPool = VK_NULL_HANDLE; // profiling is best-effort, never fatal
        spdlog::warn("timestamp query pool unavailable - GPU profiling disabled");
    }
    return true;
}

void App::syncWorldLayerList()
{
    if (!m_layers.loaded())
        return;
    // every manifest layer shows up in the GUI, plus previously discovered
    // folder entries that are still unlisted (shown as "(new)")
    std::vector<vf::voxel::worldfile::WorldLayer> l = m_layers.layers();
    for (const auto& u : m_worldLayers)
        if (!u.listed && std::none_of(l.begin(), l.end(),
                                      [&](const vf::voxel::worldfile::WorldLayer& e) {
                                          return e.file == u.file;
                                      }))
            l.push_back(u);
    // enforce unique ids: one row per file
    auto dupEnd = std::unique(l.begin(), l.end(),
                              [](const vf::voxel::worldfile::WorldLayer& a,
                                 const vf::voxel::worldfile::WorldLayer& b) {
                                  return a.file == b.file;
                              });
    l.erase(dupEnd, l.end());
    m_worldLayers = std::move(l);
}

bool App::uploadTerrainTexture()
{
    const std::vector<glm::vec2>& htx = m_layers.field().heightTexture();
    const uint32_t lat = uint32_t(m_layers.field().latN());
    // The height texture is constant-size (latN x latN); create it once and
    // only re-upload contents on reload. Recreating would invalidate the
    // VkImageView bound by the (once-written) descriptor set, causing the
    // whole scene to read freed memory and crash on repeated toggles.
    if (m_heightImg.img == VK_NULL_HANDLE) {
        m_heightImg = vf::makeImage3D(m_ctx, lat, lat, 1,
                                      VK_FORMAT_R32G32_SFLOAT,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_heightImg.img)
            return false;
    }
    if (!vf::uploadToImage3D(m_ctx, m_heightImg, htx.data(),
                             htx.size() * sizeof(glm::vec2)))
        return false;
    m_heightCpu = htx; // mirror for live-edit patches (patchHeightTexture)
    m_ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_heightImg.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    });
    m_svoPass.setHeightmapView(m_heightImg.view);
    return true;
}

bool App::uploadObjVolTexture()
{
    const auto& ov = m_layers.field().objectVolume();
    const int n = vf::voxel::VoxelField::kObjVolN;
    // Constant-size (kObjVolN^3) volume; create once and re-upload only.
    // Recreating would free the VkImageView still referenced by the (once-
    // written) descriptor set, producing stale reads and memory exceptions.
    if (m_objVolImg.img == VK_NULL_HANDLE) {
        m_objVolImg = vf::makeImage3D(m_ctx, uint32_t(n), uint32_t(n), uint32_t(n),
                                      VK_FORMAT_R8_SNORM,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_objVolImg.img)
            return false;
    }
    if (!vf::uploadToImage3D(m_ctx, m_objVolImg, ov.data(), ov.size()))
        return false;
    m_ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_objVolImg.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    });
    m_svoPass.setObjVolumeView(m_objVolImg.view);
    return true;
}

// Reload the texture atlas when the manifest or any declared PNG changed.
// The atlas image/view are stable (fixed layer count), so this re-uploads
// pixels + the mip chain and re-writes the UBO table, but never touches the
// descriptors (setTexAtlas stays a once-only write).
bool App::reloadTexAtlas()
{
    if (!m_texAtlas.valid())
        return false;
    // In-flight frames still sample the atlas image: idle before rewriting
    // its pixels + layout. Callers that already idled pay nothing extra.
    vkDeviceWaitIdle(m_ctx.device());
    if (!m_texAtlas.load(m_ctx, m_manifestPath))
        return false;
    return true;
}

// Scan assets/textures/ for swap-in material textures (PNG or JPG). The list
// only feeds the GUI picker; nothing is uploaded until a material selects it.
void App::rescanTextureFiles()
{
    m_texFiles.clear();
    const std::string dir = std::string(VOXELFORGE_ASSET_DIR) + "/textures";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file())
            continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            continue;
        m_texFiles.push_back("textures/" + e.path().filename().string());
    }
    std::sort(m_texFiles.begin(), m_texFiles.end());
}

// Resolve a manifest texture path the same way TexAtlas::load does: absolute
// paths verbatim, anything else against the manifest directory (assets/).
static std::string resolveTexPath(const std::string& manifestPath,
                                  const std::string& file)
{
    if (!file.empty() && (file[0] == '/' ||
                          (file.size() > 1 && file[1] == ':')))
        return file;
    std::string dir = manifestPath;
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    return dir + file;
}

// One picker action: bind `file` (manifest-relative, e.g. "textures/x.png")
// to `mat`, or unbind with an empty file. Shared by the GUI combo and the
// VF_TEST_TEX_SWAP hook so both drive exactly the same table.
void App::setTextureBinding(int mat, const std::string& file)
{
    auto it = std::find_if(m_texBindings.begin(), m_texBindings.end(),
                           [&](const vf::voxel::worldfile::TextureBinding& b) {
                               return b.mat == mat;
                           });
    if (file.empty()) {
        if (it != m_texBindings.end())
            m_texBindings.erase(it);
    } else if (it != m_texBindings.end()) {
        it->file = file;
    } else {
        m_texBindings.push_back({ file, mat, 0.5f });
    }
}

// Hot-swap watch, polled ~1/s:
//  - the folder list follows drop-ins without a manual Rescan;
//  - a bound image re-exported on disk (same path, new mtime/size) re-uploads
//    the atlas, so an art-tool save is visible live.
void App::pollTextureFiles()
{
    std::vector<std::string> files;
    const std::string texDir = std::string(VOXELFORGE_ASSET_DIR) + "/textures";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(texDir, ec)) {
        if (!e.is_regular_file())
            continue;
        std::string ext = e.path().extension().string();
        for (char& c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
            continue;
        files.push_back("textures/" + e.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    if (files != m_texFiles)
        m_texFiles = std::move(files);

    std::map<std::string, unsigned long long> sigs;
    for (const auto& b : m_texBindings) {
        const std::string path = resolveTexPath(m_manifestPath, b.file);
        std::error_code fec;
        const auto t = std::filesystem::last_write_time(path, fec);
        const unsigned long long mt = fec
            ? 0ull
            : static_cast<unsigned long long>(t.time_since_epoch().count());
        const unsigned long long sz = static_cast<unsigned long long>(
            fec ? 0ull : std::filesystem::file_size(path, fec));
        sigs[path] = mt ^ (sz * 0x9E3779B97F4A7C15ull);
    }
    for (const auto& kv : sigs) {
        auto it = m_texSig.find(kv.first);
        if (it != m_texSig.end() && it->second != kv.second) {
            spdlog::info("texture hot-swap: '{}' changed on disk, re-uploading",
                         kv.first);
            m_texReloadPending = true;
            break;
        }
    }
    m_texSig = std::move(sigs);
}

// Persist the picker's table and re-upload the atlas. Only the top-level
// "textures" array is rewritten (worldfile::writeTextureManifest preserves the
// layers and every unknown key verbatim), so swapping a texture can never
// disturb the world.
void App::applyTextureBindings()
{
    std::sort(m_texBindings.begin(), m_texBindings.end(),
              [](const vf::voxel::worldfile::TextureBinding& a,
                 const vf::voxel::worldfile::TextureBinding& b) {
                  return a.mat < b.mat;
              });
    if (!vf::voxel::worldfile::writeTextureManifest(m_manifestPath, m_texBindings))
        spdlog::warn("texture picker: could not write '{}'", m_manifestPath);
    reloadTexAtlas();
    // the atlas now matches disk: seed the mtime watch so the next poll does
    // not re-upload what the pick itself just uploaded
    pollTextureFiles();
}

void App::applyWorldReload()
{
    if (!m_layers.loaded())
        return;
    // A reload replaces the runtime store generation. Cell-state inverse
    // edits from the previous generation must never be replayed against the
    // new world, and an in-flight drag cannot be completed across that swap.
    // Flush first so loadStoreOverlay() below sees the latest queued stroke
    // rather than an older snapshot still being written by the worker.
    if (m_hasStamp || !m_undoPending.empty())
        spdlog::warn("undo: aborting active stroke across world reload");
    m_overlayWriter.flush();
    m_undo.clear();
    m_undoPending.clear();
    m_undoOverflow = false;
    m_strokeRiseCells = 2;
    m_hasStamp = false;
    m_dragging = false;
    // Preserve the current physical button state so a reload during a held
    // click cannot manufacture a fresh stamp edge on the next frame.
    m_lmbWasDown =
        glfwGetMouseButton(m_window.handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    m_ctrlWasDown =
        glfwGetKey(m_window.handle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(m_window.handle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    // swap the freshly synthesized SVO buffers under an idle device
    vkDeviceWaitIdle(m_ctx.device());
    const auto& g = m_layers.gpu();
    m_svoPass.setWorld(g);
    uploadTerrainTexture(); // layer toggles can change materials too
    uploadObjVolTexture();  // keep AI/object shadows in sync with the SVO
    reloadTexAtlas();       // world.json "textures" edits ride the same poll
    // an external manifest edit must not leave a stale picker table behind
    // (a pending GUI pick is applied after this, so it still wins the frame)
    vf::voxel::worldfile::loadTextureManifest(m_manifestPath, m_texBindings);
    rebuildSurfels();       // splat backend follows the same live field
    // the reload re-adopted the store: re-apply the persisted live overlay
    m_liveEditor.clear();
    m_overlayLoaded = false;
    loadStoreOverlay();
    syncWorldLayerList();
    rescanWorldLayers(); // layers dropped into assets/ while running show up too
    // Keep m_rotateLayer alive across the rebuild: it is the selected
    // interaction target, not a transient mouse-down state.
    if (m_rotationPreviewPending) {
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        m_rotationPreviewPending = false;
        m_taaFirstFrame = true;
    } else {
        // An unrelated layer/manifest reload invalidates an uncommitted draft;
        // never silently apply a stale staged delta to the new world.
        if (m_rotationStaged)
            cancelRotation();
        if (m_moveStaged)
            cancelMove();
    }
}

// Live-edit store overlay path: assets/runtime_edits.vxw, overridable so
// headless checks can keep a session's painting untouched (VF_OVERLAY_PATH).
static std::string overlayPath()
{
    if (const char* p = getenv("VF_OVERLAY_PATH"); p && *p)
        return p;
    return std::string(VOXELFORGE_ASSET_DIR) + "/runtime_edits.vxw";
}

void App::loadStoreOverlay()
{
    if (m_overlayLoaded)
        return;
    m_overlayLoaded = true;
    // Test/debug switch: ignore a saved live-edit overlay so gates run against
    // the baked world (a session's painted edits would otherwise change every
    // shot). The app still saves new strokes normally.
    if (getenv("VF_NO_OVERLAY")) {
        spdlog::info("live overlay: skipped (VF_NO_OVERLAY)");
        return;
    }
    const std::string path = overlayPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return;
    auto& store = m_layers.store();
    if (!store.loadOverlay(path)) {
        spdlog::warn("live overlay '{}' could not be applied", path);
        return;
    }
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // Same micro-detail default as the full bake: restored chunks keep their
    // micro tail (VF_MICRO=0 disables).
    sp.microDetail = m_microDetail;
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    const std::vector<int> chunks = store.editedChunks();
    size_t n = 0;
    for (int ci : chunks) {
        // Start from what the GPU already renders (base surfels), then refresh
        // exactly the region the edit touched — the same splice a live stamp
        // would have produced, so the restored frame matches the session.
        // (skipped in --mode svo: the splat backend has no surfel data)
        if (m_renderMode == RenderMode::Splats) {
            m_liveEditor.chunkSurfels(ci, sp); // seeds (GPU source or store fallback)
            int lo[3], hi[3];
            if (store.chunkEditBounds(ci, lo, hi)) {
                // Exact margin: the persisted stroke solved the same store
                // band a live stamp did, so the restored run matches the
                // session (Smooth especially).
                const int m = vf::voxel::LiveEditor::kExactStampMargin;
                m_liveEditor.refreshRegion(ci, glm::ivec3(lo[0] - m, lo[1] - m, lo[2] - m),
                                           glm::ivec3(hi[0] + m + 1, hi[1] + m + 1,
                                                      hi[2] + m + 1), sp);
            } else {
                m_liveEditor.seedFromStore(ci, sp);
            }
            const std::vector<vf::voxel::Surfel>& surfels =
                m_liveEditor.chunkRun(ci, sp);
            n += surfels.size();
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.microStartOf(ci),
                                          m_liveEditor.edgeCountOf(ci));
        }
        // the restored edit also changed the terrain surface the water
        // shading reads: re-derive its height-texture window
        {
            int lo[3], hi[3];
            if (store.chunkEditBounds(ci, lo, hi))
                // Restoration may contain a Smooth raise larger than the
                // current cached top; scan from the world top for correctness.
                patchHeightTexture(lo[0] - 3, lo[2] - 3, hi[0] + 3, hi[2] + 3,
                                   store.latN());
        }
        const auto& pool = store.pool(ci);
        if (pool)
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    spdlog::info("live overlay: restored {} chunks ({} run surfels) from {}", chunks.size(),
                 n, path);
}

void App::recordPhotorealism(VkCommandBuffer cmd, const vf::RaymarchPush& push)
{
    // In-place LDR chain on m_offscreen (GENERAL layout throughout).
    // Order: SSR adds reflections -> SSAO grounds contact areas -> volumetric
    // fog hazes valleys -> motion blur smears camera movement -> DoF pulls
    // focus. TAA (interactive) resolves afterwards.
    const uint32_t W = m_offscreen.extent.width, H = m_offscreen.extent.height;
    auto barrier = [&] {
        vf::transitionImage(cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    if ((m_renderFlags & (1 << 5)) != 0) {
        barrier();
        m_ssrPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_gnorm.view,
                                    m_offscreen.view);
        m_ssrPass.record(cmd, W, H, push);
    }
    if ((m_renderFlags & (1 << 6)) != 0) {
        // scratch AO target: contents are per-frame, discard-and-transition
        vf::transitionImage(cmd, m_ssaoAo.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        barrier();
        m_ssaoPass.updateDescriptors(m_gpos.view, m_gnorm.view, m_ssaoAo.view,
                                     m_offscreen.view, m_offscreen.view);
        m_ssaoPass.record(cmd, W, H, push,
                          glm::vec4(float(m_ssaoDebug), m_ssaoStrength,
                                    m_ssaoRadius, m_ssaoBlur ? 1.0f : 0.0f));
    }
    if (m_volFogEnabled) {
        barrier();
        m_volFogPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_volFogPass.record(cmd, W, H, push);
    }
    if (m_motionBlurEnabled) {
        barrier();
        m_motionBlurPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_motionBlurPass.record(cmd, W, H, push, m_prevCam);
    }
    if (m_dofEnabled) {
        barrier();
        m_dofPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_dofPass.record(cmd, W, H, push, m_dofFocusDist, m_dofFocalLength);
    }
}

void App::rebuildSurfels()
{
    if (!m_layers.loaded())
        return;
    vkDeviceWaitIdle(m_ctx.device());
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // micro-detail: texture texels as real micro-surfel geometry (moss,
    // pebbles, bark relief, leaflets). VF_MICRO=0 disables for perf/debug.
    sp.microDetail = m_microDetail;
    // LOD rings: baked 2x2x2 / 4x4x4 merged-terrain surfel runs per chunk;
    // the renderer picks a ring per chunk by distance (VF_LOD1/VF_LOD2).
    sp.lodRings = true;
    if (const char* e = getenv("VF_LOD"))
        sp.lodRings = atoi(e) != 0;
    // material-split LOD merging (VF_LOD_SPLIT=0: single majority disk)
    sp.lodMaterialSplit = true;
    if (const char* e = getenv("VF_LOD_SPLIT"))
        sp.lodMaterialSplit = atoi(e) != 0;
    // anisotropic footprints: disks stretch along local creases (VF_ANISO=0)
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    // debug/experiment overrides for the surfel bake (default = tuned values)
    if (const char* e = getenv("VF_SURFEL_SMOOTH"))
        sp.smoothNormals = atoi(e) != 0;
    if (const char* e = getenv("VF_SURFEL_HFBLEND"))
        sp.terrainHeightfieldNormals = atoi(e) != 0;
    vf::voxel::SurfelSet set = vf::voxel::buildSurfels(m_layers.field(), sp);
    // The water is ONE fixed-level plane subdivided into a world-wide 0.2 m
    // grid of coplanar surfels (coverage only - the shader intersects the
    // analytic plane per fragment). The depth test against the opaque prepass
    // hides the cells standing on dry land and objects, so a live dig below
    // the level is water with no per-column bookkeeping and every water
    // fragment shades identically. Bucket the run per chunk (deterministic
    // order) so the draw code can frustum-cull.
    m_waterSurfels = vf::voxel::buildWaterSurfels();
    const uint32_t waterStart = uint32_t(set.surfels.size());
    m_waterRel = updateWaterBuckets();
    std::vector<uint32_t> waterRange(m_waterRel.size(), waterStart);
    for (size_t i = 0; i < m_waterRel.size(); ++i)
        waterRange[i] = waterStart + m_waterRel[i];
    set.surfels.resize(set.surfels.size() + m_waterSurfels.size());
    std::copy(m_waterSurfels.begin(), m_waterSurfels.end(),
              set.surfels.begin() + waterStart);
    // chunkRange only covers opaque surfels; waterRange buckets the trailing
    // water run per chunk for frustum-culled water draws; microStart splits
    // each chunk into base + micro-detail for distance culling
    m_splatPass.setSurfels(set.surfels.data(),
                           set.surfels.size() * sizeof(vf::voxel::Surfel),
                           set.surfels.size(), set.chunkRange, waterStart, waterRange,
                           set.microStart, set.edgeStart, set.lod1Range, set.lod2Range,
                           set.objectChunks);
    spdlog::info("splat backend: {} surfels ({} water, {} edge bridges on {} parents), "
                 "{} chunks, lod1 {} lod2 {}",
                 set.surfels.size(), m_waterSurfels.size(), set.edgeBridgeCount,
                 set.edgeParentCount,
                 set.chunkRange.empty() ? 0 : set.chunkRange.size() - 1,
                 set.lod1Count, set.lod2Count);
}

// The water-plane run is a world-wide 0.2 m grid of coplanar surfels. Keep it
// chunk-bucketed (ascending chunk) so the draw code can frustum-cull.
std::vector<uint32_t> App::updateWaterBuckets()
{
    constexpr uint32_t kChunks = 16 * 16 * 16;
    m_waterRel.assign(kChunks + 1, 0);
    auto chunkOf = [](const vf::voxel::Surfel& s) {
        const float px = s.pos_rU.x, py = s.pos_rU.y, pz = s.pos_rU.z;
        const auto ax = std::clamp(int(std::floor((px + 51.2f) / 6.4f)), 0, 15);
        const auto ay = std::clamp(int(std::floor((py + 51.2f) / 6.4f)), 0, 15);
        const auto az = std::clamp(int(std::floor((pz + 51.2f) / 6.4f)), 0, 15);
        return uint32_t(vf::voxel::chunkIndexOf(ax, ay, az));
    };
    if (m_waterSurfels.empty())
        return m_waterRel;
    std::vector<uint32_t> order(m_waterSurfels.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return chunkOf(m_waterSurfels[a]) < chunkOf(m_waterSurfels[b]);
    });
    std::vector<vf::voxel::Surfel> sorted;
    sorted.reserve(m_waterSurfels.size());
    uint32_t open = chunkOf(m_waterSurfels[order[0]]);
    m_waterRel[open] = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint32_t c = chunkOf(m_waterSurfels[order[k]]);
        if (c != open) {
            for (uint32_t f = open + 1; f <= c; ++f)
                m_waterRel[f] = uint32_t(k);
            open = c;
        }
        sorted.push_back(m_waterSurfels[order[k]]);
    }
    for (uint32_t f = open + 1; f < m_waterRel.size(); ++f)
        m_waterRel[f] = uint32_t(order.size());
    m_waterSurfels.swap(sorted);
    return m_waterRel;
}

// Live edits change the terrain surface, and several shader paths read the
// records-derived height texture (rg32f: R = top world Y, G = material/255):
// the water shading (shore foam, absorption, the reflected-bed march) and the
// splat shadow/AO marches (softShadowSplat, splatAO, splatSceneDist). The bake
// fills it once, so without this a carve would leave it stale - the water over
// a dug channel would still read "land above the plane" and shade as a thin
// foam-washed sheet instead of the same water as the river.
// Re-derive the affected lattice columns from the runtime store (top terrain
// cell -> top face + material, same rule as storeTerrainTopY) and re-upload
// that sub-rect. `riseCells` bounds how far above the old top the new surface
// can sit (the Add dome: its depth); the scan starts there and walks down, so
// it costs brush-sized work, not a full-column sweep. Undo may pass a larger
// bound derived from inverse Set cells when the post-stamp surface is lower.
void App::patchHeightTexture(int x0, int z0, int x1, int z1, int riseCells)
{
    auto& store = m_layers.store();
    const int latN = store.latN();
    if (latN <= 0 || m_heightCpu.size() != size_t(latN) * latN)
        return;
    x0 = std::clamp(x0, 0, latN - 1); x1 = std::clamp(x1, 0, latN - 1);
    z0 = std::clamp(z0, 0, latN - 1); z1 = std::clamp(z1, 0, latN - 1);
    if (x1 < x0 || z1 < z0)
        return;
    const float half = 0.5f * vf::voxel::WORLD;
    int topMost = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            // current (kept in sync) top -> lattice, then scan past the rise
            const float oldTop = m_heightCpu[size_t(z) * latN + size_t(x)].x;
            int y = (oldTop > -1e29f)
                        ? int(std::floor((oldTop + half) / vf::voxel::VOXEL)) - 1
                        : latN - 1;
            y = std::min(latN - 1, y + std::max(riseCells, 0));
            float topY = -1e30f;
            uint8_t mat = 0;
            for (; y >= 0; --y) {
                const vf::voxel::StoreCell c = store.cellAt(x, y, z);
                if (c.solid && !c.obj) {
                    topY = -half + (float(y) + 1.0f) * vf::voxel::VOXEL;
                    mat = c.mat;
                    break;
                }
            }
            m_heightCpu[size_t(z) * latN + size_t(x)] =
                glm::vec2(topY, float(mat) / 255.0f);
            topMost = std::max(topMost, y);
        }
    if (m_heightImg.img == VK_NULL_HANDLE)
        return;
    const uint32_t w = uint32_t(x1 - x0 + 1), h = uint32_t(z1 - z0 + 1);
    std::vector<glm::vec2> rect(size_t(w) * h);
    for (uint32_t z = 0; z < h; ++z)
        std::memcpy(rect.data() + size_t(z) * w,
                    m_heightCpu.data() + size_t(z0 + z) * latN + size_t(x0),
                    size_t(w) * sizeof(glm::vec2));
    if (!vf::uploadSubImage3D(m_ctx, m_heightImg, rect.data(), uint32_t(x0),
                              uint32_t(z0), w, h, sizeof(glm::vec2)))
        spdlog::warn("live edit: height-texture patch {}x{} at ({},{}) failed", w, h,
                     x0, z0);
}

// The edit brush: rasterize the volume (reusing the layer rasterizers), apply
// the cells to the runtime store, rebuild only the dirty chunks, then
// regenerate their surfels and patch them into the GPU buffers. This is the
// only edit path - the live store is the edit truth (the legacy carve/raise
// record layers are no longer written).
// Resolve the exact object layer carried by the picked VoxelField cell.
// Returns an empty string for terrain or live geometry with no base-file owner.
std::string App::rotateTargetLayer(const vf::voxel::PickHit& hit) const
{
    if (!m_layers.loaded() || !hit.hit || !hit.object || hit.layer == 0)
        return {};
    return m_layers.layerFile(hit.layer);
}

// Apply the accumulated trackball-ring rotation to the active layer and
// persist it once on release. The object stays activated so another ring drag
// can be made without selecting it again.
void App::commitRotation()
{
    if (m_rotateLayer.empty())
        return;
    m_rotationStaged = false;
    if (std::abs(m_rotateDy) < 1e-4f && std::abs(m_rotateDx) < 1e-4f &&
        std::abs(m_rotateDz) < 1e-4f) {
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        m_rotateLastMouse = {};
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        return;
    }
    const auto it = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) { return l.file == m_rotateLayer; });
    if (it == m_worldLayers.end()) {
        spdlog::warn("rotate: layer '{}' no longer in the manifest", m_rotateLayer);
        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
        m_rotateLayer.clear();
        m_rotating = false;
        m_rotateHandle = TrackballHandle::None;
        m_rotateLastMouse = {};
        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        return;
    }
    // add to whatever placement the layer already had (drag is a delta)
    it->rotDeg += m_rotateDy;
    it->rotX   += m_rotateDx;
    it->rotZ   += m_rotateDz;
    it->listed = true;
    persistWorldLayers();
    spdlog::info("rotate: {} -> yaw {:+.1f} pitch {:+.1f} roll {:+.1f}",
                 m_rotateLayer, it->rotDeg, it->rotX, it->rotZ);
    // Keep the final GPU preview active until the rebuilt world is swapped in;
    // disabling it here would flash the old pose during the async rebuild.
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
    m_rotationPreviewPending = true;
    m_taaFirstFrame = true;
    m_rotating = false;
    m_rotateHandle = TrackballHandle::None;
    m_rotateLastMouse = {};
    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
}

void App::cancelRotation()
{
    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
    m_rotating = false;
    m_rotationStaged = false;
    m_rotateHandle = TrackballHandle::None;
    m_rotateLastMouse = {};
    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
}

void App::commitMove()
{
    if (m_moveLayer.empty())
        return;
    if (glm::length(m_moveDelta) < 1e-4f) {
        m_moveStaged = false;
        return;
    }
    const auto it = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) {
            return l.file == m_moveLayer;
        });
    if (it == m_worldLayers.end()) {
        spdlog::warn("move: layer '{}' no longer in the manifest", m_moveLayer);
        cancelMove();
        m_moveLayer.clear();
        return;
    }
    it->pos[0] += m_moveDelta.x;
    it->pos[1] += m_moveDelta.y;
    it->pos[2] += m_moveDelta.z;
    it->listed = true;
    persistWorldLayers();
    spdlog::info("move: {} -> pos ({:+.2f}, {:+.2f}, {:+.2f})",
                 m_moveLayer, it->pos[0], it->pos[1], it->pos[2]);
    m_pendingWorldReload = false;
    m_layers.requestReload(m_camera.pos, false);
    m_rotationPreviewPending = true; // keep the final translation visible
    m_moveStaged = false;
    m_moving = false;
    m_moveDelta = glm::vec3(0.f);
    m_moveLastMouse = {};
    m_taaFirstFrame = true;
}

void App::cancelMove()
{
    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
    m_moving = false;
    m_moveStaged = false;
    m_moveDelta = glm::vec3(0.f);
    m_moveLastMouse = {};
}

void App::applyEditLive()
{
    if (!m_hoverHit.hit)
        return;
    glm::vec3 n = m_hoverHit.normal;
    if (glm::length(n) < 1e-3f)
        n = glm::vec3(0.f, 1.f, 0.f);
    n = glm::normalize(n);
    // Keep malformed headless values out of float-to-int conversions and the
    // analytic rasterizers. Interactive values already arrive clamped. The
    // floor is half a voxel, not 0.1 m, so a 1-voxel brush stays 1 voxel wide
    // (a radius clamp at 0.1 m silently made it 3 cells across).
    const float diameter = std::isfinite(m_editDiameter) ? m_editDiameter : 2.0f;
    const float radius = std::clamp(diameter * 0.5f, vf::voxel::VOXEL * 0.5f, 16.0f);
    const float length = std::clamp(
        std::isfinite(m_editDepth) ? m_editDepth : 1.5f, vf::voxel::VOXEL, 12.0f);
    // Per-voxel mode: a 1-voxel Add/Carve touches exactly one cell, so it skips
    // the volume rasterizers entirely (a radius-0.05 dome would still pick up
    // the cell above along the normal).
    const bool perVoxel =
        brushIsPerVoxel() && (m_editBrush == EditBrush::Add ||
                              m_editBrush == EditBrush::Carve);

    auto& store = m_layers.store();
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);

    std::vector<vf::voxel::StoreEdit> edits;
    int riseCells = 2;
    // Smooth refreshes its splats over the full store band (the object
    // relaxation changes neighbouring normals/AO beyond the brush), every
    // other brush keeps the cheap +-3 margin.
    int refreshMargin = vf::voxel::LiveEditor::kStampMargin;
    if (m_editBrush == EditBrush::Smooth) {
        // Smooth uses a terrain height relaxation for terrain picks and a
        // generalised surface-axis relaxation for object picks. The store
        // helper keeps the two ownership classes separate: terrain never
        // becomes an object, and object surface edits never overwrite terrain.
        vf::voxel::SmoothTerrainEdits smooth =
            store.makeSmoothEdits(m_hoverHit.voxel, radius, m_smoothStrength);
        edits = std::move(smooth.edits);
        riseCells = std::max(2, smooth.riseCells + 2);
        if (smooth.objectSurface)
            refreshMargin = vf::voxel::LiveEditor::kExactStampMargin;
    } else {
        riseCells = int(length / vf::voxel::VOXEL) + 2;
        std::vector<vf::voxel::VoxelRecord> recs;
        if (perVoxel) {
            // exactly one cell: Carve removes the voxel under the cursor, Add
            // places the one just outside the surface along the normal.
            recs = m_add.makeSingleVoxel(m_hoverHit.voxel, n, m_editMat,
                                         m_editBrush == EditBrush::Add);
        } else {
            switch (m_editBrush) {
            case EditBrush::Carve:
                recs = m_carve.makeOrientedCylinder(
                    m_hoverHit.voxel, -n, radius, length, m_editMat,
                    /*carve=*/true);
                break;
            case EditBrush::Add:
                recs = m_add.makeDome(m_hoverHit.voxel, n, radius, length,
                                      m_editMat);
                break;
            case EditBrush::Rotate:
            case EditBrush::Move:
                return; // object transforms are handled by their gizmos
            case EditBrush::Delete:
            case EditBrush::Paint:
                // brush ball centred on the hit cell: delete clears it, paint
                // recolours
                recs = m_carve.makeSphere(m_hoverHit.voxel, radius, m_editMat);
                break;
            case EditBrush::Smooth:
                return; // handled above; keeps the switch exhaustive
            }
        }
        if (recs.empty())
            return;

        edits.reserve(recs.size());
        for (const vf::voxel::VoxelRecord& r : recs) {
            vf::voxel::StoreEdit e;
            e.x = r.x;
            e.y = r.y;
            e.z = r.z;
            e.mode = (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Delete)
                         ? vf::voxel::StoreEdit::Mode::Clear
                     : (m_editBrush == EditBrush::Paint)
                         ? vf::voxel::StoreEdit::Mode::Paint
                         : vf::voxel::StoreEdit::Mode::Set;
            e.mat = r.materialId;
            e.hasColor = true;
            e.r = r.r; e.g = r.g; e.b = r.b;
            e.reflectivity = r.reflectivity;
            e.roughness = r.roughness;
            edits.push_back(e);
        }
    }
    if (edits.empty())
        return;

    // Paint must not create geometry: the brush ball covers air cells too, so
    // keep only cells that are already solid in the store (Clear is a no-op on
    // air and needs no filter).
    if (m_editBrush == EditBrush::Paint) {
        std::vector<vf::voxel::StoreEdit> solid;
        solid.reserve(edits.size());
        for (vf::voxel::StoreEdit& e : edits)
            if (store.cellAt(e.x, e.y, e.z).solid)
                solid.push_back(e);
        edits.swap(solid);
        if (edits.empty())
            return; // nothing solid in the ball
    }

    // Undo bookkeeping: record the pre-stamp state of every cell this stamp
    // touches (first occurrence wins, so one undo returns the whole stroke to
    // the state before it started). Keep the original terrain/object bit when
    // restoring a cell that was already solid.
    if (!m_undoOverflow) {
        for (const vf::voxel::StoreEdit& e : edits) {
            const uint64_t key = (uint64_t(uint32_t(e.x)) << 42) |
                                 (uint64_t(uint32_t(e.y)) << 21) | uint64_t(uint32_t(e.z));
            const vf::voxel::StoreCell c = store.cellAt(e.x, e.y, e.z);
            vf::voxel::StoreEdit inv;
            inv.x = e.x; inv.y = e.y; inv.z = e.z;
            if (!c.solid) {
                inv.mode = vf::voxel::StoreEdit::Mode::Clear;
            } else {
                inv.mode = vf::voxel::StoreEdit::Mode::Set;
                inv.terrain = !c.obj;
                inv.mat = c.mat;
                inv.tags = c.tags;
                inv.hasColor = true;
                inv.r = c.r; inv.g = c.g; inv.b = c.b;
                inv.reflectivity = c.reflectivity;
                inv.roughness = c.roughness;
            }
            m_undoPending.emplace(key, inv);
        }
        if (m_undoPending.size() > kUndoMaxCells) {
            m_undoOverflow = true;
            m_undoPending.clear();
            spdlog::warn("undo: stroke too large to record, this stroke is not undoable");
        }
    }

    m_strokeRiseCells = std::max(m_strokeRiseCells, riseCells);
    commitStoreEdits(edits, riseCells, "", refreshMargin);
    if (!m_dragging)
        finishStroke(); // a lone stamp (incl. the headless VF_TEST_EDIT) is a stroke
}

// Shared tail of every store mutation (brush stamp and undo): apply the edits,
// rebuild the dirty store regions, re-derive the affected chunks' surfels and
// octree pools, patch the GPU buffers (splat + SVO), refresh the edited
// height-texture columns and update the HUD / trace log.
void App::commitStoreEdits(std::vector<vf::voxel::StoreEdit>& edits, int riseCells,
                           const char* what, int margin)
{
    if (edits.empty())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    auto& store = m_layers.store();
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);

    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // Live patches keep micro detail: LiveEditor regenerates the chunk's micro
    // tail with the same deterministic hash the bake uses (VF_MICRO=0 off).
    sp.microDetail = m_microDetail;
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    std::vector<int> changed = m_liveEditor.stamp(edits, sp, margin);
    const auto t1 = std::chrono::steady_clock::now();

    size_t nRunSurfels = 0;
    for (int ci : changed) {
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        nRunSurfels += surfels.size();
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.microStartOf(ci),
                                          m_liveEditor.edgeCountOf(ci));
        // SVO reference backend follows the same edit (chunk-local pool patch)
        const auto& pool = store.pool(ci);
        if (pool && !getenv("VF_LIVE_NOSVO"))
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    // keep the height texture current for the edited columns: the water plane
    // is already there (one fixed level, world-wide), and the foam /
    // absorption / reflected bed all read this texture, so a stale patch would
    // make the new water shade differently from the river.
    {
        int hx0 = edits[0].x, hx1 = hx0, hz0 = edits[0].z, hz1 = hz0;
        for (const vf::voxel::StoreEdit& e : edits) {
            hx0 = std::min(hx0, e.x); hx1 = std::max(hx1, e.x);
            hz0 = std::min(hz0, e.z); hz1 = std::max(hz1, e.z);
        }
        patchHeightTexture(hx0 - 3, hz0 - 3, hx1 + 3, hz1 + 3, riseCells);
    }
    m_taaFirstFrame = true; // no history across a geometry change
    const auto t2 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    m_lastEditMs = float(ms(t0, t2));
    m_lastEditSurfels = nRunSurfels;
    if (!m_dragging || getenv("VF_TRACE"))
        spdlog::info("live edit{}: {} cells, {} chunks, {} run surfels, {:.1f} ms "
                     "(stamp {:.1f}, gpu {:.1f})",
                     what, edits.size(), changed.size(), nRunSurfels, m_lastEditMs,
                     ms(t0, t1), ms(t1, t2));
}

// Stroke end: collapse the stroke's recorded cells into one undo step so
// Ctrl+Z reverts the stroke the user just painted, not one stamp of it.
void App::finishStroke()
{
    if (!m_undoPending.empty()) {
        std::vector<vf::voxel::StoreEdit> inv;
        inv.reserve(m_undoPending.size());
        size_t nClear = 0;
        for (auto& kv : m_undoPending) {
            if (kv.second.mode == vf::voxel::StoreEdit::Mode::Clear)
                ++nClear;
            inv.push_back(kv.second);
        }

        // The forward edit can only raise a column by `m_strokeRiseCells`.
        // Undo is different: a Smooth/Delete stamp may have lowered a tall
        // spike, and replaying its inverse Set starts that column above the
        // current height texture. Derive the undo scan bound from the recorded
        // pre-stroke cells and the post-stamp tops, rather than assuming that
        // a lowering needs no upward headroom. This also covers a multi-stamp
        // stroke whose cumulative drop is larger than any one stamp.
        int undoRise = m_strokeRiseCells;
        const int latN = m_layers.loaded() ? m_layers.store().latN() : 0;
        if (latN > 0 && m_heightCpu.size() == size_t(latN) * size_t(latN)) {
            const float half = 0.5f * vf::voxel::WORLD;
            for (const vf::voxel::StoreEdit& e : inv) {
                if (e.mode != vf::voxel::StoreEdit::Mode::Set)
                    continue;
                const size_t h = size_t(e.z) * size_t(latN) + size_t(e.x);
                if (h >= m_heightCpu.size())
                    continue;
                const float top = m_heightCpu[h].x;
                if (top <= -1e29f) {
                    // A restored cell in a column with no current top must be
                    // found by a full-column scan.
                    undoRise = std::max(undoRise, latN + 2);
                    continue;
                }
                const int topCell = std::clamp(
                    int(std::floor((top + half) / vf::voxel::VOXEL)) - 1,
                    0, latN - 1);
                undoRise = std::max(undoRise, e.y - topCell + 2);
            }
        } else {
            undoRise = std::max(undoRise, latN + 2);
        }
        m_undo.emplace_back(std::move(inv), undoRise);
        if (m_undo.size() > kUndoDepth)
            m_undo.erase(m_undo.begin());
        spdlog::info("undo: stroke recorded ({} cells: {} clear, {} restore, "
                     "{} steps available, height rise {})",
                     m_undo.back().first.size(), nClear,
                     m_undo.back().first.size() - nClear, m_undo.size(),
                     m_undo.back().second);
    }
    m_undoPending.clear();
    m_strokeRiseCells = 2;
    m_undoOverflow = false;
}

void App::undoEdit()
{
    if (m_undo.empty()) {
        spdlog::info("undo: nothing to undo");
        return;
    }
    std::vector<vf::voxel::StoreEdit> inv = std::move(m_undo.back().first);
    const int rise = m_undo.back().second;
    m_undo.pop_back();
    // Replay the recorded pre-stroke cells through the same store/GPU path a
    // stamp uses, then persist the reverted state like a stroke end does.
    // Restore over the exact store band: an undone Smooth stroke changed
    // normals/AO further than the cheap margin covers, so +-3 would leave
    // stale splats behind.
    commitStoreEdits(inv, rise, " undo",
                     vf::voxel::LiveEditor::kExactStampMargin);
    // Persist the reverted state (no full rebuild here: undo must stay
    // instant; the touched chunks keep the live path's store-derived shading
    // until the next reload, exactly like a painted stroke).
    m_overlayWriter.queue(m_layers.store(), overlayPath());
}

// "Clear live edits": drop every runtime edit in the store, delete the
// persisted overlay and re-patch the chunks the edits touched from the baked
// pools. In-place (no world reload), so it lands in the same frame.
void App::clearLiveEdits()
{
    auto& store = m_layers.store();
    if (!store.hasEditedChunks()) {
        spdlog::info("live edits: nothing to clear");
        return;
    }
    // Stop a pending overlay write first: a snapshot queued before the click
    // could otherwise land after the delete and resurrect the edits.
    m_overlayWriter.flush();
    const std::string path = overlayPath();
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) && ec)
        spdlog::warn("live edits: cannot remove '{}': {}", path, ec.message());

    const std::vector<int> chunks = store.editedChunks();
    // the per-chunk edit AABBs drive the height-texture refresh; collect them
    // before the revert (the store forgets the edits)
    std::vector<std::pair<glm::ivec3, glm::ivec3>> bounds;
    bounds.reserve(chunks.size());
    for (int ci : chunks) {
        int lo[3], hi[3];
        if (store.chunkEditBounds(ci, lo, hi))
            bounds.emplace_back(glm::ivec3(lo[0], lo[1], lo[2]),
                                glm::ivec3(hi[0], hi[1], hi[2]));
    }
    // re-adopt the resident (baked) pools: every runtime edit is dropped
    m_layers.invalidateStore();
    auto& fresh = m_layers.store();
    m_liveEditor.attach(&fresh);

    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.microDetail = m_microDetail;
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    size_t n = 0;
    for (int ci : chunks) {
        m_liveEditor.seedFromStore(ci, sp);
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        n += surfels.size();
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.microStartOf(ci),
                                          m_liveEditor.edgeCountOf(ci));
        const auto& pool = fresh.pool(ci);
        if (pool && !getenv("VF_LIVE_NOSVO"))
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    // A cleared edit can restore a live Smooth column far above the cached
    // post-edit top. Use a full-column scan rather than a magic headroom.
    for (const auto& b : bounds)
        patchHeightTexture(b.first.x - 3, b.first.z - 3, b.second.x + 3,
                           b.second.z + 3, fresh.latN());
    m_undo.clear();
    m_undoPending.clear();
    m_strokeRiseCells = 2;
    m_undoOverflow = false;
    m_taaFirstFrame = true;
    spdlog::info("live edits: cleared {} chunks ({} surfels re-derived), overlay removed",
                 chunks.size(), n);
    // The in-place revert above lands this frame; a full rebuild then restores
    // what a live patch cannot (baked LOD rings, exact bake normals), so the
    // cleared world converges to the pristine bake.
    requestWorldReload();
}

glm::vec3 App::storeNormalAt(const glm::ivec3& v)
{
    auto& store = m_layers.store();
    const glm::vec3 p = vf::voxel::voxelCenter(v);
    const float e = 0.15f;
    glm::vec3 n(
        store.sampleWorld(p + glm::vec3(e, 0, 0)).d -
            store.sampleWorld(p - glm::vec3(e, 0, 0)).d,
        store.sampleWorld(p + glm::vec3(0, e, 0)).d -
            store.sampleWorld(p - glm::vec3(0, e, 0)).d,
        store.sampleWorld(p + glm::vec3(0, 0, e)).d -
            store.sampleWorld(p - glm::vec3(0, 0, e)).d);
    return glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.f, 1.f, 0.f);
}

void App::persistWorldLayers()
{    std::vector<vf::voxel::worldfile::WorldLayer> out;
    out.reserve(m_worldLayers.size());
    for (const auto& l : m_worldLayers)
        if (l.listed &&
            std::none_of(out.begin(), out.end(),
                         [&](const vf::voxel::worldfile::WorldLayer& e) {
                             return e.file == l.file;
                         }))
            out.push_back(l);
    vf::voxel::worldfile::writeManifest(std::string(VOXELFORGE_ASSET_DIR) + "/world.json",
                                        out);
}

void App::rescanWorldLayers()
{
    namespace fs = std::filesystem;
    const fs::path dir = std::string(VOXELFORGE_ASSET_DIR);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string f = it->path().filename().string();
        if (it->path().extension() != ".vxw" || f == "world.vxw")
            continue;
        bool known = false;
        for (const auto& l : m_worldLayers)
            known |= (l.file == f);
        if (known)
            continue;
        vf::voxel::worldfile::WorldLayer nl;
        nl.file = f;
        nl.name = it->path().stem().string();
        nl.role = "object";
        nl.listed = false;
        nl.enabled = false;
        m_worldLayers.push_back(nl);
    }
}

void App::rescanMeshFiles()
{
    m_meshFiles.clear();
    namespace fs = std::filesystem;
    const fs::path assetDir(std::string(VOXELFORGE_ASSET_DIR));
    auto addFile = [&](const fs::path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec)
            return;
        std::string ext = path.extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".stl" && ext != ".obj")
            return;
        std::error_code relEc;
        fs::path rel = fs::relative(path, assetDir, relEc);
        const std::string name = (relEc || rel.empty() ? path.filename() : rel)
                                     .generic_string();
        if (!name.empty() &&
            std::find(m_meshFiles.begin(), m_meshFiles.end(), name) == m_meshFiles.end())
            m_meshFiles.push_back(name);
    };

    for (const fs::path& root : { assetDir / "models", assetDir }) {
        std::error_code rootEc;
        for (fs::directory_iterator it(root, rootEc), end;
             !rootEc && it != end; it.increment(rootEc))
            addFile(it->path());
    }
    std::sort(m_meshFiles.begin(), m_meshFiles.end());
}

void App::useMeshImportLayer(const std::string& file)
{
    if (file.empty())
        return;
    namespace fs = std::filesystem;
    const std::string stem = fs::path(file).stem().string();
    const std::string safe = vf::voxel::EditableWorld::sanitizeLayerName(stem);
    if (safe.empty()) {
        m_meshStatus = "Layer name must contain letters, numbers, '_' or '-'";
        m_meshStatusError = true;
        return;
    }
    std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safe.c_str());

    vf::voxel::WorldFileData data;
    const std::string path = std::string(VOXELFORGE_ASSET_DIR) + "/" + file;
    glm::vec3 pivot(0.f);
    if (!vf::voxel::worldfile::read(path, data) ||
        !vf::voxel::worldfile::recordBottomCenter(data.voxels, data.meta, pivot)) {
        m_meshStatus = "Could not read source layer '" + file + "' for its anchor";
        m_meshStatusError = true;
        return;
    }
    m_meshAnchor = {
        int(std::floor((pivot.x + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL)),
        int(std::floor((pivot.y + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL)),
        int(std::floor((pivot.z + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL))
    };
    m_meshStatus = "Anchor set to the untransformed bottom-center of " + file;
    m_meshStatusError = false;
}

void App::prepareMeshImport()
{
    rescanMeshFiles();
    if (m_meshImportPrepared)
        return;

    if (m_meshPath[0] == '\0') {
        const std::string cabin = "models/Forrest_Hunting_Cabin.stl";
        if (std::find(m_meshFiles.begin(), m_meshFiles.end(), cabin) != m_meshFiles.end())
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", cabin.c_str());
        else if (!m_meshFiles.empty())
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", m_meshFiles.front().c_str());
    }

    bool usedLayerPivot = false;
    if (m_meshLayerName[0] == '\0') {
        const bool selectedIsReplaceable =
            !m_selectedLayer.empty() && m_selectedLayer != "landscape.vxw" &&
            m_selectedLayer != vf::voxel::EditableWorld::kFileName;
        if (selectedIsReplaceable) {
            useMeshImportLayer(m_selectedLayer);
            usedLayerPivot = m_meshStatus.find("Anchor set") != std::string::npos;
        } else if (std::filesystem::exists(std::string(VOXELFORGE_ASSET_DIR) +
                                            "/hamlet_cabin.vxw")) {
            // The authored hamlet uses the imported cabin layer.  Starting
            // with that target makes a re-import a replacement, not a second
            // object, and writeObjectLayer preserves its manifest pose.
            useMeshImportLayer("hamlet_cabin.vxw");
            usedLayerPivot = m_meshStatus.find("Anchor set") != std::string::npos;
        } else if (!m_meshFiles.empty()) {
            const std::string stem = std::filesystem::path(m_meshFiles.front()).stem().string();
            const std::string safe = vf::voxel::EditableWorld::sanitizeLayerName(stem);
            if (!safe.empty())
                std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safe.c_str());
        }
    }
    // A replacement of an existing layer must use that layer's source pivot;
    // a picked surface voxel is only the fallback for a brand-new layer.
    if (m_hasSelection && !usedLayerPivot)
        m_meshAnchor = m_selectedHit.voxel;
    m_meshImportPrepared = true;
}

bool App::importMeshFromGui()
{
    const std::string path(m_meshPath);
    const std::string name(m_meshLayerName);
    const std::string safeName = vf::voxel::EditableWorld::sanitizeLayerName(name);
    if (safeName.empty()) {
        m_meshStatus = "Enter a layer name (letters, numbers, '_' or '-')";
        m_meshStatusError = true;
        return false;
    }
    if (safeName == "landscape" || safeName == vf::voxel::EditableWorld::kLayerName) {
        m_meshStatus = "Mesh import cannot overwrite the landscape or live AI-edit layer";
        m_meshStatusError = true;
        return false;
    }
    const std::string targetFile = safeName + ".vxw";
    const auto protectedTarget = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& layer) {
            return layer.file == targetFile &&
                   (layer.role == "landscape" || layer.role == "packed");
        });
    if (protectedTarget != m_worldLayers.end()) {
        m_meshStatus = "Mesh import cannot overwrite a landscape or packed layer";
        m_meshStatusError = true;
        return false;
    }

    vf::voxel::MeshImportOptions options;
    options.hasFit = m_meshUseFit;
    options.fitMeters = m_meshFitMeters;
    options.scale = m_meshScale;
    options.rotY = m_meshRotY;
    options.swapYz = m_meshSwapYz;
    options.flip = m_meshFlip;
    options.mat = std::clamp(m_meshMaterial, 0, int(vf::voxel::kPaletteN) - 1);

    std::vector<vf::voxel::VoxelRecord> records;
    vf::voxel::MeshImportStats stats;
    std::string err;
    if (!vf::voxel::convertMeshToRecords(path, options, m_meshSolid,
                                        m_meshAnchor, records, stats, err)) {
        m_meshStatus = err;
        m_meshStatusError = true;
        spdlog::warn("mesh GUI import failed: {}", err);
        return false;
    }
    if (!m_editable.writeObjectLayer(safeName, records)) {
        m_meshStatus = "Could not write the object layer or update world.json";
        m_meshStatusError = true;
        return false;
    }

    m_selectedLayer = safeName + ".vxw";
    std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safeName.c_str());
    std::ostringstream out;
    out << "Imported " << stats.triangles << " triangles -> " << safeName
        << ".vxw (" << records.size() << " voxels, "
        << stats.nx << "x" << stats.ny << "x" << stats.nz << " grid)";
    if (stats.clamped > 0)
        out << "; " << stats.clamped << " cells were outside the lattice";
    if (m_meshSolid && stats.leak)
        out << "; warning: no interior volume, mesh is not watertight at this scale";
    if (safeName == "hamlet_cabin")
        out << "; existing manifest pose/orientation preserved";
    m_meshStatus = out.str();
    m_meshStatusError = m_meshSolid && stats.leak;
    m_taaFirstFrame = true;
    requestWorldReload();
    spdlog::info("mesh GUI import: {} ({} voxels)", m_meshStatus, records.size());
    return true;
}

// ---------------------------------------------------------------------------
// The editor UI is a single docked left sidebar: an icon rail selects one of
// six sections, a content pane shows it, and a fixed footer carries the status
// that is relevant from anywhere. This replaced five floating windows
// (Dashboard, World Layers, Toolbox, Materials, Mesh Import, AI Assistant).
// ---------------------------------------------------------------------------
void App::drawHud()
{
    drawSidebar();
    drawSceneOverlays();
}

void App::drawSidebar()
{
    ImGuiIO& io = ImGui::GetIO();
    const float height = io.DisplaySize.y;

    // The width is user-owned once the right-edge grip is dragged, but stays
    // clamped to the current display. Width zero means "use the responsive
    // default"; double-clicking the grip restores that default.
    float totalW = m_sidebarCollapsed
        ? std::min(kRailW, io.DisplaySize.x)
        : sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    bool overResizeGrip = !m_sidebarCollapsed &&
        overSidebarResizeGrip(io, totalW, height);
    if (m_sidebarCollapsed) {
        m_sidebarResizing = false;
    } else if (io.MouseDoubleClicked[0] && overResizeGrip) {
        m_sidebarWidth = 0.0f;
        m_sidebarResizing = false;
        totalW = sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    } else if (!m_sidebarResizing && io.MouseClicked[0] &&
               overResizeGrip) {
        m_sidebarResizing = true;
    }
    if (m_sidebarResizing) {
        if (io.MouseDown[0]) {
            m_sidebarWidth = clampSidebarWidth(io.MousePos.x,
                                                io.DisplaySize.x);
        } else {
            m_sidebarResizing = false;
            spdlog::info("sidebar width -> {:.0f} px", m_sidebarWidth);
        }
        totalW = sidebarWidthFor(m_sidebarWidth, io.DisplaySize.x);
    }
    const bool resizeCapturesMouse = overResizeGrip || m_sidebarResizing;

    // A trackball ring is drawn over the scene, so it can land on top of the
    // sidebar. Ring hits are tested BEFORE ImGui mouse capture, and while the
    // pointer is on a ring the whole sidebar stops accepting input: otherwise
    // the panel would either steal the drag or an unrelated button underneath
    // the ring would toggle. An in-progress drag also fades the sidebar so the
    // object being rotated or moved stays visible. The docked resize edge wins
    // a tie so dragging it can never rotate a ring underneath the grip.
    const TrackballHandle hoveredHandle =
        m_gizmoValid && !resizeCapturesMouse
        ? trackballHandleAt(glm::vec2(io.MousePos.x, io.MousePos.y),
                            m_gizmoCentre, m_gizmoRadius)
        : TrackballHandle::None;
    const bool overRing = hoveredHandle != TrackballHandle::None;
    const bool dragging = m_rotating || m_moving;

    // Docked, not floating: the opaque panel occupies the whole left edge,
    // top to bottom, with no inset margin, title bar, rounding, or border.

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    // Child surfaces normally have their own translucent colour, leaving
    // darker top/bottom padding bands that make a flush window look inset.
    // Match the dock background exactly while idle. During a trackball drag
    // the parent supplies the 0.30 fade, so children must contribute no second
    // translucent layer of their own.
    ImVec4 dockBackground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    dockBackground.w = dragging ? 0.0f : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, dockBackground);
    // NoSavedSettings + Cond_Always: a stale imgui.ini saved by the old
    // floating layout can never drag this panel off-screen or resize it.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(totalW, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(dragging ? 0.30f : 1.0f);
    // NoDecoration = NoTitleBar | NoResize | NoScrollbar | NoCollapse
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollWithMouse;
    if (overRing)
        flags |= ImGuiWindowFlags_NoInputs;

    const bool open = ImGui::Begin("Voxelforge##Sidebar", nullptr, flags);
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor();
    if (open) {
        const float topH = std::max(0.0f, height - kFooterH - 2.0f * ImGui::GetStyle().WindowPadding.y);

        // ---- icon rail -------------------------------------------------
        ImGui::BeginChild("rail", ImVec2(kRailW, topH), ImGuiChildFlags_None);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        for (int i = 0; i < kPanelCount; ++i) {
            const Panel p = Panel(i);
            const bool active = (m_panel == p);
            if (actionButton(kPanels[i].label, active, ImVec2(-1.0f, 30.0f))) {
                if (p == Panel::Mesh)
                    prepareMeshImport();
                m_panel = p;
            }
            // Armed-tool marker as an accent bar on the button's right edge:
            // the brush is armed independently of the section on screen, and a
            // bar costs no vertical row the way a ">" text line did.
            if (p == Panel::Edit && m_editActive) {
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    ImVec2(b.x - 3.0f, a.y + 2.0f), ImVec2(b.x, b.y - 2.0f),
                    IM_COL32(33, 212, 237, 255));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kPanels[i].full);
        }
        ImGui::EndChild();

        // ---- content pane ----------------------------------------------
        if (!m_sidebarCollapsed) {
            ImGui::SameLine();
            if (ImGui::BeginChild("pane", ImVec2(0.0f, topH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollWithMouse)) {
                switch (m_panel) {
                case Panel::Edit:      drawPanelEdit(); break;
                case Panel::World:     drawPanelWorld(); break;
                case Panel::Render:    drawPanelRender(); break;
                case Panel::Textures:  drawPanelTextures(); break;
                case Panel::Mesh:      drawPanelMesh(); break;
                case Panel::AI:        drawPanelAI(); break;
                case Panel::kCount:    break;
                }
            }
            ImGui::EndChild();
        }
        drawSidebarFooter();
    }
    ImGui::End();

    // Custom horizontal splitter. It lives entirely inside the opaque panel,
    // so the resize affordance cannot introduce a transparent seam at the
    // dock edge. The input loop yields to the same 8 px hit zone before scene
    // picking/stamping; double-click resets to the responsive default.
    if (!m_sidebarCollapsed && totalW > 0.0f && height > 0.0f) {
        const float gripLeft = std::max(0.0f, totalW - kSidebarGripW);
        const ImU32 tint = m_sidebarResizing
            ? IM_COL32(33, 212, 237, 210)
            : (overResizeGrip ? IM_COL32(33, 212, 237, 150)
                              : IM_COL32(120, 145, 170, 105));
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        if (m_sidebarResizing || overResizeGrip) {
            draw->AddRectFilled(
                ImVec2(gripLeft, 0.0f), ImVec2(totalW, height),
                IM_COL32(33, 212, 237, m_sidebarResizing ? 34 : 18));
        }
        const float centreY = height * 0.5f;
        for (float y = std::max(6.0f, centreY - 18.0f);
             y <= std::min(height - 6.0f, centreY + 18.0f); y += 6.0f) {
            draw->AddLine(ImVec2(gripLeft + 2.0f, y),
                          ImVec2(totalW - 2.0f, y), tint, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Footer: the always-visible status line, two rows tall so it never grows or
// jumps. A staged transform takes over the whole footer from every section —
// it is a pending write to world.json, and burying the commit inside the Edit
// section made it easy to lose. Collapsed, the sidebar is only as wide as the
// rail, so the footer degrades to a single frame-time figure.
// ---------------------------------------------------------------------------
void App::drawSidebarFooter()
{
    ImGui::Separator();
    const float w = ImGui::GetContentRegionAvail().x;
    const char* mode = m_renderMode == RenderMode::Splats ? "splat" : "SVO";

    if (m_rotationStaged || m_moveStaged) {
        if (w < 90.0f) {
            ImGui::TextColored(kWarn, "EDIT");
            return;
        }
        ImGui::TextColored(kWarn, "%s staged — not written",
                           m_rotationStaged ? "Rotation" : "Move");
        ImGui::TextDisabled("Ctrl+1 to %s",
                            m_panel == Panel::Edit ? "apply or cancel"
                                                  : "review (Edit section)");
        return;
    }

    if (w < 90.0f) {                       // rail-only sidebar
        ImGui::Text("%.0f", m_lastFrameMs);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tab expands the sidebar\n%.1f ms CPU | geo %.1f ms",
                              m_avgMs, m_profAvg[0]);
        return;
    }
    if (m_editActive)
        ImGui::TextColored(kAccent, "%s  %.1f ms", mode, m_avgMs);
    else
        ImGui::Text("%s  %.1f ms", mode, m_avgMs);
    if (m_editActive) {
        if (m_editBrush == EditBrush::Smooth)
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | smooth %dvox %.0f%%",
                               m_lastFrameMs, m_profAvg[0], brushVoxels(),
                               m_smoothStrength * 100.0f);
        else if (brushIsPerVoxel() &&
                 (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve))
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s 1 voxel",
                               m_lastFrameMs, m_profAvg[0], brushName(m_editBrush));
        else
            ImGui::TextColored(kAccent, "%.0f CPU | geo %.1f | %s %dvox",
                               m_lastFrameMs, m_profAvg[0], brushName(m_editBrush),
                               brushVoxels());
    } else {
        ImGui::TextDisabled("%.0f CPU  |  geo %.1f", m_lastFrameMs, m_profAvg[0]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tab collapses the sidebar; Ctrl+1..6 switch section; "
                          "C arms the brush");
}

// ---------------------------------------------------------------------------
// Edit section: the brush toolbox. One mode at a time, with only the controls
// relevant to that mode, plus the staged-transform commit path.
// ---------------------------------------------------------------------------
void App::drawPanelEdit()
{
    sectionHeader("EDIT TOOLBOX", "C arms/disarms the brush");

    if (m_rotationPreviewPending) {
        ImGui::SeparatorText("Pending");
        ImGui::TextColored(kWarn, "Applying staged transform...");
    }
    if (m_rotationStaged || m_moveStaged) {
        ImGui::TextColored(kWarn, "%s staged — not written yet",
                           m_rotationStaged ? "Rotation" : "Move");
        if (m_rotationStaged) {
            ImGui::Text("Yaw %+.1f  Pitch %+.1f  Roll %+.1f", m_rotateDy,
                        m_rotateDx, m_rotateDz);
            if (ImGui::Button("Apply rotation##applyRotation", ImVec2(-1.0f, 30.0f)))
                commitRotation();
            if (ImGui::Button("Cancel rotation##cancelRotation", ImVec2(-1.0f, 30.0f)))
                cancelRotation();
        } else {
            const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                              : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                              : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
            ImGui::Text("Delta %+.2f m along %s", amount, moveAxisName(m_moveAxis));
            if (ImGui::Button("Apply move##applyMove", ImVec2(-1.0f, 30.0f)))
                commitMove();
            if (ImGui::Button("Cancel move##cancelMove", ImVec2(-1.0f, 30.0f)))
                cancelMove();
        }
    }

    // Arm/disarm is a mode switch, not a window toggle: disarming keeps the
    // section (and its settings) visible so it can be re-armed in one press.
    if (ImGui::Checkbox("Brush armed##editArmed", &m_editActive))
        spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");

    auto chooseMode = [&](EditBrush mode) {
        if (m_rotationPreviewPending ||
            (mode == EditBrush::Move && m_rotationStaged) ||
            (mode == EditBrush::Rotate && m_moveStaged))
            return;
        m_editBrush = mode;
        if (mode == EditBrush::Rotate) {
            if (!m_rotationStaged || m_rotateLayer.empty())
                m_rotateLayer = m_selectedLayer;
            m_rotating = false;
            m_rotateHandle = TrackballHandle::None;
            if (!m_rotationStaged)
                m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
            m_renderMode = RenderMode::Splats;
            m_taaFirstFrame = true;
        } else if (mode == EditBrush::Move) {
            m_moveLayer = m_selectedLayer;
            m_moving = false;
            if (!m_moveStaged)
                m_moveDelta = glm::vec3(0.f);
        } else if (!m_rotationStaged) {
            m_rotateLayer.clear();
            m_rotateHandle = TrackballHandle::None;
            m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
        }
        if (mode != EditBrush::Move && !m_moveStaged) {
            m_moving = false;
            m_moveLayer.clear();
        }
    };

    ImGui::SeparatorText("Mode");
    if (ImGui::BeginTable("BrushModes", 2, ImGuiTableFlags_SizingStretchSame)) {
        const struct { const char* label; EditBrush mode; } modes[] = {
            { "Carve", EditBrush::Carve }, { "Add", EditBrush::Add },
            { "Delete", EditBrush::Delete }, { "Paint", EditBrush::Paint },
            { "Smooth", EditBrush::Smooth }, { "Rotate", EditBrush::Rotate },
            { "Move", EditBrush::Move }
        };
        for (int i = 0; i < int(sizeof(modes) / sizeof(modes[0])); ++i) {
            if (i % 2 == 0)
                ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(i % 2);
            if (actionButton(modes[i].label, m_editBrush == modes[i].mode,
                             ImVec2(-1.0f, 32.0f)))
                chooseMode(modes[i].mode);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Brush settings");
    if (m_editBrush != EditBrush::Rotate &&
        m_editBrush != EditBrush::Move) {
        // The brush lives on the 0.1 m lattice, so it is sized in voxels: the
        // minimum is one voxel, which is the per-voxel Add/Carve mode.
        int vox = brushVoxels();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragInt("Width##brushSize", &vox, 1, kBrushMinVox,
                           kBrushMaxVox, "%d vox")) {
            setBrushVoxels(vox);
            if (m_editDepth < vf::voxel::VOXEL)
                m_editDepth = vf::voxel::VOXEL;
        }
        ImGui::TextDisabled("%.2f m across%s", m_editDiameter,
                            brushIsPerVoxel() ? "  (per-voxel: one cell)" : "");
    }
    if (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Add) {
        ImGui::BeginDisabled(brushIsPerVoxel());
        if (ImGui::DragFloat("Depth / height##brushDepth", &m_editDepth, 0.1f,
                             0.1f, 12.0f, "%.1f m"))
            setBrushDepthVoxels(brushDepthVoxels());
        if (brushIsPerVoxel())
            ImGui::TextDisabled("Depth is ignored at 1 voxel.");
        ImGui::EndDisabled();
    }
    if (m_editBrush == EditBrush::Smooth) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("Strength##smoothStrength", &m_smoothStrength, 0.01f,
                         0.0f, 1.0f, "%.2f");
    }
    if (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Paint) {
        int mat = int(m_editMat);
        ImGui::SetNextItemWidth(-60.0f);
        if (ImGui::BeginCombo("Material", kMatNames[std::min(mat, 20)])) {
            for (int i = 0; i < 21; ++i)
                if (ImGui::Selectable(kMatNames[i], mat == i))
                    m_editMat = uint8_t(i);
            ImGui::EndCombo();
        }
        const glm::vec3 c = vf::voxel::kPalette[std::min(int(m_editMat), 16)];
        ImGui::SameLine();
        ImGui::ColorButton("##materialSwatch", ImVec4(c.r, c.g, c.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(24.0f, 24.0f));
    }

    ImGui::SeparatorText("Action");
    if (m_editBrush == EditBrush::Rotate) {
        if (m_selectedLayer.empty()) {
            ImGui::TextColored(kWarn, "No object layer selected");
            ImGui::TextWrapped("Pick one in World layers, activate its trackball "
                               "there, or click an object in the scene.");
        } else {
            ImGui::Text("Target  %s", m_selectedLayer.c_str());
            const std::string hoverLayer = m_hoverHit.hit
                ? m_layers.layerFile(m_hoverHit.layer) : std::string();
            if (!hoverLayer.empty())
                ImGui::TextDisabled("Hover resolves to: %s", hoverLayer.c_str());
            ImGui::TextWrapped(
                "Click the object to activate it. Drag outer/local-Y, wide/local-X, "
                "or tall/local-Z ring; release stages it, then press Apply.");
            if (m_rotating)
                ImGui::Text("Yaw %+.1f  Pitch %+.1f  Roll %+.1f", m_rotateDy,
                            m_rotateDx, m_rotateDz);
        }
        if (m_renderMode != RenderMode::Splats)
            ImGui::TextDisabled("Live preview requires Gaussian surfels.");
    } else if (m_editBrush == EditBrush::Move) {
        if (m_moveLayer.empty()) {
            ImGui::TextColored(kWarn, "No object layer grabbed");
            ImGui::TextWrapped("Click an owned object, then use the colored X/Y/Z handles.");
        } else {
            ImGui::Text("Target  %s", m_moveLayer.c_str());
            ImGui::TextWrapped("Click the object to grab it, or click an X/Y/Z "
                               "handle. Drag to stage; press Apply to persist.");
            ImGui::SeparatorText("Move axis");
            ImGui::BeginDisabled(m_moveStaged || m_moving);
            if (actionButton("X##moveX", m_moveAxis == MoveAxis::X, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::X;
            ImGui::SameLine();
            if (actionButton("Y##moveY", m_moveAxis == MoveAxis::Y, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::Y;
            ImGui::SameLine();
            if (actionButton("Z##moveZ", m_moveAxis == MoveAxis::Z, ImVec2(-1.0f, 28.0f)))
                m_moveAxis = MoveAxis::Z;
            ImGui::EndDisabled();
            const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                          : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                          : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
            ImGui::Text("Delta %+.2f m%s", amount,
                        m_moveStaged ? "  (staged)" :
                        m_moving ? "  (dragging)" : "");
        }
        if (m_renderMode != RenderMode::Splats)
            ImGui::TextDisabled("Live move preview needs Gaussian surfels; "
                                "Apply still rebuilds the SVO.");
    } else {
        switch (m_editBrush) {
        case EditBrush::Carve:
            ImGui::TextWrapped("LMB drag: scoop along the surface normal.");
            break;
        case EditBrush::Add:
            ImGui::TextWrapped("LMB drag: grow the surface along the picked normal.");
            break;
        case EditBrush::Delete:
            ImGui::TextWrapped("LMB drag: clear every voxel in the ball.");
            break;
        case EditBrush::Paint:
            ImGui::TextWrapped("LMB drag: recolor solid cells in the ball.");
            break;
        case EditBrush::Smooth:
            ImGui::TextWrapped("LMB drag: relax the picked surface toward its "
                               "local average — terrain heights on ground, an "
                               "object's own surface on objects; terrain and "
                               "objects never convert into each other.");
            break;
        case EditBrush::Rotate:
        case EditBrush::Move:
            break;
        }
        ImGui::TextDisabled(m_editBrush == EditBrush::Smooth
                            ? "Blue preview shows the terrain footprint."
                            : "Preview tints exactly the affected surfels.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo (Ctrl+Z)", ImVec2(-1.0f, 30.0f)))
        undoEdit();
    ImGui::EndDisabled();
    ImGui::TextDisabled("%zu undo step%s", m_undo.size(),
                        m_undo.size() == 1 ? "" : "s");
    const double now = glfwGetTime();
    const bool confirmingClear = now < m_clearConfirmUntil;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          confirmingClear ? kDanger : ImVec4(0.24f, 0.08f, 0.11f, 1.0f));
    if (ImGui::Button(confirmingClear ? "Click again to clear all"
                                      : "Clear live edits",
                       ImVec2(-1.0f, 30.0f))) {
        if (confirmingClear) {
            clearLiveEdits();
            m_clearConfirmUntil = 0.0;
        } else {
            m_clearConfirmUntil = now + 3.0;
        }
    }
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Last stamp: %.1f ms  |  %zu surfels", m_lastEditMs,
                        m_lastEditSurfels);
}

// ---------------------------------------------------------------------------
// World section: searchable layer inventory plus one focused inspector for the
// selected layer (placement pose, import-a-copy, trackball activation).
// ---------------------------------------------------------------------------
void App::drawPanelWorld()
{
    sectionHeader("WORLD LAYERS", "Picking resolves cell provenance, not AABBs");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##layerFilter", "Filter .vxw files", m_layerFilter,
                             sizeof(m_layerFilter));
    const std::string filter = foldCase(m_layerFilter);
    size_t enabledCount = 0;
    for (const auto& l : m_worldLayers)
        enabledCount += l.enabled ? 1 : 0;
    ImGui::TextDisabled("%zu files  |  %zu enabled  |  %zu records live",
                        m_worldLayers.size(), enabledCount, m_layers.stats().records);

    // Bounded list height: the inspector below must stay reachable without a
    // long scroll even in a short window.
    const float listH = std::max(110.0f, ImGui::GetContentRegionAvail().y * 0.30f);
    if (ImGui::BeginTable("LayerInventory", 2,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, listH))) {
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableHeadersRow();
        for (auto& l : m_worldLayers) {
            if (!filter.empty() && foldCase(l.file).find(filter) == std::string::npos)
                continue;
            ImGui::PushID(l.file.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // One row = the enable checkbox plus the selection highlight, so
            // the inventory stays two columns wide in a 300 px pane.
            bool enabled = l.enabled;
            if (l.role == "landscape")
                ImGui::BeginDisabled(true);
            if (ImGui::Checkbox("##enabled", &enabled)) {
                l.enabled = enabled;
                l.listed = true;
                persistWorldLayers();
                m_pendingWorldReload = true;
            }
            if (l.role == "landscape")
                ImGui::EndDisabled();
            ImGui::SameLine();
            const bool selected = m_selectedLayer == l.file;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, kAccentSoft);
            // The label doubles as the row select target: a leading bullet
            // marks the enabled layers without spending a column on it.
            const std::string label = l.enabled ? "* " + l.file : "  " + l.file;
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedLayer = l.file;
            }
            if (selected)
                ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            if (l.enabled)
                ImGui::TextColored(kAccent, "LIVE");
            else
                ImGui::TextDisabled("OFF");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("* = merged into the world");

    auto selected = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& l) {
            return l.file == m_selectedLayer;
        });
    if (selected == m_worldLayers.end()) {
        if (!m_selectedLayer.empty())
            m_selectedLayer.clear();
        ImGui::SeparatorText("Inspector");
        ImGui::TextWrapped("Select a layer to inspect, place, import, or rotate it.");
    } else {
        ImGui::SeparatorText("Inspector");
        ImGui::TextColored(kAccent, "%s", selected->file.c_str());
        const uint8_t ownerId = m_layers.layerId(selected->file);
        ImGui::TextDisabled("role: %s  |  owner id: %u", selected->role.c_str(),
                            unsigned(ownerId));
        glm::vec3 pivot;
        if (m_layers.layerPivot(selected->file, pivot))
            ImGui::TextDisabled("pivot: %.2f, %.2f, %.2f", pivot.x, pivot.y, pivot.z);

        const bool canImport = m_hasSelection && selected->role != "landscape";
        ImGui::BeginDisabled(!canImport);
        if (ImGui::Button("Import copy at anchor##import", ImVec2(-1.0f, 30.0f))) {
            const std::string path = std::string(VOXELFORGE_ASSET_DIR) + "/" +
                                     selected->file;
            if (m_editable.importLayer(path, m_selectedHit.voxel) > 0)
                requestWorldReload();
        }
        ImGui::EndDisabled();
        if (!m_hasSelection)
            ImGui::TextDisabled("Import needs a Ctrl+LMB anchor.");

        const bool rotatable = ownerId != 0 && selected->enabled &&
                               selected->file != vf::voxel::EditableWorld::kFileName;
        const bool canActivate = !m_rotationStaged ||
                                  m_rotateLayer == selected->file;
        ImGui::BeginDisabled(!rotatable || !canActivate || m_rotationPreviewPending);
        if (ImGui::Button("Activate trackball##rotate", ImVec2(-1.0f, 30.0f))) {
            m_editActive = true;
            m_editBrush = EditBrush::Rotate;
            m_panel = Panel::Edit;
            m_rotateLayer = selected->file;
            m_rotating = false;
            m_rotateHandle = TrackballHandle::None;
            if (!m_rotationStaged)
                m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
            m_taaFirstFrame = true;
            m_renderMode = RenderMode::Splats; // live preview backend
        }
        ImGui::EndDisabled();
        if (selected->file == vf::voxel::EditableWorld::kFileName)
            ImGui::TextWrapped("AI edits are world-space, not one rotatable object.");

        // Placement: 3 columns, one row per axis group. The old 2-column
        // label/field table needed ~250 px of content width; three compact
        // drag fields per row fit the pane and are read at a glance.
        glm::vec3 posDraft(selected->pos[0], selected->pos[1], selected->pos[2]);
        float angleDraft[3] = { selected->rotDeg, selected->rotX, selected->rotZ };
        bool transformChanged = false;
        static const char* kPosIds[3] = { "##posX", "##posY", "##posZ" };
        static const char* kAngIds[3] = { "##yaw", "##pitch", "##roll" };
        ImGui::TextDisabled("Position (m)");
        if (ImGui::BeginTable("TransformPos", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (int c = 0; c < 3; ++c) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(c);
                ImGui::TextDisabled("%c", "XYZ"[c]);
                ImGui::SetNextItemWidth(-1.0f);
                transformChanged |= ImGui::DragFloat(kPosIds[c], &posDraft[c], 0.1f,
                                                    -100.0f, 100.0f, "%.1f");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Rotation (deg)");
        if (ImGui::BeginTable("TransformRot", 3, ImGuiTableFlags_SizingStretchSame)) {
            for (int c = 0; c < 3; ++c) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(c);
                ImGui::TextDisabled("%s", c == 0 ? "Yaw" : c == 1 ? "Pitch" : "Roll");
                ImGui::SetNextItemWidth(-1.0f);
                transformChanged |= ImGui::DragFloat(kAngIds[c], &angleDraft[c], 1.0f,
                                                    -360.0f, 360.0f, "%.0f");
            }
            ImGui::EndTable();
        }
        if (transformChanged)
            ImGui::TextColored(kWarn, "Unsaved transform");
        if (ImGui::Button("Apply transform##apply", ImVec2(-1.0f, 30.0f))) {
            selected->pos[0] = posDraft.x;
            selected->pos[1] = posDraft.y;
            selected->pos[2] = posDraft.z;
            selected->rotDeg = angleDraft[0];
            selected->rotX = angleDraft[1];
            selected->rotZ = angleDraft[2];
            selected->listed = true;
            persistWorldLayers();
            m_pendingWorldReload = true;
            m_taaFirstFrame = true;
        }
        ImGui::BeginDisabled(!transformChanged);
        if (ImGui::Button("Reset transform##resetTransform", ImVec2(-1.0f, 30.0f))) {
            selected->pos[0] = selected->pos[1] = selected->pos[2] = 0.0f;
            selected->rotDeg = selected->rotX = selected->rotZ = 0.0f;
            selected->listed = true;
            persistWorldLayers();
            m_pendingWorldReload = true;
            m_taaFirstFrame = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Written to world.json on Apply.");
    }

    ImGui::Separator();
    if (ImGui::Button("Rescan assets##rescan", ImVec2(-1.0f, 28.0f)))
        rescanWorldLayers();
}

// ---------------------------------------------------------------------------
// Render section: the renderer switch, the visual toggles, the splat knobs,
// and the camera/selection readout that used to live in the Dashboard.
// ---------------------------------------------------------------------------
void App::drawPanelRender()
{
    sectionHeader("RENDERING", "F switches renderer");

    if (ImGui::BeginTable("RendererButtons", 2, ImGuiTableFlags_SizingStretchSame)) {
        const bool splats = m_renderMode == RenderMode::Splats;
        if (actionButton("Gaussian surfels", splats, ImVec2(-1.0f, 32.0f)))
            m_renderMode = RenderMode::Splats;
        ImGui::TableNextColumn();
        if (actionButton("SVO reference", !splats, ImVec2(-1.0f, 32.0f)))
            m_renderMode = RenderMode::Svo;
        ImGui::EndTable();
    }

    ImGui::SeparatorText("View");
    const glm::vec3 fwd = m_camera.forward();
    float heading = glm::degrees(std::atan2(fwd.x, fwd.z));
    if (heading < 0.0f)
        heading += 360.0f;
    const char* cardinal = (heading < 22.5f || heading >= 337.5f) ? "N" :
                          (heading < 67.5f) ? "NE" :
                          (heading < 112.5f) ? "E" :
                          (heading < 157.5f) ? "SE" :
                          (heading < 202.5f) ? "S" :
                          (heading < 247.5f) ? "SW" :
                          (heading < 292.5f) ? "W" : "NW";
    ImGui::Text("Pos  %.1f, %.1f, %.1f", m_camera.pos.x, m_camera.pos.y,
                m_camera.pos.z);
    ImGui::Text("Head %.0f deg %s", heading, cardinal);
    ImGui::TextDisabled("%.1f ms CPU  |  frame %.1f ms", m_avgMs, m_lastFrameMs);
    ImGui::TextDisabled("gpu geo %.1f  post %.1f  fx %.1f  taa %.1f",
                        m_profAvg[0], m_profAvg[1], m_profAvg[2], m_profAvg[3]);

    if (m_hasSelection) {
        ImGui::SeparatorText("Selection");
        ImGui::Text("Voxel %d, %d, %d", m_selectedHit.voxel.x,
                    m_selectedHit.voxel.y, m_selectedHit.voxel.z);
        if (m_selectedLayer.empty())
            ImGui::TextDisabled("Terrain / unowned live geometry");
        else
            ImGui::TextColored(kAccent, "Layer %s", m_selectedLayer.c_str());
    } else {
        ImGui::SeparatorText("Selection");
        ImGui::TextWrapped("Ctrl + LMB picks a voxel and resolves its exact .vxw owner.");
    }
    if (m_hoverHit.hit && m_hoverHit.object) {
        const std::string hoverLayer = m_layers.layerFile(m_hoverHit.layer);
        if (!hoverLayer.empty())
            ImGui::TextDisabled("Hover owner: %s", hoverLayer.c_str());
    }

    ImGui::SeparatorText("Effects");
    auto flagToggle = [&](const char* label, int mask) {
        bool enabled = (m_renderFlags & mask) != 0;
        if (ImGui::Checkbox(label, &enabled)) {
            if (enabled)
                m_renderFlags |= mask;
            else
                m_renderFlags &= ~mask;
        }
    };
    if (ImGui::BeginTable("RenderToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
        flagToggle("Ambient occlusion", 1 << 0);
        ImGui::TableNextColumn();
        flagToggle("Sun shadows", 1 << 1);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Flora shading", 1 << 2);
        ImGui::TableNextColumn();
        flagToggle("Water", 1 << 3);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Reflections (G)", 1 << 5);
        ImGui::TableNextColumn();
        flagToggle("SSAO (H)", 1 << 6);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Detail normals (B)", 1 << 7);
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("TAA (N)", &m_taaEnabled))
            m_taaFirstFrame = true;
        ImGui::EndTable();
    }
    // Fog / motion blur / DoF are separate opt-in members, not render-flag
    // bits (bit 4 is the unused outline flag and stays unexposed).
    ImGui::Checkbox("Volumetric fog (J)", &m_volFogEnabled);
    ImGui::Checkbox("Motion blur (K)", &m_motionBlurEnabled);
    ImGui::Checkbox("Depth of field (L)", &m_dofEnabled);

    ImGui::SeparatorText("Surfels");
    if (m_renderMode == RenderMode::Splats) {
        ImGui::SetNextItemWidth(-1.0f);
        float radius = m_splatPass.radiusScale();
        if (ImGui::SliderFloat("Splat radius##radius", &radius, 0.5f, 2.0f, "%.2f"))
            m_splatPass.setRadiusScale(radius);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Sharp-edge fit##edgeShrink", &m_edgeShrink, 0.0f,
                           0.8f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            requestWorldReload();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reduce only opaque parents on genuine voxel edges; "
                              "smooth curvature keeps full coverage. Rebuilds.");
        if (ImGui::Checkbox("Interpolate crease splats", &m_edgeFill))
            requestWorldReload();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 4.0f, "%.2f");
    if (ImGui::Checkbox("Micro geometry (M; rebuilds)", &m_microDetail))
        requestWorldReload();

    if (m_scenePreview) {
        if (!m_sceneTexId)
            m_sceneTexId = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                m_uiSampler, m_offscreen.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ImGui::SeparatorText("Scene preview");
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Image(m_sceneTexId, ImVec2(w, w * 9.0f / 16.0f));
    }

    if (ImGui::CollapsingHeader("Keyboard")) {
        ImGui::BulletText("WASD move; Q/E down/up");
        ImGui::BulletText("RMB drag: look; wheel: speed");
        ImGui::BulletText("Ctrl+LMB: pick exact voxel / layer");
        ImGui::BulletText("Tab: collapse sidebar; Ctrl+1..6: section");
        ImGui::BulletText("Rotate: click object, then drag a trackball ring");
        ImGui::BulletText("C: arm brush; F: renderer; N: TAA");
        ImGui::BulletText("B/G/H/J/K/L: visual effects");
        ImGui::BulletText("[/]: splat radius; M: micro rebuild");
        ImGui::BulletText("Ctrl+Z: undo last stroke");
        ImGui::BulletText("Window close button: quit");
    }
}

// ---------------------------------------------------------------------------
// Textures section: per-material albedo source and tiling scale. Stacked
// two-line rows instead of a 3-column table, which did not fit the pane.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Mesh section: STL/OBJ import. Same parser + voxelizer as vf_mesh2vox and the
// MCP import_mesh tool. Replacing an existing object layer changes only its
// record file; the manifest pose is deliberately not touched.
// ---------------------------------------------------------------------------
void App::drawPanelMesh()
{
    sectionHeader("MESH IMPORT", "voxelizes onto the 10 cm lattice");
    ImGui::TextWrapped(
        "Choose a model, place its bottom-center, and write a named .vxw layer. "
        "STL/OBJ files are read from the working directory, assets/, or assets/models/.");

    if (ImGui::Button("Rescan model files##meshRescan", ImVec2(-1.0f, 26.0f)))
        rescanMeshFiles();
    ImGui::TextDisabled("%zu model file%s found", m_meshFiles.size(),
                        m_meshFiles.size() == 1 ? "" : "s");

    const std::string currentPath(m_meshPath);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Mesh file##meshFile", currentPath.c_str())) {
        for (const std::string& file : m_meshFiles)
            if (ImGui::Selectable(file.c_str(), file == currentPath))
                std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", file.c_str());
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Path##meshPath", m_meshPath, sizeof(m_meshPath));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Layer name##meshLayer", m_meshLayerName, sizeof(m_meshLayerName));

    ImGui::SeparatorText("Placement");
    ImGui::TextDisabled("Anchor is a lattice cell; the solid AABB is centered on "
                        "X/Z and its lowest cell sits on Y.");
    if (ImGui::BeginTable("MeshAnchor", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("X##meshAnchorX", &m_meshAnchor.x);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Y##meshAnchorY", &m_meshAnchor.y);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("Z##meshAnchorZ", &m_meshAnchor.z);
        ImGui::EndTable();
    }
    if (ImGui::Button("Use Ctrl+LMB voxel##meshUsePick", ImVec2(-1.0f, 28.0f))) {
        if (m_hasSelection) {
            m_meshAnchor = m_selectedHit.voxel;
            m_meshStatus = "Anchor set to the selected voxel";
            m_meshStatusError = false;
        } else {
            m_meshStatus = "Pick a voxel with Ctrl+LMB first";
            m_meshStatusError = true;
        }
    }
    ImGui::BeginDisabled(m_selectedLayer.empty() ||
                         m_selectedLayer == "landscape.vxw" ||
                         m_selectedLayer == vf::voxel::EditableWorld::kFileName);
    if (ImGui::Button("Use layer source pivot##meshUseLayer", ImVec2(-1.0f, 28.0f)))
        useMeshImportLayer(m_selectedLayer);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Transform");
    ImGui::BeginDisabled(!m_meshUseFit);
    ImGui::InputFloat("Fit longest side (m)##meshFit", &m_meshFitMeters, 0.1f,
                      0.1f, "%.2f");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(m_meshUseFit);
    ImGui::InputFloat("Scale (m per unit)##meshScale", &m_meshScale, 0.001f,
                      0.0001f, "%.4f");
    ImGui::EndDisabled();
    ImGui::InputFloat("Mesh-local yaw (deg)##meshRotY", &m_meshRotY, 1.0f, 0.1f,
                      "%.1f");
    const char* materialName = kMatNames[std::clamp(m_meshMaterial, 0, 20)];
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Material##meshMaterial", materialName)) {
        for (int mat = 0; mat < int(vf::voxel::kPaletteN); ++mat)
            if (ImGui::Selectable(kMatNames[std::min(mat, 20)], mat == m_meshMaterial))
                m_meshMaterial = mat;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Z-up source (swap Y/Z)##meshSwap", &m_meshSwapYz);
    ImGui::Checkbox("Reverse winding##meshFlip", &m_meshFlip);
    ImGui::Checkbox("Solid interior##meshSolid", &m_meshSolid);
    ImGui::TextDisabled("Solid fill is safest for VoxelField; shell mode is smaller "
                        "but can render hollow after loading.");

    const std::string targetFile = std::string(m_meshLayerName) + ".vxw";
    auto target = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& layer) {
            return layer.file == targetFile;
        });
    if (target != m_worldLayers.end()) {
        ImGui::SeparatorText("Existing layer");
        ImGui::TextDisabled("pos %.2f, %.2f, %.2f", target->pos[0], target->pos[1],
                            target->pos[2]);
        ImGui::TextDisabled("yaw %.1f  pitch %.1f  roll %.1f", target->rotDeg,
                            target->rotX, target->rotZ);
        ImGui::TextColored(kAccent, "Import replaces records, keeps this pose.");
    }

    if (!m_meshStatus.empty()) {
        ImGui::SeparatorText("Result");
        ImGui::TextWrapped("%s", m_meshStatus.c_str());
    }
    if (ImGui::Button("Import / replace layer##meshImportNow", ImVec2(-1.0f, 36.0f)))
        importMeshFromGui();
}

// ---------------------------------------------------------------------------
// AI section: a thin wrapper. The chat draws its own body into the pane, so
// there is no window, position, or visibility state of its own any more.
// ---------------------------------------------------------------------------
void App::drawPanelAI()
{
    sectionHeader("AI ASSISTANT", "object authoring via tools");
    auto reloadFn = [this] { requestWorldReload(); };
    m_chatUi.drawPanel(m_editable, m_layers, m_hoverHit.hit ? &m_hoverHit : nullptr,
                       m_hasSelection ? &m_selectedHit : nullptr, m_hasSelection,
                       reloadFn);
}

// ---------------------------------------------------------------------------
// Scene overlays: the trackball rings, the move handles, and the selection
// marker. Drawn on the foreground draw list, outside any window, because they
// belong to the scene rather than to any one section of the sidebar.
// ---------------------------------------------------------------------------
void App::drawSceneOverlays()
{
    ImGuiIO& io = ImGui::GetIO();
    if (m_gizmoValid && m_editBrush == EditBrush::Rotate) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 c(m_gizmoCentre.x, m_gizmoCentre.y);
        const float radius = m_gizmoRadius;
        const ImVec2 mouse = io.MousePos;
        const TrackballHandle hovered = trackballHandleAt(
            glm::vec2(mouse.x, mouse.y), m_gizmoCentre, radius);
        const ImU32 idle = IM_COL32(54, 160, 205, 220);
        const ImU32 hover = IM_COL32(220, 248, 255, 255);
        const ImU32 active = IM_COL32(255, 205, 92, 255);
        auto ringStyle = [&](TrackballHandle handle) {
            const bool isActive = m_rotating && m_rotateHandle == handle;
            const ImU32 color = isActive ? active
                                         : (hovered == handle ? hover : idle);
            return std::pair<ImU32, float>{color, isActive ? 3.5f :
                                           (hovered == handle ? 3.0f : 2.0f)};
        };
        const auto [yawColor, yawThickness] = ringStyle(TrackballHandle::Yaw);
        const auto [pitchColor, pitchThickness] = ringStyle(TrackballHandle::Pitch);
        const auto [rollColor, rollThickness] = ringStyle(TrackballHandle::Roll);
        dl->AddCircle(c, radius, yawColor, 0, yawThickness);
        dl->AddEllipse(c, ImVec2(radius, radius * 0.42f),
                       pitchColor, 0, 48, pitchThickness);
        dl->AddEllipse(c, ImVec2(radius * 0.42f, radius),
                       rollColor, 0, 48, rollThickness);
        // Local-axis labels make the three otherwise-similar rings
        // discoverable: outer/Y, wide/X, tall/Z.
        dl->AddText(ImVec2(c.x + radius + 5.0f, c.y - 7.0f), yawColor, "Y");
        dl->AddText(ImVec2(c.x - 3.0f, c.y - radius * 0.42f - 16.0f),
                    pitchColor, "X");
        dl->AddText(ImVec2(c.x + radius * 0.42f + 5.0f, c.y - radius - 16.0f),
                    rollColor, "Z");
        dl->AddCircleFilled(c, 4.0f, hovered == TrackballHandle::None
                                      ? idle : hover, 20);
    } else if (m_gizmoValid && m_editBrush == EditBrush::Move &&
               !m_moveLayer.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const vf::voxel::WorldAABB b = m_layers.layerBox(m_moveLayer);
        if (b.valid()) {
            const glm::vec3 centre = 0.5f * (b.lo + b.hi);
            const ImVec2 display = io.DisplaySize;
            const glm::ivec2 fb(static_cast<int>(display.x),
                                static_cast<int>(display.y));
            const float tanHalfFov = tanf(glm::radians(60.0f) * 0.5f);
            const ImU32 axisColors[3] = {
                IM_COL32(244, 96, 96, 235),  // X
                IM_COL32(104, 226, 132, 235), // Y
                IM_COL32(104, 176, 255, 235)  // Z
            };
            const MoveAxis axes[3] = { MoveAxis::X, MoveAxis::Y, MoveAxis::Z };
            const glm::vec2 p0 = projectScreen(centre, m_camera, tanHalfFov, fb);
            const ImVec2 centreI(p0.x, p0.y);
            for (int i = 0; i < 3; ++i) {
                const MoveAxis axis = axes[i];
                const MoveAxisLine line = moveAxisScreenLine(
                    centre, axis, m_camera, tanHalfFov, fb, m_gizmoRadius);
                const bool selected = m_moveAxis == axis;
                const ImU32 color = selected ? IM_COL32(255, 205, 92, 255)
                                             : axisColors[i];
                dl->AddLine(ImVec2(line.a.x, line.a.y),
                            ImVec2(line.b.x, line.b.y), color,
                            selected ? 3.5f : 2.0f);
                dl->AddCircleFilled(ImVec2(line.b.x, line.b.y),
                                    selected ? 5.0f : 3.5f, color, 20);
                dl->AddText(ImVec2(line.b.x + 5.0f, line.b.y - 7.0f), color,
                            moveAxisName(axis));
            }
            dl->AddCircleFilled(centreI, 4.0f, IM_COL32(220, 248, 255, 235), 20);
            if (m_moveStaged) {
                const float amount = m_moveAxis == MoveAxis::X ? m_moveDelta.x
                                  : m_moveAxis == MoveAxis::Y ? m_moveDelta.y
                                  : m_moveAxis == MoveAxis::Z ? m_moveDelta.z : 0.f;
                char text[64];
                snprintf(text, sizeof(text), "Move %s %+.2f m%s",
                         moveAxisName(m_moveAxis), amount,
                         m_moving ? " (dragging)" : " (staged)");
                dl->AddText(ImVec2(centreI.x - 48.0f, centreI.y + 22.0f),
                            IM_COL32(255, 205, 92, 255), text);
            }
        }
    }

    if (m_hasSelection) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const ImVec2 marker(centre.x - 42.0f, centre.y + 25.0f);
        dl->AddCircleFilled(marker, 3.5f, IM_COL32(64, 224, 160, 235), 16);
        dl->AddText(ImVec2(centre.x - 33.0f, centre.y + 20.0f),
                    IM_COL32(160, 245, 210, 245), "Selected voxel");
    }
}

bool App::runSelftest()
{
    vkDeviceWaitIdle(m_ctx.device()); // all frames must finish before layout surgery
    std::vector<uint8_t> pixels;
    if (!vf::readbackImage2D(m_ctx, m_offscreen.img, m_offscreen.extent.width,
                             m_offscreen.extent.height, pixels)) {
        spdlog::error("selftest: readback failed");
        return false;
    }
    const uint32_t W = m_offscreen.extent.width, H = m_offscreen.extent.height;
    size_t geometryPixels = 0, total = size_t(W) * H;
    for (size_t p = 0; p < total; ++p) {
        uint8_t r = pixels[p * 4], g = pixels[p * 4 + 1], b = pixels[p * 4 + 2];
        bool isSky = b > r + 12 && g > r + 4 && b > 120; // blue-dominant sky
        if (!isSky)
            ++geometryPixels;
    }
    float geoRatio = float(geometryPixels) / float(total);

    // sky probe: upper-right area, clear of the default HUD window position
    uint32_t sx = W * 15 / 16, sy = H / 8;
    size_t sIdx = (size_t(sy) * W + sx) * 4;
    uint8_t tr = pixels[sIdx], tg = pixels[sIdx + 1], tb = pixels[sIdx + 2];
    bool skyOk = tb >= tr;

    spdlog::info("selftest[voxel]: geometry coverage {:.1f}%, sky probe ({},{},{})",
                 geoRatio * 100.0f, tr, tg, tb);
    // region diagnostics: 3x3 grid average colors
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            uint64_t r = 0, g = 0, b = 0;
            size_t n = 0;
            uint32_t x0 = uint32_t(gx) * W / 3, x1 = uint32_t(gx + 1) * W / 3;
            uint32_t y0 = uint32_t(gy) * H / 3, y1 = uint32_t(gy + 1) * H / 3;
            for (uint32_t y = y0; y < y1; y += 4)
                for (uint32_t x = x0; x < x1; x += 4) {
                    size_t i = (size_t(y) * W + x) * 4;
                    r += pixels[i];
                    g += pixels[i + 1];
                    b += pixels[i + 2];
                    ++n;
                }
            if (!n)
                continue;
            fprintf(stderr, "[grid %d,%d] avg (%u,%u,%u)\n", gx, gy,
                    unsigned(r / n), unsigned(g / n), unsigned(b / n));
        }
    }

    // Coverage bounds match visual_check.py: clear shallow-water coves can
    // legitimately render up to ~98% non-sky pixels (water/bed count as
    // geometry); a buried camera still fails via ~100% + sky probe.
    if (geoRatio < 0.03f || !skyOk || geoRatio > 0.985f) {
        spdlog::error("selftest FAILED");
        return false;
    }
    spdlog::info("selftest PASSED");
    return true;
}

int App::run(const Args& args)
{
    const auto finiteEnv = [](const char* text, float fallback) {
        if (!text || !*text)
            return fallback;
        char* end = nullptr;
        const float value = std::strtof(text, &end);
        return end != text && std::isfinite(value) ? value : fallback;
    };
    {
        const float e = glm::radians(args.sunElev), a = glm::radians(args.sunAzim);
        m_sunDir = glm::vec4(
            glm::normalize(glm::vec3(cosf(e) * sinf(a), sinf(e), cosf(e) * cosf(a))), 0.0f);
    }
    m_animTime = args.animTime;
    m_tonemapLook = args.tonemap;
    if (const char* smooth = getenv("VF_SMOOTH_STRENGTH")) {
        m_smoothStrength = std::clamp(finiteEnv(smooth, m_smoothStrength), 0.0f, 1.0f);
    } else if (const char* strength = getenv("VF_EDIT_STRENGTH")) {
        m_smoothStrength = std::clamp(finiteEnv(strength, m_smoothStrength), 0.0f, 1.0f);
    }
    if (args.probeSet) {
        // probes read the live layered world (ai_edits included as a layer)
        vf::voxel::LayeredWorld probeWorld;
        const std::string manifest = std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
        if (!probeWorld.load(manifest)) {
            spdlog::critical("probe: cannot load world.json");
            return 1;
        }
        // VF_PROBE_RELOAD=N: re-run load() N times on the warm instance to
        // measure the interactive toggle / edit reload cost
        if (const char* r = getenv("VF_PROBE_RELOAD")) {
            int n = atoi(r);
            for (int i = 0; i < n; ++i) {
                auto t0 = std::chrono::steady_clock::now();
                probeWorld.load(manifest);
                spdlog::info("probe: warm reload {:d}: {:.0f} ms", i,
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count());
            }
        }
        auto s = probeWorld.field().sampleWorld(args.probe);
        spdlog::info("probe({:.2f},{:.2f},{:.2f}): d={:+.3f} mat={} tex={} {}", args.probe.x,
                     args.probe.y, args.probe.z, s.d, int(s.mat), int(s.tex),
                     s.d < 0 ? "solid" : "empty");
        return 0;
    }
    if (!std::filesystem::exists(std::string(VOXELFORGE_ASSET_DIR) + "/world.json")) {
        spdlog::critical("assets/world.json missing - run 'ninja -C build world' to bake assets first");
        return 1;
    }
    if (!initWindow(args)) {
        spdlog::critical("window init failed");
        return 1;
    }
    // persistent AI edits: survives restarts, hot-reload via layered world poll
    m_editable.load();
    m_editable.ensureManifest();
    // m_args stored for chat ui
    m_args = args;
    if (!initVulkan()) {
        spdlog::critical("vulkan init failed");
        destroy();
        return 1;
    }

    // Headless exercise of the exact GUI import path.  The comma-separated
    // form is: file,name,x,y,z,fit,mat,rotY,solid (the last three are
    // optional).  It is useful for CI and for re-authoring a known model
    // without having to drive a native ImGui window; the interactive button
    // calls the same importMeshFromGui() method.
    if (const char* tm = getenv("VF_TEST_MESH_IMPORT"); tm && *tm) {
        std::vector<std::string> fields;
        std::stringstream spec(tm);
        std::string field;
        while (std::getline(spec, field, ','))
            fields.push_back(field);
        int x = 512, y = 512, z = 512, mat = 6, solid = 1;
        float fit = 5.0f, rotY = 0.0f;
        const bool valid = fields.size() >= 6 &&
                           sscanf(fields[2].c_str(), "%d", &x) == 1 &&
                           sscanf(fields[3].c_str(), "%d", &y) == 1 &&
                           sscanf(fields[4].c_str(), "%d", &z) == 1 &&
                           sscanf(fields[5].c_str(), "%f", &fit) == 1;
        if (fields.size() >= 7)
            sscanf(fields[6].c_str(), "%d", &mat);
        if (fields.size() >= 8)
            sscanf(fields[7].c_str(), "%f", &rotY);
        if (fields.size() >= 9)
            sscanf(fields[8].c_str(), "%d", &solid);
        if (!valid || fields[0].empty() || fields[1].empty()) {
            spdlog::warn("bad VF_TEST_MESH_IMPORT; expected "
                         "file,name,x,y,z,fit[,mat,rotY,solid]");
        } else {
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", fields[0].c_str());
            std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s",
                          fields[1].c_str());
            m_meshAnchor = { x, y, z };
            m_meshUseFit = true;
            m_meshFitMeters = fit;
            m_meshMaterial = std::clamp(mat, 0, int(vf::voxel::kPaletteN) - 1);
            m_meshRotY = rotY;
            m_meshSolid = solid != 0;
            m_meshImportPrepared = true;
            rescanMeshFiles();
            if (importMeshFromGui()) {
                m_panel = Panel::Mesh;
                // The hook is deterministic: force its reload synchronously so
                // the first captured frame already contains the new layer.
                const char* oldSync = getenv("VF_SYNC_RELOAD");
                const bool hadSync = oldSync != nullptr;
                const std::string oldSyncValue = hadSync ? oldSync : "";
                setenv("VF_SYNC_RELOAD", "1", 1);
                m_layers.requestReload(m_camera.pos, true);
                if (hadSync)
                    setenv("VF_SYNC_RELOAD", oldSyncValue.c_str(), 1);
                else
                    unsetenv("VF_SYNC_RELOAD");
                m_pendingWorldReload = false;
                applyWorldReload();
            } else {
                spdlog::error("VF_TEST_MESH_IMPORT failed");
                return 1;
            }
        }
    }

    // VF_TEST_ROTATE="yaw,pitch,roll" on the layer named by VF_ROTATE_LAYER:
    // apply the trackball's commit path (manifest write) without a window, so
    // headless shots can verify the placement pipeline end-to-end.
    if (const char* tr = getenv("VF_TEST_ROTATE"); tr && *tr) {
        float y = 0.f, p = 0.f, r = 0.f;
        if (sscanf(tr, "%f,%f,%f", &y, &p, &r) == 3) {
            m_rotateDy = y; m_rotateDx = p; m_rotateDz = r;
            m_rotateLayer = "";
            if (const char* ln = getenv("VF_ROTATE_LAYER"))
                m_rotateLayer = ln;
            if (m_rotateLayer.empty()) {
                // default: the first enabled object layer in the manifest
                for (const auto& l : m_layers.layers())
                    if (l.enabled && l.role == "object" && l.file != vf::voxel::EditableWorld::kFileName) {
                        m_rotateLayer = l.file;
                        break;
                    }
            }
            if (!m_rotateLayer.empty()) {
                commitRotation();
                // the write happens after load, so a headless shot would
                // otherwise capture the pre-rotation world: force the
                // incremental rebuild now (interactive runs just wait for the
                // mtime poll)
                requestWorldReload();
                m_layers.requestReload(m_camera.pos, false);
                while (m_layers.consumeRebuild())
                    applyWorldReload();
            }
        }
    }

    // VF_TEST_EDIT="x,y,z,carve|add|delete|paint|smooth": apply one live store
    // edit right after load so headless shots can verify the patch path
    // deterministically. VF_EDIT_DIAM / VF_EDIT_DEPTH override the brush size;
    // Smooth uses VF_SMOOTH_STRENGTH (or its VF_EDIT_STRENGTH alias).
    if (const char* te = getenv("VF_TEST_EDIT"); te && *te) {
        glm::ivec3 v(0);
        char mode[16] = {};
        if (sscanf(te, "%d,%d,%d,%15s", &v.x, &v.y, &v.z, mode) == 4) {
            m_editActive = true;
            m_editBrush = brushFromName(mode);
            if (const char* d = getenv("VF_EDIT_DIAM"))
                m_editDiameter = finiteEnv(d, m_editDiameter);
            if (const char* d = getenv("VF_EDIT_DEPTH"))
                m_editDepth = finiteEnv(d, m_editDepth);
            quantiseBrush(); // snap onto the lattice: 1 voxel is reachable
            m_hoverHit = {};
            m_hoverHit.hit = true;
            m_hoverHit.voxel = v;
            m_hoverHit.normal = storeNormalAt(v);
            applyEditLive();
            spdlog::info("VF_TEST_EDIT {} {} {} {}", v.x, v.y, v.z,
                         brushName(m_editBrush));
        }
    }

    // VF_TEST_STROKE="x,y,z,steps[,carve|add|delete|paint|smooth]": simulate
    // drag-painting by stamping `steps` times along +X with the same spacing
    // the interactive stroke uses; logs per-stamp latency (instant-feedback
    // gate).
    if (const char* ts = getenv("VF_TEST_STROKE"); ts && *ts) {
        glm::ivec3 v(0);
        int steps = 8;
        char mode[16] = {};
        const int nf = sscanf(ts, "%d,%d,%d,%d,%15s", &v.x, &v.y, &v.z, &steps, mode);
        if (nf >= 4) {
            m_editActive = true;
            m_editBrush = nf >= 5 ? brushFromName(mode) : EditBrush::Add;
            if (const char* d = getenv("VF_EDIT_DIAM"))
                m_editDiameter = finiteEnv(d, m_editDiameter);
            if (const char* d = getenv("VF_EDIT_DEPTH"))
                m_editDepth = finiteEnv(d, m_editDepth);
            quantiseBrush(); // snap onto the lattice: 1 voxel is reachable
            const float strokeDiameter =
                std::clamp(m_editDiameter, vf::voxel::VOXEL, 32.0f);
            const int spacing = std::max(
                1, int(strokeDiameter * 0.25f / vf::voxel::VOXEL));
            std::vector<float> ms;
            ms.reserve(size_t(steps));
            for (int i = 0; i < steps; ++i) {
                glm::ivec3 p = v + glm::ivec3(i * spacing, 0, 0);
                m_hoverHit = {};
                m_hoverHit.hit = true;
                m_hoverHit.voxel = p;
                m_hoverHit.normal = storeNormalAt(p);
                const auto t0 = std::chrono::steady_clock::now();
                m_dragging = true;
                applyEditLive();
                ms.push_back(float(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count()));
            }
            m_dragging = false;
            std::vector<float> sorted = ms;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0;
            for (float t : ms)
                sum += t;
            spdlog::info("stroke: {} stamps, avg {:.1f} ms, p50 {:.1f}, max {:.1f} "
                         "(spacing {} cells)",
                         steps, sum / std::max<size_t>(1, ms.size()),
                         sorted.empty() ? 0.f : sorted[sorted.size() / 2],
                         sorted.empty() ? 0.f : sorted.back(), spacing);
            if (getenv("VF_TEST_STROKE_SAVE")) {
                // simulate the stroke-end async persistence and wait for it
                m_overlayWriter.queue(m_layers.store(), overlayPath());
                m_overlayWriter.flush();
                spdlog::info("stroke: saved live overlay");
            }
        }
    }

    // VF_TEST_UNDO=1: close the pending stroke and undo it (headless check of
    // the undo path); VF_TEST_CLEAR=1: drop every runtime edit + the overlay
    // file (headless check of the "Clear live edits" button).
    if (getenv("VF_TEST_UNDO")) {
        finishStroke();
        spdlog::info("VF_TEST_UNDO: {} stroke(s) on the undo stack before",
                     m_undo.size());
        undoEdit();
    }
    if (getenv("VF_TEST_CLEAR"))
        clearLiveEdits();

    // ImGui ---------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    // Dense professional editor theme: midnight surfaces, one cyan action
    // colour, restrained borders. Tokens stay local to ImGui so rendering
    // and world content are unaffected.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.CellPadding = ImVec2(7.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 10.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.055f, 0.090f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.070f, 0.110f, 0.96f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.070f, 0.110f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.30f, 0.40f, 0.55f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.94f, 0.98f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.57f, 0.67f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.070f, 0.105f, 0.155f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.085f, 0.190f, 0.245f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.075f, 0.310f, 0.385f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.035f, 0.055f, 0.090f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.055f, 0.145f, 0.205f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.065f, 0.160f, 0.220f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.080f, 0.310f, 0.390f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.090f, 0.390f, 0.480f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.075f, 0.145f, 0.215f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.080f, 0.300f, 0.380f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.090f, 0.420f, 0.510f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.13f, 0.83f, 0.93f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.65f, 0.76f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.18f, 0.88f, 0.96f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.30f, 0.40f, 0.65f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.060f, 0.105f, 0.155f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.16f, 0.25f, 0.34f, 0.75f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.040f, 0.065f, 0.100f, 1.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.050f, 0.080f, 0.120f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.08f, 0.36f, 0.45f, 0.55f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.13f, 0.83f, 0.93f, 0.65f);
    ImGui_ImplGlfw_InitForVulkan(m_window.handle(), true);

    ImGui_ImplVulkan_InitInfo vi {};
    vi.Instance = m_ctx.instance();
    vi.PhysicalDevice = m_ctx.physicalDevice();
    vi.Device = m_ctx.device();
    vi.QueueFamily = m_ctx.graphicsFamily();
    vi.Queue = m_ctx.graphicsQueue();
    vi.MinImageCount = 3;
    vi.ImageCount = uint32_t(m_swapchain.imageCount());
    vi.DescriptorPoolSize = 128;
    vi.UseDynamicRendering = true;
    vi.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    vi.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineRenderingCreateInfo.pColorAttachmentFormats = m_swapchain.formatPtr();
    vi.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS)
            spdlog::error("ImGui Vulkan backend error {}", int(r));
    };
    ImGui_ImplVulkan_Init(&vi);
    ImGui_ImplVulkan_CreateFontsTexture();

    // linear sampler for UI textures (e.g. the live scene-preview thumbnail)
    VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx.device(), &sci, nullptr, &m_uiSampler);
    m_scenePreview = getenv("VF_SCENE_PREVIEW") != nullptr;

    // AI chat
    if (!m_chatInitialized) {
        m_chatUi.init(args.llmUrl, args.llmModel);
        m_chatInitialized = true;
    }

    // per-backend camera spawn
    // reference view (house.jpeg): over the pond toward the cabin, dock
    // left-of-centre, cabin right, sun raking from the west
    m_camera.pos = { 1.0f, 2.0f, 1.5f };
    glm::vec3 dir = glm::normalize(glm::vec3(5.3f, 1.0f, 11.3f) - m_camera.pos);
    m_camera.yaw = atan2(dir.z, dir.x);
    m_camera.pitch = asin(dir.y);

    // --shot holds one view (its camera comes from --cam); --shotlist holds
    // any number. Both share the headless capture loop below.
    std::vector<ShotSpec> shots = args.shots;
    if (shots.empty() && !args.shot.empty()) {
        ShotSpec s;
        s.path = args.shot;
        s.camx = args.camx; s.camy = args.camy; s.camz = args.camz;
        s.tx = args.tx;     s.ty = args.ty;     s.tz = args.tz;
        shots.push_back(s);
    }
    if (!shots.empty()) {
        m_camera.pos = { shots[0].camx, shots[0].camy, shots[0].camz };
        glm::vec3 d = glm::normalize(
            glm::vec3(shots[0].tx, shots[0].ty, shots[0].tz) - m_camera.pos);
        m_camera.yaw = atan2(d.z, d.x);
        m_camera.pitch = asin(d.y);
    } else if (args.camSet) {
        m_camera.pos = { args.camx, args.camy, args.camz };
        glm::vec3 dir = glm::normalize(glm::vec3(args.tx, args.ty, args.tz) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
    }

    const bool shotMode = !shots.empty();
    // a shotlist run is headless too (anim clock frozen, no window input)
    const bool shotRun = !args.shot.empty() || !args.shots.empty();
    // "frames:path": after N presented frames, dump the swapchain (incl. HUD)
    uint64_t hudShotFrame = 0;
    std::string hudShotPath;
    if (const char* hs = getenv("VF_HUD_SHOT")) {
        char* endp = nullptr;
        hudShotFrame = strtoull(hs, &endp, 10);
        if (!endp || *endp != ':' || hudShotFrame == 0) {
            hudShotFrame = 0;
            spdlog::warn("bad VF_HUD_SHOT, expected frames:path");
        } else {
            hudShotPath = endp + 1;
        }
    }
    const float tanHalfFov = tanf(glm::radians(60.0f) * 0.5f);
    auto last = std::chrono::steady_clock::now();
    // debug/CI override for the render-flag bitmask (AO/shadow/flora/water/outline)
    if (const char* rf = getenv("VF_RENDER_FLAGS"))
        m_renderFlags = atoi(rf);
    // headless/CI overrides for the photorealism toggles (default off = deterministic)
    if (const char* e = getenv("VF_VOLFOG"))
        m_volFogEnabled = atoi(e) != 0;
    if (const char* e = getenv("VF_MOTIONBLUR"))
        m_motionBlurEnabled = atoi(e) != 0;
    if (const char* e = getenv("VF_DOF"))
        m_dofEnabled = atoi(e) != 0;
    // SSAO tuning (world-scale AO; the effect itself stays opt-in via bit 6)
    if (const char* e = getenv("VF_SSAO_STRENGTH"))
        m_ssaoStrength = std::clamp(float(atof(e)), 0.0f, 1.0f);
    if (const char* e = getenv("VF_SSAO_RADIUS"))
        m_ssaoRadius = std::clamp(float(atof(e)), 0.1f, 4.0f);
    if (const char* e = getenv("VF_SSAO_DEBUG"))
        m_ssaoDebug = atoi(e);
    if (const char* e = getenv("VF_SSAO_BLUR"))
        m_ssaoBlur = atoi(e) != 0;
    // TAA history blend override (debug): 1.0 = pure reprojected history.
    // In a static scene that must stay coherent while rotating (1 frame of
    // lag); if it tears instead, the G-buffer reprojection itself is broken.
    if (const char* e = getenv("VF_TAA_BLEND"))
        m_taaBlend = std::clamp(float(atof(e)), 0.0f, 1.0f);

    while (!m_window.shouldClose()) {
        m_window.pollEvents();

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::clamp(dt, 1e-5f, 0.1f);
        m_lastFrameMs = dt * 1000.0f;
        m_avgMs += (m_lastFrameMs - m_avgMs) * 0.05;
        m_minMs = std::min(m_minMs, m_lastFrameMs);
        m_maxMs = std::max(m_maxMs, m_lastFrameMs);

        // animation clock only advances interactively - headless shots stay
        // deterministic (misc.y feeds wind/grass shading)
        const bool headlessRun =
            args.selftest || args.smokeFrames > 0 || shotRun;
        if (!headlessRun)
            m_animTime += dt;

        if (m_window.resized()) {
            m_window.clearResized();
            handleResize();
        }

        // GUI test hook: toggle one layer exactly like the checkbox does
        static const char* guiTestName = getenv("VF_GUI_TEST");
        if (guiTestName && *guiTestName && m_frameIdx == 20 && !m_worldLayers.empty()) {
            for (auto& l : m_worldLayers) {
                if (l.role != "landscape" && l.name == guiTestName) {
                    l.enabled = !l.enabled;
                    if (l.enabled)
                        l.listed = true;
                    persistWorldLayers();
                    spdlog::info("VF_GUI_TEST: {} -> {}", l.name,
                                 l.enabled ? "enabled" : "disabled");
                    break;
                }
            }
            m_pendingWorldReload = true;
        }

        // test hook: deterministic selection for headless highlight shots
        static const char* testSel = getenv("VF_TEST_SELECT");
        if (testSel && *testSel && !m_hasSelection && m_layers.loaded()) {
            glm::ivec3 v;
            if (sscanf(testSel, "%d,%d,%d", &v.x, &v.y, &v.z) == 3) {
                m_selectedHit = {};
                m_selectedHit.hit = true;
                m_selectedHit.voxel = v;
                m_selectedHit.mat =
                    m_layers.field().sampleWorld(vf::voxel::voxelCenter(v)).mat;
                m_hasSelection = true;
                spdlog::info("VF_TEST_SELECT {} {} {}", v.x, v.y, v.z);
            }
        }

        // live world reload: MCP/chat edits and layer toggles land in the
        // layer files; poll for changes and swap the SVO in-place. Reloads are
        // camera-distance-priority and run off the render thread (see
        // LayeredWorld), so an in-progress rebuild never blocks a frame; the
        // finished world is swapped in here on the next consumeRebuild().
        m_layerPollT += dt;
        if (m_layers.loaded() && m_layerPollT >= 0.5f) {
            m_layerPollT = 0.f;
            if (m_layers.reloadIfChanged(m_camera.pos) == vf::voxel::LayeredWorld::kSyncDone)
                applyWorldReload();
        }
        if (m_layers.consumeRebuild())
            applyWorldReload();
        if (m_pendingWorldReload) {
            m_pendingWorldReload = false;
            m_layers.requestReload(m_camera.pos, true);
        }

        // texture hot-swap: a picker edit (write world.json + re-upload) or an
        // on-disk change of a bound image (re-upload only) lands here, between
        // frames, so no recorded command buffer is ever left sampling the
        // atlas while it is rewritten. The apply wins over the reload.
        if (m_texApplyPending) {
            m_texApplyPending = false;
            m_texReloadPending = false;
            applyTextureBindings();
        } else if (m_texReloadPending) {
            m_texReloadPending = false;
            reloadTexAtlas();
        }
        m_texPollT += dt;
        if (m_texPollT >= 1.0f) {
            m_texPollT = 0.f;
            pollTextureFiles();
        }

        // VF_TEST_ROTATE_LIVE is a synthetic, persistent preview state. Keep
        // it out of the interactive release/ring paths so a headless shot
        // cannot commit the manifest or accumulate cursor-driven deltas.
        const char* testRotateLive = getenv("VF_TEST_ROTATE_LIVE");
        const bool rotateLiveTest = testRotateLive && *testRotateLive;

        // Update the camera before any screen-space interaction. Picking,
        // trackball hit-testing, and rendering must all use the same pose;
        // otherwise the visible ring trails a moving camera by one frame.
        bool chatCaptures = m_chatInitialized && m_chatUi.wantsCaptureKeyboard();
        if (chatCaptures) {
            // block camera move while typing; consume mouse delta to avoid jump
            double _dx, _dy;
            m_window.getMouseDelta(_dx, _dy);
        } else {
            m_camera.update(m_window, dt);
        }

        // Voxel picking: Ctrl+LMB
        {
            ImGuiIO& pickIo = ImGui::GetIO();
            bool ctrl = glfwGetKey(m_window.handle(), GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS ||
                        glfwGetKey(m_window.handle(), GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
            double mx=0,my=0;
            glfwGetCursorPos(m_window.handle(), &mx, &my);
            glm::ivec2 fb = m_window.framebufferSize();
            // GLFW cursor coordinates are logical window pixels, while
            // framebufferSize() is physical pixels. Use ImGui's logical space
            // for picking so high-DPI activation clicks hit the same object as
            // the trackball rings.
            const ImVec2 pickDisplay = pickIo.DisplaySize;
            if (pickDisplay.x > 0.f && pickDisplay.y > 0.f)
                fb = glm::ivec2(int(pickDisplay.x), int(pickDisplay.y));
            const float pickSidebarW = m_sidebarCollapsed
                ? std::min(kRailW, pickDisplay.x)
                : sidebarWidthFor(m_sidebarWidth, pickDisplay.x);
            const bool overSidebarResize =
                !m_sidebarCollapsed && overSidebarResizeGrip(
                    pickIo, pickSidebarW, pickDisplay.y);
            const bool sidebarResizeCapturesMouse =
                overSidebarResize || m_sidebarResizing;
            bool wantMouse = pickIo.WantCaptureMouse ||
                            sidebarResizeCapturesMouse;
            bool lmb = glfwGetMouseButton(m_window.handle(), GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
            // hover while Ctrl held (anchor pick) OR while the edit tool is active
            // (so an LMB click can stamp a carve/add at the pointed surface)
            bool computeHover = (ctrl || m_editActive) && !wantMouse && fb.x>0 && fb.y>0;
            if (computeHover) {
                float tanHalf = tanHalfFov;
                float aspect = float(fb.x)/float(fb.y);
                glm::vec3 rd = vf::voxel::screenRayDir(mx,my,fb.x,fb.y,tanHalf,aspect,
                                                       m_camera.forward(), m_camera.right(), m_camera.up());
                m_hoverHit = vf::voxel::rayPickStore(m_layers.store(),
                                                     m_camera.pos, rd);
            } else {
                m_hoverHit.hit = false;
            }
            // forgiving trigger: fire on whichever edge arrives second, so a
            // few ms between LMB-down and Ctrl-down still picks
            bool lmbEdge = lmb && !m_lmbWasDown;
            bool ctrlEdge = ctrl && !m_ctrlWasDown;
            m_lmbWasDown = lmb;
            m_ctrlWasDown = ctrl;
            bool justPressed =
                ((lmbEdge && ctrl) || (ctrlEdge && lmb)) && !wantMouse;
            if (justPressed && m_hoverHit.hit) {
                m_selectedHit = m_hoverHit;
                m_hasSelection = true;
                const std::string pickedLayer = rotateTargetLayer(m_selectedHit);
                if (!pickedLayer.empty())
                    m_selectedLayer = pickedLayer;
                glm::vec3 w = vf::voxel::voxelCenter(m_selectedHit.voxel);
                spdlog::info("pick selected {} {} {} world {:.2f} {:.2f} {:.2f} mat {} layer {}",
                    m_selectedHit.voxel.x, m_selectedHit.voxel.y, m_selectedHit.voxel.z,
                    w.x,w.y,w.z, int(m_selectedHit.mat),
                    pickedLayer.empty() ? "<terrain/unowned>" : pickedLayer);
            }
            // Edit tool: plain LMB stamps at the hover point. With "Live patch"
            // on, holding LMB keeps painting (stamp spacing = a quarter brush
            // diameter) so the result appears while drawing; the stroke is
            // saved asynchronously on release.
            const bool editLmb = lmb && !ctrl && !wantMouse && m_editActive &&
                                 m_editBrush != EditBrush::Rotate &&
                                 m_editBrush != EditBrush::Move;
            bool doStamp = lmbEdge && editLmb;
            if (editLmb && m_hoverHit.hit) {
                // at most one stamp per voxel crossing: a per-voxel brush that
                // re-stamped every 0.08 m would redo the same cell 1.25x
                const float spacing =
                    std::max(vf::voxel::VOXEL, m_editDiameter * 0.25f);
                if (!m_hasStamp ||
                    glm::distance(vf::voxel::voxelCenter(m_hoverHit.voxel),
                                  vf::voxel::voxelCenter(m_lastStampVoxel)) >= spacing) {
                    // ...and a click is ONE edit. Measured from the PRESS point,
                    // so a held click whose own output became the next pick
                    // (the new voxel's top face) stays put and does not stack.
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    const float travel = std::hypot(mp.x - m_stampPressMouse.x,
                                                    mp.y - m_stampPressMouse.y);
                    if (!m_hasStamp || travel >= kDragTravelPx)
                        doStamp = true;
                }
            }
            if (doStamp && m_hoverHit.hit) {
                m_lastStampVoxel = m_hoverHit.voxel;
                if (!m_hasStamp) {
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    m_stampPressMouse = glm::vec2(mp.x, mp.y);
                }
                m_hasStamp = true;
                m_dragging = editLmb;
                applyEditLive();
            }
            // Trackball workflow: one plain click on an owned object activates
            // it and shows bounds-sized rings. A later press on the yaw,
            // pitch, or roll ring starts the live drag; release stages it and
            // leaves the same object active for another adjustment or Apply.
            const glm::vec2 inputMouse{float(mx), float(my)};
            const bool overTrackball = !sidebarResizeCapturesMouse &&
                m_gizmoValid &&
                trackballHandleAt(inputMouse, m_gizmoCentre, m_gizmoRadius) !=
                    TrackballHandle::None;
            MoveAxis hoveredMoveAxis = MoveAxis::None;
            if (!sidebarResizeCapturesMouse && m_gizmoValid && m_editActive &&
                m_editBrush == EditBrush::Move && !m_moveLayer.empty()) {
                const vf::voxel::WorldAABB moveBox = m_layers.layerBox(m_moveLayer);
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const glm::ivec2 fb(static_cast<int>(display.x),
                                    static_cast<int>(display.y));
                const float fov = tanf(glm::radians(60.0f) * 0.5f);
                hoveredMoveAxis = moveAxisHandleAt(
                    inputMouse, 0.5f * (moveBox.lo + moveBox.hi), m_camera,
                    fov, fb, m_gizmoRadius);
            }
            const bool overMoveHandle = hoveredMoveAxis != MoveAxis::None;
            const bool rotateMode = m_editActive && !ctrl &&
                                    (!wantMouse || overTrackball) &&
                                    m_editBrush == EditBrush::Rotate &&
                                    !m_rotationPreviewPending;
            const bool rotLmb = lmb && rotateMode;
            if (!rotLmb && !m_rotating && !m_rotationPreviewPending &&
                !m_rotationStaged && !m_moveStaged && !m_moving)
                m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
            // m_rotateLayer is activation state, not button state: keep it
            // while Rotate remains selected, including after mouse-up/reload.
            if (m_editBrush != EditBrush::Rotate &&
                !m_rotationPreviewPending) {
                if (m_rotating) {
                    m_rotating = false;
                    m_rotateHandle = TrackballHandle::None;
                    m_rotateLastMouse = {};
                    m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
                }
                // A staged pose survives a temporary mode switch so the
                // explicit Apply/Cancel controls remain meaningful. It is
                // cleared only by Apply, Cancel, or target loss.
                if (!m_rotationStaged) {
                    m_rotateLayer.clear();
                    m_rotateHandle = TrackballHandle::None;
                    m_rotateLastMouse = {};
                    m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                }
            }
            if (m_editBrush != EditBrush::Move && !m_moveStaged) {
                if (m_moving)
                    cancelMove();
                m_moveLayer.clear();
            }

            // Ring input takes precedence over the object ray pick: clicking
            // a ring must rotate the already activated object, even when the
            // ring lies over another surface in screen space.
            if (rotLmb && lmbEdge && !m_rotating && m_gizmoValid) {
                m_rotateHandle = trackballHandleAt(
                    inputMouse, m_gizmoCentre, m_gizmoRadius);
                if (m_rotateHandle != TrackballHandle::None) {
                    m_rotating = true;
                    m_rotateLastMouse = inputMouse;
                    if (!m_rotationStaged)
                        m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                    m_taaFirstFrame = true;
                    if (getenv("VF_TRACE"))
                        spdlog::info("trackball input: ring={} centre=({:.1f},{:.1f}) radius={:.1f}",
                                     int(m_rotateHandle), m_gizmoCentre.x,
                                     m_gizmoCentre.y, m_gizmoRadius);
                }
            }

            // A plain click activates exactly the object under the cursor.
            // It does not start rotation, so selecting and manipulating are
            // separate, discoverable actions.
            if (rotLmb && lmbEdge && !m_rotating && m_hoverHit.hit) {
                const std::string picked = rotateTargetLayer(m_hoverHit);
                if (!picked.empty() && m_layers.layerId(picked) != 0) {
                    if (m_rotationStaged && m_rotateLayer != picked) {
                        spdlog::warn("rotate: apply or cancel the staged pose before selecting another object");
                    } else {
                        m_rotateLayer = picked;
                        m_selectedLayer = picked;
                        m_selectedHit = m_hoverHit;
                        m_hasSelection = true;
                        m_rotating = false;
                        m_rotateHandle = TrackballHandle::None;
                        if (!m_rotationStaged)
                            m_rotateDy = m_rotateDx = m_rotateDz = 0.f;
                        m_taaFirstFrame = true;
                        spdlog::info("rotate: activated {}", m_rotateLayer);
                    }
                }
            }

            if (m_rotating && !rotateLiveTest) {
                const glm::vec2 delta = inputMouse - m_rotateLastMouse;
                m_rotateLastMouse = inputMouse;
                const float degreesPerPixel =
                    120.f / (glm::pi<float>() * std::max(m_rotateRadius, 48.f));
                float localAngle = 0.0f;
                vf::voxel::worldfile::PlacementAxis localAxis =
                    vf::voxel::worldfile::PlacementAxis::Y;
                switch (m_rotateHandle) {
                case TrackballHandle::Yaw:
                    localAxis = vf::voxel::worldfile::PlacementAxis::Y;
                    localAngle = delta.x * degreesPerPixel;
                    break;
                case TrackballHandle::Pitch:
                    localAxis = vf::voxel::worldfile::PlacementAxis::X;
                    localAngle = -delta.y * degreesPerPixel;
                    break;
                case TrackballHandle::Roll:
                    localAxis = vf::voxel::worldfile::PlacementAxis::Z;
                    localAngle = delta.x * degreesPerPixel;
                    break;
                case TrackballHandle::None:
                    m_rotating = false;
                    break;
                }

                if (m_rotateHandle != TrackballHandle::None) {
                    // Compose the drag on the right of the current absolute
                    // pose: this is a rotation about the object's own local
                    // axis, not a camera/world-axis Euler increment. Convert
                    // back to the manifest's Ry*Rx*Rz angles only at the
                    // preview/commit boundary.
                    const auto layer = std::find_if(
                        m_worldLayers.begin(), m_worldLayers.end(),
                        [&](const vf::voxel::worldfile::WorldLayer& l) {
                            return l.file == m_rotateLayer;
                        });
                    glm::vec3 pivot;
                    if (layer != m_worldLayers.end() &&
                        m_layers.layerPivot(m_rotateLayer, pivot)) {
                        const glm::mat3 oldR =
                            vf::voxel::worldfile::placementRotation(
                                layer->rotDeg, layer->rotX, layer->rotZ);
                        const glm::mat3 currentR =
                            vf::voxel::worldfile::placementRotation(
                                layer->rotDeg + m_rotateDy,
                                layer->rotX + m_rotateDx,
                                layer->rotZ + m_rotateDz);
                        const glm::mat3 nextR =
                            vf::voxel::worldfile::rotatePlacementLocal(
                                currentR, localAxis, localAngle);
                        const glm::vec3 nextEuler =
                            vf::voxel::worldfile::placementEuler(nextR);
                        m_rotateDy = nextEuler.x - layer->rotDeg;
                        m_rotateDx = nextEuler.y - layer->rotX;
                        m_rotateDz = nextEuler.z - layer->rotZ;
                        const glm::mat3 R = nextR * glm::transpose(oldR);
                        m_splatPass.setRotatePreview(
                            pivot, R, true, m_layers.layerId(m_rotateLayer));
                    }
                }
            }
            // Move mode uses the exact picked owner and one explicit world
            // axis. It stages the delta on release; Apply is the only write.
            const bool moveMode = m_editActive && !ctrl &&
                                  (!wantMouse || overMoveHandle) &&
                                  m_editBrush == EditBrush::Move &&
                                  !m_rotationStaged && !m_rotationPreviewPending;
            const bool moveLmb = lmb && moveMode;
            if (!moveMode && m_moving)
                cancelMove();
            if (moveLmb && lmbEdge && !m_moving &&
                (m_hoverHit.hit || overMoveHandle)) {
                const std::string picked = overMoveHandle
                    ? m_moveLayer : rotateTargetLayer(m_hoverHit);
                if (!picked.empty() && m_layers.layerId(picked) != 0) {
                    if (m_moveStaged && m_moveLayer != picked) {
                        spdlog::warn("move: apply or cancel the staged move before selecting another object");
                    } else {
                        if (m_moveLayer != picked) {
                            m_moveLayer = picked;
                            m_moveDelta = glm::vec3(0.f);
                            m_moveStaged = false;
                        }
                        if (overMoveHandle)
                            m_moveAxis = hoveredMoveAxis;
                        m_selectedLayer = picked;
                        if (m_hoverHit.hit)
                            m_selectedHit = m_hoverHit;
                        m_hasSelection = true;
                        m_moving = true;
                        m_moveLastMouse = inputMouse;
                        m_taaFirstFrame = true;
                        spdlog::info("move: grabbed {} on axis {}",
                                     m_moveLayer, moveAxisName(m_moveAxis));
                    }
                }
            }
            if (m_moving && moveLmb) {
                const glm::vec2 delta = inputMouse - m_moveLastMouse;
                m_moveLastMouse = inputMouse;
                constexpr float metresPerPixel = 0.05f;
                switch (m_moveAxis) {
                case MoveAxis::X: m_moveDelta.x += delta.x * metresPerPixel; break;
                case MoveAxis::Y: m_moveDelta.y -= delta.y * metresPerPixel; break;
                case MoveAxis::Z: m_moveDelta.z += delta.x * metresPerPixel; break;
                case MoveAxis::None: break;
                }
            }
            if (m_editBrush == EditBrush::Move && !m_moveLayer.empty() &&
                (m_moving || m_moveStaged)) {
                glm::vec3 pivot;
                if (m_layers.layerPivot(m_moveLayer, pivot)) {
                    m_splatPass.setRotatePreview(
                        pivot, glm::mat3(1.f), m_moveDelta, true,
                        m_layers.layerId(m_moveLayer));
                }
            }

            if (!lmb) {
                if (m_rotating && !rotateLiveTest) {
                    m_rotating = false;
                    m_rotateHandle = TrackballHandle::None;
                    m_rotateLastMouse = {};
                    if (std::abs(m_rotateDy) >= 1e-4f ||
                        std::abs(m_rotateDx) >= 1e-4f ||
                        std::abs(m_rotateDz) >= 1e-4f) {
                        m_rotationStaged = true;
                        spdlog::info("rotate: staged {} (press Apply to persist)", m_rotateLayer);
                    } else if (!m_rotationStaged) {
                        m_splatPass.setRotatePreview({}, glm::mat3(1.f), false, 0);
                    }
                }
                if (m_moving) {
                    m_moving = false;
                    m_moveLastMouse = {};
                    if (glm::length(m_moveDelta) >= 1e-4f) {
                        m_moveStaged = true;
                        spdlog::info("move: staged {} (press Apply to persist)", m_moveLayer);
                    }
                }
                if (m_hasStamp && m_liveEditor.attached()) {
                    // one undo step per stroke (mouse down..up)
                    finishStroke();
                    m_overlayWriter.queue(m_layers.store(), overlayPath());
                }
                m_hasStamp = false;
                m_dragging = false;
            }
        }

        // deterministic hover injection for headless shots: VF_TEST_HOVER
        // pins the hover point; VF_TEST_BRUSH additionally activates the edit
        // tool with a brush mode (diameter/depth via VF_EDIT_DIAM/DEPTH and
        // Smooth strength via VF_SMOOTH_STRENGTH) so the affected-splat preview
        // renders without any input.
        static const char* testHov = getenv("VF_TEST_HOVER");
        static const char* testBrush = getenv("VF_TEST_BRUSH");
        {
            glm::ivec3 v(0);
            bool have = false;
            if (testBrush && *testBrush) {
                char mode[16] = {};
                if (sscanf(testBrush, "%d,%d,%d,%15s", &v.x, &v.y, &v.z, mode) == 4) {
                    m_editActive = true;
                    m_editBrush = brushFromName(mode);
                    if (const char* d = getenv("VF_EDIT_DIAM"))
                        m_editDiameter = finiteEnv(d, m_editDiameter);
                    if (const char* d = getenv("VF_EDIT_DEPTH"))
                        m_editDepth = finiteEnv(d, m_editDepth);
                    quantiseBrush(); // snap onto the lattice
                    have = true;
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        spdlog::info(
                            "VF_TEST_BRUSH {} {} {} {} d={} vox depth={:.1f}",
                            v.x, v.y, v.z, brushName(m_editBrush),
                            brushVoxels(), m_editDepth);
                    }
                }
            } else if (testHov && *testHov && !m_hoverHit.hit) {
                if (sscanf(testHov, "%d,%d,%d", &v.x, &v.y, &v.z) == 3) {
                    have = true;
                    spdlog::info("VF_TEST_HOVER {} {} {}", v.x, v.y, v.z);
                }
            }
            if (have && m_layers.loaded()) {
                m_hoverHit = {};
                m_hoverHit.hit = true;
                m_hoverHit.voxel = v;
                if (m_editActive)
                    m_hoverHit.normal = storeNormalAt(v);
            }
        }

        // VF_TEST_ROTATE_LIVE="yaw,pitch,roll"[,layer]: seed a synthetic
        // trackball pose (accumulated angles + selected-owner preview armed,
        // NO manifest commit) so a headless shot can verify GPU rotation.
        // VF_TEST_ROTATE commits + rebuilds; this one is preview-only.
        if (rotateLiveTest) {
            float y = 0.f, p = 0.f, r = 0.f;
            if (sscanf(testRotateLive, "%f,%f,%f", &y, &p, &r) == 3) {
                m_editActive = true;
                m_editBrush = EditBrush::Rotate;
                m_panel = Panel::Edit;
                m_rotateLayer.clear();
                if (const char* ln = getenv("VF_ROTATE_LAYER"))
                    m_rotateLayer = ln;
                if (m_rotateLayer.empty())
                    for (const auto& l : m_layers.layers())
                        if (l.enabled && l.role == "object" &&
                            l.file != vf::voxel::EditableWorld::kFileName) {
                            m_rotateLayer = l.file;
                            break;
                        }
                if (!m_rotateLayer.empty()) {
                    const auto layer = std::find_if(
                        m_worldLayers.begin(), m_worldLayers.end(),
                        [&](const vf::voxel::worldfile::WorldLayer& l) {
                            return l.file == m_rotateLayer;
                        });
                    glm::vec3 pivot;
                    if (layer != m_worldLayers.end() &&
                        m_layers.layerPivot(m_rotateLayer, pivot)) {
                        m_rotating = true;
                        m_rotateDy = y; m_rotateDx = p; m_rotateDz = r;
                        const glm::mat3 R =
                            vf::voxel::worldfile::relativePlacementRotation(
                                layer->rotDeg, layer->rotX, layer->rotZ,
                                layer->rotDeg + y, layer->rotX + p,
                                layer->rotZ + r);
                        m_splatPass.setRotatePreview(
                            pivot, R, true, m_layers.layerId(m_rotateLayer));
                    }
                }
            }
        }

        // VF_TEST_MOVE_LIVE="dx,dy,dz[,layer]" is a preview-only Move hook.
        // It exercises the selected-owner translation lane without writing
        // world.json; the interactive Apply button remains the commit path.
        if (const char* testMoveLive = getenv("VF_TEST_MOVE_LIVE");
            testMoveLive && *testMoveLive) {
            float dx = 0.f, dy = 0.f, dz = 0.f;
            if (sscanf(testMoveLive, "%f,%f,%f", &dx, &dy, &dz) == 3) {
                m_editActive = true;
                m_editBrush = EditBrush::Move;
                m_panel = Panel::Edit;
                m_moveLayer.clear();
                const char* comma = strchr(testMoveLive, ',');
                if (comma) {
                    const char* secondComma = strchr(comma + 1, ',');
                    const char* thirdComma = secondComma
                        ? strchr(secondComma + 1, ',') : nullptr;
                    if (thirdComma) {
                        std::string layerName(thirdComma + 1);
                        const size_t end = layerName.find(',');
                        if (end != std::string::npos)
                            layerName.resize(end);
                        if (!layerName.empty())
                            m_moveLayer = layerName;
                    }
                }
                if (m_moveLayer.empty()) {
                    for (const auto& l : m_layers.layers())
                        if (l.enabled && l.role == "object" &&
                            l.file != vf::voxel::EditableWorld::kFileName) {
                            m_moveLayer = l.file;
                            break;
                        }
                }
                glm::vec3 pivot;
                if (!m_moveLayer.empty() &&
                    m_layers.layerPivot(m_moveLayer, pivot)) {
                    m_moveDelta = glm::vec3(dx, dy, dz);
                    m_moveStaged = true;
                    m_splatPass.setRotatePreview(
                        pivot, glm::mat3(1.f), m_moveDelta, true,
                        m_layers.layerId(m_moveLayer));
                }
            }
        }

        // Brush hover preview -> splat backend: tint the splats whose centre
        // lies inside the volume the next stamp would affect - the carve
        // cylinder, the delete/paint ball, the add growth (whose tint shows the
        // surface patch the raised shell will bury), or the conservative blue
        // footprint used by terrain Smooth.
        {
            glm::vec4 vol(0.f), axis(0.f), tint(0.f);
            // skin: surfels sit at cell centre + 0.05 m along their normal, so
            // grow the volume a little past the cell centres the CPU
            // rasterizer selects (see App::applyEditLive).
            constexpr float kBrushSkin = 0.06f;
            m_gizmoValid = false;
            if (m_editActive && m_editBrush == EditBrush::Move &&
                !m_moveLayer.empty()) {
                const uint8_t layerId = m_layers.layerId(m_moveLayer);
                const vf::voxel::WorldAABB b = m_layers.layerBox(m_moveLayer);
                if (b.valid() && layerId != 0) {
                    const glm::vec3 c = 0.5f * (b.lo + b.hi);
                    const glm::vec3 he = 0.5f * (b.hi - b.lo) + kBrushSkin;
                    vol = glm::vec4(c, 0.f);
                    axis = glm::vec4(he, 0.f);
                    tint = glm::vec4(0.20f, 0.85f, 0.55f, 0.22f);
                    const ImVec2 display = ImGui::GetIO().DisplaySize;
                    const glm::ivec2 fbExt =
                        display.x > 0.f && display.y > 0.f
                            ? glm::ivec2(int(display.x), int(display.y))
                            : glm::ivec2(int(m_swapchain.extent().width),
                                         int(m_swapchain.extent().height));
                    m_gizmoValid = trackballScreenForBox(
                        b, m_camera, tanHalfFov, fbExt,
                        m_gizmoCentre, m_gizmoRadius);
                    static bool moveProbeLogged = false;
                    if (m_gizmoValid && getenv("VF_TEST_MOVE_PROBE") &&
                        !moveProbeLogged) {
                        moveProbeLogged = true;
                        const glm::vec3 c = 0.5f * (b.lo + b.hi);
                        const auto probe = [&](MoveAxis axis) {
                            const MoveAxisLine line = moveAxisScreenLine(
                                c, axis, m_camera, tanHalfFov, fbExt,
                                m_gizmoRadius);
                            return int(moveAxisHandleAt(
                                line.b, c, m_camera, tanHalfFov, fbExt,
                                m_gizmoRadius));
                        };
                        spdlog::info("move probe: hitX={} hitY={} hitZ={}",
                                     probe(MoveAxis::X), probe(MoveAxis::Y),
                                     probe(MoveAxis::Z));
                    }
                }
            } else if (m_editActive && m_editBrush == EditBrush::Rotate &&
                       !m_rotateLayer.empty()) {
                // One click activates the owner; keep its full layer tinted
                // while the bounds-centered trackball remains available.
                const uint8_t layerId = m_layers.layerId(m_rotateLayer);
                const vf::voxel::WorldAABB b = m_layers.layerBox(m_rotateLayer);
                if (b.valid() && layerId != 0) {
                    const glm::vec3 c = 0.5f * (b.lo + b.hi);
                    const glm::vec3 he = 0.5f * (b.hi - b.lo) + kBrushSkin;
                    vol = glm::vec4(c, 0.f);
                    axis = glm::vec4(he, 0.f);
                    tint = glm::vec4(0.20f, 0.75f, 1.00f, 0.35f);
                    // ImGui mouse/draw coordinates are logical display pixels;
                    // use them for the gizmo so high-DPI windows do not split
                    // the ring hit-test from the rendered trackball.
                    const ImVec2 display = ImGui::GetIO().DisplaySize;
                    const glm::ivec2 fbExt =
                        display.x > 0.f && display.y > 0.f
                            ? glm::ivec2(int(display.x), int(display.y))
                            : glm::ivec2(int(m_swapchain.extent().width),
                                         int(m_swapchain.extent().height));
                    m_gizmoValid = trackballScreenForBox(
                        b, m_camera, tanHalfFov, fbExt,
                        m_gizmoCentre, m_gizmoRadius);
                    m_rotateRadius = m_gizmoValid ? m_gizmoRadius : 0.f;
                    static bool trackballProbeLogged = false;
                    if (m_gizmoValid && getenv("VF_TEST_TRACKBALL_PROBE") &&
                        !trackballProbeLogged) {
                        trackballProbeLogged = true;
                        const float r = m_gizmoRadius;
                        const int yaw = int(trackballHandleAt(
                            m_gizmoCentre + glm::vec2(r * 0.8f, r * 0.6f),
                            m_gizmoCentre, r));
                        const int pitch = int(trackballHandleAt(
                            m_gizmoCentre + glm::vec2(0.f, r * 0.42f),
                            m_gizmoCentre, r));
                        const int roll = int(trackballHandleAt(
                            m_gizmoCentre + glm::vec2(r * 0.42f, 0.f),
                            m_gizmoCentre, r));
                        const int centre = int(trackballHandleAt(
                            m_gizmoCentre, m_gizmoCentre, r));
                        spdlog::info(
                            "trackball probe: centre=({:.3f},{:.3f}) radius={:.3f} "
                            "hitYaw={} hitPitch={} hitRoll={} hitCentre={} "
                            "bounds=({:.3f},{:.3f},{:.3f})-({:.3f},{:.3f},{:.3f}) "
                            "camPos=({:.3f},{:.3f},{:.3f}) camYaw={:.5f} camPitch={:.5f}",
                            m_gizmoCentre.x, m_gizmoCentre.y, r,
                            yaw, pitch, roll, centre,
                            b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y, b.hi.z,
                            m_camera.pos.x, m_camera.pos.y, m_camera.pos.z,
                            m_camera.yaw, m_camera.pitch);
                    }
                    if (getenv("VF_TRACE") && m_frameIdx == 1)
                        spdlog::info("trackball: layer={} box={} id={} valid={} centre=({:.1f},{:.1f}) radius={:.1f} bounds=({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f}) campos=({:.2f},{:.2f},{:.2f}) yaw={:.3f} pitch={:.3f}",
                                     m_rotateLayer, b.valid(), unsigned(layerId),
                                     m_gizmoValid, m_gizmoCentre.x, m_gizmoCentre.y,
                                     m_gizmoRadius, b.lo.x, b.lo.y, b.lo.z,
                                     b.hi.x, b.hi.y, b.hi.z,
                                     m_camera.pos.x, m_camera.pos.y, m_camera.pos.z,
                                     m_camera.yaw, m_camera.pitch);
                }
            } else if (m_editActive && m_hoverHit.hit) {
                {
                    const glm::vec3 c = vf::voxel::voxelCenter(m_hoverHit.voxel);
                    glm::vec3 n = m_hoverHit.normal;
                    if (glm::length(n) < 1e-3f)
                        n = glm::vec3(0.f, 1.f, 0.f);
                    n = glm::normalize(n);
                    const float r = m_editDiameter * 0.5f + kBrushSkin;
                    if (brushIsPerVoxel() && (m_editBrush == EditBrush::Add ||
                                              m_editBrush == EditBrush::Carve)) {
                        // per-voxel: the tint is the single cell the stamp would
                        // touch (a 1-voxel box, not the depth-extent volume), so
                        // the highlight IS the target. The hover outline in the
                        // post pass marks the picked voxel itself.
                        const glm::ivec3 t = perVoxelTarget(n);
                        vol = glm::vec4(vf::voxel::voxelCenter(t), 0.f);
                        // box form: bAxis.xyz is the half extent on ALL THREE
                        // axes (bAxis.w == 0), so it must be set per axis
                        const float he = 0.5f * vf::voxel::VOXEL + kBrushSkin * 0.5f;
                        axis = glm::vec4(he, he, he, 0.f);
                        // A 1-voxel volume cannot over-tint the scene, so the
                        // strength goes up: at 0.55 a single cell measured only
                        // dG=+3, which reads as nothing next to the outline.
                        tint = (m_editBrush == EditBrush::Add)
                                   ? glm::vec4(0.30f, 0.95f, 0.40f, 0.90f)
                                   : glm::vec4(1.00f, 0.45f, 0.10f, 0.90f);
                    } else if (m_editBrush == EditBrush::Carve) {
                        // the exact volume makeOrientedCylinder emits: a cylinder
                        // reaching kCarveTopMargin above the hit cell, `depth`
                        // long along -normal (the top margin is what opens the
                        // surface, so the preview must include it)
                        const glm::vec3 a = -n;
                        const float margin =
                            vf::voxel::EditableWorld::kCarveTopMargin;
                        const float half = (m_editDepth + margin) * 0.5f + kBrushSkin;
                        vol = glm::vec4(c + a * ((m_editDepth - margin) * 0.5f), r);
                        axis = glm::vec4(a, half);
                        tint = glm::vec4(1.00f, 0.45f, 0.10f, 0.45f); // warm = cut
                    } else if (m_editBrush == EditBrush::Delete) {
                        vol = glm::vec4(c, r);
                        axis = glm::vec4(0.f, 1.f, 0.f, 0.f); // ball
                        tint = glm::vec4(1.00f, 0.12f, 0.10f, 0.55f); // red = remove
                    } else if (m_editBrush == EditBrush::Add) {
                        // makeDome emits an extruded disk plus a rounded lip;
                        // encode that growth volume as a negative axis half
                        // length for the shader dome case, with the brush skin
                        // folded into the reach.
                        axis = glm::vec4(n, -(m_editDepth + kBrushSkin));
                        vol = glm::vec4(c, r);
                        tint = glm::vec4(0.30f, 0.95f, 0.40f, 0.50f); // green = raise
                    } else if (m_editBrush == EditBrush::Smooth) {
                        // Smooth relaxes terrain columns or an object surface
                        // rather than a fixed 3D volume. A conservative ball
                        // gives the user a clear blue footprint without
                        // claiming that the relaxation touches every cell at
                        // exactly the same height.
                        vol = glm::vec4(c, r);
                        axis = glm::vec4(0.f, 1.f, 0.f, 0.f);
                        tint = glm::vec4(0.20f, 0.68f, 1.00f, 0.45f);
                    } else { // Paint: preview the chosen material colour
                        vol = glm::vec4(c, r);
                        axis = glm::vec4(0.f, 1.f, 0.f, 0.f); // ball
                        const glm::vec3 pc = vf::voxel::kPalette[std::min<int>(m_editMat, 16)];
                        tint = glm::vec4(pc, 0.55f);
                    }
                }
            }
            m_splatPass.setBrush(
                vol, axis, tint,
                (m_editBrush == EditBrush::Rotate && !m_rotateLayer.empty())
                    ? m_layers.layerId(m_rotateLayer)
                    : ((m_editBrush == EditBrush::Move && !m_moveLayer.empty())
                           ? m_layers.layerId(m_moveLayer)
                           : uint8_t(0)));
        }

        // highlight feeds: selected (strong) + hover (faint) -> shader UBO
        {
            glm::vec4 selFeed(0.f), hovFeed(0.f);
            if (m_hasSelection)
                selFeed = glm::vec4(vf::voxel::voxelCenter(m_selectedHit.voxel), 1.f);
            if (m_hoverHit.hit)
                hovFeed = glm::vec4(vf::voxel::voxelCenter(m_hoverHit.voxel), 1.f);
            m_postPass.setSelection(selFeed);
            m_postPass.setHover(hovFeed);
        }

        // ---- render-option hotkeys (edge-triggered, interactive only:
        // headless runs must stay deterministic - key state on a hidden
        // window can phantom-trigger toggles) ----
        if (!chatCaptures && !headlessRun) {
            auto edge = [](int k, GLFWwindow* w) {
                static std::vector<uint8_t> prev(1024, 0);
                bool now = glfwGetKey(w, k) == GLFW_PRESS;
                bool e = now && !prev[k];
                prev[k] = now ? 1 : 0;
                return e;
            };
            GLFWwindow* hw = m_window.handle();
            bool shift = glfwGetKey(hw, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                         glfwGetKey(hw, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            bool ctrl = glfwGetKey(hw, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                        glfwGetKey(hw, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            // Ctrl+Z: undo the last stroke (works with the edit tool on/off,
            // but only when the chat input does not capture the keyboard)
            if (ctrl && edge(GLFW_KEY_Z, hw))
                undoEdit();
            if (edge(GLFW_KEY_T, hw)) {
                m_tonemapLook = (m_tonemapLook + 1) % 3;
                spdlog::info("tonemap look -> {}", m_tonemapLook);
            }
            if (edge(GLFW_KEY_F, hw)) {
                m_renderMode = (m_renderMode == RenderMode::Splats) ? RenderMode::Svo
                                                                    : RenderMode::Splats;
                spdlog::info("render mode -> {}",
                             m_renderMode == RenderMode::Splats ? "splats" : "svo");
            }
            if (edge(GLFW_KEY_N, hw)) {
                m_taaEnabled = !m_taaEnabled;
                m_taaFirstFrame = true; // never blend stale history on re-enable
                spdlog::info("TAA -> {}", m_taaEnabled ? "on" : "off");
            }
            // splat disk size ([ shrink / ] grow), live for the splat backend
            if (edge(GLFW_KEY_LEFT_BRACKET, hw)) {
                m_splatPass.setRadiusScale(m_splatPass.radiusScale() / 1.12f);
                spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
            }
            if (edge(GLFW_KEY_RIGHT_BRACKET, hw)) {
                m_splatPass.setRadiusScale(m_splatPass.radiusScale() * 1.12f);
                spdlog::info("splat radius -> {:.2f}", m_splatPass.radiusScale());
            }
            // toggle the carve / add edit tool
            if (edge(GLFW_KEY_C, hw)) {
                m_editActive = !m_editActive;
                // Arming also reveals the brush controls, so the key that
                // starts an edit is never a two-step hunt through the rail.
                if (m_editActive)
                    m_panel = Panel::Edit;
                spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");
            }
            // Sidebar chrome. Both are suppressed while a text field owns the
            // keyboard (the chat input is a multiline InputText, and Tab is a
            // normal character there).
            if (!ImGui::GetIO().WantTextInput) {
                if (edge(GLFW_KEY_TAB, hw)) {
                    m_sidebarCollapsed = !m_sidebarCollapsed;
                    spdlog::info("sidebar -> {}",
                                 m_sidebarCollapsed ? "collapsed" : "expanded");
                }
                if (ctrl) {
                    for (int i = 0; i < kPanelCount; ++i) {
                        if (!edge(GLFW_KEY_1 + i, hw))
                            continue;
                        if (Panel(i) == Panel::Mesh)
                            prepareMeshImport();
                        m_panel = Panel(i);
                        spdlog::info("sidebar section -> {}",
                                     kPanels[i].label);
                    }
                }
            }
            if (m_editActive) {
                // + / - change the width by one voxel; Shift + / - change the
                // depth (or Smooth strength when the relaxation brush is
                // selected). The floor is one voxel = per-voxel mode.
                if (edge(GLFW_KEY_EQUAL, hw) || edge(GLFW_KEY_KP_ADD, hw)) {
                    if (shift) {
                        if (m_editBrush == EditBrush::Smooth)
                            m_smoothStrength = std::min(m_smoothStrength + 0.05f, 1.0f);
                        else
                            setBrushDepthVoxels(brushDepthVoxels() + 2);
                    } else {
                        setBrushVoxels(brushVoxels() + 1);
                    }
                }
                if (edge(GLFW_KEY_MINUS, hw) || edge(GLFW_KEY_KP_SUBTRACT, hw)) {
                    if (shift) {
                        if (m_editBrush == EditBrush::Smooth)
                            m_smoothStrength = std::max(m_smoothStrength - 0.05f, 0.0f);
                        else
                            setBrushDepthVoxels(brushDepthVoxels() - 2);
                    } else {
                        setBrushVoxels(brushVoxels() - 1);
                    }
                }
            } else {
                if (edge(GLFW_KEY_EQUAL, hw) || edge(GLFW_KEY_KP_ADD, hw)) {
                    m_exposure = m_exposure * 1.1f < 4.0f ? m_exposure * 1.1f : 4.0f;
                    spdlog::info("exposure -> {:.2f}", m_exposure);
                }
                if (edge(GLFW_KEY_MINUS, hw) || edge(GLFW_KEY_KP_SUBTRACT, hw)) {
                    m_exposure = m_exposure / 1.1f > 0.1f ? m_exposure / 1.1f : 0.1f;
                    spdlog::info("exposure -> {:.2f}", m_exposure);
                }
            }
            // Bare 1..5 toggle render flags. edge() is a per-key-code latch,
            // not a consume: the Ctrl+1..6 sidebar switch above polls these
            // same codes first, but only while Ctrl is held, so it takes the
            // edge on exactly the frames this loop must not act on.
            for (int i = 0; i < 5; ++i) {
                if (edge(GLFW_KEY_1 + i, hw)) {
                    m_renderFlags ^= (1 << i);
                    spdlog::info("render flag {} -> {}", i, (m_renderFlags >> i) & 1);
                }
            }
            if (edge(GLFW_KEY_0, hw)) {
                m_renderFlags = 31;
                spdlog::info("render flags reset -> 31");
            }
            // photorealism feature toggles
            if (edge(GLFW_KEY_G, hw)) {
                m_renderFlags ^= (1 << 5); // SSR
                spdlog::info("SSR -> {}", (m_renderFlags >> 5) & 1);
            }
            if (edge(GLFW_KEY_H, hw)) {
                m_renderFlags ^= (1 << 6); // SSAO
                spdlog::info("SSAO -> {}", (m_renderFlags >> 6) & 1);
            }
            if (edge(GLFW_KEY_B, hw)) {
                m_renderFlags ^= (1 << 7); // texture detail normals
                spdlog::info("detail normals -> {}", (m_renderFlags >> 7) & 1);
            }
            if (edge(GLFW_KEY_J, hw)) {
                m_volFogEnabled = !m_volFogEnabled;
                spdlog::info("volumetric fog -> {}", m_volFogEnabled);
            }
            if (edge(GLFW_KEY_K, hw)) {
                m_motionBlurEnabled = !m_motionBlurEnabled;
                spdlog::info("motion blur -> {}", m_motionBlurEnabled);
            }
            if (edge(GLFW_KEY_L, hw)) {
                m_dofEnabled = !m_dofEnabled;
                spdlog::info("depth of field -> {}", m_dofEnabled);
            }
            if (edge(GLFW_KEY_M, hw)) {
                // Micro detail is baked into the surfel stream, so flipping it
                // has to re-run the surfelizer - the same path a layer toggle
                // uses (stalls the device, so it is a deliberate action).
                m_microDetail = !m_microDetail;
                requestWorldReload();
                spdlog::info("micro detail -> {} (rebuilding surfels)", m_microDetail);
            }
        }

        uint32_t f = m_frameIdx % kMaxFramesInFlight;
        FrameSync& fr = m_frames[f];
        vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);
        // that slot's submission (3 frames ago) is complete: harvest its marks
        accumulateProf(f, m_frameIdx);

        if (headlessRun) {
            // Automated mode: zero window-system interaction.
            vkResetFences(m_ctx.device(), 1, &fr.inFlight);
            vkResetCommandBuffer(fr.cmd, 0);
            VkCommandBufferBeginInfo hbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(fr.cmd, &hbi);
            const uint32_t profBase = uint32_t(m_frameIdx % kMaxFramesInFlight) * kProfMarks;
            if (m_profPool)
                vkCmdResetQueryPool(fr.cmd, m_profPool, profBase, kProfMarks);
            auto profMark = [&](uint32_t mark) {
                if (m_profPool)
                    vkCmdWriteTimestamp2(fr.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         m_profPool, profBase + mark);
            };
            profMark(0);

            vf::RaymarchPush push {};
            push.camPos = glm::vec4(m_camera.pos, 0);
            push.camRight = glm::vec4(m_camera.right(), 0);
            push.camUp = glm::vec4(m_camera.up(), 0);
            push.camFwd = glm::vec4(m_camera.forward(), 0);
            push.a = glm::vec4(tanHalfFov,
                               float(m_swapchain.extent().width) / float(m_swapchain.extent().height),
                               float(m_offscreen.extent.width),
                               float(m_offscreen.extent.height));
            push.b = glm::vec4(m_pushB.x, m_pushB.y, m_pushB.z, float(m_frameIdx % 1024));
            push.sunDir = m_sunDir;
            push.misc = glm::vec4(float(m_renderFlags), m_animTime, float(m_tonemapLook), m_exposure);
            // buried camera (inside solid): render shells two-sided this frame
            updateBuriedProbe();
            {
                if (m_renderMode == RenderMode::Splats) {
                    // splat raster writes linear HDR + G-buffer
                    vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    vf::transitionImage(fr.cmd, m_splatPass.depthImage().img,
                                        VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                           VK_IMAGE_ASPECT_STENCIL_BIT),
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
                    m_splatPass.record(fr.cmd, push,
                                       { m_offscreen.extent.width,
                                         m_offscreen.extent.height });
                    // barrier: HDR/G-buffer written -> read by post pass
                    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                    VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    di.memoryBarrierCount = 1;
                    di.pMemoryBarriers = &mb;
                    vkCmdPipelineBarrier2(fr.cmd, &di);
                } else {
                // ray-march writes linear HDR + G-buffer
                vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                m_svoPass.record(fr.cmd, push);
                // barrier: HDR/G-buffer written -> read by post pass
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(fr.cmd, &di);
                }
                profMark(1);
                // post pass reads HDR/G-buffer, writes LDR offscreen
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                m_postPass.record(fr.cmd, push);
                profMark(2);

                // ---- photorealism passes (G/H/J/K/L toggles, shared helper) ----
                recordPhotorealism(fr.cmd, push);
                profMark(3);
                profMark(4); // no TAA in headless: fx == taa mark, tail = 0
            }
            profMark(5);
            vkEndCommandBuffer(fr.cmd);
            VkSubmitInfo hsi { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            hsi.commandBufferCount = 1;
            hsi.pCommandBuffers = &fr.cmd;
            vkQueueSubmit(m_ctx.graphicsQueue(), 1, &hsi, fr.inFlight);
            ++m_frameIdx;
            if (getenv("VF_TRACE"))
                fprintf(stderr, "[f%llu] headless submitted\n", (unsigned long long)m_frameIdx);
            if ((args.selftest) && m_frameIdx == 30)
                return runSelftest() ? 0 : 1;
            if (shotMode && m_frameIdx == 3) {
                vkDeviceWaitIdle(m_ctx.device());
                std::vector<uint8_t> px;
                vf::readbackImage2D(m_ctx, m_offscreen.img, m_offscreen.extent.width,
                                    m_offscreen.extent.height, px);
                const std::string& out = shots[m_shotIdx].path;
                FILE* fp = fopen(out.c_str(), "wb");
                if (fp) {
                    fprintf(fp, "P6\n%u %u\n255\n", m_offscreen.extent.width,
                            m_offscreen.extent.height);
                    for (size_t i = 0; i < px.size(); i += 4)
                        fwrite(&px[i], 3, 1, fp);
                    fclose(fp);
                    spdlog::info("shot written: {}", out);
                }
                if (++m_shotIdx < shots.size()) {
                    // next camera: reset the warmup frame counter so the
                    // capture is identical to a single-shot run of the
                    // same view (3 frames rendered before the readback).
                    const ShotSpec& sh = shots[m_shotIdx];
                    m_camera.pos = { sh.camx, sh.camy, sh.camz };
                    const glm::vec3 d = glm::normalize(
                        glm::vec3(sh.tx, sh.ty, sh.tz) - m_camera.pos);
                    m_camera.yaw = atan2(d.z, d.x);
                    m_camera.pitch = asin(d.y);
                    m_frameIdx = 0;
                    continue;
                }
                return 0;
            }
            if (args.smokeFrames > 0 && m_frameIdx >= uint64_t(args.smokeFrames)) {
                spdlog::info("smoke done: {} frames, avg {:.2f} ms, min {:.2f}, max {:.2f}",
                             m_frameIdx, m_avgMs, m_minMs, m_maxMs);
                break;
            }
            continue;
        }
        if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] pre-acquire\n", (unsigned long long)m_frameIdx);
        uint32_t imgIdx = 0;
        // Dedicated acquire semaphore per swapchain image: avoids the
        // NVIDIA/X11 present deadlock seen with per-frame-slot reuse.
        VkSemaphore acquireSem = !m_acquireSems.empty()
                                     ? m_acquireSems[m_nextAcquire % m_acquireSems.size()]
                                     : fr.imageAvailable;
        VkResult acq = vkAcquireNextImageKHR(m_ctx.device(), m_swapchain.handle(),
                                             UINT64_MAX, acquireSem, VK_NULL_HANDLE,
                                             &imgIdx);
        m_nextAcquire = imgIdx; // its semaphore is reused when this image comes back
        if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] acquired %u\n", (unsigned long long)m_frameIdx, imgIdx);
        if (firstSight(acq, "acquire")) {
            // a persistent failure here skips the rest of the frame body every
            // iteration, so the app keeps running and keeps consuming input
            // while nothing is ever drawn
            const bool fatal = acq == VK_ERROR_DEVICE_LOST ||
                               acq == VK_ERROR_OUT_OF_HOST_MEMORY;
            spdlog::log(fatal ? spdlog::level::err : spdlog::level::warn,
                        "acquire returned {} at frame {} - the frame body will "
                        "be skipped each frame (the loop keeps spinning)",
                        vkResultName(acq), m_frameIdx);
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            handleResize();
            continue;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
            continue;

        vkResetFences(m_ctx.device(), 1, &fr.inFlight);
        vkResetCommandBuffer(fr.cmd, 0);

        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fr.cmd, &bi);
        const uint32_t profBase = uint32_t(m_frameIdx % kMaxFramesInFlight) * kProfMarks;
        if (m_profPool)
            vkCmdResetQueryPool(fr.cmd, m_profPool, profBase, kProfMarks);
        auto profMark = [&](uint32_t mark) {
            if (m_profPool)
                vkCmdWriteTimestamp2(fr.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     m_profPool, profBase + mark);
        };
        profMark(0);

        vf::RaymarchPush push {};
        push.camPos = glm::vec4(m_camera.pos, 0);
        push.camRight = glm::vec4(m_camera.right(), 0);
        push.camUp = glm::vec4(m_camera.up(), 0);
        push.camFwd = glm::vec4(m_camera.forward(), 0);
        push.a = glm::vec4(tanHalfFov,
                           float(m_swapchain.extent().width) / float(m_swapchain.extent().height),
                           float(m_offscreen.extent.width), float(m_offscreen.extent.height));
        push.b = glm::vec4(m_pushB.x, m_pushB.y, m_pushB.z, float(m_frameIdx % 1024));
        push.sunDir = m_sunDir;
        push.misc = glm::vec4(float(m_renderFlags), m_animTime, float(m_tonemapLook), m_exposure);
        // buried camera (inside solid): render shells two-sided this frame
        updateBuriedProbe();

        // splat raster or ray-march -> HDR + G-buffer --------------------
        if (m_renderMode == RenderMode::Splats) {
            vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_splatPass.depthImage().img,
                                VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                   VK_IMAGE_ASPECT_STENCIL_BIT),
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            m_splatPass.record(fr.cmd, push,
                               { m_offscreen.extent.width, m_offscreen.extent.height });
        } else {
        vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_svoPass.record(fr.cmd, push);
        }
        profMark(1);
        // barrier: HDR/G-buffer written -> read by post pass
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        if (m_renderMode == RenderMode::Splats) {
            // forward raster writes attachments; the tile path (VF_TILE)
            // writes HDR/G-buffer from compute instead - cover both
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        } else {
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        }
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(fr.cmd, &di);
        // post pass reads HDR/G-buffer, writes LDR offscreen ---------------
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_postPass.record(fr.cmd, push);
        profMark(2);

        // photorealism toggles (G/H/J/K/L) run here so they affect what you
        // see; TAA resolves the effected image afterwards
        recordPhotorealism(fr.cmd, push);
        profMark(3);

        // TAA resolve (interactive only, not for headless tests) ----------
        VkImage taaSrc = m_offscreen.img;
        VkImageLayout taaSrcLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkPipelineStageFlags2 taaSrcStage =
            VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        VkAccessFlags2 taaSrcAccess =
            VkAccessFlags2(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (m_taaEnabled && !headlessRun) {
            // current -> SHADER_READ for TAA
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                taaSrcLayout, VK_IMAGE_LAYOUT_GENERAL,
                                taaSrcStage, taaSrcAccess,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            // Ping-pong: read history from idx, write resolved into the OTHER
            // buffer, then make that the read buffer next frame (exact 1-frame
            // history, no stale/garbage read).
            const uint32_t hidx = uint32_t(m_taaHistoryIdx);
            const uint32_t widx = hidx ^ 1u;
            VkImageView histView = m_taaHistory[hidx].view;
            // history already in GENERAL from previous frame's copy, make it readable
            // (first frame history is undefined but TAA handles firstFrame)
            m_taaPass.updateDescriptors(m_offscreen.view, histView, m_taaResolved.view, m_gpos.view);
            // history -> SHADER_READ (if not first frame, already GENERAL)
            // resolved -> GENERAL for write
            vf::transitionImage(fr.cmd, m_taaResolved.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            m_taaPass.record(fr.cmd, m_offscreen.extent.width, m_offscreen.extent.height,
                             m_taaFirstFrame ? 0.0f : m_taaBlend, m_taaFirstFrame, m_prevCam);
            // TAA output -> TRANSFER_SRC for blit, and copy to history for next frame
            vf::transitionImage(fr.cmd, m_taaResolved.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            // copy resolved -> history (for next frame)
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.extent = {m_offscreen.extent.width, m_offscreen.extent.height, 1};
            // history need to be DST
            vf::transitionImage(fr.cmd, m_taaHistory[widx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(fr.cmd, m_taaResolved.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_taaHistory[widx].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            vf::transitionImage(fr.cmd, m_taaHistory[widx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            // also keep resolved as TRANSFER_SRC for blit (already)
            taaSrc = m_taaResolved.img;
            taaSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            taaSrcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            taaSrcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
            m_taaHistoryIdx = int(widx);
            m_taaFirstFrame = false;
            // remember this frame's camera so next frame can reproject history
            m_prevCam.pos = m_camera.pos;
            m_prevCam.right = m_camera.right();
            m_prevCam.up = m_camera.up();
            m_prevCam.fwd = m_camera.forward();
            m_prevCam.tanHalfFov = tanHalfFov;
            m_prevCam.aspect = float(m_swapchain.extent().width) / float(m_swapchain.extent().height);
        } else {
            // no TAA: offscreen -> TRANSFER_SRC directly
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                taaSrcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                taaSrcStage, taaSrcAccess,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            taaSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            taaSrcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            taaSrcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        profMark(4);
        // swapchain -> transfer-dst, blit ----------
        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkOffset3D b0 { 0, 0, 0 };
        VkOffset3D b1 { int(m_offscreen.extent.width), int(m_offscreen.extent.height), 1 };
        VkOffset3D s1 { int(m_swapchain.extent().width), int(m_swapchain.extent().height), 1 };
        VkImageBlit blit {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0] = b0;
        blit.srcOffsets[1] = b1;
        blit.dstOffsets[0] = b0;
        blit.dstOffsets[1] = s1;
        vkCmdBlitImage(fr.cmd, taaSrc, taaSrcLayout,
                       m_swapchain.image(imgIdx), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawHud();
        ImGui::Render();
        if (getenv("VF_IMGUI_DEBUG") && m_frameIdx == 5) {
            ImDrawData* dd = ImGui::GetDrawData();
            spdlog::warn("imgui dbg: valid={} display=({:.0f},{:.0f}) idxcount={} cmdlists={}",
                         dd ? 1 : 0, dd ? dd->DisplaySize.x : -1.f,
                         dd ? dd->DisplaySize.y : -1.f,
                         dd ? dd->TotalIdxCount : -1, dd ? dd->CmdListsCount : -1);
        }
        ImDrawData* dd = ImGui::GetDrawData();
        if (dd && dd->TotalIdxCount > 0) {
            // swapchain -> color attachment for ImGui (LOAD keeps the blitted world)
            vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            if (m_scenePreview) {
                // offscreen is TRANSFER_SRC after the blit; make it shader-readable for ImGui
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            }
            // With UseDynamicRendering we must open the render pass ourselves and
            // target the swapchain view; LOAD keeps the blitted world underneath.
            VkRenderingAttachmentInfo att { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            att.imageView = m_swapchain.imageViews()[imgIdx];
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
            VkRect2D area { { 0, 0 }, m_swapchain.extent() };
            ri.renderArea = area;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(fr.cmd, &ri);
            ImGui_ImplVulkan_RenderDrawData(dd, fr.cmd);
            vkCmdEndRendering(fr.cmd);
        }

        // VF_HUD_SHOT: blit the composed frame (HUD included) back to offscreen
        if (hudShotFrame && m_frameIdx + 1 >= hudShotFrame) {
            VkImageCopy region {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { m_swapchain.extent().width, m_swapchain.extent().height, 1 };
            vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx),
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_MEMORY_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT);
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                m_scenePreview ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(fr.cmd, m_swapchain.image(imgIdx),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_offscreen.img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx),
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        }

        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        profMark(5);
        vkEndCommandBuffer(fr.cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &fr.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &fr.renderDone;
        if (vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, fr.inFlight) != VK_SUCCESS) {
            spdlog::critical("vkQueueSubmit failed");
            return 1;
        }

        if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] submitted\n", (unsigned long long)m_frameIdx);
        VkPresentInfoKHR pi { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &fr.renderDone;
        pi.swapchainCount = 1;
        pi.pSwapchains = m_swapchain.handlePtr();
        pi.pImageIndices = &imgIdx;
        VkResult pres = vkQueuePresentKHR(m_ctx.graphicsQueue(), &pi);
        if (firstSight(pres, "present")) {
            const bool fatal = pres == VK_ERROR_DEVICE_LOST ||
                               pres == VK_ERROR_OUT_OF_HOST_MEMORY;
            spdlog::log(fatal ? spdlog::level::err : spdlog::level::warn,
                        "present returned {} at frame {}", vkResultName(pres),
                        m_frameIdx);
        }
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
            handleResize();

        ++m_frameIdx;
        if (hudShotFrame && m_frameIdx >= hudShotFrame) {
            vkQueueWaitIdle(m_ctx.graphicsQueue());
            std::vector<uint8_t> px;
            vf::readbackImage2D(m_ctx, m_offscreen.img, m_swapchain.extent().width,
                                m_swapchain.extent().height, px);
            FILE* fp = fopen(hudShotPath.c_str(), "wb");
            if (fp) {
                fprintf(fp, "P6\n%u %u\n255\n", m_swapchain.extent().width,
                        m_swapchain.extent().height);
                for (size_t i = 0; i < px.size(); i += 4)
                    fwrite(&px[i], 3, 1, fp);
                fclose(fp);
                spdlog::info("hud shot written: {}", hudShotPath);
            }
            return 0;
        }
        if (args.smokeFrames > 0 && m_frameIdx % 200 == 0)
            spdlog::info("smoke progress: {} frames, avg {:.2f} ms", m_frameIdx, m_avgMs);

        if (args.selftest && m_frameIdx == 30)
            return runSelftest() ? 0 : 1;
        if (args.smokeFrames > 0 && m_frameIdx >= uint64_t(args.smokeFrames)) {
            spdlog::info("smoke: {} frames, avg {:.2f} ms, min {:.2f}, max {:.2f}",
                         m_frameIdx, m_avgMs, m_minMs, m_maxMs);
            break;
        }
    }

    vkDeviceWaitIdle(m_ctx.device());
    if (m_uiSampler) {
        vkDestroySampler(m_ctx.device(), m_uiSampler, nullptr);
        m_uiSampler = VK_NULL_HANDLE;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    destroy();
    return 0;
}

void App::destroy()
{
    m_chatUi.shutdown();
    if (!m_ctx.device()) {
        // init failed before device creation: only tear down what exists
        m_swapchain.destroy();
        m_ctx.shutdown();
        m_window.shutdown();
        return;
    }
    vkDeviceWaitIdle(m_ctx.device());
    for (auto& f : m_frames) {
        vkDestroySemaphore(m_ctx.device(), f.imageAvailable, nullptr);
        vkDestroySemaphore(m_ctx.device(), f.renderDone, nullptr);
        vkDestroyFence(m_ctx.device(), f.inFlight, nullptr);
    }
    for (VkSemaphore s : m_acquireSems)
        vkDestroySemaphore(m_ctx.device(), s, nullptr);
    m_frames.clear();
    if (m_framePool)
        vkDestroyCommandPool(m_ctx.device(), m_framePool, nullptr);

    m_svoPass.destroy();
    m_splatPass.destroy();
    m_taaPass.destroy();
    m_postPass.destroy();
    if (m_profPool)
        vkDestroyQueryPool(m_ctx.device(), m_profPool, nullptr);
    vf::destroyImage3D(m_ctx, m_objVolImg);
    vf::destroyImage3D(m_ctx, m_heightImg);
    vf::destroyImage3D(m_ctx, m_offscreen);
    vf::destroyImage3D(m_ctx, m_hdr);
    vf::destroyImage3D(m_ctx, m_gpos);
    vf::destroyImage3D(m_ctx, m_gnorm);
    vf::destroyImage3D(m_ctx, m_ssaoAo);
    vf::destroyImage3D(m_ctx, m_taaHistory[0]);
    vf::destroyImage3D(m_ctx, m_taaHistory[1]);
    vf::destroyImage3D(m_ctx, m_taaResolved);
    m_texAtlas.destroy(); // atlas owns a sampler + a 2D-array image + a UBO
    m_swapchain.destroy();
    m_ctx.shutdown();
    m_window.shutdown();
}

} // namespace

int main(int argc, char** argv)
{
    vf::initLogging();
    Args args = parseArgs(argc, argv);
    App app;
    return app.run(args);
}
