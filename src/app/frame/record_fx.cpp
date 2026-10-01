// The in-place photorealism chain on m_offscreen (post-tonemap LDR):
// SSR / SSAO / volumetric fog / motion blur / DoF per the G/H/J/K/L toggles.
// Shared by the headless and interactive frame paths so they cannot drift.
#include "app/app.hpp"

namespace vf {
namespace app {

void App::recordPhotorealism(VkCommandBuffer cmd, const vf::RaymarchPush& push)
{
    // In-place LDR chain on m_offscreen (GENERAL layout throughout).
    // Order: SSR adds reflections -> SSAO grounds contact areas -> volumetric
    // fog hazes valleys -> motion blur smears camera movement -> DoF pulls
    // focus. TAA (interactive) resolves afterwards.
    const uint32_t W = m_offscreen.extent.width, H = m_offscreen.extent.height;
    auto barrier = [&] {
        vf::transitionImage(cmd, m_offscreen.img, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    if ((m_renderFlags & (1 << 5)) != 0) {
        barrier();
        m_ssrPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_gnorm.view,
                                    m_offscreen.view);
        m_ssrPass.record(cmd, W, H, push);
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
                                     m_offscreen.view, m_offscreen.view);
        m_ssaoPass.record(cmd, W, H, push,
                          glm::vec4(float(m_ssaoDebug), m_ssaoStrength,
                                    m_ssaoRadius, m_ssaoBlur ? 1.0f : 0.0f));
    }
    if (m_volFogEnabled) {
        barrier();
        m_volFogPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_volFogPass.record(cmd, W, H, push);
    }
    if (m_motionBlurEnabled) {
        barrier();
        m_motionBlurPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_motionBlurPass.record(cmd, W, H, push, m_prevCam);
    }
    if (m_dofEnabled) {
        barrier();
        m_dofPass.updateDescriptors(m_offscreen.view, m_gpos.view, m_offscreen.view);
        m_dofPass.record(cmd, W, H, push, m_dofFocusDist, m_dofFocalLength);
    }
}

} // namespace app
} // namespace vf
