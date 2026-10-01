// The GPU timestamp profiler: read back this frame slot's marks and fold them
// into the smoothed per-pass milliseconds that the HUD prints and VF_TRACE logs.
#include "app/app.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// GPU timestamp profiling (no-op until m_profPool exists)

void App::accumulateProf(uint32_t slot, uint64_t frameIdx)

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

} // namespace app
} // namespace vf
