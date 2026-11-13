// The ANIMATION STUDIO frame.
//
// The studio is not a second renderer: it is the micro-overlay chain on its
// own. The world backend (splat or SVO), the water plane, the surfel stream
// and every photorealism pass are skipped, the three G-buffer targets are
// cleared to a neutral backdrop, and the micro worlds composite into that.
// The post pass still runs, because the micro shader writes linear HDR and
// the tonemap is what turns it into the image the user authoring the clip
// sees.
//
// This lives in ONE place because the interactive and headless paths had
// already drifted once: a --shot of the studio rendered the object while the
// window showed the full hamlet behind it, so "what the editor shows" and
// "what the test checks" were two different pictures.
#include "app/app.hpp"

namespace vf {
namespace app {

namespace {

// Make an image readable+writeable by the passes that follow the clear. The
// clear is a transfer op, so it needs its own stage/access pair; the consumers
// need read AND write (the micro overlay composites into the image it reads),
// and the FRAGMENT stage is included because the micro pass's raster half (the
// mesh worlds) writes these images too.
void barrierForComputeReadWrite(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT |
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT |
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

void computeToCompute(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

void App::recordStudioFrame(VkCommandBuffer cmd, const vf::RaymarchPush& push)
{
    // -- geometry ---------------------------------------------------------
    // The studio has no world, so the G-buffer starts EMPTY rather than
    // "whatever the last frame left". gpos stays zero, which the micro
    // shader reads as "no world surface here" (it then wins any pixel its
    // trace hits), and gnorm is unused by the micro pass but must not be
    // stale for the post pass's neighbourhood reads.
    vf::transitionImage(cmd, m_hdr.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vf::transitionImage(cmd, m_gpos.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vf::transitionImage(cmd, m_gnorm.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkImageSubresourceRange rng { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    // A dark neutral, not pure black: the object's silhouette has to read
    // against it and an accidental lit-vs-unlit mistake shows as a colour
    // shift rather than as "black on black".
    VkClearColorValue backdrop { { 0.02f, 0.025f, 0.03f, 1.f } };
    VkClearColorValue empty { { 0.f, 0.f, 0.f, 0.f } };
    vkCmdClearColorImage(cmd, m_hdr.img, VK_IMAGE_LAYOUT_GENERAL, &backdrop, 1, &rng);
    vkCmdClearColorImage(cmd, m_gpos.img, VK_IMAGE_LAYOUT_GENERAL, &empty, 1, &rng);
    vkCmdClearColorImage(cmd, m_gnorm.img, VK_IMAGE_LAYOUT_GENERAL, &empty, 1, &rng);
    barrierForComputeReadWrite(cmd);

    // -- micro worlds -----------------------------------------------------
    // Same ordering as the world paths: micro overlay first, the fur cards
    // over it (their depth reference is the G-buffer the overlay just wrote),
    // then post.
    if (!m_microSvoPass.empty()) {
        m_microSvoPass.record(cmd, push, uint32_t(m_frameIdx % kMaxFramesInFlight));
        computeToCompute(cmd);
    }
    if (!m_furPass.empty()) {
        m_furPass.record(cmd, push);
        computeToCompute(cmd);
    }

    const uint32_t profBase = uint32_t(m_frameIdx % kMaxFramesInFlight) * kProfMarks;
    auto profMark = [&](uint32_t mark) {
        if (m_profPool)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 m_profPool, profBase + mark);
    };
    profMark(1);

    // -- post -------------------------------------------------------------
    vf::transitionImage(cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_postPass.record(cmd, push);
    profMark(2);
}

} // namespace app
} // namespace vf
