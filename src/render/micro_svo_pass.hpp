#pragma once
// GPU overlay for SEPARATE micro-voxel SVO worlds (micro_svo_pass.hpp).
//
// Mirrors SvoPass, but the data it uploads is a set of INDEPENDENT fine-lattice
// SVO worlds (micro_svo.hpp) rather than the main world. Its buffers, its
// descriptor set and its pipeline are all private: nothing here is shared with
// SvoPass, so a micro object can never be mistaken for world geometry, and the
// 0.1 m world's render path is not touched at all.
//
// Its output is not a new target. The pass reads the main backend's own G-buffer
// (hdr / gpos / gnorm) and overwrites a pixel only where a micro surface is
// nearer than what the main backend already resolved there. That makes the
// world/micro boundary fall out of a distance comparison instead of a depth
// attachment, and it is why no edit to the splat or water pass is needed.
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "render/svo_pass.hpp" // RaymarchPush (same camera push layout)
#include "voxel/micro_svo.hpp"
#include "voxel/worldfile.hpp"
#include <cstdint>
#include <vector>

namespace vf {

class MicroSvoPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // Upload the micro worlds. Worlds are CONCATENATED into the pass's own
    // buffers and described by a per-world table (voxel size, gridN, chunkM and
    // the base of each world's grid/chunkInfo slice). Each world's chunk bases
    // are rebased onto the concatenated arrays here, so the shader indexes them
    // with one flat offset and never needs a second indirection.
    // Blocking (immediate submits), like SvoPass::setWorld.
    bool setMicroWorlds(const std::vector<voxel::MicroWorld>& worlds);

    // Set the LIVE PREVIEW offset of one micro world (a staged Move delta).
    // The field stays where it was baked; the shader offsets the RENDERED
    // position, so a drag is instant and costs no re-bake. `active` clears it.
    void setWorldPreview(size_t worldIndex, glm::vec3 offset, bool active);

    // Set the LIVE PREVIEW rotation of one micro world (a staged trackball
    // rotation about `pivot`). Same contract as the Move offset: the field
    // and the material lookup stay in the baked frame, and the RENDERED
    // position is rotated out of it, so a drag costs no re-bake. A micro
    // object has no world owner id, so the splat pass's rotate preview cannot
    // show it - this is its only live rotation.
    void setWorldRotation(size_t worldIndex, glm::mat3 R, glm::vec3 pivot,
                          bool active);

    bool empty() const { return m_worldCount == 0; }
    size_t worldCount() const { return m_worldCount; }
    size_t gpuBytes() const;

    // Shared-model resources. The textures are the SAME views the other passes
    // already hold, but this pass owns its own descriptors, so binding them
    // here cannot disturb the splat/SVO sets.
    void updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage,
                           const Image3D& gnormImage);
    void setHeightmapView(VkImageView view) { m_heightView = view; }
    void setObjVolumeView(VkImageView view) { m_objVolView = view; }
    void setTexAtlas(VkImageView view, VkSampler sampler, VkBuffer tableUbo);
    void setIrrVolView(VkImageView view);
    void setLights(const vf::voxel::LightUBO& lights);

    // Record the overlay dispatch. Must run AFTER the main backend has written
    // its G-buffer for this frame: gpos is the depth reference the pass reads.
    // A no-op when no micro world is loaded, so the cost with the feature off is
    // a single branch.
    void record(VkCommandBuffer cmd, const RaymarchPush& push);

private:
    struct Ssbo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        size_t bytes = 0;
    };
    bool uploadSsbo(Ssbo& s, const void* data, size_t bytes);
    void destroySsbo(Ssbo& s);
    void bindMicroBuffers();

    const Context* m_ctx = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    // The micro worlds' own pools. These are NOT the world's SVO buffers.
    Ssbo m_grid, m_childBase, m_payload, m_handles, m_bricks, m_chunkInfo;
    // The DENSE signed-distance fields, one byte per cell, all worlds
    // concatenated. Separate from the bricks because the field has to be
    // CONTINUOUS across brick boundaries (see MicroWorld::field).
    Ssbo m_field;
    // Per-world scale table (std140): 4 vec4 per world, world count in the
    // first ivec4. Host-visible so setMicroWorlds can write it directly.
    Buffer m_worldTable {};
    size_t m_worldCount = 0;

    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;
    VkImageView m_irrView = VK_NULL_HANDLE;
    VkSampler m_irrSampler = VK_NULL_HANDLE;
    Buffer m_lightsBuf {};
};

} // namespace vf