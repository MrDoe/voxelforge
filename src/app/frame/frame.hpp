#pragma once

// The frame loop is cut into slices (run_input.cpp, run_brush_preview.cpp,
// run_hotkeys.cpp, run_capture.cpp, ...), so a few values can no longer be
// locals of one giant while body. They are gathered here rather than threaded
// through as a long parameter list, because each is genuinely "the state of
// this frame" instead of an argument to one call.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <glm/glm.hpp>

namespace vf {
namespace app {

// What a recorded frame tells the loop to do next. The headless and
// interactive paths both end in `continue` / `break` in the original single
// body; those keywords cannot cross a function boundary, so each path returns
// this instead.
//
//   kFrameDone  the path handled the frame; go round again (was `continue`)
//   kFrameExit  leave the loop, run the shutdown epilogue, return 0
//               (was `break`)
//   >= 0        the run is over with that exit status, and the epilogue is
//               SKIPPED - which is what the original `return` from inside the
//               loop did
constexpr int kFrameDone = -1;
constexpr int kFrameExit = -2;

// The app renders with a fixed 60-degree vertical FOV. It used to be a local of
// App::run, which meant no UI slice could reach it without being passed it
// (see the gizmo hit-tests, which need the same projection the splat VS uses).
// One shared constant keeps the drawn gizmo and the tested gizmo in step.
inline float tanHalfFov60()
{
    return tanf(glm::radians(60.0f) * 0.5f);
}

// A float environment override, parsed once and validated: an unset, empty,
// unparseable or non-finite value keeps the caller's fallback. Every VF_*
// tuning knob that must accept "1.5" or "0" goes through here, so a typo in CI
// degrades to the default instead of a silent zero.
inline float finiteEnv(const char* text, float fallback)
{
    if (!text || !*text)
        return fallback;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    return end != text && std::isfinite(value) ? value : fallback;
}

// Per-frame values shared between the frame-loop slices.
struct FrameInputs {
    // A run with no window interaction: --selftest, --smoke, or a shot run.
    // The animation clock does not advance and no key state is read, so a
    // hidden window can never phantom-trigger a toggle.
    bool headlessRun = false;
    // The chat pane owns the keyboard, so the camera must not move.
    bool chatCaptures = false;
    // "frames:path" - dump the swapchain (HUD included) after N presented
    // frames. A headless run never reaches it (it renders offscreen).
    uint64_t hudShotFrame = 0;
    std::string hudShotPath;
};

} // namespace app
} // namespace vf
