// Voxelforge - window, chunked-SVO ray marcher, HUD.
#include "core/camera.hpp"
#include "core/log.hpp"
#include "platform/window.hpp"
#include "rhi/swapchain.hpp"
#include "render/svo_pass.hpp"
#include "render/taa_pass.hpp"
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
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
        } else if (s == "--animtime" && i + 1 < argc) {
            a.animTime = atof(argv[++i]);
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
    void syncWorldLayerList();
    bool uploadTerrainTexture();
    bool uploadObjVolTexture();
    void persistWorldLayers();
    void rescanWorldLayers();
    void applyWorldReload();

    Args m_args;
    vf::Window m_window;
    vf::Context m_ctx;
    vf::Swapchain m_swapchain;

    vf::Image3D m_offscreen;
    vf::Image3D m_heightImg;
    vf::Image3D m_objVolImg;
    vf::SvoPass m_svoPass;
    vf::TaaPass m_taaPass;
    vf::Image3D m_taaHistory[2];
    vf::Image3D m_taaResolved;
    bool m_taaEnabled = true;
    bool m_taaFirstFrame = true;
    int m_taaHistoryIdx = 0;
    glm::vec4 m_pushB { 1.0f };      // shader .b block: worldSize, voxelSize, gridN

    // layered world state (GUI)
    std::vector<vf::voxel::worldfile::WorldLayer> m_worldLayers;

    // single world source for every renderer path
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
    bool m_showControls = true;
    float m_animTime = 0.0f;
    uint32_t m_nextAcquire = 0;

    // AI chat + picking + editable world
    vf::voxel::EditableWorld m_editable { std::string(VOXELFORGE_ASSET_DIR) };
    vf::ai::ChatUi m_chatUi;
    vf::voxel::PickHit m_hoverHit;
    vf::voxel::PickHit m_selectedHit;
    bool m_hasSelection = false;
    bool m_ctrlLmbWasDown = false;
    bool m_chatInitialized = false;
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
    m_offscreen = vf::makeImage3D(
        m_ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
    if (!m_offscreen.img || !m_taaHistory[0].img || !m_taaHistory[1].img || !m_taaResolved.img)
        return false;
    m_svoPass.updateDescriptors(m_offscreen);
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

    // chunked-SVO world synthesis (single render path) ---------------------
    // layered world is the single source: the SVO is synthesized directly
    // from the enabled layer files (no merged cache, no procedural fallback)
    {
        const std::string manifestPath =
            std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
        if (!m_layers.load(manifestPath)) {
            spdlog::critical("cannot load {} - run 'ninja -C build world' to bake assets",
                             manifestPath);
            return 1;
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
    m_worldLayers = std::move(l);
}

bool App::uploadTerrainTexture()
{
    const std::vector<glm::vec2>& htx = m_layers.field().heightTexture();
    const uint32_t lat = uint32_t(m_layers.field().latN());
    vf::destroyImage3D(m_ctx, m_heightImg);
    m_heightImg = vf::makeImage3D(m_ctx, lat, lat, 1,
                                  VK_FORMAT_R32G32_SFLOAT,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_STORAGE_BIT);
    if (!m_heightImg.img)
        return false;
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
    vf::destroyImage3D(m_ctx, m_objVolImg);
    m_objVolImg = vf::makeImage3D(m_ctx, uint32_t(n), uint32_t(n), uint32_t(n),
                                  VK_FORMAT_R8_SNORM,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_STORAGE_BIT);
    if (!m_objVolImg.img)
        return false;
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
    syncWorldLayerList();
    rescanWorldLayers(); // layers dropped into assets/ while running show up too
}

void App::persistWorldLayers()
{
    std::vector<vf::voxel::worldfile::WorldLayer> out;
    out.reserve(m_worldLayers.size());
    for (const auto& l : m_worldLayers)
        if (l.listed)
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
    ImGui::Text("Render: chunked SVO");
    ImGui::Text("%u x %u @ %.1f fps (%.2f ms)", m_swapchain.extent().width,
                m_swapchain.extent().height, 1000.0 / m_avgMs, m_avgMs);
    ImGui::Separator();

    ImGui::Text("Cam  %.1f %.1f %.1f", m_camera.pos.x, m_camera.pos.y, m_camera.pos.z);
    ImGui::Text("Yaw/pitch %.0f/%.0f  speed %.1f", glm::degrees(m_camera.yaw),
                glm::degrees(m_camera.pitch), m_camera.speed);
    ImGui::Checkbox("Show controls", &m_showControls);
    if (m_showControls) {
        ImGui::Separator();
        ImGui::BulletText("WASD move, Q/E down/up");
        ImGui::BulletText("RMB hold: look");
        ImGui::BulletText("Wheel: speed, Shift/Ctrl boost/slow");
        ImGui::BulletText("ESC: quit");
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
                }
            }
        }
        if (ImGui::Button("Rescan assets folder"))
            rescanWorldLayers();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu records live", m_layers.stats().records);
        ImGui::TextDisabled("toggles & AI edits hot-reload live");
    }
    ImGui::End();

    // AI Chat (pass picking state; edits trigger an immediate world reload)
    {
        auto reloadFn = [this](){ this->requestWorldReload(); };
        m_chatUi.draw(m_editable, m_layers, m_hoverHit.hit ? &m_hoverHit : nullptr,
                      m_hasSelection ? &m_selectedHit : nullptr, m_hasSelection, reloadFn);
        // hover highlight (world->screen)
        if (m_hoverHit.hit) {
            // project hover voxel center to screen for a tiny reticle
            // simple: draw crosshair at mouse cursor when Ctrl held
            // plus small text already via ChatUi; keep for debug
        }
        // selected voxel world-space reticle via ImGui foreground
        if (m_hasSelection) {
            glm::vec3 selW = vf::voxel::voxelCenter(m_selectedHit.voxel);
            // project using same logic as push: need view-proj
            // Instead draw at screen center when picked via Ctrl+LMB? Provide feedback via overlay
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            dl->AddText(ImVec2(center.x - 40, center.y + 20), IM_COL32(80,255,80,220), "● selected");
            (void)selW;
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
    if (args.probeSet) {
        // probes read the live layered world (ai_edits included as a layer)
        vf::voxel::LayeredWorld probeWorld;
        if (!probeWorld.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json")) {
            spdlog::critical("probe: cannot load world.json");
            return 1;
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
    // AI chat
    if (!m_chatInitialized) {
        m_chatUi.init(args.llmUrl, args.llmModel);
        m_chatInitialized = true;
    }

    // per-backend camera spawn
    // hero shot: across the river toward the cabin, sun raking from the west
    m_camera.pos = { -16.f, 6.5f, -14.f };
    glm::vec3 dir = glm::normalize(glm::vec3(6.5f, 0.8f, 11.0f) - m_camera.pos);
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

        bool chatWantsKeys = m_chatInitialized && m_chatUi.wantsCaptureKeyboard();
        (void)chatWantsKeys;

        // animation clock only advances interactively - headless shots stay
        // deterministic (misc.y feeds wind/grass shading)
        const bool headlessMode =
            args.selftest || args.smokeFrames > 0 || !args.shot.empty();
        if (!headlessMode)
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

        // live world reload: MCP/chat edits and layer toggles land in the
        // layer files; poll for changes and swap the SVO in-place
        m_layerPollT += dt;
        if (m_layers.loaded() && m_layerPollT >= 0.5f) {
            m_layerPollT = 0.f;
            if (m_layers.reloadIfChanged())
                applyWorldReload();
        }
        if (m_pendingWorldReload) {
            m_pendingWorldReload = false;
            m_layers.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json");
            applyWorldReload();
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
            // hover when Ctrl held (update 15Hz throttled implicitly every frame okay)
            if (ctrl && !wantMouse && fb.x>0 && fb.y>0) {
                float tanHalf = tanHalfFov;
                float aspect = float(fb.x)/float(fb.y);
                glm::vec3 rd = vf::voxel::screenRayDir(mx,my,fb.x,fb.y,tanHalf,aspect,
                                                       m_camera.forward(), m_camera.right(), m_camera.up());
                m_hoverHit = vf::voxel::rayPick(m_layers.field(), m_camera.pos, rd);
            } else {
                m_hoverHit.hit = false;
            }
            bool justPressed = lmb && !m_ctrlLmbWasDown && ctrl && !wantMouse;
            m_ctrlLmbWasDown = lmb;
            if (justPressed && m_hoverHit.hit) {
                m_selectedHit = m_hoverHit;
                m_hasSelection = true;
                glm::vec3 w = vf::voxel::voxelCenter(m_selectedHit.voxel);
                spdlog::info("pick selected {} {} {} world {:.2f} {:.2f} {:.2f} mat {}", 
                    m_selectedHit.voxel.x, m_selectedHit.voxel.y, m_selectedHit.voxel.z, w.x,w.y,w.z, int(m_selectedHit.mat));
            }
        }

        // camera: skip WASD when chat input focused
        bool chatCaptures = m_chatInitialized && m_chatUi.wantsCaptureKeyboard();
        if (chatCaptures) {
            // block camera move while typing; consume mouse delta to avoid jump
            double _dx,_dy; m_window.getMouseDelta(_dx,_dy);
        } else {
            m_camera.update(m_window, dt);
        }

        uint32_t f = m_frameIdx % kMaxFramesInFlight;
        FrameSync& fr = m_frames[f];
        vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

        const bool headlessRun = args.selftest || args.smokeFrames > 0 || !args.shot.empty();
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
            push.misc = glm::vec4(0.0f, m_animTime, 0.0f, 0.0f);
            {
                vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                m_svoPass.record(fr.cmd, push);
            }
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
        push.misc = glm::vec4(0.0f, m_animTime, 0.0f, 0.0f);

        // offscreen -> compute-writable -----------------------------------
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_svoPass.record(fr.cmd, push);

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
            VkImageView histView = m_taaHistory[m_taaHistoryIdx].view;
            // history already in GENERAL from previous frame's copy, make it readable
            // (first frame history is undefined but TAA handles firstFrame)
            m_taaPass.updateDescriptors(m_offscreen.view, histView, m_taaResolved.view);
            // history -> SHADER_READ (if not first frame, already GENERAL)
            // resolved -> GENERAL for write
            vf::transitionImage(fr.cmd, m_taaResolved.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            m_taaPass.record(fr.cmd, m_offscreen.extent.width, m_offscreen.extent.height,
                             m_taaFirstFrame ? 0.0f : 0.92f, m_taaFirstFrame);
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
            vf::transitionImage(fr.cmd, m_taaHistory[m_taaHistoryIdx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(fr.cmd, m_taaResolved.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_taaHistory[m_taaHistoryIdx].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            vf::transitionImage(fr.cmd, m_taaHistory[m_taaHistoryIdx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            // also keep resolved as TRANSFER_SRC for blit (already)
            taaSrc = m_taaResolved.img;
            taaSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            taaSrcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            taaSrcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
            m_taaHistoryIdx ^= 1;
            m_taaFirstFrame = false;
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
        if (getenv("VF_IMGUI_DEBUG") && m_frameIdx == 5) {
            ImDrawData* dd = ImGui::GetDrawData();
            spdlog::warn("imgui dbg: valid={} display=({:.0f},{:.0f}) idxcount={} cmdlists={}",
                         dd ? 1 : 0, dd ? dd->DisplaySize.x : -1.f,
                         dd ? dd->DisplaySize.y : -1.f,
                         dd ? dd->TotalIdxCount : -1, dd ? dd->CmdListsCount : -1);
        }
        // With UseDynamicRendering we must open the render pass ourselves and
        // target the swapchain view; LOAD keeps the blitted world underneath.
        {
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
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fr.cmd);
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
                                VK_IMAGE_LAYOUT_GENERAL,
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
    m_taaPass.destroy();
    vf::destroyImage3D(m_ctx, m_objVolImg);
    vf::destroyImage3D(m_ctx, m_heightImg);
    vf::destroyImage3D(m_ctx, m_offscreen);
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
