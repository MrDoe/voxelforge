#pragma once
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "voxel/world.hpp"
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

    // Upload the whole synthesized world (chunk-local handles + uChunkInfo).
    void setWorld(const voxel::GpuWorld& world);

    // Live-edit patch: replace one chunk's octree with a rebuilt pool. The
    // chunk's data is appended into reserved per-chunk regions (relocating a
    // chunk whose pool outgrew its slot, growing the array buffers if needed)
    // and the chunk's grid root + base table entry are updated in place.
    // Blocking (device idle + immediate submit): call between frames.
    void patchChunk(uint32_t chunk, const voxel::ChunkPool& pool);

    void updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage);
    void setHeightmapView(VkImageView view);
    void setObjVolumeView(VkImageView view);
    // Highlight feeds written to a persistently mapped UBO (binding 8):
    // slot 0 = selected voxel (strong warm), slot 1 = hover preview (faint).
    // xyz = voxel center (world), w = active flag.
    void setSelection(const glm::vec4& sel) { m_selFeed = sel; }
    void setHover(const glm::vec4& hov) { m_hovFeed = hov; }
    void record(VkCommandBuffer cmd, const RaymarchPush& push);

private:
    struct Ssbo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        size_t bytes = 0;
    };
    bool uploadSsbo(Ssbo& s, const void* data, size_t bytes);
    void destroySsbo(Ssbo& s);
    // Bind the 5 world SSBOs (grid/childBase/payload/handles/bricks) and the
    // per-chunk base table to set 0 bindings 1-5 and 11.
    void bindWorldBuffers();
    // Copy `bytes` into an SSBO at `offset` (staging + immediate submit).
    bool uploadRange(Ssbo& s, size_t offset, const void* data, size_t bytes);
    // Grow one SSBO to `minBytes` (device copy of the old contents).
    bool growSsbo(Ssbo& s, size_t minBytes);
    uint32_t readChunkRoot(uint32_t chunk) const;

    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    Ssbo m_grid, m_childBase, m_payload, m_handles, m_bricks;
    Ssbo m_chunkInfo; // uvec4 per chunk: nodeBase, childBase, brickBase, _
    // paged patch layout: per-chunk capacity for each array + relocation cursor
    std::vector<voxel::GpuChunkInfo> m_info; // CPU mirror of m_chunkInfo
    std::vector<uint32_t> m_capPayload, m_capHandles, m_capBricks; // per chunk
    uint32_t m_highPayload = 0, m_highHandles = 0, m_highBricks = 0;
    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;

    // persistently mapped 32 B uniform buffer: selection + hover feeds
    Buffer m_selection {};
    glm::vec4 m_selFeed { 0.f }; // staged on CPU, flushed in record()
    glm::vec4 m_hovFeed { 0.f };
};
} // namespace vf
