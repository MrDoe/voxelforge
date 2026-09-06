#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include "render/svo_pass.hpp"


namespace vf {

class EnvironmentPass {
public:
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView srcView, VkImageView envView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height, int face, int mip, int res, const RaymarchPush& push) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
