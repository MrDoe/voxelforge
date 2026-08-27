#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "render/svo_pass.hpp" // RaymarchPush

namespace vf {

// Deferred post pass: reads the ray-march HDR + G-buffer, applies HDR bloom,
// AgX tonemapping (look/exposure from RaymarchPush.misc), and the selection
// outline, writing the final LDR image.
class PostPass {
public:
    bool init(const Context& ctx);
    void destroy();

    void updateDescriptors(VkImageView hdrView, VkImageView gposView, VkImageView outView);
    void record(VkCommandBuffer cmd, const RaymarchPush& push) const;

    void setSelection(const glm::vec4& sel) { m_selFeed = sel; }
    void setHover(const glm::vec4& hov) { m_hovFeed = hov; }

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    Buffer m_selection {};
    glm::vec4 m_selFeed { 0.f };
    glm::vec4 m_hovFeed { 0.f };
};

} // namespace vf
