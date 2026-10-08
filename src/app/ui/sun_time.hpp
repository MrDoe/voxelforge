// Clock <-> sun mapping for the Render panel's time field (HH:MM) and the
// VF_TEST_SUN_TIME headless hook. Pure and header-only so the test-app group
// can pin it without a window or a GPU.
//
// The forward map is an analytic 12 h arc: sunrise 06:00 due east, noon at
// +60 deg elevation due south, sunset 18:00 due west, midnight at -60 deg due
// north (full night: nightFactor() is 1 for anything below y = -0.14).
// Azimuth runs 15 deg/h like the earth. This is a CONTROL curve, not an
// ephemeris: it exists so a clock position always means the same sun.
//
// The inverse is best-effort and DISPLAY-ONLY: the sun has 2 DOF, the clock
// has 1, so an off-arc sun (the -30/96 night preset, any slider drag) cannot
// be recovered from azimuth alone. sunTimeForAngles reports `approx` when
// |elev - predicted| > 3 deg; the panel shows "~HH:MM" then and never writes
// it back anywhere.
//
// Two facts a gate author must not get wrong. First, 12:00 on this arc is
// +60 deg elevation, NOT the 34/238 reference day every shot is calibrated
// to (34/238 decodes to ~15:52); never calibrate a gate to noon. Second,
// submitting a displayed read-back ALWAYS sets the arc value: 15:52 commits
// to 31.8 deg, moving the default sun 2.2 deg silently, because the display
// is a projection, not the stored state. Measured anchors (640x360 hero --shot, splat): 12:00 ->
// mean luma 121.2, 00:00 -> 34.6, 06:00 -> 88.1, 18:00 -> 87.4. Note midnight
// is NOT the -30/96 night preset (mean 32.9): -60/0 lifts the moon higher
// (moonDir lift 0.950 vs 0.675), so "night via clock" is its own point.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace vf {
namespace app {

// Wrap into [0, 24).
inline float wrapHours(float h)
{
    float t = std::fmod(h, 24.0f);
    if (t < 0.0f)
        t += 24.0f;
    return t;
}

// Clock hours -> sun angles. 00 -> (-60, 0), 06 -> (0, 90), 12 -> (60, 180),
// 18 -> (0, 270); azimuth closes 360 deg in 24 h at 15 deg/h.
inline void sunAnglesForTime(float hours, float& elevDeg, float& azimDeg)
{
    const float t = wrapHours(hours);
    constexpr float kPi = 3.14159265f;
    elevDeg = 60.0f * std::sin(kPi * (t - 6.0f) / 12.0f);
    azimDeg = 90.0f + 15.0f * (t - 6.0f);
    azimDeg -= 360.0f * std::floor(azimDeg / 360.0f);
}

// "HH:MM" or "H:MM", 00:00-23:59, nothing else. Returns false on garbage; the
// panel keeps the old text so the user can correct it.
inline bool parseSunTime(const char* s, float& hours)
{
    if (!s)
        return false;
    int hh = -1, mm = -1;
    char extra = '\0';
    if (std::sscanf(s, "%d:%d%c", &hh, &mm, &extra) != 2)
        return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
        return false;
    hours = float(hh) + float(mm) / 60.0f;
    return true;
}

// Best-effort inverse for the greyed readout: the azimuth branch picks the
// unique clock time, then `approx` marks |elev - predicted| > 3 deg, i.e.
// the sun sits off the clock arc (night preset: -30/96 decodes to 06:24,
// off by 36.3 deg of elevation, because azimuth carries no elevation sign).
inline void sunTimeForAngles(float elevDeg, float azimDeg, float& hours, bool& approx)
{
    const float a = azimDeg - 360.0f * std::floor(azimDeg / 360.0f);
    hours = wrapHours(6.0f + (a - 90.0f) / 15.0f);
    float pred = 0.0f, dummy = 0.0f;
    sunAnglesForTime(hours, pred, dummy);
    approx = std::fabs(elevDeg - pred) > 3.0f;
}

inline void formatSunTime(float hours, char* buf, std::size_t n)
{
    if (!buf || n == 0)
        return;
    const float t = wrapHours(hours);
    int hh = int(t);
    int mm = int((t - float(hh)) * 60.0f + 0.5f);
    if (mm == 60) {
        mm = 0;
        hh = (hh + 1) % 24;
    }
    std::snprintf(buf, n, "%02d:%02d", hh, mm);
}

} // namespace app
} // namespace vf
