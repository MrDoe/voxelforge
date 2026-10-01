// Window, swapchain, the offscreen render targets, and device bring-up /
// teardown. The teardown order here is load-bearing (device idle before any
// object is destroyed), so it lives in one file rather than next to whoever
// last added a member.
//
// The ImGui backend appears here because the offscreen targets double as the
// HUD's live scene preview: a resize must drop the old texture handle before
// the image is recreated.
#include "app/app.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

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

} // namespace app
} // namespace vf
