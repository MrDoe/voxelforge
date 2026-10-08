#pragma once

// Voxelforge - the application object: window, renderers, editor, frame loop.
//
// App owns every long-lived resource of the app (window + swapchain, both render
// backends, the layered world and the editor state). It is declared here and
// DEFINED across the per-functionality .cpp files under src/app/, so a reader
// looking for a subsystem finds one file per subsystem:
//
//   cli/       the command line and the --shotlist view list
//   rhi/       surface/swapchain bring-up, and the present-result probe
//   world/     the layered world: the layer list, the terrain/objvol uploads,
//              the reload path, the runtime-edit overlay, the surfel stream
//   textures/  the material atlas binding table (world.json "textures")
//   edit/      the live-edit brush (store stamps, undo, clear) and the object
//              rotate/move pose commit
//   mesh/      STL/OBJ import (the GUI section and its headless hook)
//   ui/        the one docked sidebar: chrome, the six sections, the scene
//              overlays, and the gizmo screen maths
//   frame/     startup, the frame loop and its slices, the headless capture
//              path, the photorealism chain, the selftest, the profiler
//
// frame/run.cpp is the orchestrator; every other file is one slice it calls. The
// member groups below are labelled with the file that defines them, so a state
// you need to change always names its home.

#include "core/camera.hpp"
#include "core/log.hpp"
#include "platform/window.hpp"
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "rhi/swapchain.hpp"
#include "render/dof_pass.hpp"
#include "render/environment_pass.hpp"
#include "render/motion_blur_pass.hpp"
#include "render/post_pass.hpp"
#include "render/splat_pass.hpp"
#include "render/ssr_pass.hpp"
#include "render/ssao_pass.hpp"
#include "render/svo_pass.hpp"
#include "render/taa_pass.hpp"
#include "render/texture_atlas.hpp"
#include "render/volumetric_fog_pass.hpp"
#include "voxel/chunk_index.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/live_editor.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/picking.hpp"
#include "voxel/surfelize.hpp"
#include "voxel/worldfile.hpp"
#include "ai/ollama_client.hpp"
#include "app/chat_ui.hpp"
#include "app/cli/args.hpp"
#include "app/frame/frame.hpp"
#include "app/ui/ui_types.hpp"

#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace vf {
namespace app {

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
    // Bottom-of-screen reminder of the currently available hotkeys. Shows the
    // edit keys while the brush is armed and the flight keys while it is not,
    // because A/D/S mean different things in the two states.
    void drawHotkeyBar();
    bool runSelftest();
    void syncWorldLayerList();
    bool uploadTerrainTexture();
    bool uploadObjVolTexture();
    // world.json "lights" -> both backends' LightUBO (binding 25).
    void uploadLightSources();
    // Emission table shared by the bake upload and the live trigger:
    // palette emitters + emissive-flagged atlas textures (read from the
    // ATLAS, so VF_TEXTURES=0 kills derived lights with the glow).
    static std::vector<glm::vec3> buildEmissionTable(
        const std::vector<vf::voxel::worldfile::TextureBinding>& bindings,
        const vf::TexAtlas& atlas);
    // Stroke-end derived-light refresh (binding 25 in RAM, no reload): redo
    // the derived set for the stroke's changed chunks from the live store and
    // re-upload. Flicker-free by construction: untouched chunks keep their
    // cached clusters verbatim.
    void refreshLiveLights();
    // Follow-attachment (moving light sources): resolve authored lights with
    // `follow` set to world positions at the layers' CURRENT manifest
    // placement (placedPivot + placementR * storedOffset). Both the bake
    // upload and the live trigger resolve through here, so a carried lamp
    // agrees everywhere. Unknown follow targets resolve to a zero offset
    // (warned only when `warn` - the trigger passes false to avoid
    // per-stroke spam).
    std::vector<vf::voxel::worldfile::LightSource>
    resolveFollowLights(const std::vector<vf::voxel::worldfile::LightSource>& authored,
                        bool warn) const;
    // Move/rotate preview light overlay (splat backend ONLY): rigid-shift the
    // followed lights + derived clusters of the previewed layer by the staged
    // delta and re-upload binding 25 to the splat pass. SVO is deliberately
    // untouched - it ignores the geometry preview too, so its unmoved
    // geometry stays consistent with unmoved lights. No-op when no preview
    // is staged; restores the base set to splat when a preview just ended.
    void refreshPreviewLights();
    // Idempotent bake-seed for the splice base (field enumeration truncated
    // to the authored room); shared by the trigger and the preview overlay.
    void ensureDerivedBase();
    // Coarse irradiance volume (binding 26) -> both backends. Takes the SAME
    // LightUBO uploadLightSources() just built and is called at its tail, so
    // the volume can never be baked from a stale copy of the emitter set -
    // the direct term and the indirect term are derived from one upload.
    // Also the single place that creates the image (once, at init) and writes
    // the descriptor (once); reloads only re-upload pixels.
    void uploadIrradianceVolume(const vf::voxel::worldfile::LightUBO& lights);
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
    // Sun: m_sunDir is the single source of truth for where the sun is (it
    // rides the per-frame push constant), so every control that moves the sun
    // funnels through setSunAngles and nothing keeps parallel angle floats.
    void setSunAngles(float elevDeg, float azimDeg);
    // Snap to a measured day/night preset and rebake the CPU-baked per-surfel
    // sun shadows for it. Session-only: it deliberately does NOT write the
    // choice back to world.json (see the "sun" manifest key), so the shared
    // manifest is never rewritten by a runtime toggle.
    void setSunPhase(bool night);
    // Snap to a clock time on the 12 h analytic arc (ui/sun_time.hpp) and
    // rebake, exactly like the presets. Write-only like them: the panel shows
    // a best-effort read-back ("~HH:MM" when the sun sits off the arc) that
    // never feeds a write. Session-only, no world.json write; an untouched
    // startup stays at 34/238.
    void setSunTime(float hours);
    // Log every splat belonging to one lattice cell: stable id, segment, radii,
    // and whether its disk connects to any neighbour (the floating-splat
    // verdict). Pure - builds a throwaway 3x3x3 range, touches no live state.
    void describeSurfelsAt(const glm::ivec3& cell);
    // List every splat in the edited chunks (plus a 1-chunk ring, so boundary
    // disks see their neighbours) that no other splat reaches, with its stable
    // id. A detached disk cannot be CLICKED - the pick ray passes through it -
    // so this is how one gets named.
    void listFloatingSurfels();
    void cancelRotation();
    void commitMove();
    void cancelMove();

// Defined in frame/profiler.cpp, so this header stays a declaration of the
// app rather than of its instrumentation.

    void accumulateProf(uint32_t slot, uint64_t frameIdx);

    // Startup (frame/run_startup.cpp, ui/theme.cpp).
    // --probe X Y Z: answer from the load-time field, then the run is over.
    // Returns true if the world could not be loaded (exit status 1).
    bool runProbe();
    // The headless VF_TEST_* hooks: mesh import, rotate, edit, stroke, undo,
    // clear. Non-zero aborts the run.
    int runStartupTestHooks();
    // Per-backend camera spawn, the --shot / --shotlist resolution, and the
    // hud-shot request. Fills m_shots.
    void setupCameraAndShots();
    // The one-shot env overrides (render flags, photorealism, SSAO, TAA).
    void applyStartupEnvOverrides(FrameInputs& fx);
    // ImGui context, theme, backends, fonts, and the HUD's linear sampler.
    void initImGui();
    // One step of the frame loop (frame/run_*.cpp).
    // The layer/texture poll: whatever changed outside the process lands here.
    void pollWorldAndTextures(float dt);
    // Claim this frame's slot: fence wait + GPU timestamp harvest.
    FrameSync& waitFrameSlot();
    // Camera update. Returns true when the chat pane owns the keyboard.
    bool updateCamera(float dt);
    // Picking, the brush stamp, and the rotate/move gizmo drags.
    // `rotateLiveTest` suppresses the release/ring paths for the synthetic
    // VF_TEST_ROTATE_LIVE preview, which must not accumulate cursor deltas.
    void processInput(bool chatCaptures, bool rotateLiveTest);
    // The in-loop VF_TEST_* hooks, before and after input.
    void runPreInputTestHooks();
    void runPostInputTestHooks(const char* testRotateLive);
    // Tint the splats the next stamp would affect, then feed the highlight.
    void updateBrushPreview();
    // The keyboard: render flags, tool shortcuts, sidebar chrome.
    void handleHotkeys(bool chatCaptures, bool headlessRun);
    // The two recording paths. Both return kFrameDone to end the frame, or a
    // value >= 0 to end the run with that status (see frame/frame.hpp).
    int recordHeadlessFrame(FrameSync& fr);
    int recordInteractiveFrame(FrameSync& fr, const FrameInputs& fx);
    // The resolved shot list (--shot plus any --shotlist entries): built by
    // setupCameraAndShots() and walked by the headless capture path, so it is App
    // state rather than a local of whichever loop happened to own it.
    std::vector<ShotSpec> m_shots;
    

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
    vf::Image3D m_irrImg; // 64^3 irradiance volume (binding 26): created once,
                          // never recreated - replacing it would free the view
                          // the descriptor set already references
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
    // Dynamic derived lights (refreshLiveLights): last uploaded derived set
    // (splice base: survivors keep their slots) and the chunks the current
    // stroke touched (set by commitStoreEdits, consumed+cleared by the
    // trigger). The emission table + authored lights are rebuilt per trigger
    // from the atlas/manifest (microsecond lookups, always current).
    std::vector<vf::voxel::VoxelField::EmissiveCluster> m_derivedClusters;
    std::vector<int> m_lastChangedChunks;
    // Preview overlay state (refreshPreviewLights): true while the splat
    // pass holds the shifted set and the SVO pass the base set. Cleared by
    // any both-pass upload (bake/trigger) and by the overlay restore itself.
    bool m_previewLightsShifted = false;
    // Commit window: set by commitMove/commitRotation alongside the final
    // overlay push, cleared by any both-pass upload. While held, the overlay
    // keeps the shifted set (the final geometry preview stays visible until
    // the reload swaps in, so the lights must not restore early).
    bool m_previewLightsHold = false;

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
    // untextured materials, so VF_TEXTURES=0 stays bit-exact. Bit 8 =
    // enclosed-space sky occlusion: attenuates sky/IBL ambient where a ray
    // along the bent normal hits geometry, so caves/interiors stop reading as
    // open-sky daylight; authored lights (world.json "lights") take over there.
    // VolFog / motion blur / DoF stay opt-in (separate toggles; volfog is WIP -
    // see shaders/volumetric_fog.comp).
    int m_renderFlags = 511;
    float m_exposure = 1.15f;
    // SSAO tuning (VF_SSAO_*): world-scale two-band AO, opt-in via H / bit 6.
    float m_ssaoStrength = 0.6f;
    float m_ssaoRadius = 0.8f; // far-band world radius (m)
    int m_ssaoDebug = 0;       // 1 = raw AO, 2 = G-buffer normal
    bool m_ssaoBlur = true;    // cross-bilateral denoise before applying
    bool m_volFogEnabled = false;
    // Tighten only genuine hard-edge object parents; small tangent-aligned
    // bridge splats preserve crease coverage. Both are baked, so changing
    // either setting requests a world reload.
    float m_edgeShrink = 0.25f;
    bool m_edgeFill = true;
    float m_edgeBridgeSize = 1.0f;
    bool m_cornerFill = true;
    // Point-light budget: global emitter cap N (VF_LIGHT_BUDGET, 1..256,
    // default kLightBudgetDefault) + per-pixel nearest-K (VF_LIGHT_K, 1..8,
    // default 4).
    // Both re-upload the binding-25 UBO only - dragging never reloads.
    int m_lightBudget = vf::voxel::worldfile::kLightBudgetDefault;
    int m_lightK = 4;
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
    // The last pick the pointer actually made in the SCENE, kept so the
    // brush preview survives UI interaction. Hover picking is gated on
    // !io.WantCaptureMouse, so reaching for a sidebar slider cleared
    // m_hoverHit and the preview (tint + hover outline) vanished for exactly
    // as long as the brush was being resized - the size you were setting was
    // never the size you saw. The latch is world-anchored, so it stays glued
    // to the same surface cells while the camera moves; it is a PREVIEW only
    // and never feeds a stamp (the stamp path still requires a live hit).
    vf::voxel::PickHit m_latchedHover;
    // Depth-axis marker for the brush preview (xyz = world point, w = active).
    // The tint cannot show depth - it only marks existing surfels, and the
    // extra volume of a Carve/Add brush is solid material or empty air - so
    // the post pass draws this extent instead. m_depthFar is the end that
    // moves with the Depth slider; m_depthNear is the fixed end (the hit
    // plane, plus Carve's kCarveTopMargin above it).
    glm::vec4 m_depthMarker {0.f};
    glm::vec4 m_depthFar {0.f};
    glm::vec4 m_depthNear {0.f};
    glm::vec4 m_depthTint {0.f}; // rgb = the volume's own tint colour
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
    // The ONE place a brush mode changes - the sidebar's mode buttons and the
    // A/D/S/C/M hotkeys both go through this, so a pending rotate/move stage
    // can never be silently discarded by one caller and honoured by the other.
    // Refuses the switch while a transform preview is in flight or would orphan
    // a staged transform (that guard is the reason it is a function and not a
    // bare m_editBrush write).
    void chooseEditMode(EditBrush mode);
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
    // Radial falloff curve for Add, Carve, Delete and Paint: how influence
    // decays from the cursor to the rim of Width. Constant is the original
    // hard-edged footprint (and makes flat-bottomed digs); the rest taper.
    // A NAMED curve rather than a scalar, because the old 0..1 slider had a
    // cliff at 0 (exactly flat) and no usable middle (its lowest non-zero
    // setting was already 0.5 at half radius, and its top end a spike).
    // See EditableWorld::falloffCurveAt for the table.
    vf::voxel::EditableWorld::FalloffCurve m_editFalloffCurve =
        vf::voxel::EditableWorld::FalloffCurve::Smooth;
    // Smooth: soften each stamp by ChunkStore::kSmoothTaubinReinflate so a held
    // brush shrinks relief less. Off by default - it is a 47% reduction in
    // effective smoothing strength and nothing has measured it end-to-end yet.
    bool m_smoothPreserveVolume = false;
    // Footer readout of the last Smooth planner batch (columns touched, largest
    // single move). A relaxation that "did nothing" is invisible in a render,
    // and a number is the difference between legible and broken.
    int m_smoothLastColumns = 0;
    int m_smoothLastMaxDelta = 0;
    int m_smoothLastRise = 0;
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
    // per stroke: the inverse edits, the height-texture rise they need, and
    // the refresh margin the stroke itself used. The margin must be replayed
    // or the undo re-derives far more geometry than the stroke touched.
    struct UndoStep {
        std::vector<vf::voxel::StoreEdit> edits;
        int rise = 2;
        int margin = vf::voxel::LiveEditor::kStampMargin;
    };
    std::vector<UndoStep> m_undo;
    int m_strokeRiseCells = 2; // forward height-texture headroom; finishStroke
                                // derives the inverse/undo headroom separately
    int m_strokeMargin = vf::voxel::LiveEditor::kStampMargin; // margin the
                                // in-flight stroke stamps with
    // Ownership class of the pick the current stamp came from: -1 no pick, 0
    // terrain, 1 object. Logged with the stamp because a live edit that landed
    // on terrain when the user aimed at an object is worth seeing in the log
    // whether or not a test reads it, and it is the one signal that tells
    // 'tested the object class' from 'tested terrain again'. Set where the
    // hover is known (applyEditLive) and read at the log site, so
    // commitStoreEdits' signature stays untouched across its many call sites.
    // An undo line reports the class of the pick its stroke was made from.
    int m_lastPickObject = -1;
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
    // Where the pointer was at the last stamp, and which cell that stamp
    // WROTE. The first tells a drag from jitter; the second is the exact
    // signature of a held click stacking voxels, which no threshold separates
    // as cleanly as identity does.
    glm::vec2 m_lastStampMouse { 0.f };
    glm::ivec3 m_lastStampWroteCell { -1, -1, -1 };

    void applyEditLive();
    // Fill the ownership class of a HOOK-BUILT pick. An interactive pick gets
    // it from the camera ray (rayPickStore); a headless hook that builds the
    // pick straight from a lattice cell must ask the load-time oracle instead,
    // or everything reading the pick's class - the stamp log, the tests - reads
    // "terrain" for a cabin wall, which is a false statement in a log line
    // rather than a missing field.
    void adoptPickOwnership(glm::ivec3 v);
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

} // namespace app
} // namespace vf
