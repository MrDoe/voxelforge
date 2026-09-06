#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include "render/svo_pass.hpp"


namespace vf {

class DepthOfFieldPass {
public:
    // Push block: shared camera push + focus params (x=focusDist m,
    // y=focalLength); total must stay <= 256 B.
    struct PC {
        RaymarchPush base;
        alignas(16) float focus[4];
    };
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView sceneView, VkImageView gposView, VkImageView outView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const RaymarchPush& push, float focusDist, float focalLength) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
