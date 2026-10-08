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
#include "app/sun_angles.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/sun_time.hpp"
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
// The one elevation/azimuth -> direction conversion in the app. Every sun
// control (the --sun/manifest startup path, the Render panel sliders, the
// preset buttons and the P hotkey) funnels through here, so a degree/direction
// convention change cannot be applied to some controls and forgotten in others.
void App::setSunAngles(float elevDeg, float azimDeg)
{
    const float er = glm::radians(elevDeg), ar = glm::radians(azimDeg);
    m_sunDir = glm::vec4(
        glm::normalize(glm::vec3(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar))), 0.0f);
}

// The coarse day/night switch. The sliders stay the fine control; this snaps
// between two measured points on them.
//
//  day 34/238  - the angles every reference shot is calibrated to, so snapping
//                back to day restores the exact frame the gates were measured
//                against rather than merely "a bright one".
//  night -30/96 - the settled night value: mean luma 32.9 vs day's 120.4,
//                dark pixels 0.01% -> 23.5%, sky RGB (12.5, 23.4, 37.0). The
//                moon colour it depends on is kMoonCol * 0.14; the earlier 0.62
//                read as dusk, so do not "brighten" it back.
//
// requestWorldReload() is required, not cosmetic: the direct sun and sky follow
// the push constant immediately, but each surfel's SHADOW is CPU-baked at
// surfelize time, so without the reload the shadows stay at the old sun.
void App::setSunPhase(bool night)
{
    // Angles live in ui/sun_angles.hpp so the night gate's test can pin the
    // SAME definition this snaps to instead of a second copy.
    if (night)
        setSunAngles(kSunNightElev, kSunNightAzim);
    else
        setSunAngles(kSunDayElev, kSunDayAzim);
    requestWorldReload(); // rebake the per-surfel sun shadows
    spdlog::info("sun phase -> {} (elev {:.0f} deg, rebaking surfels)",
                 night ? "night" : "day",
                 night ? kSunNightElev : kSunDayElev);
}

// The clock-time snap: HH:MM on the analytic arc (ui/sun_time.hpp) through
// setSunAngles, with exactly one reload like setSunPhase. Session-only, no
// world.json write.
void App::setSunTime(float hours)
{
    float e = 0.0f, a = 0.0f;
    sunAnglesForTime(hours, e, a);
    setSunAngles(e, a);
    requestWorldReload(); // rebake the per-surfel sun shadows
    char disp[8] = "";
    formatSunTime(hours, disp, sizeof(disp));
    spdlog::info("sun time -> {} (elev {:.1f} deg azim {:.1f} deg, rebaking surfels)",
                 disp, e, a);
}

int App::run(const Args& args)
{
    // Every slice below reads the arguments through m_args, so publish them
    // before any of them can run (the sun/anim/tonemap block used to have the
    // local `args`, and the init block assigned m_args several lines later).
    m_args = args;

    // --sun elevation/azimuth, in degrees, into a direction TOWARD the sun.
    {
        m_manifestPath = std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
        float e = args.sunElev, a = args.sunAzim;
        // The scene's own "sun" block wins unless --sun was given, so an
        // authored evening/night persists across runs instead of needing a
        // flag every time. An elevation below the horizon is a valid scene.
        float me = 0.f, ma = 0.f;
        if (!args.sunSet && vf::voxel::worldfile::loadSunManifest(m_manifestPath, me, ma)) {
            e = me;
            a = ma;
            spdlog::info("scene sun: elev {:.1f} deg azim {:.1f} deg (world.json)", e, a);
        }
        setSunAngles(e, a);
        // VF_TEST_SUN_PHASE="day"|"night" drives the SAME switch the Render
        // panel's preset buttons use, so a headless render can prove the
        // day/night path end to end instead of the assertion being untested.
        if (const char* phase = getenv("VF_TEST_SUN_PHASE")) {
            setSunPhase(!strcasecmp(phase, "night"));
        }
        // VF_TEST_SUN_TIME="HH:MM" drives the time field's mapping headlessly.
        // Order is manifest, PHASE, then TIME: TIME wins when both are set.
        if (const char* tod = getenv("VF_TEST_SUN_TIME")) {
            float h = 0.0f;
            if (parseSunTime(tod, h))
                setSunTime(h);
            else
                spdlog::warn("VF_TEST_SUN_TIME='{}' ignored (use HH:MM)", tod);
        }
    }
    m_animTime = args.animTime;
    m_tonemapLook = args.tonemap;
    if (const char* smooth = getenv("VF_SMOOTH_STRENGTH")) {
        m_smoothStrength = std::clamp(finiteEnv(smooth, m_smoothStrength), 0.0f, 1.0f);
    } else if (const char* strength = getenv("VF_EDIT_STRENGTH")) {
        m_smoothStrength = std::clamp(finiteEnv(strength, m_smoothStrength), 0.0f, 1.0f);
    }
    // VF_EDIT_FALLOFF_CURVE=constant|sphere|root|smooth|linear|sharp
    if (const char* curve = getenv("VF_EDIT_FALLOFF_CURVE")) {
        using C = vf::voxel::EditableWorld::FalloffCurve;
        for (int i = 0; i < int(C::Count); ++i) {
            const C c = C(i);
            if (!strcasecmp(curve, vf::voxel::EditableWorld::falloffCurveName(c)))
                m_editFalloffCurve = c;
        }
    }
    if (const char* pv = getenv("VF_SMOOTH_PRESERVE_VOLUME")) {
        m_smoothPreserveVolume = finiteEnv(pv, 0.0f) != 0.0f;
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
