// Voxelforge - window, chunked-SVO ray marcher, HUD.
#include "core/camera.hpp"
#include "core/log.hpp"
#include "platform/window.hpp"
#include "rhi/swapchain.hpp"
#include "render/svo_pass.hpp"
#include "render/splat_pass.hpp"
#include "render/post_pass.hpp"
#include "render/taa_pass.hpp"
#include "render/ssr_pass.hpp"
#include "render/ssao_pass.hpp"
#include "render/volumetric_fog_pass.hpp"
#include "render/motion_blur_pass.hpp"
#include "render/dof_pass.hpp"
#include "render/environment_pass.hpp"
#include "voxel/surfelize.hpp"
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
#include "voxel/layered_world.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

namespace {

struct Args {
    bool selftest = false;
    int smokeFrames = 0;
    int width = 1600, height = 900;
    std::string shot;    // dump one frame to PPM and exit
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
        else if (s == "--cam" && i + 6 < argc) {
            a.camx = atof(argv[++i]); a.camy = atof(argv[++i]); a.camz = atof(argv[++i]);
            a.tx = atof(argv[++i]); a.ty = atof(argv[++i]); a.tz = atof(argv[++i]);
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
    bool runSelftest();
    void syncWorldLayerList();
    bool uploadTerrainTexture();
    bool uploadObjVolTexture();
    void persistWorldLayers();
    void rescanWorldLayers();
    void applyWorldReload();
    void rebuildSurfels(); // (re)build the surfel set from the live field
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
    vf::voxel::LayeredWorld m_layers;
    float m_layerPollT = 0.f;
    bool m_pendingWorldReload = false;

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
    // GPU timestamp profiling state (see kProfMarks)
    VkQueryPool m_profPool = VK_NULL_HANDLE;
    double m_profPeriodNs = 1.0;
    double m_profAvg[5] = { 0, 0, 0, 0, 0 };
    bool m_showControls = true;
    VkSampler m_uiSampler = VK_NULL_HANDLE;
    ImTextureID m_sceneTexId = 0;
    bool m_scenePreview = false;
    float m_animTime = 0.0f;
    int m_tonemapLook = 2;
    int m_renderFlags = 31;   // bit0 AO,bit1 shadows,bit2 flora,bit3 water,bit4 outline,bit5 SSR,bit6 SSAO,bit7 volFog,bit8 motionBlur,bit9 DoF
    float m_exposure = 1.15f;
    bool m_volFogEnabled = false;
    bool m_motionBlurEnabled = false;
    bool m_dofEnabled = false;
    float m_dofFocusDist = 10.0f;
    float m_dofFocalLength = 50.0f;
    uint32_t m_nextAcquire = 0;

    // AI chat + picking + editable world
    vf::voxel::EditableWorld m_editable { std::string(VOXELFORGE_ASSET_DIR) };
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

    // carve / add edit tool: stamps an oriented cylinder (carve = subtractive,
    // add = solid) of m_editDiameter at the hovered surface point, along its
    // normal. Diameter/depth adjustable via +/-; mode toggled with [C].
    bool m_editActive = false;
    bool m_editCarve = true;
    float m_editDiameter = 2.0f; // meters
    float m_editDepth = 1.5f;    // meters (carve depth / add length)
    uint8_t m_editMat = 6;       // wood/rock-ish palette id for the add mode

    void applyEdit();
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
    if (!m_splatPass.recreateDepth(w, h))
        return false;
    if (!m_offscreen.img || !m_taaHistory[0].img || !m_taaHistory[1].img ||
        !m_taaResolved.img || !m_hdr.img || !m_gpos.img)
        return false;
    m_svoPass.updateDescriptors(m_hdr, m_gpos);
    m_splatPass.updateDescriptors(m_hdr.view, m_gpos.view, m_heightImg.view,
                                  m_objVolImg.view);
    m_postPass.updateDescriptors(m_hdr.view, m_gpos.view, m_offscreen.view);
    m_taaFirstFrame = true;
    m_taaHistoryIdx = 0;
    return true;
}

void App::handleResize()
{
    vkDeviceWaitIdle(m_ctx.device());
    glm::ivec2 fbs = m_window.framebufferSize();
    if (!m_swapchain.recreate(uint32_t(fbs.x), uint32_t(fbs.y)) ||
        !createOffscreen(m_swapchain.extent().width, m_swapchain.extent().height))
        spdlog::error("resize failed");
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
        const std::string manifestPath =
            std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
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
            m_svoPass.setWorld(g.chunkGrid, g.childBase, g.payload, g.handles, g.bricks);
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

    if (!createOffscreen(m_swapchain.extent().width, m_swapchain.extent().height))
        return false;

    // splat backend owns the primary view: build surfels from the live field
    m_renderMode = (m_args.mode == "svo") ? RenderMode::Svo : RenderMode::Splats;
    if (m_args.mode != "splat" && m_args.mode != "svo")
        spdlog::warn("--mode '{}' unknown (use splat|svo), defaulting to splat", m_args.mode);
    rebuildSurfels();

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

void App::applyWorldReload()
{
    if (!m_layers.loaded())
        return;
    // swap the freshly synthesized SVO buffers under an idle device
    vkDeviceWaitIdle(m_ctx.device());
    const auto& g = m_layers.gpu();
    m_svoPass.setWorld(g.chunkGrid, g.childBase, g.payload, g.handles, g.bricks);
    uploadTerrainTexture(); // layer toggles can change materials too
    uploadObjVolTexture();  // keep AI/object shadows in sync with the SVO
    rebuildSurfels();       // splat backend follows the same live field
    syncWorldLayerList();
    rescanWorldLayers(); // layers dropped into assets/ while running show up too
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
        m_ssrPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_ssrPass.record(cmd, W, H, push);
    }
    if ((m_renderFlags & (1 << 6)) != 0) {
        barrier();
        m_ssaoPass.updateDescriptors(m_gpos.view, m_offscreen.view, m_offscreen.view);
        m_ssaoPass.record(cmd, W, H, push);
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
    sp.sunDir = glm::vec3(m_sunDir);
    // micro-detail: texture texels as real micro-surfel geometry (moss,
    // pebbles, bark relief, leaflets). VF_MICRO=0 disables for perf/debug.
    sp.microDetail = true;
    if (const char* e = getenv("VF_MICRO"))
        sp.microDetail = atoi(e) != 0;
    // LOD rings: baked 2x2x2 / 4x4x4 merged-terrain surfel runs per chunk;
    // the renderer picks a ring per chunk by distance (VF_LOD1/VF_LOD2).
    sp.lodRings = true;
    if (const char* e = getenv("VF_LOD"))
        sp.lodRings = atoi(e) != 0;
    // debug/experiment overrides for the surfel bake (default = tuned values)
    if (const char* e = getenv("VF_SURFEL_SMOOTH"))
        sp.smoothNormals = atoi(e) != 0;
    if (const char* e = getenv("VF_SURFEL_HFBLEND"))
        sp.terrainHeightfieldNormals = atoi(e) != 0;
    vf::voxel::SurfelSet set = vf::voxel::buildSurfels(m_layers.field(), sp);
    std::vector<vf::voxel::Surfel> water =
        vf::voxel::buildWaterSurfels(m_layers.field());
    const uint32_t waterStart = uint32_t(set.surfels.size());
    // bucket water surfels per chunk (stable order => deterministic) so the
    // frame loop skips off-screen lake chunks instead of rasterizing the
    // whole water grid every frame
    std::vector<uint32_t> waterRange(16 * 16 * 16 + 1, waterStart);
    if (!water.empty()) {
        auto chunkOf = [](const vf::voxel::Surfel& s) {
            const float px = s.pos_rU.x, py = s.pos_rU.y, pz = s.pos_rU.z;
            const auto ax = std::clamp(int(std::floor((px + 51.2f) / 6.4f)), 0, 15);
            const auto ay = std::clamp(int(std::floor((py + 51.2f) / 6.4f)), 0, 15);
            const auto az = std::clamp(int(std::floor((pz + 51.2f) / 6.4f)), 0, 15);
            return uint32_t((ax * 16 + ay) * 16 + az);
        };
        std::vector<uint32_t> order(water.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(),
                         [&](uint32_t a, uint32_t b) {
                             return chunkOf(water[a]) < chunkOf(water[b]);
                         });
        std::vector<vf::voxel::Surfel> sorted;
        sorted.reserve(water.size());
        uint32_t open = chunkOf(water[order[0]]);
        waterRange[open] = waterStart;
        for (size_t k = 0; k < order.size(); ++k) {
            const uint32_t c = chunkOf(water[order[k]]);
            if (c != open) {
                for (uint32_t f = open + 1; f <= c; ++f)
                    waterRange[f] = waterStart + uint32_t(k);
                open = c;
            }
            sorted.push_back(water[order[k]]);
        }
        for (uint32_t f = open + 1; f < waterRange.size(); ++f)
            waterRange[f] = waterStart + uint32_t(order.size());
        water.swap(sorted);
    }
    set.surfels.insert(set.surfels.end(), water.begin(), water.end());
    // chunkRange only covers opaque surfels; waterRange buckets the trailing
    // water run per chunk for frustum-culled water draws; microStart splits
    // each chunk into base + micro-detail for distance culling
    m_splatPass.setSurfels(set.surfels.data(),
                           set.surfels.size() * sizeof(vf::voxel::Surfel),
                           set.surfels.size(), set.chunkRange, waterStart, waterRange,
                           set.microStart, set.lod1Range, set.lod2Range);
    spdlog::info("splat backend: {} surfels ({} water), {} chunks, lod1 {} lod2 {}",
                 set.surfels.size(), water.size(),
                 set.chunkRange.empty() ? 0 : set.chunkRange.size() - 1,
                 set.lod1Count, set.lod2Count);
}

void App::applyEdit()
{
    if (!m_hoverHit.hit)
        return;
    glm::vec3 n = m_hoverHit.normal;
    if (glm::length(n) < 1e-3f)
        n = glm::vec3(0.f, 1.f, 0.f);
    n = glm::normalize(n);
    const float radius = m_editDiameter * 0.5f;
    const float length = m_editDepth;

    if (m_editCarve) {
        // carve volume goes INTO the surface (along -normal) to cut a depression
        std::vector<vf::voxel::VoxelRecord> recs =
            m_carve.makeOrientedCylinder(m_hoverHit.voxel, -n, radius, length, m_editMat, /*carve=*/true);
        if (!recs.empty()) {
            m_carve.append(recs);
            spdlog::info("carve: {} voxels at {},{},{} d={:.1f} depth={:.1f}",
                         recs.size(), m_hoverHit.voxel.x, m_hoverHit.voxel.y, m_hoverHit.voxel.z,
                         m_editDiameter, m_editDepth);
        }
    } else {
        // add: raise a half-sphere bump ON the surface (along +normal). The dome
        // volume's top surface follows height(r)=depth*sqrt(1-(r/radius)^2), so the
        // terrain is lifted most at the centre and tapers to the rim.
        std::vector<vf::voxel::VoxelRecord> recs =
            m_add.makeDome(m_hoverHit.voxel, n, radius, length, m_editMat);
        if (!recs.empty()) {
            m_add.append(recs);
            spdlog::info("raise: {} voxels at {},{},{} d={:.1f} height={:.1f}",
                         recs.size(), m_hoverHit.voxel.x, m_hoverHit.voxel.y, m_hoverHit.voxel.z,
                         m_editDiameter, m_editDepth);
        }
    }
    requestWorldReload();
}

void App::persistWorldLayers()
{
    std::vector<vf::voxel::worldfile::WorldLayer> out;
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

void App::drawHud()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Voxelforge", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("GPU: %s", m_ctx.gpuName());
    ImGui::Text("Render: %s (F to switch), TAA: %s (N to switch)",
                m_renderMode == RenderMode::Splats ? "Gaussian surfels" : "chunked SVO",
                m_taaEnabled ? "on" : "off");
    ImGui::Text("GPU ms: geo %.1f | post %.1f | fx %.1f | taa %.1f | tail %.1f",
                m_profAvg[0], m_profAvg[1], m_profAvg[2], m_profAvg[3], m_profAvg[4]);
    if (m_renderMode == RenderMode::Splats)
        ImGui::Text("Splat size: %.2f ([ / ] to adjust)", m_splatPass.radiusScale());
    ImGui::Text("SSR: %s (G), SSAO: %s (H), Fog: %s (J), MotionBlur: %s (K), DoF: %s (L)",
                 (m_renderFlags & (1 << 5)) ? "on" : "off",
                 (m_renderFlags & (1 << 6)) ? "on" : "off",
                 m_volFogEnabled ? "on" : "off",
                 m_motionBlurEnabled ? "on" : "off",
                 m_dofEnabled ? "on" : "off");
    ImGui::Text("Keys: WASD/QE move, RMB+mouse look, wheel speed, Ctrl+LMB pick,");
    ImGui::Text("F toggle splat/SVO, N TAA on/off, [ / ] splat size, G/H/J/K/L photorealism,");
    ImGui::Text("T tonemap look, C edit tool, Esc quit");
    ImGui::Separator();

    {
        const glm::vec3 fwd = m_camera.forward();
        ImGui::Text("Viewer");
        ImGui::Indent();
        ImGui::Text("pos  x %6.1f  y %6.1f  z %6.1f", m_camera.pos.x,
                     m_camera.pos.y, m_camera.pos.z);
        // Heading in the world azimuth convention (sun/probe): +Z = 0 deg (N),
        // +X = 90 deg (E). So heading = atan2(forward.x, forward.z).
        const float heading = glm::degrees(std::atan2(fwd.x, fwd.z));
        const float hNorm = (heading < 0.0f) ? heading + 360.0f : heading;
        const char* card = (hNorm < 22.5f || hNorm >= 337.5f) ? "N" :
                           (hNorm < 67.5f) ? "NE" :
                           (hNorm < 112.5f) ? "E" :
                           (hNorm < 157.5f) ? "SE" :
                           (hNorm < 202.5f) ? "S" :
                           (hNorm < 247.5f) ? "SW" :
                           (hNorm < 292.5f) ? "W" : "NW";
        ImGui::Text("facing %5.1f deg %s", hNorm, card);
        ImGui::Text("look   x %6.2f  y %6.2f  z %6.2f", fwd.x, fwd.y, fwd.z);

        // Compact compass: North (+Z) up, East (+X) right, needle = heading.
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        const float R = 26.0f;
        const ImVec2 ctr = ImVec2(c0.x + R + 4.0f, c0.y + R + 4.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircle(ctr, R, IM_COL32(180, 200, 220, 180), 0, 1.5f);
        dl->AddText(ImVec2(ctr.x - 4.0f, ctr.y - R - 11.0f), IM_COL32(170, 200, 255, 255), "N");
        dl->AddText(ImVec2(ctr.x + R - 1.0f, ctr.y - 4.0f), IM_COL32(160, 200, 200, 255), "E");
        dl->AddText(ImVec2(ctr.x - 4.0f, ctr.y + R + 1.0f), IM_COL32(160, 200, 200, 255), "S");
        dl->AddText(ImVec2(ctr.x - R - 9.0f, ctr.y - 4.0f), IM_COL32(160, 200, 200, 255), "W");
        const float a = glm::radians(hNorm);
        const ImVec2 tip = ImVec2(ctr.x + std::sin(a) * (R - 4.0f),
                                  ctr.y - std::cos(a) * (R - 4.0f));
        dl->AddLine(ctr, tip, IM_COL32(255, 220, 120, 255), 2.0f);
        dl->AddCircleFilled(ctr, 2.0f, IM_COL32(255, 220, 120, 255));
        ImGui::Dummy(ImVec2((R + 4.0f) * 2.0f, (R + 4.0f) * 2.0f));
        ImGui::Unindent();
    }
    ImGui::Checkbox("Show controls", &m_showControls);
    if (m_showControls) {
        ImGui::Separator();
        ImGui::BulletText("WASD move, Q/E down/up");
        ImGui::BulletText("RMB hold: look");
        ImGui::BulletText("Wheel: speed, Shift/Ctrl boost/slow");
        ImGui::BulletText("ESC: quit");
        ImGui::BulletText("C: carve/add tool (LMB to stamp, +/- size)");
    }

    if (ImGui::CollapsingHeader("World layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_worldLayers.empty()) {
            ImGui::TextDisabled("no .vxw files found in assets/");
        } else {
            ImGui::TextDisabled("load .vxw content into the world:");
            for (auto& l : m_worldLayers) {
                const std::string id = "##layer_" + l.file;
                bool en = l.enabled;
                const bool isLandscape = l.role == "landscape";
                if (isLandscape)
                    ImGui::BeginDisabled(true);
                if (ImGui::Checkbox(id.c_str(), &en)) {
                    l.enabled = en;
                    l.listed = true; // every listed file joins the manifest
                    persistWorldLayers();
                    m_pendingWorldReload = true;
                }
                if (isLandscape)
                    ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(l.file.c_str());
                if (isLandscape) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[terrain]");
                } else if (l.file == vf::voxel::EditableWorld::kFileName) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[AI edits]");
                } else {
                    // import at the picked anchor instead of enabling the
                    // whole baked layer at its authored coordinates
                    ImGui::SameLine();
                    const std::string imp = "Import##imp_" + l.file;
                    if (!m_hasSelection)
                        ImGui::BeginDisabled(true);
                    if (ImGui::SmallButton(imp.c_str())) {
                        const std::string path =
                            std::string(VOXELFORGE_ASSET_DIR) + "/" + l.file;
                        if (m_editable.importLayer(path, m_selectedHit.voxel) > 0)
                            requestWorldReload();
                    }
                    if (!m_hasSelection) {
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Ctrl+LMB pick an anchor first");
                    } else if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("place a copy of this object at the selected voxel");
                    }
                }
            }
        }
        if (ImGui::Button("Rescan assets folder"))
            rescanWorldLayers();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu records live", m_layers.stats().records);
        ImGui::TextDisabled("Import copies an object to the picked voxel (Ctrl+LMB)");
        ImGui::TextDisabled("toggles & AI edits hot-reload live");
    }

    // carve / add edit tool panel (only while the tool is active)
    if (m_editActive) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 234.f, 12.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(220, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Carve / Add", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Carve / Add tool  ([C] to toggle)");
            if (ImGui::RadioButton("Carve", m_editCarve))
                m_editCarve = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Add", !m_editCarve))
                m_editCarve = false;
            ImGui::Separator();
            ImGui::SliderFloat("Diameter (m)", &m_editDiameter, 0.2f, 12.0f, "%.1f");
            if (ImGui::Button("-##diam")) m_editDiameter = std::max(0.2f, m_editDiameter - 0.2f);
            ImGui::SameLine();
            if (ImGui::Button("+##diam")) m_editDiameter = std::min(12.0f, m_editDiameter + 0.2f);
            ImGui::SliderFloat("Depth (m)", &m_editDepth, 0.2f, 12.0f, "%.1f");
            if (ImGui::Button("-##depth")) m_editDepth = std::max(0.2f, m_editDepth - 0.2f);
            ImGui::SameLine();
            if (ImGui::Button("+##depth")) m_editDepth = std::min(12.0f, m_editDepth + 0.2f);
            ImGui::Separator();
            ImGui::Text("LMB on terrain to %s", m_editCarve ? "carve a hole" : "raise a dome");
            ImGui::TextDisabled("Shift + / - adjust depth");
            ImGui::TextDisabled("Ctrl+LMB: set import anchor");
            if (ImGui::Button("Clear carve edits")) {
                m_carve.clear();
                requestWorldReload();
            }
            if (ImGui::Button("Clear raise edits")) {
                m_add.clear();
                requestWorldReload();
            }
        }
        ImGui::End();
    }

    if (m_scenePreview) {
        if (!m_sceneTexId)
            m_sceneTexId = (ImTextureID)ImGui_ImplVulkan_AddTexture(m_uiSampler, m_offscreen.view,
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ImGui::Separator();
        ImGui::Text("Scene preview");
        ImGui::Image(m_sceneTexId, ImVec2(240.0f, 135.0f));
    }
    ImGui::End();

    // AI Chat (pass picking state; edits trigger an immediate world reload)
    {
        auto reloadFn = [this](){ this->requestWorldReload(); };
        m_chatUi.draw(m_editable, m_layers, m_hoverHit.hit ? &m_hoverHit : nullptr,
                      m_hasSelection ? &m_selectedHit : nullptr, m_hasSelection, reloadFn);
        // selected voxel feedback via ImGui foreground text at screen center
        if (m_hasSelection) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            dl->AddText(ImVec2(center.x - 40, center.y + 20), IM_COL32(80,255,80,220), "● selected");
        }
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

    if (geoRatio < 0.03f || !skyOk || geoRatio > 0.97f) {
        spdlog::error("selftest FAILED");
        return false;
    }
    spdlog::info("selftest PASSED");
    return true;
}

int App::run(const Args& args)
{
    {
        const float e = glm::radians(args.sunElev), a = glm::radians(args.sunAzim);
        m_sunDir = glm::vec4(
            glm::normalize(glm::vec3(cosf(e) * sinf(a), sinf(e), cosf(e) * cosf(a))), 0.0f);
    }
    m_animTime = args.animTime;
    m_tonemapLook = args.tonemap;
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
        spdlog::info("probe({:.2f},{:.2f},{:.2f}): d={:+.3f} mat={} {}", args.probe.x,
                     args.probe.y, args.probe.z, s.d, int(s.mat),
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

    // ImGui ---------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
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

    if (args.camSet) {
        m_camera.pos = { args.camx, args.camy, args.camz };
        glm::vec3 dir = glm::normalize(glm::vec3(args.tx, args.ty, args.tz) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
    }

    const bool shotMode = !args.shot.empty();
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
    // TAA history blend override (debug): 1.0 = pure reprojected history.
    // In a static scene that must stay coherent while rotating (1 frame of
    // lag); if it tears instead, the G-buffer reprojection itself is broken.
    if (const char* e = getenv("VF_TAA_BLEND"))
        m_taaBlend = std::clamp(float(atof(e)), 0.0f, 1.0f);

    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        if (m_window.keyPressed(GLFW_KEY_ESCAPE))
            glfwSetWindowShouldClose(m_window.handle(), GLFW_TRUE);

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
            args.selftest || args.smokeFrames > 0 || !args.shot.empty();
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

        // Voxel picking: Ctrl+LMB
        {
            ImGuiIO& pickIo = ImGui::GetIO();
            bool wantMouse = pickIo.WantCaptureMouse;
            bool ctrl = glfwGetKey(m_window.handle(), GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS ||
                        glfwGetKey(m_window.handle(), GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
            double mx=0,my=0;
            glfwGetCursorPos(m_window.handle(), &mx, &my);
            glm::ivec2 fb = m_window.framebufferSize();
            bool lmb = glfwGetMouseButton(m_window.handle(), GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
            // hover while Ctrl held (anchor pick) OR while the edit tool is active
            // (so an LMB click can stamp a carve/add at the pointed surface)
            bool computeHover = (ctrl || m_editActive) && !wantMouse && fb.x>0 && fb.y>0;
            if (computeHover) {
                float tanHalf = tanHalfFov;
                float aspect = float(fb.x)/float(fb.y);
                glm::vec3 rd = vf::voxel::screenRayDir(mx,my,fb.x,fb.y,tanHalf,aspect,
                                                       m_camera.forward(), m_camera.right(), m_camera.up());
                m_hoverHit = vf::voxel::rayPick(m_layers.field(), m_camera.pos, rd);
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
                glm::vec3 w = vf::voxel::voxelCenter(m_selectedHit.voxel);
                spdlog::info("pick selected {} {} {} world {:.2f} {:.2f} {:.2f} mat {}",
                    m_selectedHit.voxel.x, m_selectedHit.voxel.y, m_selectedHit.voxel.z, w.x,w.y,w.z, int(m_selectedHit.mat));
            }
            // edit tool: plain LMB click (no Ctrl) stamps a carve/add at the hover point
            bool justPressedApply = lmbEdge && !ctrl && !wantMouse && m_editActive;
            if (justPressedApply)
                applyEdit();
        }

        static const char* testHov = getenv("VF_TEST_HOVER");
        if (testHov && *testHov && !m_hoverHit.hit && m_layers.loaded()) {
            glm::ivec3 v;
            if (sscanf(testHov, "%d,%d,%d", &v.x, &v.y, &v.z) == 3) {
                m_hoverHit = {};
                m_hoverHit.hit = true;
                m_hoverHit.voxel = v;
                spdlog::info("VF_TEST_HOVER {} {} {}", v.x, v.y, v.z);
            }
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

        // camera: skip WASD when chat input focused
        bool chatCaptures = m_chatInitialized && m_chatUi.wantsCaptureKeyboard();
        if (chatCaptures) {
            // block camera move while typing; consume mouse delta to avoid jump
            double _dx,_dy; m_window.getMouseDelta(_dx,_dy);
        } else {
            m_camera.update(m_window, dt);
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
                spdlog::info("edit tool -> {}", m_editActive ? "active" : "off");
            }
            if (m_editActive) {
                // + / - change diameter; Shift + / - change depth
                if (edge(GLFW_KEY_EQUAL, hw) || edge(GLFW_KEY_KP_ADD, hw)) {
                    if (shift) m_editDepth = std::min(m_editDepth + 0.2f, 12.0f);
                    else       m_editDiameter = std::min(m_editDiameter + 0.2f, 12.0f);
                }
                if (edge(GLFW_KEY_MINUS, hw) || edge(GLFW_KEY_KP_SUBTRACT, hw)) {
                    if (shift) m_editDepth = std::max(m_editDepth - 0.2f, 0.2f);
                    else       m_editDiameter = std::max(m_editDiameter - 0.2f, 0.2f);
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
                FILE* fp = fopen(args.shot.c_str(), "wb");
                if (fp) {
                    fprintf(fp, "P6\n%u %u\n255\n", m_offscreen.extent.width,
                            m_offscreen.extent.height);
                    for (size_t i = 0; i < px.size(); i += 4)
                        fwrite(&px[i], 3, 1, fp);
                    fclose(fp);
                    spdlog::info("shot written: {}", args.shot);
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
        m_svoPass.record(fr.cmd, push);
        }
        profMark(1);
        // barrier: HDR/G-buffer written -> read by post pass
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        if (m_renderMode == RenderMode::Splats) {
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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
    vf::destroyImage3D(m_ctx, m_taaHistory[0]);
    vf::destroyImage3D(m_ctx, m_taaHistory[1]);
    vf::destroyImage3D(m_ctx, m_taaResolved);
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
