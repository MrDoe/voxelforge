// The frame loop and its startup ordering. This is the ONLY file that knows
// the sequence; each step it performs lives in the slice named in the call.
//
// The loop body is deliberately thin. Two rules keep it from regrowing into a
// monolith, and both are load-bearing rather than stylistic:
//
//  1. A step that needs to know about the pointer, the hover, or a gizmo lives
//     in run_input.cpp, and the steps that CONSUME what it produced
//     (run_brush_preview.cpp, scene_overlays.cpp) come after it. Order here is
//     therefore semantic, not cosmetic.
//  2. The two recording paths are siblings. A headless run never acquires a
//     swapchain image and an interactive run never reads back, so they return a
//     verdict instead of using continue/break directly.
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/rhi/present_probe.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The shared 60-degree FOV and the env parser live in frame.hpp, not here: the
// UI slices need the FOV too, and a loop-local would force it to be passed
// everywhere, while a file-static parser would be invisible to run_hooks.cpp.
int App::run(const Args& args)
{
    // Every slice below reads the arguments through m_args, so publish them
    // before any of them can run (the sun/anim/tonemap block used to have the
    // local `args`, and the init block assigned m_args several lines later).
    m_args = args;

    // --sun elevation/azimuth, in degrees, into a direction TOWARD the sun.
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
    if (args.probeSet)
        return runProbe() ? 1 : 0;

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
    if (!initVulkan()) {
        spdlog::critical("vulkan init failed");
        destroy();
        return 1;
    }

    // Headless hooks first: a --shot must already contain their result.
    if (const int rc = runStartupTestHooks())
        return rc;

    initImGui();
    // The per-frame state has to exist before the env overrides run, because
    // VF_HUD_SHOT fills a field of it.
    FrameInputs fx;
    setupCameraAndShots();
    applyStartupEnvOverrides(fx);

    auto last = std::chrono::steady_clock::now();
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
        // The animation clock only advances interactively - a headless shot
        // must stay deterministic (misc.y feeds wind/grass shading).
        fx.headlessRun = args.selftest || args.smokeFrames > 0 ||
                         !args.shot.empty() || !args.shots.empty();
        if (!fx.headlessRun)
            m_animTime += dt;

        if (m_window.resized()) {
            m_window.clearResized();
            handleResize();
        }
        runPreInputTestHooks();
        pollWorldAndTextures(dt);
        // VF_TEST_ROTATE_LIVE is a synthetic, PERSISTENT preview state. It is
        // hoisted here because two consumers need it - processInput (which must
        // not treat a synthetic pose as a live drag) and the post-input hook
        // that seeds it.
        const char* testRotateLive = getenv("VF_TEST_ROTATE_LIVE");
        const bool rotateLiveTest = testRotateLive && *testRotateLive;
        fx.chatCaptures = updateCamera(dt);
        processInput(fx.chatCaptures, rotateLiveTest);
        runPostInputTestHooks(testRotateLive);
        updateBrushPreview();
        handleHotkeys(fx.chatCaptures, fx.headlessRun);

        FrameSync& fr = waitFrameSlot();
        const int rc = fx.headlessRun ? recordHeadlessFrame(fr)
                                      : recordInteractiveFrame(fr, fx);
        if (rc == kFrameDone)
            continue;
        if (rc == kFrameExit)
            break;
        // An exit status from inside the loop skips the shutdown epilogue
        // below, which is the original behaviour: a failed submit or a written
        // HUD shot returned straight out of run().
        return rc;
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

} // namespace app
} // namespace vf
