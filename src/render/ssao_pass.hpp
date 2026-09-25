#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include "render/svo_pass.hpp"


namespace vf {

class SSAOPass {
public:
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView gposView, VkImageView gnormView,
                           VkImageView aoView, VkImageView currentView,
                           VkImageView outView);
    // `params` rides a local extra push range after the shared 128 B block:
    // x = debug (0 apply, 1 raw AO, 2 G-buffer normal), y = strength,
    // z = far-band radius (m), w = blur enable (0 = apply the raw AO).
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const RaymarchPush& push, const glm::vec4& params) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;      // ssao.comp (raw AO)
    VkPipeline m_applyPipeline = VK_NULL_HANDLE; // ssao_apply.comp (blur + apply)
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
