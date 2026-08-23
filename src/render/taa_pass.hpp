#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"

namespace vf {

class TaaPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // current = offscreen after raymarch (GENERAL), history = previous frame's output (GENERAL), out = resolve target (GENERAL)
    void updateDescriptors(VkImageView currentView, VkImageView historyView, VkImageView outView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height, float historyBlend, bool firstFrame) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
