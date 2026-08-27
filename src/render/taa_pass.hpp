#pragma once
#include <glm/glm.hpp>
#include "rhi/context.hpp"
#include "rhi/resources.hpp"

namespace vf {

// Previous-frame camera, used to reproject the TAA history via the world-pos G-buffer.
struct TaaPrevCam {
    glm::vec3 pos = {0.0f, 0.0f, 0.0f};
    glm::vec3 right = {1.0f, 0.0f, 0.0f};
    glm::vec3 up = {0.0f, 1.0f, 0.0f};
    glm::vec3 fwd = {0.0f, 0.0f, 1.0f};
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
};

class TaaPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // current = offscreen after raymarch (GENERAL), history = previous frame's output (GENERAL),
    // out = resolve target (GENERAL), gpos = world-pos G-buffer (GENERAL, rgba16f)
    void updateDescriptors(VkImageView currentView, VkImageView historyView, VkImageView outView, VkImageView gposView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height, float historyBlend, bool firstFrame,
                const TaaPrevCam& prev) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
