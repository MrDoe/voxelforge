#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include "render/svo_pass.hpp"

namespace vf {

// Scene-referred auto-exposure meter. Owns the SSBO that post.comp reads, so
// the measured value never has to round-trip through the CPU.
class ExposurePass {
public:
    // RaymarchPush is 128 B; the two trailing vec4s are the mode/key/rate/min
    // and the max-stop limit. Total 160 B, within the common 256 B push range.
    struct PC {
        RaymarchPush base;
        alignas(16) float params[4];
        alignas(16) float limits[4];
    };
    bool init(const Context& ctx);
    void destroy();
    void updateDescriptors(VkImageView hdrView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const RaymarchPush& push) const;

    VkBuffer exposureBuffer() const { return m_buf.buf; }
    // Only for diagnostics after the device is idle (the headless shot path):
    // reading this mid-frame without a fence would be a host-coherency race.
    float currentExposure() const;

private:
    const Context* m_ctx = nullptr;
    Buffer m_buf{};
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
