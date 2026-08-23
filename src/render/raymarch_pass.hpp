#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <glm/glm.hpp>

namespace vf {

// Push-constant block shared with raymarch.comp (112 bytes).
struct alignas(16) RaymarchPush {
    glm::vec4 camPos;
    glm::vec4 camRight;
    glm::vec4 camUp;
    glm::vec4 camFwd;
    glm::vec4 a; // tanHalfFov, aspect, extentX, extentY
    glm::vec4 b; // worldSize, maxEncodedDist, voxelSize, frameIdx
    glm::vec4 sunDir; // normalized direction TOWARD the sun (xyz)
};
static_assert(sizeof(RaymarchPush) == 112);

class RaymarchPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // Rebind after (re)creating volume images or the offscreen target.
    void updateDescriptors(const Image3D& sdfVol, VkSampler sdfSampler,
                           const Image3D& albedoVol, VkSampler albedoSampler,
                           const Image3D& outImage);

    void record(VkCommandBuffer cmd, const RaymarchPush& push) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
