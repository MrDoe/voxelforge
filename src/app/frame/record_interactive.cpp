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
    // The STUDIO is its own path: no world backend, no water, no
    // photorealism. It shares one implementation with the headless path
    // (frame/record_studio.cpp), so the window shows exactly what a --shot
    // of the studio renders.
    if (m_studio.active) {
        recordStudioFrame(fr.cmd, push);
        profMark(3); // no photorealism in the studio
    } else {
    // Dgs shares the splat raster's G-buffer + depth + culled draw list; it
    // then re-draws the surfels into the OIT targets and composites over HDR.
    if (m_renderMode == RenderMode::Splats || m_renderMode == RenderMode::Dgs) {

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
    // --mode 3dgs: accumulate all surfels (no depth rejection) into the OIT
    // targets, then composite into m_hdr with transmittance. The standard
    // record() above already filled the G-buffer + depth, which the FX chain
    // and micro overlay still read.
    if (m_renderMode == RenderMode::Dgs) {
        const VkExtent2D shotExtent { m_offscreen.extent.width, m_offscreen.extent.height };
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
        // OIT targets -> GENERAL for the composite's storage-image reads.
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
        // record() wrote m_hdr as a colour attachment. Copy it to m_oitBase so
        // the composite can keep solid interiors pixel-identical to the shipped
        // frame; the copy reads m_hdr and the composite overwrites it.
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
    // The micro pass is no longer compute-only: its raster half (the mesh
    // worlds) writes hdr/gpos/gnorm from the FRAGMENT stage, so the barrier
    // must order those writes against the world's colour-attachment stores
    // too. With COMPUTE alone the mesh fragment could land its store while the
    // world pass's store was still in flight - a write-after-write hazard on
    // the very pixels the mesh covers, which read as run-to-run flicker on the
    // object's outline.
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(fr.cmd, &di);
    // ---- micro-voxel overlay (SEPARATE SVO worlds) --------------------
    // Composites high-detail objects that live on their own fine lattice (a 30
    // cm vase at 5 mm) into the G-buffer the main backend just resolved. It
    // reads gpos as its depth reference - sky writes zero and the water pass
    // writes the plane - so a micro object standing in the shallows wins
    // against the lake plane in front of it and one sunk on the bed loses to
    // it, with no depth attachment and no change to either pass. Runs for both
    // render modes, after the barrier above has made the images readable.
    if (!m_microSvoPass.empty()) {
        m_microSvoPass.record(fr.cmd, push, uint32_t(m_frameIdx % kMaxFramesInFlight));
        // Src must include FRAGMENT: the mesh worlds' raster fragments write
        // the G-buffer, and the next stage (the fur draw, then post) reads it.
        VkMemoryBarrier2 mb2 { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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
        // The cards are a RASTER draw and the composite is a dispatch, so both
        // stages can have pending stores here.
        VkMemoryBarrier2 mb3 { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb3.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb3.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb3.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb3.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo di3 { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di3.memoryBarrierCount = 1;
        di3.pMemoryBarriers = &mb3;
        vkCmdPipelineBarrier2(fr.cmd, &di3);
    }
    // post pass reads HDR/G-buffer, writes LDR offscreen ---------------
    vf::transitionImage(fr.cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // See record_headless.cpp: one flag chooses the order, and post.comp's input
    // is uHdr, so the linear path is a reorder rather than a second pipeline.
    if (m_linearFx) {
        // photorealism on the linear HDR target, then the tone map
        recordPhotorealism(fr.cmd, push, true);
        profMark(2);
        if (m_autoExposure)
            m_expoPass.record(fr.cmd, uint32_t(push.a.z), uint32_t(push.a.w), push);
        m_postPass.record(fr.cmd, push);
    } else {
        if (m_autoExposure)
            m_expoPass.record(fr.cmd, uint32_t(push.a.z), uint32_t(push.a.w), push);
        m_postPass.record(fr.cmd, push);
        profMark(2);
        // photorealism toggles (G/H/J/K/L) run here so they affect what you
        // see; TAA resolves the effected image afterwards
        recordPhotorealism(fr.cmd, push);
    }
    profMark(3);
    }

    // TAA resolve (interactive only, not for headless tests) ----------
    VkImage taaSrc = m_offscreen.img;
    VkImageLayout taaSrcLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags2 taaSrcStage =
        VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    VkAccessFlags2 taaSrcAccess =
        VkAccessFlags2(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (m_taaEnabled && !headlessRun && !m_studio.active) {
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
    // Remember this frame's camera for NEXT frame's reprojection, in BOTH
    // branches. TAA needs it to blend history; the opt-in motion blur (K)
    // reprojects against the same m_prevCam, so it must advance every
    // interactive frame regardless of m_taaEnabled.
    advancePrevCam();
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
    drawTimeline();
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
