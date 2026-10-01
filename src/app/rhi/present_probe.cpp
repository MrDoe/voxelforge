// Why a swapchain VkResult has to be reported at all, and the probe that does
// it. handleResize() covers only OUT_OF_DATE/SUBOPTIMAL and logs nothing, and
// every other result from acquire/present falls through unlogged - including
// device-lost and out-of-host-memory. A persistent failure at acquire makes
// the frame loop skip its tail every iteration (it never draws, never
// crashes, never says so), which is indistinguishable from "the app hung"
// when all you have is pixels.
//
// Log-once, NOT per frame: the failure modes above are exactly the ones that
// spin, and a per-frame error would emit thousands of lines a second and
// destroy the instrument. Gated on VF_TRACE, so a default run is unchanged.
// This deliberately does NOT make device-lost exit: that is a behaviour
// decision, not a logging side effect.

#include "app/app.hpp"

#include "app/rhi/present_probe.hpp"

#include <map>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// forced-error hook to be spelled as a raw int.
struct VkResultName { VkResult r; const char* name; };
constexpr VkResultName kVkResults[] = {
    { VK_SUCCESS, "VK_SUCCESS" },
    { VK_SUBOPTIMAL_KHR, "VK_SUBOPTIMAL_KHR" },
    { VK_ERROR_OUT_OF_DATE_KHR, "VK_ERROR_OUT_OF_DATE_KHR" },
    { VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY" },
    { VK_ERROR_DEVICE_LOST, "VK_ERROR_DEVICE_LOST" },
    { VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,
      "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT" },
    { VK_ERROR_SURFACE_LOST_KHR, "VK_ERROR_SURFACE_LOST_KHR" },
    { VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR" },
};

const char* vkResultName(VkResult r)
{
    for (const VkResultName& e : kVkResults)
        if (e.r == r)
            return e.name;
    return "other";
}

// Inverse lookup for VF_TEST_FORCE_PRESENT_ERR. Returns false on an unknown
// name so the caller can say so instead of silently probing VK_SUCCESS.
bool vkResultFromName(const char* name, VkResult& out)
{
    for (const VkResultName& e : kVkResults)
        if (std::strcmp(e.name, name) == 0) {
            out = e.r;
            return true;
        }
    return false;
}

// true the first time this result is seen, so a spinning failure logs once.
//
// A SET of codes per slot, not one remembered code. With a single remembered
// code the latch means "log whenever the code CHANGES", so two failures
// alternating (device-lost, surface-lost, device-lost, ...) log EVERY frame -
// measured, via VF_TEST_FORCE_PRESENT_ERR - which is exactly the flood the
// latch exists to prevent, and it hid because a real persistent failure
// repeats one code and behaves correctly. Bounded, and it saturates rather
// than wrapping, so a caller cannot make it forget and start re-logging.
bool firstSight(VkResult r, const char* what)
{
    if (!getenv("VF_TRACE"))
        return false;
    constexpr int kMaxCodes = 8;
    static int seen[2][kMaxCodes] = {};
    static int nSeen[2] = {};
    const int slot = what[0] == 'a' ? 0 : 1;
    const int code = int(r);
    for (int i = 0; i < nSeen[slot]; ++i)
        if (seen[slot][i] == code)
            return false;
    if (nSeen[slot] < kMaxCodes)
        seen[slot][nSeen[slot]++] = code;
    return true;
}

// The probe itself, split out of the frame loop so a test can drive it
// directly. Reporting is separated from the Vulkan call ON PURPOSE: the forced
// -error hook feeds a synthetic result through THIS function, so the name, the
// severity choice and the log-once latch are all exercised for real while the
// actual present/acquire the frame depends on is never touched. A test that
// broke the present to test the present logger would be a worse test than none.
void reportFrameResult(VkResult r, const char* what, unsigned long long frame)
{
    if (!firstSight(r, what))
        return;
    const bool fatal = r == VK_ERROR_DEVICE_LOST ||
                       r == VK_ERROR_OUT_OF_HOST_MEMORY;
    if (what[0] == 'a')
        spdlog::log(fatal ? spdlog::level::err : spdlog::level::warn,
                    "acquire returned {} at frame {} - the frame body will be "
                    "skipped each frame (the loop keeps spinning)",
                    vkResultName(r), frame);
    else
        spdlog::log(fatal ? spdlog::level::err : spdlog::level::warn,
                    "present returned {} at frame {}", vkResultName(r), frame);
}

// VF_TEST_FORCE_PRESENT_ERR="CODE[,CODE...]" (a VkResult name or an int): drive
// the probe with SYNTHETIC results, once per frame, so its reporting has a
// positive firing test. Unset, this costs one getenv and changes nothing; the
// real present is never touched, because the thing under test is the logger,
// not the swapchain.
//
// It is called from the HEADLESS frame body as well as after the real present,
// and that is not redundancy: a headless render never acquires or presents the
// swapchain at all (it submits, then reads back offscreen), so a hook placed
// only beside vkQueuePresentKHR is unreachable from every test in the repo -
// the first version of this was, and it fired zero times. Repeating a code in
// the list is deliberate: the log-once latch is what stops a spinning failure
// from emitting thousands of lines per second, and it is only observable if
// something asks more than once.
void forceFrameResults(unsigned long long frame)
{
    const char* forced = getenv("VF_TEST_FORCE_PRESENT_ERR");
    if (!forced || !*forced)
        return;
    std::string rest(forced);
    for (size_t at = 0; at <= rest.size();) {
        const size_t comma = rest.find(',', at);
        const std::string tok = rest.substr(
            at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? rest.size() + 1 : comma + 1;
        if (tok.empty())
            continue;
        VkResult r = VK_SUCCESS;
        if (vkResultFromName(tok.c_str(), r)) {
            reportFrameResult(r, "present", frame);
            continue;
        }
        char* end = nullptr;
        const long v = std::strtol(tok.c_str(), &end, 0);
        if (end && *end == '\0')
            reportFrameResult(VkResult(v), "present", frame);
        else
            spdlog::error("VF_TEST_FORCE_PRESENT_ERR: '{}' is neither a known "
                          "VkResult name nor an int", tok);
    }
}

} // namespace app
} // namespace vf
