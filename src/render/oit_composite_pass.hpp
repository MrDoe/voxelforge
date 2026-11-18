#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>

namespace vf {

// Final composite of the --mode 3dgs weighted-blended OIT chain. Reads the
// premultiplied-colour/alpha accumulation and the transmittance product the
// splat pass wrote, plus a sky backdrop, and writes the linear HDR image the
// post/FX chain consumes. No push constants and no host-visible buffers: the
// pass is a pure image-to-image function, so a descriptor is all it owns.
class OitCompositePass {
public:
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView accum, VkImageView reveal,
                           VkImageView backdrop, VkImageView hdr,
                           VkImageView base);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
