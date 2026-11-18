// One half of the frame-recording pair. The two paths are siblings, not halves
// of one function: the headless path renders to m_offscreen and reads back with
// no window interaction, the interactive path acquires a swapchain image and
// presents. Both return kFrameDone to say "frame finished, go round again", or
// a non-negative exit status to end the run; see frame/frame.hpp.
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

namespace {

// IEEE 754 half -> float. Needed because the G-buffer readback hands back the
// raw rgba16f bytes; a guidance lane is worthless if the only way to look at it
// is to believe a shader.
float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t man = h & 0x3ffu;
    uint32_t bits = 0;
    if (exp == 0u) {
        if (man == 0u)
            bits = sign;  // +-0
        else {
            int e = 127 - 15 + 1;
            while ((man & 0x400u) == 0u) { man <<= 1; --e; }
            man &= 0x3ffu;
            bits = sign | (uint32_t(e) << 23) | (man << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7f800000u | (man << 13);  // inf / nan
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    float f = 0.0f;
    memcpy(&f, &bits, 4);
    return f;
}

// Read one rgba16f storage image back as floats, 4 per pixel.
bool readLane(Context& ctx, const Image3D& img, uint32_t w, uint32_t h,
              std::vector<float>& out)
{
    if (img.img == VK_NULL_HANDLE)
        return false;
    std::vector<uint8_t> raw;
    // 8 bytes per texel: the lanes are R16G16B16A16_SFLOAT, and the helper's
    // default is the rgba8 offscreen's size.
    readbackImage2D(ctx, img.img, w, h, raw, 8);
    if (raw.size() != size_t(w) * h * 8)
        return false;  // wrong texel size: refuse to decode a half-read image
    out.resize(size_t(w) * h * 4);
    const auto* asHalf = reinterpret_cast<const uint16_t*>(raw.data());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = halfToFloat(asHalf[i]);
    return true;
}

// Debug-encoded PPM from float RGB, taking the components from a stride-4
// layout. The sqrt is a DISPLAY transform so the low end is visible in a
// viewer; it is deliberately not the AgX look, because a diagnostic that shares
// the render's grading cannot show a grading error.
void writePpm(const char* path, uint32_t w, uint32_t h,
              const std::vector<float>& rgba, bool encode)
{
    FILE* fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                float v = rgba[i + c];
                if (encode) v = std::sqrt(std::clamp(v, 0.0f, 1.0f));
                row[size_t(x) * 3 + c] =
                    uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        fwrite(row.data(), 1, row.size(), fp);
    }
    fclose(fp);
}

}  // namespace

// Claim this frame's slot: wait for the submission from kMaxFramesInFlight
// frames ago, harvest its GPU timestamps (safe now precisely because that
// submission has completed), and hand the caller the slot to record into.
FrameSync& App::waitFrameSlot()
{
    const uint32_t f = m_frameIdx % kMaxFramesInFlight;
    FrameSync& fr = m_frames[f];
    vkWaitForFences(m_ctx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);
    // that slot's submission (3 frames ago) is complete: harvest its marks
    accumulateProf(f, m_frameIdx);
    return fr;
}

// Automated mode: zero window-system interaction. Records offscreen, submits,
// and at the right moment either writes the PPM, advances to the next
// --shotlist camera, or ends the run.
int App::recordHeadlessFrame(FrameSync& fr)
{
    const Args& args = m_args;
    const float tanHalfFov = tanHalfFov60();
    const bool shotMode = !m_shots.empty();
        // Automated mode: zero window-system interaction.
        vkResetFences(m_ctx.device(), 1, &fr.inFlight);
        vkResetCommandBuffer(fr.cmd, 0);
        VkCommandBufferBeginInfo hbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(fr.cmd, &hbi);
        const uint32_t profBase = uint32_t(m_frameIdx % kMaxFramesInFlight) * kProfMarks;
        if (m_profPool)
            vkCmdResetQueryPool(fr.cmd, m_profPool, profBase, kProfMarks);
        auto profMark = [&](uint32_t mark) {
            if (m_profPool)
                vkCmdWriteTimestamp2(fr.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     m_profPool, profBase + mark);
        };
        profMark(0);

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
        push.misc = glm::vec4(float(m_renderFlags), m_animTime, float(m_tonemapLook), m_exposure);
        // buried camera (inside solid): render shells two-sided this frame
        updateBuriedProbe();
        {
            if (m_renderMode == RenderMode::Splats || m_renderMode == RenderMode::Dgs) {
                // splat raster writes linear HDR + G-buffer
                vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                vf::transitionImage(fr.cmd, m_splatPass.depthImage().img,
                                    VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                       VK_IMAGE_ASPECT_STENCIL_BIT),
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
                m_splatPass.record(fr.cmd, push,
                                   { m_offscreen.extent.width,
                                     m_offscreen.extent.height });
                // --mode 3dgs: accumulate all surfels into the OIT targets with
                // no depth rejection, then composite into m_hdr. record()
                // above already filled the G-buffer + depth for the FX chain.
                if (m_renderMode == RenderMode::Dgs) {
                    const VkExtent2D shotExtent { m_offscreen.extent.width,
                                                  m_offscreen.extent.height };
                    auto toAtt = [&](VkImage img) {
                        vf::transitionImage(fr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                    };
                    toAtt(m_oitAccum.img);
                    toAtt(m_oitReveal.img);
                    toAtt(m_oitBackdrop.img);
                    m_splatPass.recordOit(fr.cmd, push, shotExtent);
                    auto toStorage = [&](VkImage img) {
                        vf::transitionImage(fr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL,
                                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                    };
                    toStorage(m_oitAccum.img);
                    toStorage(m_oitReveal.img);
                    toStorage(m_oitBackdrop.img);
                    // record() wrote m_hdr as a colour attachment. Copy it to
                    // m_oitBase so interiors stay pixel-identical to the shipped
                    // frame; the composite then overwrites m_hdr.
                    VkMemoryBarrier2 hb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    hb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    hb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    hb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    hb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT |
                                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    VkDependencyInfo hdi { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    hdi.memoryBarrierCount = 1;
                    hdi.pMemoryBarriers = &hb;
                    vkCmdPipelineBarrier2(fr.cmd, &hdi);
                    vf::transitionImage(fr.cmd, m_oitBase.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    VkImageCopy ic {};
                    ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    ic.extent = { shotExtent.width, shotExtent.height, 1 };
                    vkCmdCopyImage(fr.cmd, m_hdr.img, VK_IMAGE_LAYOUT_GENERAL,
                                   m_oitBase.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
                    VkMemoryBarrier2 cb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    cb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    cb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    cb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    cb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                    VkDependencyInfo cdi { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    cdi.memoryBarrierCount = 1;
                    cdi.pMemoryBarriers = &cb;
                    vkCmdPipelineBarrier2(fr.cmd, &cdi);
                    m_oitPass.record(fr.cmd, shotExtent.width, shotExtent.height);
                }
                // barrier: HDR/G-buffer written -> read by post pass. For Dgs
                // the HDR came from the composite compute pass, so include its
                // storage write in the source mask alongside the G-buffer writes.
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(fr.cmd, &di);
            } else {
            // ray-march writes linear HDR + G-buffer
            vf::transitionImage(fr.cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vf::transitionImage(fr.cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            m_svoPass.record(fr.cmd, push);
            // barrier: HDR/G-buffer written -> read by post pass
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(fr.cmd, &di);
            }
            // Micro-voxel overlay (SEPARATE SVO worlds): same compositing as the
            // interactive path - reads gpos as its depth reference and overwrites
            // a pixel only where a micro surface is nearer than what the main
            // backend already resolved there. A no-op when none are loaded.
            if (!m_microSvoPass.empty()) {
                m_microSvoPass.record(fr.cmd, push);
                VkMemoryBarrier2 mb2 { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb2.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo di2 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di2.memoryBarrierCount = 1;
                di2.pMemoryBarriers = &mb2;
                vkCmdPipelineBarrier2(fr.cmd, &di2);
            }
            // The FUR cards: the translucent hair-card layer over the micro
            // objects. It reads the G-buffer the micro overlay just wrote (the
            // depth reference) and composites into the same HDR image, so it
            // runs after the overlay and before the post pass. A no-op when
            // no object carries fur.
            if (!m_furPass.empty()) {
                m_furPass.record(fr.cmd, push);
                VkMemoryBarrier2 mb3 { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb3.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb3.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb3.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb3.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo di3 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di3.memoryBarrierCount = 1;
                di3.pMemoryBarriers = &mb3;
                vkCmdPipelineBarrier2(fr.cmd, &di3);
            }
            profMark(1);
            // post pass reads HDR/G-buffer, writes LDR offscreen
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            // The order is the whole difference between the two modes, and it is
            // chosen by one flag rather than duplicated: post.comp reads uHdr,
            // so running the effects chain on m_hdr first needs no shader
            // change — the tone map simply consumes what the effects produced.
            if (m_linearFx) {
                recordPhotorealism(fr.cmd, push, true);
                profMark(2);
                // Meter the frame AFTER the effects, while it is still linear,
                // then let the tone map spend the gain it derived.
                if (m_autoExposure)
                    m_expoPass.record(fr.cmd, uint32_t(push.a.z), uint32_t(push.a.w), push);
                m_postPass.record(fr.cmd, push);
            } else {
                if (m_autoExposure)
                    m_expoPass.record(fr.cmd, uint32_t(push.a.z), uint32_t(push.a.w), push);
                m_postPass.record(fr.cmd, push);
                profMark(2);
                // ---- photorealism passes (G/H/J/K/L toggles, shared helper) --
                recordPhotorealism(fr.cmd, push);
            }
            profMark(3);
            profMark(4); // no TAA in headless: fx == taa mark, tail = 0
        }
        profMark(5);
        vkEndCommandBuffer(fr.cmd);
        VkSubmitInfo hsi { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        hsi.commandBufferCount = 1;
        hsi.pCommandBuffers = &fr.cmd;
        vkQueueSubmit(m_ctx.graphicsQueue(), 1, &hsi, fr.inFlight);
        ++m_frameIdx;
        if (getenv("VF_TRACE"))
            fprintf(stderr, "[f%llu] headless submitted\n", (unsigned long long)m_frameIdx);
        forceFrameResults(m_frameIdx);
        // Advance the previous-frame camera here as well as interactively.
        // The headless camera never moves on its own (no input), but the shot
        // LIST does move it, and a reprojection pass that replays the first
        // shot's camera across every later shot produces a full-frame smear
        // that looks exactly like motion blur firing on a still scene.
        advancePrevCam();
        if ((args.selftest) && m_frameIdx == 30)
            return runSelftest() ? 0 : 1;
        if (shotMode && m_frameIdx == 3) {
            vkDeviceWaitIdle(m_ctx.device());
            // The meter's value lives only on the GPU until the device is idle,
            // which it is here. Logging it after the wait — not per frame — keeps
            // the render path free of host reads and gives a gate a number to
            // assert on (a meter that is stuck at 0 looks identical to one that
            // is not wired up in the image).
            if (m_autoExposure)
                spdlog::info("exposure: {:.3f} stops (x{:.3f})",
                             m_expoPass.currentExposure(),
                             exp2f(std::clamp(m_expoPass.currentExposure(), -8.0f, 6.0f)));
            std::vector<uint8_t> px;
            vf::readbackImage2D(m_ctx, m_offscreen.img, m_offscreen.extent.width,
                                m_offscreen.extent.height, px);
            const std::string& out = m_shots[m_shotIdx].path;
            FILE* fp = fopen(out.c_str(), "wb");
            if (fp) {
                fprintf(fp, "P6\n%u %u\n255\n", m_offscreen.extent.width,
                        m_offscreen.extent.height);
                for (size_t i = 0; i < px.size(); i += 4)
                    fwrite(&px[i], 3, 1, fp);
                fclose(fp);
                spdlog::info("shot written: {}", out);
            }
            // ---- guidance-lane capture (VF_GBUFFER_DUMP=<path-prefix>) ------
            // The reason the lanes exist is that a machine should be able to
            // read a MATERIAL off this frame, so the capture prints the numbers
            // a gate can assert on and writes a debug-encoded PPM pair for a
            // person. "albedo differs from the frame on N%" is the line that
            // pins the contract: if that ever comes back near zero the albedo
            // lane has silently become a second copy of the colour buffer and
            // carries no new information at all — which is the failure mode a
            // shader that compiles cannot show you.
            if (const char* prefix = getenv("VF_GBUFFER_DUMP")) {
                const uint32_t gw = m_offscreen.extent.width;
                const uint32_t gh = m_offscreen.extent.height;
                std::vector<float> alb, lit, gp;
                // gpos.w is the actual hit-type channel (0 = sky, 1 = solid,
                // 2 = water). Using the FRAME's blue dominance to identify sky
                // was the third broken instrument here: haze, water and bloom
                // are blue-dominant too, so it classified 5% of a house frame
                // as sky and then blamed the lane for not being zero there.
                if (readLane(m_ctx, m_galb, gw, gh, alb) &&
                    readLane(m_ctx, m_gmat, gw, gh, lit) &&
                    readLane(m_ctx, m_gpos, gw, gh, gp)) {
                    // What the lanes must demonstrate is that they carry information
                    // the colour buffer does not. The first instrument for that
                    // counted how often albedo disagreed with the frame, and it
                    // read 0.0% — which was the probe's fault, not the lane's: it
                    // compared a LINEAR material value against a tone-mapped,
                    // display-referred luma, and for mid-tones AgX plus the sRGB
                    // encode puts those two within a few codes of each other by
                    // construction. The scale-free statement of "different
                    // information" is correlation: if the lane were secretly a copy
                    // of the frame, r would sit at 1.0; a real material varies with
                    // the surface, not with the light that hit it.
                    double aSum = 0.0, aMax = 0.0, rSum = 0.0, lSum = 0.0;
                    double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
                    size_t nSurf = 0, nSky = 0, nSkyZeroAlb = 0;
                    for (size_t p = 0; p < size_t(gw) * gh; ++p) {
                        const size_t i = p * 4;
                        const float aLuma = (alb[i] + alb[i + 1] + alb[i + 2]) / 3.0f;
                        const float lLuma = (lit[i] + lit[i + 1] + lit[i + 2]) / 3.0f;
                        aSum += aLuma;
                        aMax = std::max(aMax, double(std::max(std::max(alb[i], alb[i + 1]),
                                                              alb[i + 2])));
                        rSum += alb[i + 3];  // roughness rides att3.a
                        lSum += lLuma;
                        const bool isSky = gp[p * 4 + 3] < 0.5f;  // gpos.w = hitType
                        if (isSky) {
                            ++nSky;
                            if (aLuma <= 1e-3f)
                                ++nSkyZeroAlb;
                            continue;  // sky is not a surface: both lanes are 0 by contract
                        }
                        const uint8_t* s = &px[p * 4];
                        const double y =
                            (0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2]) / 255.0;
                        const double x = aLuma;
                        ++nSurf;
                        sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
                    }
                    char path[600];
                    snprintf(path, sizeof(path), "%s.albedo.ppm", prefix);
                    writePpm(path, gw, gh, alb, true);
                    snprintf(path, sizeof(path), "%s.light.ppm", prefix);
                    writePpm(path, gw, gh, lit, true);
                    const size_t n = std::max<size_t>(1, size_t(gw) * gh);
                    double corr = 0.0;
                    if (nSurf > 2) {
                        const double nn = double(nSurf);
                        const double cov = sxy / nn - (sx / nn) * (sy / nn);
                        const double vx = sxx / nn - (sx / nn) * (sx / nn);
                        const double vy = syy / nn - (sy / nn) * (sy / nn);
                        if (vx > 1e-9 && vy > 1e-9)
                            corr = cov / std::sqrt(vx * vy);
                    }
                    spdlog::info("gbuffer: albedo mean {:.4f} max {:.3f}, rough mean {:.4f}, "
                                 "light mean {:.3f}; albedo-vs-frame r={:.3f} over {} "
                                 "surface pixels; sky albedo-zero {:.1f}% of {}",
                                 aSum / double(n), aMax, rSum / double(n), lSum / double(n),
                                 corr, nSurf,
                                 nSky ? 100.0 * double(nSkyZeroAlb) / double(nSky) : 0.0,
                                 nSky);
                } else {
                    spdlog::warn("gbuffer: guidance readback failed (lanes not bound?)");
                }
            }
            // ---- OIT target capture (VF_OIT_DUMP=<prefix>) -----------------
            // The composite is T = reveal.r and colour = accum.rgb/accum.a.
            // A broken reveal blend reads as T ~ 1 everywhere (sky wash); a
            // broken accum blend reads as alpha ~ 0. These numbers pin which
            // half of the chain is wrong instead of guessing from a still.
            if (m_renderMode == RenderMode::Dgs && getenv("VF_OIT_DUMP")) {
                const uint32_t ow = m_offscreen.extent.width;
                const uint32_t oh = m_offscreen.extent.height;
                std::vector<float> acc, rev, bg, base;
                const bool gotBase = readLane(m_ctx, m_oitBase, ow, oh, base);
                double baseMean = 0.0;
                if (gotBase)
                    for (size_t p = 0; p < base.size() / 4; ++p)
                        baseMean += (base[p * 4] + base[p * 4 + 1] + base[p * 4 + 2]) / 3.0;
                if (gotBase && base.size())
                    baseMean /= double(base.size() / 4);
                spdlog::info("oit: base copy mean {:.3f}", baseMean);
                if (readLane(m_ctx, m_oitAccum, ow, oh, acc) &&
                    readLane(m_ctx, m_oitReveal, ow, oh, rev) &&
                    readLane(m_ctx, m_oitBackdrop, ow, oh, bg)) {
                    double aSum = 0.0, rSum = 0.0, rMin = 1e30, rMax = -1e30;
                    double bSum = 0.0, sealed = 0.0, aZero = 0.0;
                    // Coverage histogram: the per-pixel edge mask is built from
                    // accum.a, so these buckets pick the silhouette threshold.
                    size_t h0 = 0, h01 = 0, h05 = 0, h1 = 0, h2 = 0, h5 = 0;
                    const size_t np = size_t(ow) * oh;
                    for (size_t p = 0; p < np; ++p) {
                        const float a = acc[p * 4 + 3];
                        const float t = rev[p * 4];
                        aSum += a; rSum += t;
                        rMin = std::min(rMin, double(t));
                        rMax = std::max(rMax, double(t));
                        bSum += (bg[p * 4] + bg[p * 4 + 1] + bg[p * 4 + 2]) / 3.0;
                        sealed += t < 0.5;
                        aZero += a < 1e-5;
                        if (a < 0.1) ++h0;
                        else if (a < 0.5) ++h01;
                        else if (a < 1.0) ++h05;
                        else if (a < 2.0) ++h1;
                        else if (a < 5.0) ++h2;
                        else ++h5;
                    }
                    spdlog::info("oit: accum.a buckets <0.1 {:.1f}% 0.1-0.5 {:.1f}% "
                                 "0.5-1 {:.1f}% 1-2 {:.1f}% 2-5 {:.1f}% >=5 {:.1f}%",
                                 100.0 * h0 / np, 100.0 * h01 / np, 100.0 * h05 / np,
                                 100.0 * h1 / np, 100.0 * h2 / np, 100.0 * h5 / np);
                    spdlog::info("oit: accum.a mean {:.3f}, zero {:.1f}%; "
                                 "reveal T mean {:.3f} [{:.3f}..{:.3f}], sealed(<0.5) "
                                 "{:.1f}%; backdrop mean {:.3f}",
                                 aSum / np, 100.0 * aZero / np, rSum / np,
                                 rMin, rMax, 100.0 * sealed / np, bSum / np);
                } else {
                    spdlog::warn("oit: target readback failed");
                }
            }
            if (++m_shotIdx < m_shots.size()) {
                // next camera: reset the warmup frame counter so the
                // capture is identical to a single-shot run of the
                // same view (3 frames rendered before the readback).
                const ShotSpec& sh = m_shots[m_shotIdx];
                m_camera.pos = { sh.camx, sh.camy, sh.camz };
                const glm::vec3 d = glm::normalize(
                    glm::vec3(sh.tx, sh.ty, sh.tz) - m_camera.pos);
                m_camera.yaw = atan2(d.z, d.x);
                m_camera.pitch = asin(d.y);
                m_frameIdx = 0;
                return kFrameDone;
            }
            return 0;
        }
        if (args.smokeFrames > 0 && m_frameIdx >= uint64_t(args.smokeFrames)) {
            spdlog::info("smoke done: {} frames, avg {:.2f} ms, min {:.2f}, max {:.2f}",
                         m_frameIdx, m_avgMs, m_minMs, m_maxMs);
            return 0;
        }
        return kFrameDone;
    return kFrameDone;
}

} // namespace app
} // namespace vf
