// Voxelforge M0/M1 - window, dense-volume ray marcher, HUD.
#include "core/camera.hpp"
#include "core/log.hpp"
#include "platform/window.hpp"
#include "rhi/swapchain.hpp"
#include "render/raymarch_pass.hpp"
#include "render/splat_pass.hpp"
#include "render/svo_pass.hpp"
#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/volume.hpp"
#include <algorithm>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class RenderMode { VoxelRaymarch, GaussianSplats };

struct Args {
    bool selftest = false;
    bool compare = false;
    int smokeFrames = 0;
    int width = 1600, height = 900;
    std::string mode;    // "voxel" | "splat"
    std::string backend; // "svo" | "dense"
    std::string shot;    // dump one frame to PPM and exit
    float camx=0,camy=0,camz=0,tx=0,ty=0,tz=0;
    bool camSet=false;
    float sunElev=34.0f, sunAzim=238.0f; // golden-hour: long visible shadows
    bool sunSet=false;
    float splatScale=1.0f;
    bool probeSet=false;
    glm::vec3 probe { 0.f };
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
        else if (s == "--mode" && i + 1 < argc)
            a.mode = argv[++i];
        else if (s == "--backend" && i + 1 < argc)
            a.backend = argv[++i];
        else if (s == "--compare")
            a.compare = true;
        else if (s == "--shot" && i + 1 < argc)
            a.shot = argv[++i];
        else if (s == "--cam" && i + 6 < argc) {
            a.camx = atof(argv[++i]); a.camy = atof(argv[++i]); a.camz = atof(argv[++i]);
            a.tx = atof(argv[++i]); a.ty = atof(argv[++i]); a.tz = atof(argv[++i]);
            a.camSet = true;
        } else if (s == "--sun" && i + 2 < argc) {
            a.sunElev = atof(argv[++i]); a.sunAzim = atof(argv[++i]);
            a.sunSet = true;
        } else if (s == "--splatscale" && i + 1 < argc) {
            a.splatScale = atof(argv[++i]);
        } else if (s == "--probe" && i + 3 < argc) {
            a.probe = { float(atof(argv[i + 1])), float(atof(argv[i + 2])),
                        float(atof(argv[i + 3])) };
            i += 3;
            a.probeSet = true;
        }
    }
    return a;
}

constexpr uint32_t kMaxFramesInFlight = 2;

struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderDone = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
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
    bool runSelftest();
    bool runCompare();

    Args m_args;
    vf::Window m_window;
    vf::Context m_ctx;
    vf::Swapchain m_swapchain;

    vf::Image3D m_sdfVol, m_albedoVol, m_offscreen;
    vf::Image3D m_heightImg;
    VkSampler m_sdfSampler = VK_NULL_HANDLE, m_albedoSampler = VK_NULL_HANDLE;
    vf::RaymarchPass m_pass;
    vf::SvoPass m_svoPass;
    vf::SplatPass m_splatPass;
    enum class Backend { Svo, Dense };
    Backend m_backend = Backend::Svo;
    bool m_compareMode = false;
    glm::vec4 m_pushB { 1.0f };      // active backend's .b block
    glm::vec4 m_pushBSvo { 1.0f };   // cached per backend
    glm::vec4 m_pushBDense { 1.0f };

    glm::vec4 pushBFor(Backend b) const { return b == Backend::Svo ? m_pushBSvo : m_pushBDense; }

    // Direction TOWARD the sun, derived from --sun elevation/azimuth (degrees).
    glm::vec4 m_sunDir { 0.449f, 0.8338f, 0.3207f, 0.0f };

    VkCommandPool m_framePool = VK_NULL_HANDLE;
    std::vector<FrameSync> m_frames;
    std::vector<VkSemaphore> m_acquireSems; // one per swapchain image

    vf::Camera m_camera;
    RenderMode m_renderMode = RenderMode::VoxelRaymarch;
    float m_lastFrameMs = 16.7f;
    double m_avgMs = 16.7f;
    float m_minMs = 1e9f, m_maxMs = 0.0f;
    uint64_t m_frameIdx = 0;
    bool m_showControls = true;
    bool m_fWasDown = false;
    float m_splatScale = 1.0f;    uint32_t m_nextAcquire = 0;
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
    m_offscreen = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!m_offscreen.img)
        return false;
    m_pass.updateDescriptors(m_sdfVol, m_sdfSampler, m_albedoVol, m_albedoSampler,
                             m_offscreen);
    m_svoPass.updateDescriptors(m_offscreen);
    m_splatPass.setOutputView(m_offscreen.view);
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

    // Build compute pipeline before any window-system interaction.
    if (!m_pass.init(m_ctx))
        return false;

    glm::ivec2 fb = m_window.framebufferSize();
    vf::PresentPolicy policy = (m_args.selftest || m_args.smokeFrames > 0)
                                   ? vf::PresentPolicy::Immediate
                                   : vf::PresentPolicy::PreferMailbox;
    if (const char* pm = getenv("VF_PRESENT")) {
        // manual override for testing WSI paths
        policy = strcmp(pm, "immediate") == 0 ? vf::PresentPolicy::Immediate
                                              : vf::PresentPolicy::PreferMailbox;
    }
    if (!m_swapchain.init(m_ctx, uint32_t(fb.x), uint32_t(fb.y), policy))
        return false;

    // terrain heightmap -> R32F storage image, kept in GENERAL for shader reads
    {
        const vf::voxel::HeightMap& hm = vf::voxel::sharedHeightmap();
        m_heightImg = vf::makeImage3D(m_ctx, hm.width(), hm.height(), 1,
                                      VK_FORMAT_R32_SFLOAT,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_heightImg.img)
            return false;
        if (!vf::uploadToImage3D(m_ctx, m_heightImg, hm.data(), hm.bytes()))
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
        m_pass.setHeightmapView(m_heightImg.view);
        m_svoPass.setHeightmapView(m_heightImg.view);
    }

    // volume -----------------------------------------------------------
    const bool useDense = m_backend == Backend::Dense;
    const bool buildDense = useDense || m_compareMode;
    const bool buildSvo = !useDense || m_compareMode;
    const bool denseIsPrimary = useDense;

    vf::SplatVertexData sd;

    if (buildDense) {
        // Full-world reference volume so the dense backend renders the same
        // scene as the SVO backend (same voxel count as the old 25.6 m patch,
        // just at 0.4 m over the entire WORLD span).
        vf::voxel::DenseVolume vol;
        vol.worldSize = vf::voxel::WORLD;
        vol.generate();

        std::vector<uint8_t> sdfBytes(vol.sdf.size());
        for (size_t i = 0; i < vol.sdf.size(); ++i)
            sdfBytes[i] = vf::voxel::encodeSnorm8(vol.sdf[i]);

        const size_t voxels = vol.sdf.size();
        std::vector<uint8_t> albedoRGBA(voxels * 4);
        for (size_t i = 0; i < voxels; ++i) {
            albedoRGBA[i * 4 + 0] = vol.albedo[i * 3 + 0];
            albedoRGBA[i * 4 + 1] = vol.albedo[i * 3 + 1];
            albedoRGBA[i * 4 + 2] = vol.albedo[i * 3 + 2];
            albedoRGBA[i * 4 + 3] = 255;
        }

        m_sdfVol = vf::makeImage3D(m_ctx, uint32_t(vol.n), uint32_t(vol.n), uint32_t(vol.n),
                                   VK_FORMAT_R8_SNORM,
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_albedoVol = vf::makeImage3D(m_ctx, uint32_t(vol.n), uint32_t(vol.n),
                                      uint32_t(vol.n), VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!m_sdfVol.img || !m_albedoVol.img) {
            spdlog::critical("volume image creation failed");
            return false;
        }
        if (!vf::uploadToImage3D(m_ctx, m_sdfVol, sdfBytes.data(), sdfBytes.size()) ||
            !vf::uploadToImage3D(m_ctx, m_albedoVol, albedoRGBA.data(), albedoRGBA.size())) {
            spdlog::critical("volume upload failed");
            return false;
        }
        spdlog::info("Dense volume {}^3 uploaded ({:.1f} m region)", vol.n, vol.worldSize);

        m_pushBDense = glm::vec4(vol.worldSize, vf::voxel::MAX_ENCODED_DIST,
                                 vol.worldSize / float(vol.n), 0);

        // surface splats straight from the dense field
        const int n = vol.n;
        for (int z = 1; z < n - 1; ++z)
            for (int y = 1; y < n - 1; ++y)
                for (int x = 1; x < n - 1; ++x) {
                    size_t i = (size_t(z) * n + y) * n + x;
                    if (std::abs(vol.sdf[i]) > 0.15f)
                        continue;
                    glm::vec3 p((x + 0.5f) / n * vol.worldSize - 0.5f * vol.worldSize,
                                (y + 0.5f) / n * vol.worldSize - 0.5f * vol.worldSize,
                                (z + 0.5f) / n * vol.worldSize - 0.5f * vol.worldSize);
                    const glm::vec3& c = vf::voxel::kPalette[vol.matId[i]];
                    if (denseIsPrimary) {
                        sd.posRadius.emplace_back(p + vol.originOffset, 0.145f);
                        sd.colors.emplace_back(c, 1.0f);
                    }
                }
    }

    if (buildSvo) {
        // canonical world asset first; procedural build only as fallback
        vf::voxel::WorldFileData wf;
        const std::string worldPath =
            std::string(VOXELFORGE_ASSET_DIR) + "/world.vxw";
        bool fromFile = vf::voxel::worldfile::read(worldPath, wf) &&
                        wf.meta.worldSize == vf::voxel::WORLD &&
                        wf.meta.voxelSize == vf::voxel::VOXEL &&
                        wf.meta.gridN == uint32_t(vf::voxel::GRID_N);
        size_t nodes = 0, bricks = 0, activeChunks = 0;
        double mb = 0.0;
        if (fromFile) {
            nodes = wf.payload.size();
            bricks = wf.bricks.size() / (vf::voxel::BRICK_WORDS);
            activeChunks = size_t(std::count_if(wf.chunkGrid.begin(), wf.chunkGrid.end(),
                                                [](int32_t h) { return h >= 0; }));
            mb = double((wf.childBase.size() + wf.payload.size() + wf.handles.size() +
                         wf.bricks.size() + wf.chunkGrid.size()) *
                        4) /
                 (1024.0 * 1024.0);
            spdlog::info("SVO world loaded from {}: {} nodes, {} bricks, {}/{} chunks,"
                         " {:.1f} MB",
                         worldPath, nodes, bricks, activeChunks,
                         vf::voxel::GRID_N * vf::voxel::GRID_N * vf::voxel::GRID_N, mb);
            if (!m_svoPass.init(m_ctx))
                return false;
            m_svoPass.setWorld(wf.chunkGrid, wf.childBase, wf.payload, wf.handles,
                               wf.bricks);
        } else {
            vf::voxel::World world;
            world.build();
            auto st = world.stats();
            spdlog::info("SVO world built in {:.2f}s: {} nodes, {} bricks, {}/{} chunks"
                         " active, {:.1f} MB",
                         st.buildSeconds, st.nodes, st.bricks, st.activeChunks,
                         vf::voxel::GRID_N * vf::voxel::GRID_N * vf::voxel::GRID_N,
                         world.gpu().memoryBytes() / (1024.0 * 1024.0));
            if (!m_svoPass.init(m_ctx))
                return false;
            const auto& g = world.gpu();
            m_svoPass.setWorld(g.chunkGrid, g.childBase, g.payload, g.handles, g.bricks);
        }

        m_pushBSvo = glm::vec4(vf::voxel::WORLD, vf::voxel::VOXEL, float(vf::voxel::GRID_N), 0);

        // derive preview splats on a ~0.3 m lattice over the terrain surface;
        // radius derives from lattice spacing so adjacent splats overlap even
        // across slopes; colors bake the same direct+ambient terms as the
        // raymarch shading so both modes match in brightness.
        // Source: world.vxw surface records when available, else the lattice.
        const float ext = 30.0f;
        const float spacing = 0.31f;
        const float splatRadius = spacing * 1.25f;
        const glm::vec3 sunCol = glm::vec3(1.0f, 0.95f, 0.84f) * 2.7f;
        const glm::vec3 ambBase = (glm::vec3(0.72f, 0.80f, 0.90f) +
                                   glm::vec3(0.20f, 0.36f, 0.62f)) *
                                  0.5f * 0.55f;
        const vf::voxel::HeightMap& hm = vf::voxel::sharedHeightmap();
        auto addSplat = [&](const glm::vec3& p, const glm::vec3& albedo) {
            if (!denseIsPrimary) {
                glm::vec2 g = hm.gradient(p.x, p.z);
                glm::vec3 n = glm::normalize(glm::vec3(-g.x, 1.0f, -g.y));
                float ndl = glm::max(glm::dot(n, glm::vec3(m_sunDir)), 0.0f);
                glm::vec3 amb = ambBase * (n.y * 0.5f + 0.5f);
                sd.posRadius.emplace_back(p, splatRadius);
                sd.colors.emplace_back(albedo * (sunCol * ndl + amb), 1.0f);
            }
        };
        if (fromFile && !wf.voxels.empty()) {
            for (const vf::voxel::VoxelRecord& v : wf.voxels) {
                if ((v.x % 3u) != 1u || (v.z % 3u) != 1u)
                    continue; // decimate 0.1 m records to ~0.3 m lattice
                glm::vec3 p = v.position(wf.meta);
                if (std::abs(p.x) > ext || std::abs(p.z) > ext || p.y < -8.f || p.y > 24.f)
                    continue;
                addSplat(p, glm::vec3(v.r, v.g, v.b) / 255.0f);
            }
        } else {
            const int SN = 192;
            for (int zi = 0; zi < SN; ++zi)
                for (int yi = 0; yi < SN; ++yi)
                    for (int xi = 0; xi < SN; ++xi) {
                        glm::vec3 p(glm::mix(-ext, ext, (xi + 0.5f) / SN),
                                    glm::mix(-8.f, 24.f, (yi + 0.5f) / SN),
                                    glm::mix(-ext, ext, (zi + 0.5f) / SN));
                        if (std::abs(p.y - hm.sample(p.x, p.z)) > 0.22f)
                            continue;
                        addSplat(p, vf::voxel::kPalette[vf::voxel::scene(p).mat]);
                    }
        }
    }

    m_pushB = pushBFor(m_backend);

    if (!sd.posRadius.empty()) {
        spdlog::info("Derived {} surface splats", sd.posRadius.size());
        if (!m_splatPass.init(m_ctx, VK_FORMAT_R8G8B8A8_UNORM, sd))
            return false;
    }

    m_sdfSampler = vf::makeSampler(m_ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    m_albedoSampler = vf::makeSampler(m_ctx, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);


    if (!createOffscreen(m_swapchain.extent().width, m_swapchain.extent().height))
        return false;

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
    return true;
}

void App::drawHud()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Voxelforge", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("GPU: %s", m_ctx.gpuName());
    ImGui::Text("Backend: %s", m_backend == Backend::Svo ? "chunked SVO" : "dense reference");
    ImGui::Text("%u x %u @ %.1f fps (%.2f ms)", m_swapchain.extent().width,
                m_swapchain.extent().height, 1000.0 / m_avgMs, m_avgMs);
    ImGui::Separator();

    const char* modeName = m_renderMode == RenderMode::VoxelRaymarch
                               ? "Voxel ray-march (dense M1)"
                               : "Gaussian splats [surface preview]";
    ImGui::Text("Mode: %s", modeName);
    if (m_renderMode == RenderMode::GaussianSplats) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
        ImGui::TextWrapped("Preview: isotropic surface point-splats (unsorted). True 3DGS lands in M7.");
        ImGui::PopStyleColor();
        ImGui::Text("Splat scale x%.2f  [+ / -]", m_splatScale);
    }

    ImGui::Text("Cam  %.1f %.1f %.1f", m_camera.pos.x, m_camera.pos.y, m_camera.pos.z);
    ImGui::Text("Yaw/pitch %.0f/%.0f  speed %.1f", glm::degrees(m_camera.yaw),
                glm::degrees(m_camera.pitch), m_camera.speed);
    ImGui::Checkbox("Show controls", &m_showControls);
    if (m_showControls) {
        ImGui::Separator();
        ImGui::BulletText("WASD move, Q/E down/up");
        ImGui::BulletText("RMB hold: look");
        ImGui::BulletText("Wheel: speed, Shift/Ctrl boost/slow");
        ImGui::BulletText("F: toggle voxel/splat render");
        ImGui::BulletText("ESC: quit");
    }
    ImGui::End();

    if (m_renderMode == RenderMode::GaussianSplats) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.085f);
        dl->AddText(ImVec2(c.x - 190, c.y), IM_COL32(255, 200, 120, 220),
                    "[F] Splat preview - true 3DGS in M7");
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

    const bool splatMode = m_renderMode == RenderMode::GaussianSplats;
    spdlog::info("selftest[{}]: geometry coverage {:.1f}%, sky probe ({},{},{})",
                 splatMode ? "splat" : "voxel", geoRatio * 100.0f, tr, tg, tb);
    // region diagnostics: 3x3 grid average colors
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            uint64_t r = 0, g = 0, b = 0;
            uint32_t x0 = uint32_t(gx) * W / 3, x1 = uint32_t(gx + 1) * W / 3;
            uint32_t y0 = uint32_t(gy) * H / 3, y1 = uint32_t(gy + 1) * H / 3;
            for (uint32_t y = y0; y < y1; y += 4)
                for (uint32_t x = x0; x < x1; x += 4) {
                    size_t i = (size_t(y) * W + x) * 4;
                    r += pixels[i];
                    g += pixels[i + 1];
                    b += pixels[i + 2];
                }
            size_t n = size_t(((x1 - x0) / 4) + 1) * (((y1 - y0) / 4) + 1);
            fprintf(stderr, "[grid %d,%d] avg (%u,%u,%u)\n", gx, gy,
                    unsigned(r / n), unsigned(g / n), unsigned(b / n));
        }
    }
    if (splatMode) {
        // Soft gaussian sprites blend over the whole frame: expect bluish sky
        // probe plus substantial green-tinted terrain influence.
        size_t sampled = 0, greenish = 0;
        for (size_t p = 0; p < total; p += 7) {
            ++sampled;
            if (pixels[p * 4 + 1] > pixels[p * 4])
                ++greenish;
        }
        float greenRatio = float(greenish) / float(sampled);
        spdlog::info("selftest[splat]: greenish ratio {:.1f}%", greenRatio * 100.0f);
        if (!skyOk || greenRatio < 0.15f) {
            spdlog::error("selftest FAILED");
            return false;
        }
        spdlog::info("selftest PASSED");
        return true;
    }

    if (geoRatio < 0.03f || !skyOk || geoRatio > 0.97f) {
        spdlog::error("selftest FAILED");
        return false;
    }
    spdlog::info("selftest PASSED");
    return true;
}


bool App::runCompare()
{
    // Render both backends headless with identical camera/push and compare.
    vkDeviceWaitIdle(m_ctx.device());
    auto renderPixels = [&](Backend be) {
        m_backend = be;
        createOffscreen(m_offscreen.extent.width, m_offscreen.extent.height);
        uint32_t f = 0;
        FrameSync& fr = m_frames[f];
        vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(m_ctx.device(), 1, &fr.inFlight);
        vkResetCommandBuffer(fr.cmd, 0);
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fr.cmd, &bi);
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vf::RaymarchPush push {};
        push.camPos = glm::vec4(m_camera.pos, 0);
        push.camRight = glm::vec4(m_camera.right(), 0);
        push.camUp = glm::vec4(m_camera.up(), 0);
        push.camFwd = glm::vec4(m_camera.forward(), 0);
        push.a = glm::vec4(tanf(glm::radians(60.0f) * 0.5f),
                           float(m_offscreen.extent.width) / float(m_offscreen.extent.height),
                           float(m_offscreen.extent.width), float(m_offscreen.extent.height));
        push.b = glm::vec4(pushBFor(be).x, pushBFor(be).y, pushBFor(be).z, 0);
        push.sunDir = m_sunDir;
        push.misc = glm::vec4(m_splatScale, 0.0f, 0.0f, 0.0f);
        if (be == Backend::Svo)
            m_svoPass.record(fr.cmd, push);
        else
            m_pass.record(fr.cmd, push);
        vkEndCommandBuffer(fr.cmd);
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &fr.cmd;
        vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, fr.inFlight);
        vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

        std::vector<uint8_t> px;
        vf::readbackImage2D(m_ctx, m_offscreen.img, m_offscreen.extent.width,
                            m_offscreen.extent.height, px);
        return px;
    };

    std::vector<uint8_t> svoPx = renderPixels(Backend::Svo);
    std::vector<uint8_t> densePx = renderPixels(Backend::Dense);

    auto dump = [&](const char* path, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
        FILE* f = fopen(path, "wb");
        if (!f)
            return;
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < px.size(); i += 4)
            fwrite(&px[i], 3, 1, f);
        fclose(f);
    };
    if (getenv("VF_DUMP_PPM")) {
        dump("/tmp/opencode/cmp_svo.ppm", svoPx, m_offscreen.extent.width,
             m_offscreen.extent.height);
        dump("/tmp/opencode/cmp_dense.ppm", densePx, m_offscreen.extent.width,
             m_offscreen.extent.height);
    }

    size_t total = svoPx.size() / 4;
    double diff = 0.0;
    for (size_t i = 0; i < total; ++i)
        for (int c = 0; c < 3; ++c)
            diff += std::abs(int(svoPx[i * 4 + c]) - int(densePx[i * 4 + c]));
    double meanDiff = diff / double(total * 3);

    // coverage parity (non-sky fraction per backend)
    auto coverage = [&](const std::vector<uint8_t>& px) {
        size_t geo = 0;
        for (size_t i = 0; i < px.size(); i += 4) {
            uint8_t r = px[i], g = px[i + 1], b = px[i + 2];
            if (!(b > r + 12 && g > r + 4 && b > 120))
                ++geo;
        }
        return float(geo) / float(px.size() / 4);
    };
    float covSvo = coverage(svoPx), covDense = coverage(densePx);
    spdlog::info("compare: mean abs channel diff = {:.2f}/255, coverage svo={:.1f}% dense={:.1f}%",
                 meanDiff, covSvo * 100.0f, covDense * 100.0f);

    // restore primary backend descriptors on offscreen
    createOffscreen(m_offscreen.extent.width, m_offscreen.extent.height);

    const bool ok = meanDiff < 32.0 && std::abs(covSvo - covDense) < 0.15f;
    spdlog::info("compare {}", ok ? "PASSED" : "FAILED");
    return ok;
}

int App::run(const Args& args)
{
    {
        const float e = glm::radians(args.sunElev), a = glm::radians(args.sunAzim);
        m_sunDir = glm::vec4(
            glm::normalize(glm::vec3(cosf(e) * sinf(a), sinf(e), cosf(e) * cosf(a))), 0.0f);
    }
    m_splatScale = std::clamp(args.splatScale, 0.25f, 4.0f);
    if (args.probeSet) {
        auto s = vf::voxel::scene(args.probe);
        spdlog::info("probe({:.2f},{:.2f},{:.2f}): d={:+.3f} mat={} ({})", args.probe.x,
                     args.probe.y, args.probe.z, s.d, int(s.mat),
                     int(vf::voxel::kPalette.size()) > int(s.mat) ? "ok" : "OOR");
        return 0;
    }
    if (!vf::voxel::sharedHeightmap().loaded()) {
        spdlog::critical("assets/heightmap.png missing - build & run heightmap_gen first");
        return 1;
    }
    if (!initWindow(args)) {
        spdlog::critical("window init failed");
        return 1;
    }
    // Resolve backend BEFORE initVulkan builds GPU resources.
    m_backend = args.backend == "dense" ? Backend::Dense : Backend::Svo;
    m_compareMode = args.compare;
    if (args.mode == "splat")
        m_renderMode = RenderMode::GaussianSplats;
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
    vi.DescriptorPoolSize = 64;
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

    // per-backend camera spawn
    if (args.compare) {
        // steep downward view: both backends must see terrain-only (the dense
        // reference volume is a bounded region without sky)
        m_camera.pos = { -3.f, 8.f, -5.f };
        glm::vec3 dir = glm::normalize(glm::vec3(0.f, -1.5f, 1.f) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
        m_camera.speed = 6.0f;
    } else if (m_backend == Backend::Dense) {
        m_camera.pos = { 0.f, 10.f, -14.f };
        glm::vec3 dir = glm::normalize(glm::vec3(0.f, 3.5f, 3.f) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
        m_camera.speed = 6.0f;
    } else {
        // hero shot: across the river toward the cabin, sun raking from the west
        m_camera.pos = { -16.f, 6.5f, -14.f };
        glm::vec3 dir = glm::normalize(glm::vec3(6.5f, 0.8f, 11.0f) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
    }

    if (args.camSet) {
        m_camera.pos = { args.camx, args.camy, args.camz };
        glm::vec3 dir = glm::normalize(glm::vec3(args.tx, args.ty, args.tz) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
    }

    const bool shotMode = !args.shot.empty();
    const float tanHalfFov = tanf(glm::radians(60.0f) * 0.5f);
    auto last = std::chrono::steady_clock::now();

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

        if (glfwGetKey(m_window.handle(), GLFW_KEY_F) == GLFW_PRESS) {
            if (!m_fWasDown) {
                m_renderMode = m_renderMode == RenderMode::VoxelRaymarch
                                   ? RenderMode::GaussianSplats
                                   : RenderMode::VoxelRaymarch;
                spdlog::info("Render mode -> {}",
                             m_renderMode == RenderMode::VoxelRaymarch ? "voxel ray-march"
                                                                       : "gaussian splats");
            }
            m_fWasDown = true;
        } else {
            m_fWasDown = false;
        }

        // +/- adjust the global splat size multiplier (hold to slide)
        {
            GLFWwindow* win = m_window.handle();
            bool up = glfwGetKey(win, GLFW_KEY_EQUAL) == GLFW_PRESS ||
                      glfwGetKey(win, GLFW_KEY_KP_ADD) == GLFW_PRESS;
            bool down = glfwGetKey(win, GLFW_KEY_MINUS) == GLFW_PRESS ||
                        glfwGetKey(win, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS;
            float prev = m_splatScale;
            if (up)
                m_splatScale *= std::exp2(1.6f * dt);
            if (down)
                m_splatScale *= std::exp2(-1.6f * dt);
            m_splatScale = std::clamp(m_splatScale, 0.25f, 4.0f);
            if (m_splatScale != prev && m_renderMode == RenderMode::GaussianSplats)
                spdlog::info("splat scale x{:.2f}", m_splatScale);
        }

        if (m_window.resized()) {
            m_window.clearResized();
            handleResize();
        }

        m_camera.update(m_window, dt);

        uint32_t f = m_frameIdx % kMaxFramesInFlight;
        FrameSync& fr = m_frames[f];
        vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

        const bool headlessRun = args.selftest || args.compare || args.smokeFrames > 0 || !args.shot.empty();
        if (headlessRun) {
            // Automated mode: zero window-system interaction.
            vkResetFences(m_ctx.device(), 1, &fr.inFlight);
            vkResetCommandBuffer(fr.cmd, 0);
            VkCommandBufferBeginInfo hbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(fr.cmd, &hbi);

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
            push.misc = glm::vec4(m_splatScale, 0.0f, 0.0f, 0.0f);
            const bool hSplat = m_renderMode == RenderMode::GaussianSplats && m_splatPass.count() > 0;
            if (hSplat) {
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                m_splatPass.record(fr.cmd,
                                   { m_offscreen.extent.width, m_offscreen.extent.height }, push);
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT |
                                        VK_ACCESS_2_MEMORY_READ_BIT);
            } else {
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                static bool dbgOnce = false;
                if (!dbgOnce) {
                    dbgOnce = true;
                    spdlog::info("DBG backend={} pushB=({:.2f},{:.2f},{:.2f})",
                                 m_backend == Backend::Svo ? "svo" : "dense", push.b.x,
                                 push.b.y, push.b.z);
                }
                if (m_backend == Backend::Svo)
                    m_svoPass.record(fr.cmd, push);
                else
                    m_pass.record(fr.cmd, push);
            }
            vkEndCommandBuffer(fr.cmd);
            VkSubmitInfo hsi { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            hsi.commandBufferCount = 1;
            hsi.pCommandBuffers = &fr.cmd;
            vkQueueSubmit(m_ctx.graphicsQueue(), 1, &hsi, fr.inFlight);
            ++m_frameIdx;
            if (getenv("VF_TRACE"))
                fprintf(stderr, "[f%llu] headless submitted\n", (unsigned long long)m_frameIdx);
            if ((args.selftest || args.compare) && m_frameIdx == 30)
                return args.compare ? (runCompare() ? 0 : 1) : (runSelftest() ? 0 : 1);
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
        push.misc = glm::vec4(m_splatScale, 0.0f, 0.0f, 0.0f);

        const bool splatView =
            m_renderMode == RenderMode::GaussianSplats && m_splatPass.count() > 0;
        if (splatView) {
            // offscreen -> color attachment for sprite blending
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            m_splatPass.record(fr.cmd, { m_offscreen.extent.width, m_offscreen.extent.height },
                               push);
        } else {
            // offscreen -> compute-writable -----------------------------------
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            if (m_backend == Backend::Svo)
                m_svoPass.record(fr.cmd, push);
            else
                m_pass.record(fr.cmd, push);
        }

        // rendered -> transfer-src, swapchain -> transfer-dst, blit ----------
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            splatView ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
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
        vkCmdBlitImage(fr.cmd, m_offscreen.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_swapchain.image(imgIdx), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);

        // swapchain -> color attachment for ImGui --------------------------
        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawHud();
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fr.cmd);

        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
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
        if (args.smokeFrames > 0 && m_frameIdx % 200 == 0)
            spdlog::info("smoke progress: {} frames, avg {:.2f} ms", m_frameIdx, m_avgMs);

        if ((args.selftest || args.compare) && m_frameIdx == 30)
            return args.compare ? (runCompare() ? 0 : 1) : (runSelftest() ? 0 : 1);
        if (args.smokeFrames > 0 && m_frameIdx >= uint64_t(args.smokeFrames)) {
            spdlog::info("smoke: {} frames, avg {:.2f} ms, min {:.2f}, max {:.2f}",
                         m_frameIdx, m_avgMs, m_minMs, m_maxMs);
            break;
        }
    }

    vkDeviceWaitIdle(m_ctx.device());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    destroy();
    return 0;
}

void App::destroy()
{
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
    m_pass.destroy();
    vf::destroySampler(m_ctx, m_sdfSampler);
    vf::destroySampler(m_ctx, m_albedoSampler);
    vf::destroyImage3D(m_ctx, m_sdfVol);
    vf::destroyImage3D(m_ctx, m_albedoVol);
    vf::destroyImage3D(m_ctx, m_heightImg);
    vf::destroyImage3D(m_ctx, m_offscreen);
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
