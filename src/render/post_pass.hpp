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
    // Brush depth indicator: three world points (hit, far end, fixed end) plus
    // the brush tint colour, drawn by the post pass because the splat/SVO tint
    // physically cannot show depth (it only marks existing surfels, and the
    // extra volume is solid material or empty air).
    void setDepthMarker(const glm::vec4& hit, const glm::vec4& far,
                        const glm::vec4& near, const glm::vec4& tint)
    {
        m_depthHit = hit;
        m_depthFar = far;
        m_depthNear = near;
        m_depthTint = tint;
    }

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
    glm::vec4 m_depthHit { 0.f };
    glm::vec4 m_depthFar { 0.f };
    glm::vec4 m_depthNear { 0.f };
    glm::vec4 m_depthTint { 0.f };
};

} // namespace vf
