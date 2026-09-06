#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include "render/svo_pass.hpp"
#include "render/taa_pass.hpp" // TaaPrevCam for camera-velocity reconstruction


namespace vf {

class MotionBlurPass {
public:
    // Push block: shared camera push + previous-frame camera (vec4 lanes to
    // match the shader PC block; total must stay <= 256 B).
    struct PC {
        RaymarchPush base;
        alignas(16) float prevPos[4];
        alignas(16) float prevRight[4];
        alignas(16) float prevUp[4];
        alignas(16) float prevFwd[4];
        alignas(16) float prevParams[4]; // x=tanHalfFov, y=aspect
    };
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView currentView, VkImageView gposView, VkImageView outView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const RaymarchPush& push, const TaaPrevCam& prev) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
