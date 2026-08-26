#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace vf {

// Push-constant block for the SVO ray-march compute shader (128 bytes).
struct alignas(16) RaymarchPush {
    glm::vec4 camPos;
    glm::vec4 camRight;
    glm::vec4 camUp;
    glm::vec4 camFwd;
    glm::vec4 a; // tanHalfFov, aspect, extentX, extentY
    glm::vec4 b; // worldSize, voxelSize, gridN, frameIdx
    glm::vec4 sunDir; // normalized direction TOWARD the sun (xyz)
    glm::vec4 misc;   // x = animation time (seconds); rest unused
};
static_assert(sizeof(RaymarchPush) == 128);

// Compute pass rendering the chunked-SVO world (svo_raymarch.comp).
class SvoPass {
public:
    bool init(const Context& ctx);
    void destroy();

    void setWorld(const std::vector<int32_t>& chunkGrid,
                  const std::vector<uint32_t>& childBase,
                  const std::vector<uint32_t>& payload,
                  const std::vector<uint32_t>& handles,
                  const std::vector<uint32_t>& bricks);

    void updateDescriptors(const Image3D& outImage);
    void setHeightmapView(VkImageView view);
    void setObjVolumeView(VkImageView view);
    void record(VkCommandBuffer cmd, const RaymarchPush& push) const;

private:
    struct Ssbo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
    };
    bool uploadSsbo(Ssbo& s, const void* data, size_t bytes);
    void destroySsbo(Ssbo& s);

    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    Ssbo m_grid, m_childBase, m_payload, m_handles, m_bricks;
    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;
};

} // namespace vf
