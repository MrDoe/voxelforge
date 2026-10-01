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

// Interactive mode: acquire a swapchain image, record the visible scene, run
// TAA, draw the HUD, submit and present. Dedicated acquire semaphore per
// swapchain image: avoids the NVIDIA/X11 present deadlock seen with per-frame-
// slot reuse. The body stays one function because acquire, the command buffer,
// the TAA history and the present info share a dozen locals that would each
// have to be threaded through a seam.
int App::recordInteractiveFrame(FrameSync& fr, const FrameInputs& fx)
{
    const Args& args = m_args;
    const float tanHalfFov = tanHalfFov60();
    // Only reached on an interactive frame, but the TAA guard below still reads
    // the flag, so bind it rather than silently dropping the condition.
    const bool headlessRun = fx.headlessRun;
    if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] pre-acquire\n", (unsigned long long)m_frameIdx);
    uint32_t imgIdx = 0;
    // Dedicated acquire semaphore per swapchain image: avoids the
    // NVIDIA/X11 present deadlock seen with per-frame-slot reuse.
    VkSemaphore acquireSem = !m_acquireSems.empty()
                                 ? m_acquireSems[m_nextAcquire % m_acquireSems.size()]
                                 : fr.imageAvailable;
    VkResult acq = vkAcquireNextImageKHR(m_ctx.device(), m_swapchain.handle(),
                                         UINT64_MAX, acquireSem, VK_NULL_HANDLE,
                                         &imgIdx);
    m_nextAcquire = imgIdx; // its semaphore is reused when this image comes back
    if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] acquired %u\n", (unsigned long long)m_frameIdx, imgIdx);
    // a persistent failure here skips the rest of the frame body every
    // iteration, so the app keeps running and keeps consuming input while
    // nothing is ever drawn
    reportFrameResult(acq, "acquire", m_frameIdx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        handleResize();
            return kFrameDone;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
            return kFrameDone;

    vkResetFences(m_ctx.device(), 1, &fr.inFlight);
    vkResetCommandBuffer(fr.cmd, 0);

    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(fr.cmd, &bi);
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
                       float(m_offscreen.extent.width), float(m_offscreen.extent.height));
    push.b = glm::vec4(m_pushB.x, m_pushB.y, m_pushB.z, float(m_frameIdx % 1024));
    push.sunDir = m_sunDir;
    push.misc = glm::vec4(float(m_renderFlags), m_animTime, float(m_tonemapLook), m_exposure);
    // buried camera (inside solid): render shells two-sided this frame
    updateBuriedProbe();

    // splat raster or ray-march -> HDR + G-buffer --------------------
    if (m_renderMode == RenderMode::Splats) {
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
                           { m_offscreen.extent.width, m_offscreen.extent.height });
    } else {
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
    }
    profMark(1);
    // barrier: HDR/G-buffer written -> read by post pass
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    if (m_renderMode == RenderMode::Splats) {
        // forward raster writes attachments; the tile path (VF_TILE)
        // writes HDR/G-buffer from compute instead - cover both
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    } else {
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(fr.cmd, &di);
    // post pass reads HDR/G-buffer, writes LDR offscreen ---------------
    vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_postPass.record(fr.cmd, push);
    profMark(2);

    // photorealism toggles (G/H/J/K/L) run here so they affect what you
    // see; TAA resolves the effected image afterwards
    recordPhotorealism(fr.cmd, push);
    profMark(3);

    // TAA resolve (interactive only, not for headless tests) ----------
    VkImage taaSrc = m_offscreen.img;
    VkImageLayout taaSrcLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags2 taaSrcStage =
        VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    VkAccessFlags2 taaSrcAccess =
        VkAccessFlags2(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (m_taaEnabled && !headlessRun) {
        // current -> SHADER_READ for TAA
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            taaSrcLayout, VK_IMAGE_LAYOUT_GENERAL,
                            taaSrcStage, taaSrcAccess,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        // Ping-pong: read history from idx, write resolved into the OTHER
        // buffer, then make that the read buffer next frame (exact 1-frame
        // history, no stale/garbage read).
        const uint32_t hidx = uint32_t(m_taaHistoryIdx);
        const uint32_t widx = hidx ^ 1u;
        VkImageView histView = m_taaHistory[hidx].view;
        // history already in GENERAL from previous frame's copy, make it readable
        // (first frame history is undefined but TAA handles firstFrame)
        m_taaPass.updateDescriptors(m_offscreen.view, histView, m_taaResolved.view, m_gpos.view);
        // history -> SHADER_READ (if not first frame, already GENERAL)
        // resolved -> GENERAL for write
        vf::transitionImage(fr.cmd, m_taaResolved.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_taaPass.record(fr.cmd, m_offscreen.extent.width, m_offscreen.extent.height,
                         m_taaFirstFrame ? 0.0f : m_taaBlend, m_taaFirstFrame, m_prevCam);
        // TAA output -> TRANSFER_SRC for blit, and copy to history for next frame
        vf::transitionImage(fr.cmd, m_taaResolved.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        // copy resolved -> history (for next frame)
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {m_offscreen.extent.width, m_offscreen.extent.height, 1};
        // history need to be DST
        vf::transitionImage(fr.cmd, m_taaHistory[widx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(fr.cmd, m_taaResolved.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_taaHistory[widx].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        vf::transitionImage(fr.cmd, m_taaHistory[widx].img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        // also keep resolved as TRANSFER_SRC for blit (already)
        taaSrc = m_taaResolved.img;
        taaSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        taaSrcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        taaSrcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
        m_taaHistoryIdx = int(widx);
        m_taaFirstFrame = false;
        // remember this frame's camera so next frame can reproject history
        m_prevCam.pos = m_camera.pos;
        m_prevCam.right = m_camera.right();
        m_prevCam.up = m_camera.up();
        m_prevCam.fwd = m_camera.forward();
        m_prevCam.tanHalfFov = tanHalfFov;
        m_prevCam.aspect = float(m_swapchain.extent().width) / float(m_swapchain.extent().height);
    } else {
        // no TAA: offscreen -> TRANSFER_SRC directly
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            taaSrcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            taaSrcStage, taaSrcAccess,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        taaSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        taaSrcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        taaSrcAccess = VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    profMark(4);
    // swapchain -> transfer-dst, blit ----------
    vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkOffset3D b0 { 0, 0, 0 };
    VkOffset3D b1 { int(m_offscreen.extent.width), int(m_offscreen.extent.height), 1 };
    VkOffset3D s1 { int(m_swapchain.extent().width), int(m_swapchain.extent().height), 1 };
    VkImageBlit blit {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = b0;
    blit.srcOffsets[1] = b1;
    blit.dstOffsets[0] = b0;
    blit.dstOffsets[1] = s1;
    vkCmdBlitImage(fr.cmd, taaSrc, taaSrcLayout,
                   m_swapchain.image(imgIdx), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    drawHud();
    ImGui::Render();
    if (getenv("VF_IMGUI_DEBUG") && m_frameIdx == 5) {
        ImDrawData* dd = ImGui::GetDrawData();
        spdlog::warn("imgui dbg: valid={} display=({:.0f},{:.0f}) idxcount={} cmdlists={}",
                     dd ? 1 : 0, dd ? dd->DisplaySize.x : -1.f,
                     dd ? dd->DisplaySize.y : -1.f,
                     dd ? dd->TotalIdxCount : -1, dd ? dd->CmdListsCount : -1);
    }
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->TotalIdxCount > 0) {
        // swapchain -> color attachment for ImGui (LOAD keeps the blitted world)
        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (m_scenePreview) {
            // offscreen is TRANSFER_SRC after the blit; make it shader-readable for ImGui
            vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
        // With UseDynamicRendering we must open the render pass ourselves and
        // target the swapchain view; LOAD keeps the blitted world underneath.
        VkRenderingAttachmentInfo att { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        att.imageView = m_swapchain.imageViews()[imgIdx];
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
        VkRect2D area { { 0, 0 }, m_swapchain.extent() };
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(fr.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(dd, fr.cmd);
        vkCmdEndRendering(fr.cmd);
    }

    // VF_HUD_SHOT: blit the composed frame (HUD included) back to offscreen
    if (fx.hudShotFrame && m_frameIdx + 1 >= fx.hudShotFrame) {
        VkImageCopy region {};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { m_swapchain.extent().width, m_swapchain.extent().height, 1 };
        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx),
                            VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            m_scenePreview ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(fr.cmd, m_swapchain.image(imgIdx),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_offscreen.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx),
                            VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    }

    vf::transitionImage(fr.cmd, m_swapchain.image(imgIdx), VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    profMark(5);
    vkEndCommandBuffer(fr.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &fr.renderDone;
    if (vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, fr.inFlight) != VK_SUCCESS) {
        spdlog::critical("vkQueueSubmit failed");
        return 1;
    }

    if (getenv("VF_TRACE")) fprintf(stderr, "[f%llu] submitted\n", (unsigned long long)m_frameIdx);
    VkPresentInfoKHR pi { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &fr.renderDone;
    pi.swapchainCount = 1;
    pi.pSwapchains = m_swapchain.handlePtr();
    pi.pImageIndices = &imgIdx;
    VkResult pres = vkQueuePresentKHR(m_ctx.graphicsQueue(), &pi);
    reportFrameResult(pres, "present", m_frameIdx);
    forceFrameResults(m_frameIdx);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
        handleResize();

    ++m_frameIdx;
    if (fx.hudShotFrame && m_frameIdx >= fx.hudShotFrame) {
        vkQueueWaitIdle(m_ctx.graphicsQueue());
        std::vector<uint8_t> px;
        vf::readbackImage2D(m_ctx, m_offscreen.img, m_swapchain.extent().width,
                            m_swapchain.extent().height, px);
        FILE* fp = fopen(fx.hudShotPath.c_str(), "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", m_swapchain.extent().width,
                    m_swapchain.extent().height);
            for (size_t i = 0; i < px.size(); i += 4)
                fwrite(&px[i], 3, 1, fp);
            fclose(fp);
            spdlog::info("hud shot written: {}", fx.hudShotPath);
        }
        return 0;
    }
    if (args.smokeFrames > 0 && m_frameIdx % 200 == 0)
        spdlog::info("smoke progress: {} frames, avg {:.2f} ms", m_frameIdx, m_avgMs);

    if (args.selftest && m_frameIdx == 30)
        return runSelftest() ? 0 : 1;
    if (args.smokeFrames > 0 && m_frameIdx >= uint64_t(args.smokeFrames)) {
        spdlog::info("smoke: {} frames, avg {:.2f} ms, min {:.2f}, max {:.2f}",
                     m_frameIdx, m_avgMs, m_minMs, m_maxMs);
            return 0;
    }
    return kFrameDone;
}

} // namespace app
} // namespace vf
