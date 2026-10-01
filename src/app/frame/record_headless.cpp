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
            if (m_renderMode == RenderMode::Splats) {
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
                // barrier: HDR/G-buffer written -> read by post pass
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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
            profMark(1);
            // post pass reads HDR/G-buffer, writes LDR offscreen
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            m_postPass.record(fr.cmd, push);
            profMark(2);

            // ---- photorealism passes (G/H/J/K/L toggles, shared helper) ----
            recordPhotorealism(fr.cmd, push);
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
        if ((args.selftest) && m_frameIdx == 30)
            return runSelftest() ? 0 : 1;
        if (shotMode && m_frameIdx == 3) {
            vkDeviceWaitIdle(m_ctx.device());
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
