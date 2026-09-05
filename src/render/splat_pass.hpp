#pragma once
// Gaussian-surfel raster backend (splat.vert / splat.frag).
//
// Consumes the chunk-sorted SurfelSet built by voxel::buildSurfels and draws
// it with 2D Gaussian disks (flat, normal-aligned, exact ray/plane depth)
// into the same m_hdr + m_gpos targets the SVO ray-marcher writes, so the
// post pass (bloom/AgX/outline), TAA and the headless --shot path work
// unchanged for both renderers.
//
// Frame: sky fullscreen triangle (no depth) -> one instanced quad draw per
// visible chunk (opaque, depth test+write, alpha blend for the AA annulus)
// -> water surfels (blended, depth test, no write).
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "render/svo_pass.hpp" // RaymarchPush (shared push layout)
#include <cstdint>
#include <vector>

namespace vf {

class SplatPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // Upload a fresh surfel set (device idle; called from applyWorldReload).
    // waterStart = first index of appended water surfels (== count if none).
    void setSurfels(const void* data, size_t bytes, size_t count,
                    const std::vector<uint32_t>& chunkRange, uint32_t waterStart);
    // Depth target follows the offscreen extent (D32_SFLOAT).
    bool recreateDepth(uint32_t w, uint32_t h);
    void updateDescriptors(VkImageView hdrView, VkImageView gposView,
                           VkImageView heightView, VkImageView objVolView);

    // Per-frame kernel tuning (HUD): kernel 0 = compact / 1 = gaussian,
    // core = opaque-core threshold, extent = quad half-size in radii.
    void setParams(float kernel, float core, float extent)
    {
        m_params = glm::vec4(kernel, core, extent, 0.0f);
    }

    // Record sky + opaque chunks + water. Assumes hdr/gpos already in
    // GENERAL and m_depth in DEPTH_ATTACHMENT_OPTIMAL (App transitions).
    void record(VkCommandBuffer cmd, const RaymarchPush& push, VkExtent2D extent);

    size_t surfelCount() const { return m_count; }
    uint32_t waterStart() const { return m_waterStart; }
    const Image3D& depthImage() const { return m_depth; }

private:
    bool createPipelines(VkFormat hdrFormat);
    void frustumPlanes(const RaymarchPush& push, glm::vec4 planes[6]) const;
    bool chunkVisible(const glm::vec4 planes[6], uint32_t chunk) const;
    // one instanced-quad draw per visible opaque chunk (shared by the depth
    // prepass and the shading pass)
    void drawChunks(VkCommandBuffer cmd, const RaymarchPush& push) const;

    const Context* m_ctx = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipeline m_skyPipe = VK_NULL_HANDLE;
    VkPipeline m_opaquePipe = VK_NULL_HANDLE;
    VkPipeline m_waterPipe = VK_NULL_HANDLE;

    VkBuffer m_surfelBuf = VK_NULL_HANDLE;
    VmaAllocation m_surfelAlloc = VK_NULL_HANDLE;
    Image3D m_depth {};
    Buffer m_paramsBuf {}; // persistently mapped 16 B kernel-tuning UBO
    glm::vec4 m_params { 0.0f, 0.55f, 1.02f, 0.0f };

    size_t m_count = 0;
    uint32_t m_waterStart = 0;
    std::vector<uint32_t> m_chunkRange;
    VkImageView m_hdrView = VK_NULL_HANDLE;
    VkImageView m_gposView = VK_NULL_HANDLE;
    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;
};

} // namespace vf
