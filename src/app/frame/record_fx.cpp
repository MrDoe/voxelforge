// The photorealism chain: SSR / SSAO / volumetric fog / motion blur / DoF per
// the G/H/J/K/L toggles. Shared by the headless and interactive frame paths so
// the two cannot drift apart.
//
// Which image the chain runs in place on is the whole design question.
//
// m_linearFx OFF (the default, and the shipped look): the chain runs on
// m_offscreen AFTER the post pass has tone mapped it. Historical.
//
// m_linearFx ON: the same chain runs on the LINEAR HDR target BEFORE the tone
// map, which is the order every photographic pipeline uses, for two reasons
// that are measurable rather than stylistic:
//   - HEADROOM. An AO multiply and a reflection add applied to an AgX-compressed
//     image can only darken, and can never carry energy past the ceiling the
//     tone map already imposed. The deep contact shadows and the bright glint
//     this chain exists to produce are capped before they start.
//   - FORMAT. ssr.comp, ssao_apply.comp, volumetric_fog.comp, motion_blur.comp
//     and dof.comp all declare their scene image `rgba16f`. Bound to m_offscreen
//     (VK_FORMAT_R8G8B8A8_UNORM) that is a declared-versus-actual storage image
//     mismatch: 8 declared bytes per texel against a 4-byte view, i.e. undefined
//     behaviour that survives only because this machine has no validation layers
//     installed. On the HDR target the declaration and the view finally agree.
//
// No shader change is needed to get the order, because post.comp's input IS
// uHdr: run the effects on m_hdr first and the tone map consumes what they
// produced for free.
//
// Three of the five passes GATHER — SSR marches a reflected ray across the
// screen, motion blur walks along a velocity vector, DoF gathers a disc — so
// each reads pixels it did not write. Binding one image as both source and
// destination for such a pass makes the result depend on the order the GPU
// happens to run the invocations, which is not reproducible; they therefore run
// through a scratch and are copied back. SSAO and the fog read only their own
// pixel (SSAO's occlusion term arrives in its own buffer), so they stay in
// place.
#include "app/app.hpp"

#include <functional>

namespace vf {
namespace app {

void App::recordPhotorealism(VkCommandBuffer cmd, const vf::RaymarchPush& push,
                             bool linearHdr)
{
    Image3D& tgt = linearHdr ? m_hdr : m_offscreen;
    const uint32_t W = tgt.extent.width, H = tgt.extent.height;
    // The source scope must include WRITES, not just the read the next dispatch
    // performs: naming only SHADER_STORAGE_READ leaves the previous pass's
    // stores outside the dependency, and the next dispatch may then sample a
    // half-written frame.
    auto barrier = [&] {
        vf::transitionImage(cmd, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    // Run one gathering pass out-of-place: scratch receives its output, then the
    // result is copied back over the chain image. The scratch is rgba16f, which
    // is also what the gathering shaders declare.
    auto gather = [&](const std::function<void()>& dispatch) {
        // Ping-pong exists for the LINEAR path, where both the scene and the
        // scratch are rgba16f and the shaders' declared format finally matches
        // the view. In the legacy LDR path the scene is rgba8 while those same
        // shaders declare rgba16f: writing to a float scratch makes the read
        // and write sides disagree and the composite turns to false colours.
        // Keep the historical in-place behaviour there (it is racy, but it is
        // the shipped look) until m_offscreen itself becomes an HDR image.
        if (!linearHdr) {
            barrier();
            return dispatch();
        }
        if (!m_fxScratch.img)
            return dispatch();  // no scratch bound: fall back to in-place
        barrier();
        vf::transitionImage(cmd, m_fxScratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        dispatch();
        vf::transitionImage(cmd, m_fxScratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vf::transitionImage(cmd, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy cp {};
        cp.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.extent = { W, H, 1 };
        vkCmdCopyImage(cmd, m_fxScratch.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tgt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        vf::transitionImage(cmd, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vf::transitionImage(cmd, m_fxScratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    };

    // Output view for the gathering passes: only the linear path can write to
    // the float scratch (see gather() above). The LDR path must stay in place.
    const VkImageView gatherOut =
        (linearHdr && m_fxScratch.view) ? m_fxScratch.view : tgt.view;

    // Order: SSR adds reflections -> SSAO grounds contact areas -> volumetric
    // fog hazes valleys -> motion blur smears camera movement -> DoF pulls
    // focus. TAA (interactive) resolves afterwards.
    // SSR ping-pong is not an optimisation: ssr.comp marches across the screen
    // and samples scene colour at pixels it did not write, so an in-place
    // dispatch is a data race (measured ~1500 px of difference between two
    // identical frames). It lands in a scratch and is copied back. On the LDR
    // path use main's dedicated m_ssrOut (RGBA8, bit-exact shipped look); on
    // the linear path use m_fxScratch (RGBA16F, matching the shaders' declared
    // format). `tgt` is whichever image the chain runs on this frame.
    if ((m_renderFlags & (1 << 5)) != 0) {
        Image3D& ssrScratch = linearHdr ? m_fxScratch : m_ssrOut;
        barrier();
        vf::transitionImage(cmd, ssrScratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_ssrPass.updateDescriptors(tgt.view, m_gpos.view, m_gnorm.view,
                                    ssrScratch.view, m_galb.view);
        m_ssrPass.record(cmd, W, H, push);
        vf::transitionImage(cmd, ssrScratch.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        vf::transitionImage(cmd, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy cp {};
        cp.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.extent = { W, H, 1 };
        vkCmdCopyImage(cmd, ssrScratch.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tgt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        vf::transitionImage(cmd, tgt.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    if ((m_renderFlags & (1 << 6)) != 0) {
        // scratch AO target: contents are per-frame, discard-and-transition
        vf::transitionImage(cmd, m_ssaoAo.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        barrier();
        m_ssaoPass.updateDescriptors(m_gpos.view, m_gnorm.view, m_ssaoAo.view,
                                     tgt.view, tgt.view);
        m_ssaoPass.record(cmd, W, H, push,
                          glm::vec4(float(m_ssaoDebug), m_ssaoStrength,
                                    m_ssaoRadius, m_ssaoBlur ? 1.0f : 0.0f));
    }
    if (m_volFogEnabled) {
        barrier();
        m_volFogPass.updateDescriptors(tgt.view, m_gpos.view, tgt.view);
        m_volFogPass.record(cmd, W, H, push);
    }
    if (m_motionBlurEnabled) {
        gather([&] {
            m_motionBlurPass.updateDescriptors(tgt.view, m_gpos.view, gatherOut);
            m_motionBlurPass.record(cmd, W, H, push, m_prevCam);
        });
    }
    if (m_dofEnabled) {
        gather([&] {
            m_dofPass.updateDescriptors(tgt.view, m_gpos.view, gatherOut);
            m_dofPass.record(cmd, W, H, push, m_dofFocusDist, m_dofFocalLength);
        });
    }
    // Leave the target ordered for whoever reads it next. In linear mode that is
    // the post pass, dispatched immediately after this returns, and the last
    // pass here only ever barrier()s BEFORE itself — so without this trailing
    // barrier the tone map can read the frame the previous effect left half
    // written.
    if (linearHdr)
        barrier();
}

} // namespace app
} // namespace vf
