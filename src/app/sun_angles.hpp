// The app's sun angle constants and the one test that decides "is it night".
//
// Pure and header-only, no window and no GPU, so the test-app group can pin
// these without an App instance. Two reasons this exists rather than living in
// App:
//
//  1. Reachability. setSunPhase's preset angles were function-local constexprs,
//     which no test can reach -- a test asserting "the preset is -30/96" would
//     have been asserting its own copy. Same failure mode as copying a
//     non-atomic writer as a precedent.
//  2. Drift. The -2.0 deg night threshold was written as four literals across
//     panel_render.cpp (button highlight + phase label) and run_hotkeys.cpp
//     (the P toggle). They agreed only by convention: nothing stopped someone
//     editing one, and then the Night button highlight and the P key would
//     disagree about what the current phase is. One definition, one behaviour.
//
// The presets are the values every reference shot is calibrated to: kDayElev /
// kDayAzim are also the CLI defaults in cli/args.hpp, which must stay equal or
// the shots move. kNightElev / kNightAzim are the settled night values -- mean
// luma ~32.9 vs day's ~113.8, and a kMoonCol regression to 0.62 would read ~46,
// which is what the night gate's luma ceiling exists to catch.
#pragma once

namespace vf {
namespace app {

// Coarse day/night presets the Night button and the P hotkey snap to.
inline constexpr float kSunDayElev = 34.0f;
inline constexpr float kSunDayAzim = 238.0f;
inline constexpr float kSunNightElev = -30.0f;
inline constexpr float kSunNightAzim = 96.0f;

// Solar elevation at or below which the sun counts as night. Shared by the
// button highlight, the phase label and the P toggle so they cannot disagree.
// Note this is NOT the horizon: -2 deg leaves a little margin so a sun sitting
// exactly on the horizon reads as dusk rather than flipping to night.
inline constexpr float kSunNightElevThreshold = -2.0f;

// Solar elevation above which the phase label reads "Day".
inline constexpr float kSunDayElevThreshold = 8.0f;

// Single source of truth for "is this sun below the night threshold". Takes
// elevation in DEGREES (what m_sunDir is converted back to by the panel).
inline bool sunIsNight(float elevDeg)
{
    return elevDeg <= kSunNightElevThreshold;
}

} // namespace app
} // namespace vf