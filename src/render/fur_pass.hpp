#pragma once
// Fur pass: the translucent hair-card layer over the micro-voxel objects.
//
// It is a COMPUTE pass, like the micro overlay it follows, and it composites
// into the SAME HDR image the micro overlay wrote. Two consequences fall out
// of that:
//
//   * The fur can be TRANSLUCENT without touching the opaque pipeline. The
//     world's own splats cannot: their opaque base seal (the nearest
//     fragment written at alpha 1) is exactly what turns a lone silhouette
//     splat into a hard-edged blob, and no amount of kernel tuning fixes
//     that. The fur is a separate layer, so transparency is correct by
//     construction and the sky only shows where a card reaches past the
//     body's silhouette - which is what fur should do.
//   * The depth reference is free: the G-buffer's world position already
//     holds the nearest opaque surface (the world, the water plane, or the
//     micro body the overlay drew), so a card behind it is dropped without a
//     depth attachment.
//
// The pass is INERT when no micro object carries fur: the dispatch is
// skipped, so the frame is byte-identical to the frame without it (the same
// escape hatch the rest of the renderer keeps).
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include "render/svo_pass.hpp" // RaymarchPush (the same camera push layout)
#include "voxel/fur_cards.hpp"

#include <cstdint>
#include <vector>

namespace vf {

class FurPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // Upload the cards. `objects` is one entry per micro object that carries
    // fur, in the SAME order as the micro overlay's worlds (so the preview
    // and the rotation preview can be mirrored onto them one for one).
    bool setObjects(const std::vector<voxel::FurObject>& objects);

    // The live preview state, mirrored from the micro overlay so the fur
    // follows the object it belongs to.
    void setWorldPreview(size_t worldIndex, glm::vec3 offset, bool active);
    void setWorldRotation(size_t worldIndex, glm::mat3 R, glm::vec3 pivot, bool active);
    // The autonomous behaviour's transform (the whole-object walk), mirrored
    // from the micro overlay so the coat and the body are always in the same
    // pose. Without it the coat would hang in the air where the animal used
    // to be.
    void setWorldAnimation(size_t worldIndex, glm::vec3 offset, glm::mat3 R,
                           glm::vec3 pivot, bool active);

    bool empty() const { return m_objectCount == 0; }
    size_t objectCount() const { return m_objectCount; }
    size_t cardCount() const { return m_cardCount; }
    size_t gpuBytes() const;

    // The shared-model resources. These are the SAME views the other passes
    // already hold; this pass owns its own descriptors, so binding them here
    // cannot disturb the splat/SVO sets.
    void updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage);
    void setHeightmapView(VkImageView view) { m_heightView = view; }
    void setObjVolumeView(VkImageView view) { m_objVolView = view; }
    void setTexAtlas(VkImageView view, VkSampler sampler, VkBuffer tableUbo);
    void setIrrVolView(VkImageView view);
    void setLights(const vf::voxel::LightUBO& lights);

    // Record the dispatch. Must run AFTER the micro overlay has written the
    // G-buffer for this frame: the G-buffer is the depth reference this pass
    // reads. A no-op when no object carries fur.
    void record(VkCommandBuffer cmd, const RaymarchPush& push);

private:
    struct Ssbo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        size_t bytes = 0;
    };
    bool uploadSsbo(Ssbo& s, const void* data, size_t bytes);
    void destroySsbo(Ssbo& s);
    // The 1x1 black environment the IBL terms read. The environment is black
    // everywhere in this renderer (the splat pass does the same), so this
    // exists only to complete the descriptor set: common_base.glsl declares
    // both resources.
    bool createBlackEnv(const Context& ctx);

    const Context* m_ctx = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Ssbo m_cards {};
    Ssbo m_cellStart {};
    Ssbo m_cellItems {};

    Buffer m_table {};   // the per-object table (std140)
    Buffer m_lightsBuf {};

    Image3D m_blackCube {};
    Image3D m_blackLut {};
    VkSampler m_blackSampler = VK_NULL_HANDLE;
    VkSampler m_irrSampler = VK_NULL_HANDLE;

    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;

    size_t m_objectCount = 0;
    size_t m_cardCount = 0;
};

}  // namespace vf
