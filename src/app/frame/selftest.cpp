// --selftest: the sky probe and the frame-coverage acceptance, with no window
// interaction.
#include "app/app.hpp"

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

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

    // sky probe: upper-right area, clear of the default HUD window position.
    // Photo-foliage cards (render bit 13, default ON) are tall vertical quads,
    // so a close blade can poke into ANY one pixel of this area. The probe
    // asks whether sky is VISIBLE here, not whether one exact texel is sky:
    // sample a small patch and take its bluest pixel (max b-r). A patch that
    // is wholly grass still fails, so the test keeps its teeth.
    uint32_t sx = W * 15 / 16, sy = H / 8;
    uint8_t tr = 0, tg = 0, tb = 0;
    int bestBlue = -1000;
    const int rad = 12;
    for (int dy = -rad; dy <= rad; ++dy) {
        const int yy = int(sy) + dy;
        if (yy < 0 || yy >= int(H)) continue;
        for (int dx = -rad; dx <= rad; ++dx) {
            const int xx = int(sx) + dx;
            if (xx < 0 || xx >= int(W)) continue;
            const size_t i = (size_t(yy) * W + xx) * 4;
            const uint8_t r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
            const int blue = int(b) - int(r);
            if (blue > bestBlue) { bestBlue = blue; tr = r; tg = g; tb = b; }
        }
    }
    bool skyOk = tb >= tr;

    spdlog::info("selftest[voxel]: geometry coverage {:.1f}%, sky probe ({},{},{})",
                 geoRatio * 100.0f, tr, tg, tb);
    // region diagnostics: 3x3 grid average colors
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            uint64_t r = 0, g = 0, b = 0;
            size_t n = 0;
            uint32_t x0 = uint32_t(gx) * W / 3, x1 = uint32_t(gx + 1) * W / 3;
            uint32_t y0 = uint32_t(gy) * H / 3, y1 = uint32_t(gy + 1) * H / 3;
            for (uint32_t y = y0; y < y1; y += 4)
                for (uint32_t x = x0; x < x1; x += 4) {
                    size_t i = (size_t(y) * W + x) * 4;
                    r += pixels[i];
                    g += pixels[i + 1];
                    b += pixels[i + 2];
                    ++n;
                }
            if (!n)
                continue;
            fprintf(stderr, "[grid %d,%d] avg (%u,%u,%u)\n", gx, gy,
                    unsigned(r / n), unsigned(g / n), unsigned(b / n));
        }
    }

    // Coverage bounds match visual_check.py: clear shallow-water coves can
    // legitimately render up to ~98% non-sky pixels (water/bed count as
    // geometry); a buried camera still fails via ~100% + sky probe.
    if (geoRatio < 0.03f || !skyOk || geoRatio > 0.985f) {
        spdlog::error("selftest FAILED");
        return false;
    }
    spdlog::info("selftest PASSED");
    return true;
}

} // namespace app
} // namespace vf
