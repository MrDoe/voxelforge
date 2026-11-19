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
#include "render/exposure_pass.hpp"
#include "render/oit_composite_pass.hpp"
#include "render/environment_pass.hpp"
#include "render/motion_blur_pass.hpp"
#include "render/post_pass.hpp"
#include "render/splat_pass.hpp"
#include "render/ssr_pass.hpp"
#include "render/ssao_pass.hpp"
#include "render/svo_pass.hpp"
#include "render/micro_svo_pass.hpp"
#include "render/fur_pass.hpp"
#include "render/taa_pass.hpp"
#include "render/texture_atlas.hpp"
#include "render/volumetric_fog_pass.hpp"
#include "voxel/chunk_index.hpp"
#include "voxel/box_select.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/fur_cards.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/live_editor.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/micro_behaviour.hpp"
#include "voxel/picking.hpp"
#include "voxel/scenery.hpp"
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
#include <functional>
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

// The studio's bone gizmo, in WORLD space. The three rings are the PROJECTED
// CIRCLES of the bone's own axes, not fixed screen ellipses: a screen ellipse
// claims a plane the bone does not have, so which ring "was" X depended on the
// camera rather than on the bone - the first version's rings were misleading
// for exactly that reason.
struct BoneGizmo {
    glm::vec3 centre { 0.f };   // the bone's joint, world (m)
    glm::vec3 axis[3] { glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 1.f, 0.f),
                        glm::vec3(0.f, 0.f, 1.f) }; // world dirs of LOCAL X/Y/Z
    float radius = 0.f;         // ring radius, world (m)
    glm::vec2 centrePx { 0.f }; // the joint in viewport (logical) pixels
    float radiusPx = 0.f;       // the ring's screen size, for slop
};

class App {
public:
    int run(const Args& args);

private:
    bool initWindow(const Args& args);
    bool initVulkan();
    void destroy();
    bool createOffscreen(uint32_t w, uint32_t h);
    // 3DGS OIT targets are created on first use, not for every run: four
    // full-res RGBA16F images are ~16 MiB at 960x540 and ~46 MiB at 1600x900.
    // Called at startup for --mode 3dgs, on F-cycle into Dgs, and on resize.
    bool ensureOitTargets();
    void ensureAcquireSemaphores();
    void handleResize();
    void drawHud();
    // The single editor panel: one docked left sidebar (icon rail + content
    // pane + status footer). Each section is its own method so the sidebar
    // stays a thin dispatcher and every section reads on its own.
    void drawSidebar();
    void drawTimeline();
    void drawPanelEdit();
    void drawPanelWorld();
    void drawPanelRender();
    void drawPanelTextures();
    void drawPanelMesh();
    void drawPanelAI();
    void drawPanelAnimate();
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
    // Rebuild + upload the raster ground-foliage cards from the atlas's
    // material bindings. Called after every atlas load/world reload.
    bool rebuildGrassCards();
    void persistWorldLayers();
    void rescanWorldLayers();
    // In-session scenery switch: re-home the live world at another
    // scenery/<name>/ folder (heightmap, manifest, layers, AI-edits, overlay,
    // textures, micro, surfels all follow) and persist it as the new default.
    // False when the target is unusable or the switch was rolled back.
    bool switchScenery(const std::string& name);
    void rescanMeshFiles();
    void prepareMeshImport();
    void useMeshImportLayer(const std::string& file);
    bool importMeshFromGui();
    // material texture picker (assets/textures/* -> world.json "textures")
    void rescanTextureFiles();
    void setTextureBinding(int mat, const std::string& file);
    void applyTextureBindings();
    // Texture picker gallery: load/refresh thumbnail images for m_texFiles and
    // release them. Called from drawPanelTextures when the file list changed.
    void refreshTextureThumbnails();
    void destroyTextureThumbnails();
    // Foliage-card picker
    void rescanCardFiles();
    void setCardBinding(int mat, const std::string& card);
    void applyCardBindings();
    void refreshCardThumbnails();
    void destroyCardThumbnails();
    // hot-swap watch: folder drop-ins + mtime of every bound image file
    void pollTextureFiles();
    void applyWorldReload();
    void rebuildSurfels(); // (re)build the surfel set from the live field
    // Load "role":"micro" manifest layers into SEPARATE micro SVO worlds and
    // hand them to the GPU overlay pass. They never enter the world record set,
    // the VoxelField, the ChunkStore or the live-edit overlay.
    void loadMicroObjects();
    // Pick a MICRO object under a ray. The micro objects are separate SVO
    // worlds, so the world's store march cannot see them; without this a
    // micro object is invisible to the pointer and cannot be selected.
    // Returns the file of the object hit, or an empty string.
    struct MicroObjectPick {
        bool hit = false;
        float dist = 0.f;
        glm::vec3 pos { 0.f };
        glm::vec3 normal { 0.f, 1.f, 0.f };
        glm::ivec3 cell { 0 };
        std::string file;
    };
    MicroObjectPick pickMicroObjects(const std::vector<vf::voxel::MicroOccluder>& occ,
                                     glm::vec3 ro, glm::vec3 rd) const;
    // THE pick, at a window position (logical pixels, top-left origin): the
    // screen->ray conversion, the world's store march and the micro objects'
    // march, with the nearer hit winning. processInput uses it, and so does
    // the VF_TEST_PICK_SCAN hook - so a test measures exactly what the
    // pointer does, not a re-implementation of half of it.
    vf::voxel::PickHit pickAtWindowPos(double mx, double my);
    // A TEST-ONLY pointer override. The interactive input path (the gates
    // that decide whether a pick happens at all, and the selection itself)
    // used to be reachable ONLY with a real mouse, which meant it could not
    // be tested without taking the user's pointer. This override injects a
    // window position and a button state into processInput, so a headless
    // run can drive the REAL path.
    struct TestPointer {
        bool valid = false;
        double x = 0.0, y = 0.0;
        bool ctrl = false;
        bool lmb = false;
    };
    TestPointer m_testPointer;
    // Is this manifest file a MICRO layer (a separate SVO world)?
    static bool isMicroLayer(const std::string& file) { return file.rfind("micro/", 0) == 0; }
    // The world-space AABB of a layer, for either kind: a world layer's box
    // comes from the merged field, a micro layer's from its own fine lattice.
    vf::voxel::WorldAABB layerBounds(const std::string& file) const;
    // The index of a micro layer's world in the micro pass's table, or -1.
    int microWorldIndex(const std::string& file) const;
    // The rotation pivot of a layer: the bottom-centre of its placed bounds.
    // A MICRO layer is not in the merged world, so its pivot comes from its
    // own fine lattice - but it is the SAME convention (centre in x/z, base
    // in y), so a micro object and a world object turn about the same point.
    bool layerPivotWorld(const std::string& file, glm::vec3& pivot) const;
    // Push (or clear) the live preview offset of the selected micro object.
    void updateMicroMovePreview();
    // Clear the live ROTATION preview of every micro world (the micro pass
    // holds it: a micro object has no world owner id, so the splat pass's
    // rotate preview cannot show it).
    void clearMicroRotatePreview();
    // Advance the AUTONOMOUS BEHAVIOURS (a micro layer with a "behaviour"
    // in the manifest walks on its own) and push their transforms to the
    // micro pass. Called once per frame. A walker is GROUND-CLAMPED to the
    // terrain and refuses a step into an object; see world/collision.cpp.
    void updateMicroBehaviours(float dt);
    // Tell a micro object what to do: "run", "walk", "graze", "stop",
    // "auto". `file` is a manifest file ("micro/alpaca.vxw") or a layer
    // NAME ("alpaca"). This is how the AI drives an animal ("let the alpaca
    // run") and how a user would do it.
    bool setMicroBehaviour(const std::string& file, const std::string& command,
                           float speed = 0.f);

    // ---- collision (world/collision.cpp) -----------------------------------
    // ONE solidity oracle shared by the fly camera and every autonomous
    // walker. World reads use the live ChunkStore (terrain + objects + runtime
    // edits); micro reads sample each field at its CURRENT transform (the
    // behaviour offset/rotation and a staged move/rotate preview), so a
    // collision cannot lag the geometry the user is standing next to.
    bool worldSolidAt(glm::vec3 p); // store cell solid (sdfRaw <= 0)
    bool microSolidAt(glm::vec3 p); // inside any micro object
    // ONE micro field at its current transform (behaviour + staged preview).
    // microSolidAt is the union over these; the headless check targets one so
    // a nearby object cannot answer for it.
    bool microFieldSolidAt(size_t index, glm::vec3 p);
    bool solidAt(glm::vec3 p);      // world OR micro
    // Move the camera sphere, sliding along walls. A camera that is already
    // embedded is allowed to move freely (escape hatch).
    glm::vec3 resolveCameraMove(glm::vec3 from, glm::vec3 delta);
    // A walker's obstacle test: world OBJECTS and every OTHER micro field.
    // Terrain is deliberately excluded (the walker is clamped to the
    // heightfield separately) and so is its own field. `why` (optional) gets
    // the reason: 1 = water, 2 = a world object cell, 3 = another micro
    // object - a trace only, the verdict is identical without it.
    bool microFootprintBlocked(int microIndex, glm::vec3 offset,
                               int* why = nullptr);
    // A conservative world AABB for ONE micro object at the transform
    // microFieldSolidAt applies to it (behaviour offset + rotation about the
    // pivot; a staged Move/Rotate preview is NOT included - the footprint
    // probe turns its broad phase off while one is active). false = no body.
    bool microWorldBounds(size_t microIndex, glm::vec3& outLo,
                          glm::vec3& outHi) const;
    // The terrain top at a world xz, from the CPU mirror of the SHADED height
    // texture (kept in sync after every live stamp, so an added mound is
    // ground a walker climbs). Returns -1e9 outside the landscape.
    float terrainTopAt(float x, float z);
    // The micro objects as SUN-SHADOW OCCLUDERS for the world's baked surfel
    // shadow. Filled by loadMicroObjects and consumed by rebuildSurfels, which
    // is why a world reload loads the micro objects FIRST.
    std::vector<vf::voxel::MicroOccluder> m_microOccluders;
    // The fields the occluders point at; owning storage for the pointers.
    std::vector<vf::voxel::MicroField> m_microFields;
    // MESH-AS-GROUND-TRUTH: when a micro layer has a meshSource, the field is
    // derived from the mesh (deformed via LBS) instead of being pre-baked.
    // Stored per micro world (parallel to m_microFields).
    struct MicroMeshData {
        std::vector<glm::vec3> restVerts;   // rest-pose vertices (metres, object frame)
        std::vector<glm::vec3> deformedVerts; // scratch buffer
        std::vector<int> skinBone;            // per-vertex bone index (-1 = unskinned)
        std::vector<vf::voxel::MeshTri> tris;           // rest-pose triangles (for voxelization)
        std::string meshPath;
        std::string skinPath;
        bool active = false;                  // true if meshSource is set and loaded
    };
    std::vector<MicroMeshData> m_microMeshes;
    // One behaviour per micro world (parallel to m_microOccluders). A layer
    // with a "behaviour" in the manifest walks on its own.
    std::vector<vf::voxel::MicroBehaviour> m_microBehaviours;
    // Owned clip storage (parallel-ish; indexed by micro world). Pointer-stable
    // via unique_ptr so MicroBehaviour::clip can point into it.
    std::vector<std::vector<std::unique_ptr<vf::voxel::MicroClip>>> m_microClips;

    // Animation studio state.
    struct StudioState {
        bool active = false;
        bool overWorld = false;
        std::string layerFile;
        // THE STUDIO'S TARGET: the index into the micro layers (the same
        // index m_microOccluders / m_microBehaviours / m_microClips use) of
        // the layer the editor animates. Resolved from layerFile in
        // loadMicroObjects().
        //
        // It used to be "index 0" everywhere, and index 0 in the shipped
        // manifest is the VASE - so "--anim-layer micro/alpaca.vxw" framed,
        // drove and displayed the vases while the panel header said
        // "Object: micro/alpaca.vxw". -1 = no target (a typo'd name).
        int targetIndex = -1;
        std::string clipFile;
        bool playing = true;
        float time = 0.f;
        float speed = 1.f;
        // --anim-time: the clip clock is pinned to `time` and does not
        // advance. A shot of the studio is reproducible only when this is
        // on; without it the editor plays live (the behaviour owns the clock).
        bool pinned = false;
        int selectedBone = 0;
        int selectedClip = 0;
        bool showGrid = true;
        bool showSkeleton = true;
        glm::vec3 camOffset { 0.f };
        glm::vec3 lastCenter { 0.f };
        bool camOffsetSet = false;
        // ---- the BONE GIZMO ---------------------------------------------
        // The selected bone gets three rings at its joint, and dragging one
        // rotates that bone and STORES the result as a keyframe at the
        // playhead. The state lives here because the frame loop owns the
        // pointer while the panel owns the selection.
        int gizmoDragBone = -1;            // the bone being dragged, -1 = idle
        // The dragged bone's STABLE id. The rig can be reloaded mid-drag
        // (Reload rig, a layer edit), and an index alone would then rotate
        // whichever bone landed in that slot.
        uint32_t gizmoDragBoneId = 0;
        // Which of the bone's OWN axes (0 = X, 1 = Y, 2 = Z) the drag grabbed.
        int gizmoDragAxis = -1;
        // The grabbed ring's plane, frozen at the drag's start: the angle is
        // measured in it, so the drag cannot jump when the bone turns under
        // the pointer.
        glm::vec3 gizmoCentre { 0.f };     // the joint, world
        glm::vec3 gizmoPlaneN { 0.f, 1.f, 0.f };
        glm::vec3 gizmoPlaneU { 1.f, 0.f, 0.f };
        glm::vec3 gizmoPlaneV { 0.f, 0.f, 1.f };
        float gizmoLastAngle = 0.f;        // radians, in that plane
        float gizmoRadiusPx = 0.f;         // the ring's screen size (log)
        // TRANSLATION: the joint itself is the drag handle (the centre dot),
        // and the bone slides in the camera plane.
        bool gizmoTranslating = false;
        glm::vec2 gizmoLastMouse { 0.f };
    } m_studio;

    // THE STUDIO'S TARGET, resolved. Every studio site (the panel, the
    // timeline, the camera, the clip clock) goes through these, so the
    // target can never drift back to "index 0" - which is the vase in the
    // shipped manifest, not the animal the user asked for.
    int studioTarget() const { return m_studio.targetIndex; }
    // The behaviour of the target (nullptr when there is no target).
    vf::voxel::MicroBehaviour* studioBehaviour();
    // The clip set of the target (nullptr when there is no target).
    std::vector<std::unique_ptr<vf::voxel::MicroClip>>* studioClips();
    // The occluder of the target (nullptr when there is no target).
    vf::voxel::MicroOccluder* studioOccluder();

    // The selected bone's gizmo in WORLD space, or false when there is
    // nothing to manipulate. Its rings are the projections of the bone's own
    // axes, so the ring the user sees IS the axis it rotates.
    bool studioBoneGizmo(BoneGizmo& out);
    // The viewport in LOGICAL window pixels (ImGui's DisplaySize). Picking and
    // gizmo projection must use these, not framebufferSize(), on a high-DPI
    // display.
    glm::ivec2 viewportLogical() const;
    // One point of a ring, in viewport pixels (`t` in radians; 0..2pi draws
    // the whole circle). Shared by the overlay and the hit test, so the drawn
    // ring and the tested ring cannot disagree.
    glm::vec2 studioRingPoint(const BoneGizmo& g, int axis, float t);
    // Which ring the pointer is on: 0/1/2, or -1. Distance is measured to the
    // PROJECTED polyline, so it works at any bone orientation (including a
    // ring seen nearly edge-on, where a fixed-shape ellipse test cannot).
    int studioBoneRingAt(const BoneGizmo& g, glm::vec2 p, float slop = 10.f);
    // Begin a bone drag on `axis` (screen bookkeeping, plus the key the drag
    // writes into), and apply the pointer's current position to the active
    // drag. Split out of the frame loop so a headless hook drives the SAME
    // code the mouse drives - a UI feature that only a mouse can reach cannot
    // be gated.
    bool studioBeginBoneDrag(int boneIndex, int axis, const BoneGizmo& g,
                             glm::vec2 mouse);
    void studioApplyBoneDrag(glm::vec2 mouse);
    // The target's editable clip. CREATED and selected when the rig is in
    // gait mode: a bone key has to live somewhere, and "make a clip first" is
    // a step that exists only because of the file format.
    vf::voxel::MicroClip* studioEnsureClip();    // The key at `t` for `boneId`, created (SEEDED from the pose there, so the
    // first edit never snaps the bone to zero) when absent. `t` is quantised
    // to the clip's frame grid, so one drag updates one key instead of
    // writing hundreds.
    vf::voxel::BoneKey* studioKeyAt(vf::voxel::MicroClip& clip, uint32_t boneId,
                                    float t);
    // The selected bone's authored pose at `t`, INTERPOLATED by the same rule
    // the rig samples (`sampleTrackEuler`). False when the bone has no track.
    // The playhead is almost never exactly ON a key, so a panel that only read
    // keys would show nothing for most of the timeline.
    bool studioBoneEulerAt(uint32_t boneId, float t, glm::vec3& out);
    // Push the (edited) rig's bone SEGMENTS and radii to the micro and fur
    // passes. They are uploaded once at load and otherwise treated as static,
    // so a length/radius edit that skipped this would move the drawn skeleton
    // and the CPU rig while the body kept the old capsule.
    void studioUploadRig();
    // Set a bone's LENGTH in metres, keeping its joint and direction. The one
    // path the panel slider and the headless hook share.
    bool studioSetBoneLength(int boneIndex, float metres);
    // TRANSLATE a bone by moving its JOINT. The tip stays put, so the bone's
    // LENGTH and direction change implicitly (that is the resize); a parent
    // whose tip was this joint stretches with it so the chain cannot tear.
    bool studioMoveBone(int boneIndex, glm::vec3 delta);
    // The joint's own screen hit test (the gizmo's centre dot), and the drag
    // that slides the bone in the camera plane.
    bool studioBoneCentreAt(const BoneGizmo& g, glm::vec2 p, float slop = 9.f);
    bool studioBeginBoneTranslate(int boneIndex, const BoneGizmo& g,
                                  glm::vec2 mouse);
    void studioApplyBoneTranslate(glm::vec2 mouse);

    // The fur cards of the micro objects that carry a fur material, and the
    // objects' world AABBs (the cards are generated in the field's local
    // frame, so the world min is what turns that frame into the world one).
    std::vector<vf::voxel::FurObject> m_furObjects;
    // The fur index of each MICRO world (-1 when that object carries no
    // fur), so the live preview can be mirrored onto the fur. The two lists
    // are indexed differently on purpose: the fur pass has its own table, and
    // a micro object without fur simply has no entry in it.
    std::vector<int> m_furIndexOfMicro;
    // The field each fur object was grown from, so the coat can be REBUILT
    // (the parameters changed) without re-reading the layers.
    std::vector<int> m_furSourceField;
    // The FUR COAT's look. The sliders live in the sidebar's Rendering
    // section; the per-material switch lives in Textures, because a coat is a
    // property of the MATERIAL, not of the object.
    bool m_furEnabled = true;
    int m_furPerCell = 5;
    float m_furLength = 0.030f;
    float m_furWidth = 0.0028f;
    float m_furCurl = 0.20f;
    float m_furCycles = 1.2f;
    float m_furTilt = 38.0f;
    // Backlit translucency strength (backlit fleece glows when the sun is
    // behind it). A LOOK constant, tuned to 0.8: at a backlit camera that
    // lifts the coat's changed pixels +23 luma and shifts R-B -11 -> +4,
    // confined to the animal (the house view is byte-identical). The default
    // sun is front/back-side to the reference cameras, so reference shots are
    // unchanged. VF_FUR_SSS=0 restores the original bit-exact look.
    float m_furSss = 0.8f;
    // The materials that grow a coat. 21 is `fleece`.
    std::vector<uint8_t> m_furMaterials { 21 };
    // Grow the cards with the current parameters and upload them. The cards
    // are a function of the field and the parameters, so this is the same
    // work the load does - a few hundred milliseconds, which is why the
    // sliders apply on release and not per tick.
    void rebuildFurCards();
    // Grow the cards for ONE micro field and append them to m_furObjects.
    // Returns the new object's fur index, or -1 when it grows no coat.
    int growFurCards(const vf::voxel::MicroField& field, glm::vec3 worldOffset,
                     const vf::voxel::MicroRig& rig,
                     std::function<int(glm::vec3)> boneFn = nullptr);
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
    // VF_TEST_COLLISION=1: drive the SHIPPING camera sweep and the walker
    // response headlessly and log what they did (PASS/FAIL), so a script can
    // assert that the camera stops and no walker sinks or overlaps. See
    // frame/run_startup.cpp.
    void runCollisionSelfCheck();
    // ImGui context, theme, backends, fonts, and the HUD's linear sampler.
    void initImGui();
    // One step of the frame loop (frame/run_*.cpp).
    // The layer/texture poll: whatever changed outside the process lands here.
    void pollWorldAndTextures(float dt);
    // Claim this frame's slot: fence wait + GPU timestamp harvest.
    FrameSync& waitFrameSlot();
    // Camera update. Returns true when a text field owns the keyboard
    // (textFieldOwnsKeyboard), which freezes the whole view - see the rule in
    // frame/frame.hpp.
    bool updateCamera(float dt);
    // Drain the button latches while a text field owns the keyboard, so no
    // picking/stamp/drag runs and the click that leaves the field is not
    // spent on the world behind it.
    void drainInputWhileTextFocused();
    // Picking, the brush stamp, and the rotate/move gizmo drags.
    // `rotateLiveTest` suppresses the release/ring paths for the synthetic
    // VF_TEST_ROTATE_LIVE preview, which must not accumulate cursor deltas.
    void processInput(bool textCaptures, bool rotateLiveTest);
    // The in-loop VF_TEST_* hooks, before and after input.
    void runPreInputTestHooks();
    void runPostInputTestHooks(const char* testRotateLive);
    // Tint the splats the next stamp would affect, then feed the highlight.
    void updateBrushPreview();
    // The keyboard: render flags, tool shortcuts, sidebar chrome. Suppressed
    // entirely while a text field owns the keyboard.
    void handleHotkeys(bool textCaptures, bool headlessRun);
    // The two recording paths. Both return kFrameDone to end the frame, or a
    // value >= 0 to end the run with that status (see frame/frame.hpp).
    int recordHeadlessFrame(FrameSync& fr);
    int recordInteractiveFrame(FrameSync& fr, const FrameInputs& fx);
    // The animation studio's frame: backdrop clear + micro overlay + fur +
    // post, and nothing else. Shared by both recording paths so the editor
    // and a --shot of it cannot drift apart.
    void recordStudioFrame(VkCommandBuffer cmd, const vf::RaymarchPush& push);
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
    void recordPhotorealism(VkCommandBuffer cmd, const vf::RaymarchPush& push,
                            bool linearHdr = false);

    // Write the live camera basis into m_prevCam so the NEXT frame's reprojection
    // (TAA history + motion-blur velocity) sees this frame's camera. Called at
    // the END of a recorded frame on BOTH paths, and once from App::run() just
    // after setupCameraAndShots() so the first frame is already seeded.
    //
    // The aspect ratio comes from the SWAPCHAIN, matching what both frame
    // recorders put in push.a, because that is the projection the G-buffer
    // world positions were rendered with - a mismatched aspect silently halves
    // or doubles the horizontal velocity.
    void advancePrevCam();

    Args m_args;
    vf::Window m_window;
    vf::Context m_ctx;
    vf::Swapchain m_swapchain;

    vf::Image3D m_offscreen;
    vf::Image3D m_hdr;     // ray-march linear HDR output
    vf::Image3D m_gpos;    // ray-march G-buffer: world pos + hit type
    vf::Image3D m_gnorm;   // G-buffer: shading normal (xyz), 0 on sky
    // G-buffer guidance lanes (splat attachments 3 and 4). These exist because
    // the renderer otherwise emits only colour/position/normal: albedo and
    // roughness arrive per surfel and are consumed into the lighting integral,
    // so nothing downstream can recover a surface's material. rgb = effective
    // albedo / a = roughness, and rgb = pre-atmosphere lighting / a =
    // reflectance. Linear HDR like m_hdr, and unread by the shipped chain, so
    // the visible frame does not change when they are written.
    vf::Image3D m_galb;
    vf::Image3D m_gmat;
    // --mode 3dgs OIT targets (all linear HDR). `m_oitAccum` sums premultiplied
    // colour + alpha, `m_oitReveal` accumulates transmittance Π(1-α), and
    // `m_oitBackdrop` holds the sky. OitCompositePass turns them into m_hdr.
    vf::Image3D m_oitAccum;
    vf::Image3D m_oitReveal;
    vf::Image3D m_oitBackdrop;
    vf::Image3D m_oitBase;   // copy of the standard frame; interiors come from here
    vf::Image3D m_ssaoAo;  // SSAO scratch: raw AO term (ssao.comp -> ssao_apply.comp)
    // SSR scratch. `ssr.comp` reads the scene colour at pixels it did NOT
    // write (its reflected ray marches across the screen), so it may not
    // sample the image it stores into. This is the read side's twin: the
    // pass writes here and the frame copies it back, which is what makes a
    // still frame bit-reproducible (the LDR/shipped path).
    vf::Image3D m_ssrOut;
    // Linear-HDR scratch for the FX chain under VF_LINEAR_FX. The gathering
    // passes (SSR, motion blur, DoF) write here instead of reading the image
    // they overwrite, and these five shaders declare rgba16f — which the rgba8
    // m_ssrOut/m_offscreen are not. Only used on the linear path; the shipped
    // LDR path keeps m_ssrOut's historical bit-exact behaviour.
    vf::Image3D m_fxScratch;
    vf::Image3D m_heightImg;
    vf::Image3D m_objVolImg;
    vf::Image3D m_irrImg; // 64^3 irradiance volume (binding 26): created once,
                          // never recreated - replacing it would free the view
                          // the descriptor set already references
    vf::SvoPass m_svoPass;
    // GPU overlay for SEPARATE high-detail micro SVO worlds (own fine lattice).
    // Reads the main backend's G-buffer and overwrites a pixel only where a
    // micro surface is nearer - so it needs no depth attachment and no change
    // to the splat or water pass. Empty = the feature is off and record() is a
    // single branch.
    vf::MicroSvoPass m_microSvoPass;
    // The translucent hair-card layer over the micro objects. It follows the
    // micro overlay and composites into the same HDR image, so the fur can be
    // transparent without touching the opaque pipeline. Empty = the feature
    // is off and record() is a single branch.
    vf::FurPass m_furPass;
    vf::SplatPass m_splatPass;
    vf::TaaPass m_taaPass;
    vf::PostPass m_postPass;
    // photorealism passes
    vf::SSRPass m_ssrPass;
    vf::SSAOPass m_ssaoPass;
    vf::VolumetricFogPass m_volFogPass;
    vf::MotionBlurPass m_motionBlurPass;
    vf::DepthOfFieldPass m_dofPass;
    vf::ExposurePass m_expoPass;  // scene-referred meter feeding post.comp
    vf::OitCompositePass m_oitPass; // --mode 3dgs final transmittance composite
    bool m_autoExposure = false;  // render-flag bit 10 + VF_AUTOEXPOSE
    vf::EnvironmentPass m_envPass;
    vf::TexAtlas m_texAtlas;   // optional PNG overrides (world.json "textures")
    std::string m_manifestPath; // world.json path (for atlas reloads)
    // primary visibility renderer; SVO kept as the pixel reference;
    // Dgs = opt-in 3DGS-style weighted-blended OIT transmittance composite.
    enum class RenderMode { Splats, Svo, Dgs };
    RenderMode m_renderMode = RenderMode::Splats;
    vf::Image3D m_taaHistory[2];
    vf::Image3D m_taaResolved;
    // TAA is OFF by default (N re-enables it and forces a clean first frame).
    // The viewer now shows the raw post-pass image, which is what
    // `--shot`/headless has always produced - so an interactive frame and a
    // headless frame of the same camera agree pixel-for-pixel. TAA does have a
    // real effect (reprojected reprojection + variance clamp), it just is no
    // longer what you see unless you ask for it.
    //
    // m_prevCam is advanced every interactive frame in BOTH branches of the
    // TAA gate (see record_interactive), because the opt-in motion blur (K)
    // reprojects against the same m_prevCam and would otherwise smear on a
    // stale camera while TAA is off.
    bool m_taaEnabled = false;
    float m_taaBlend = 0.92f; // base history blend (VF_TAA_BLEND overrides)
    bool m_taaFirstFrame = true;
    int m_taaHistoryIdx = 0;
    // Previous-frame camera basis (vf::PrevCam, render/taa_pass.hpp). Advanced
    // at the END of every recorded frame on both paths, and SEEDED at startup
    // so the first frame's reprojection is already valid. See advancePrevCam().
    vf::PrevCam m_prevCam;
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
    // Separate manifest watcher for micro-voxel objects: they are not part
    // of the world record set, so a manifest-only change (e.g. an MCP
    // set_layer_transform on a micro layer) never triggers applyWorldReload
    // and loadMicroObjects() would not run. Track the manifest mtime/size
    // here so such edits hot-reload the micro objects within ~0.5 s.
    unsigned long long m_microManifestMtime = 0;
    unsigned long long m_microManifestSize = 0;
    bool m_microManifestSeeded = false;
    // texture picker state: the live binding table + the files found in
    // assets/textures/ (paths relative to the manifest dir, e.g. "textures/x.png")
    std::vector<vf::voxel::worldfile::TextureBinding> m_texBindings;
    std::vector<std::string> m_texFiles;
    // Texture picker gallery thumbnails: one small Vulkan image + ImGui
    // descriptor per file in m_texFiles. Rebuilt when the file list changes.
    struct TexThumb {
        std::string file;
        vf::Image3D img {};
        ImTextureID id = 0;
    };
    std::vector<TexThumb> m_texThumbs;
    bool m_texThumbsDirty = true;
    // Foliage-card picker (world.json "foliageCards": mat -> card name, joined
    // with assets/textures/cards/cards.json). Same shape as the texture picker.
    std::vector<vf::voxel::worldfile::FoliageCardBinding> m_cardBindings;
    // available cards: name -> png file (relative to assets/textures/cards/)
    std::vector<std::pair<std::string, std::string>> m_cardFiles;
    struct CardThumb { std::string file; vf::Image3D img {}; ImTextureID id = 0; };
    std::vector<CardThumb> m_cardThumbs;
    bool m_cardThumbsDirty = true;
    bool m_cardApplyPending = false;
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
    // The camera is a collision sphere in interactive flight (VF_NO_COLLIDE=1
    // disables it, e.g. to inspect a sealed interior). Camera placement from
    // --cam/--shot is never collided: a test that pins a camera inside a wall
    // must still be able to render it.
    bool m_collisionEnabled = true;
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
    // Photo foliage cards (bit 13) ON by default: photographic CC0 grass/leaf
    // cards replace procedural blades at silhouettes. VF_RENDER_FLAGS still
    // overrides wholesale (e.g. 511 = pre-card look) for A/B gates.
    int m_renderFlags = 511 | (1 << 13);
    float m_exposure = 1.15f;
    // SSAO tuning (VF_SSAO_*): world-scale two-band AO, opt-in via H / bit 6.
    float m_ssaoStrength = 0.6f;
    float m_ssaoRadius = 0.8f; // far-band world radius (m)
    int m_ssaoDebug = 0;       // 1 = raw AO, 2 = G-buffer normal
    bool m_ssaoBlur = true;    // cross-bilateral denoise before applying
    // Run the photorealism chain on the linear HDR target BEFORE the tone map
    // instead of in place on the tone-mapped LDR offscreen. Default off: this is
    // a look change on every effect in the chain (an AO multiply in linear space
    // is not the same multiply in compressed space), so it ships behind an
    // escape hatch until each effect has been re-measured where it now runs.
    bool m_linearFx = false;
    bool m_volFogEnabled = false;
    // Tighten only genuine hard-edge object parents; small tangent-aligned
    // bridge splats preserve crease coverage. Both are baked, so changing
    // either setting requests a world reload.
    //
    // Both fills default OFF. Corner caps are a measured no-op on the shipped
    // hamlet - the parent's own disk already covers every corner vertex, so
    // the coverage guard removes ~everything (ON vs OFF: house mean 0.0076
    // L2/code, max 6, against a same-binary rerun noise floor of 0.0088 / 5;
    // a thin-beam close-up reads 0.0089 against 0.0091). The mechanism is
    // still LIVE, not dead code: a genuine thin rod emits 8 caps (unit-test
    // pinned), so the clean hamlet reading is a property of its content.
    // Edge bridges are NOT a free no-op: they re-cover what the 0.25
    // edgeShrink stops short of, and turning them off costs ~0.6 L2/code at
    // the house view (1.24% of pixels move by >12) even though mean luma and
    // world coverage stay flat. They are off because the coverage they add is
    // redundant on this scene, not because they cost nothing.
    float m_edgeShrink = 0.25f;
    bool m_edgeFill = false;
    float m_edgeBridgeSize = 1.0f;
    bool m_cornerFill = false;
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
    vf::voxel::EditableWorld m_editable { vf::voxel::scenery::root() };
    // Carve/raise rasterizer providers: the edit tool only calls their
    // makeXxx() shape builders (the carving/raising layers themselves are
    // legacy - every brush stamp now patches the runtime store instead).
    vf::voxel::EditableWorld m_carve { vf::voxel::scenery::root(),
                                       std::string(vf::voxel::EditableWorld::kCarveFileName),
                                       std::string(vf::voxel::EditableWorld::kCarveLayerName),
                                       std::string("carve") };
    vf::voxel::EditableWorld m_add { vf::voxel::scenery::root(),
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
    // ---- bounding-box selection (edit/box_select.cpp) -----------------------
    // A lattice AABB that SCOPES the live-edit brush: while active, a stamp
    // only touches cells inside it, the hover tint clips to the same set, and
    // the post pass draws the box as a wireframe. Shift+LMB drag defines it
    // (corner to corner); X clears it. The Rotate/Move transforms deliberately
    // ignore it - they act on a whole layer owner, not on cells.
    vf::voxel::CellBox m_boxSelect;
    bool m_boxDragging = false;   // Shift+LMB drag in progress
    glm::ivec3 m_boxAnchor { 0 }; // the corner the drag started from
    // Last contents report (solid cells / surfels inside the box), published
    // on release and shown in the Edit section.
    int m_boxStatCells = 0;
    int m_boxStatObjects = 0;
    int m_boxStatSplats = 0;
    // Cells the LAST stamp dropped for being outside the box (per-stamp
    // log line when non-zero; a stamp that is entirely outside does nothing).
    int m_lastStampOutside = 0;
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
    // ---- bounding-box selection (defined in edit/box_select.cpp) -----------
    // Start/extend/end the Shift+LMB corner drag. The box is anchored at the
    // pressed cell and spans to the cell under the cursor, inclusive.
    void beginBoxSelection(const glm::ivec3& anchor);
    void updateBoxSelection(const glm::ivec3& hover);
    // Commit the drag: report the contents (cells + surfels) to the log and
    // publish the sidebar readouts.
    void endBoxSelection();
    void clearBoxSelection();
    bool boxSelectionActive() const { return m_boxSelect.valid(); }
    // True when the cell may be edited: no box, or the cell is inside it.
    // The brush modes call this so a box is one rule, checked in one place.
    bool boxSelectsCell(const glm::ivec3& v) const
    {
        return !m_boxSelect.valid() || m_boxSelect.contains(v);
    }
    // Drop out-of-box edits in place (see CellBox::clampEdits).
    size_t applyBoxSelectionScope(std::vector<vf::voxel::StoreEdit>& edits);
    // Cell indices + active flag for the BrushUBO (both backends) and the
    // post-pass wireframe feed: ONE representation, so the drawn box, the
    // tinted preview and the scoped cell set cannot disagree.
    void boxSelectionFeeds(glm::vec4& lo, glm::vec4& hi) const;
    // Count solid cells / object-owned cells / surfels inside the box and log
    // them (the surfel count re-derives the shared store range builder per
    // overlapped chunk - same normal pipeline the stamps use).
    void reportBoxSelection();
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
