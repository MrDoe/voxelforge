#pragma once
#include "rhi/context.hpp"
#include "render/raymarch_pass.hpp" // RaymarchPush (shared push layout)
#include <glm/glm.hpp>
#include <vector>

namespace vf {

struct SplatVertexData {
    std::vector<glm::vec4> posRadius; // xyz + radius
    std::vector<glm::vec4> colors;    // legacy baked (kept for compat)
    std::vector<glm::vec4> albedoAO;  // rgb albedo, a = AO
    std::vector<glm::vec4> normalMat; // xyz normal, w = matId
};

// Renders surface point-splats as Gaussian sprites into the offscreen image.
// v0 of the M7 gaussian-splat path: isotropic, unsorted, additive-look.
class SplatPass {
public:
    bool init(const Context& ctx, VkFormat colorFormat, const SplatVertexData& data);
    void destroy();

    void setOutputView(VkImageView view) { m_outView = view; }
    void record(VkCommandBuffer cmd, VkExtent2D extent, const RaymarchPush& push) const;
    size_t count() const { return m_count; }
    void updateSorting(const glm::vec3& camPos, const glm::vec3& camFwd);

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkBuffer m_buf = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    VkImageView m_outView = VK_NULL_HANDLE;
    size_t m_count = 0;
    std::vector<glm::vec4> m_origPos;
    std::vector<glm::vec4> m_origCol;
    std::vector<glm::vec4> m_origAlbedoAO;
    std::vector<glm::vec4> m_origNormalMat;
};

} // namespace vf
