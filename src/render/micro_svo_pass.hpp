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

    // Set the AUTONOMOUS BEHAVIOUR's transform of one micro world: an
    // offset and a rotation about a pivot. Same contract as the Move offset
    // and the Rotate preview - the field stays in the baked frame and the
    // transform is applied when it is sampled, so a walk costs no re-bake.
    // `active` clears it (the object is static again).
    void setWorldAnimation(size_t worldIndex, glm::vec3 offset, glm::mat3 R,
                           glm::vec3 pivot, bool active);

    // Rewrite the SKELETON's bone rotations of one micro world. The gait
    // drives them every frame; the segments and the joints stay as they were
    // uploaded.
    void setBoneRotations(size_t worldIndex, const std::vector<glm::mat3>& rot,
                          const std::vector<glm::vec3>& xform);
    // Re-upload a world's bone SEGMENTS and radii. The comment above says the
    // segments "stay as they were uploaded" - true of the gait, but the
    // studio EDITS them (a bone's length and influence radius), and the
    // shader's weight kernel reads both from here. Without this an edit moved
    // the drawn skeleton and the CPU rig while the body kept the old capsule.
    void setBoneSegments(size_t worldIndex, const std::vector<glm::vec3>& a,
                         const std::vector<glm::vec3>& b,
                         const std::vector<float>& radius);

    // Upload the MESH of one micro world: its REST-pose vertices in the
    // object's ANCHORED frame (metres, with the layer's world offset already
    // applied) and the owning bone index + 1 in w (0 = unskinned). One record
    // per triangle corner, 16 B each.
    //
    // Setting a mesh makes the world RASTER-OWNED: the compute tracer stops
    // drawing its rest pose and a raster pass draws the posed mesh instead,
    // with the field demoted to the shading source (material, texture,
    // normal, AO, occlusion). That is what removes the traced path's whole
    // failure class - a hole, a cut or a ghost can only come from geometry the
    // field invented, and here the geometry is the mesh.
    //
    // Call AFTER setMicroWorlds, which resets every world's flags.
    bool setMesh(size_t worldIndex, const std::vector<glm::vec4>& verts);

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
    // Per-frame sun-space shadow camera (world -> sun clip) + enable. Off keeps
    // the field-march verdict byte-identical.
    void setSunShadow(const glm::mat4& viewProj, bool enabled,
                      bool fieldFreeShading = false);

    // Record the overlay dispatch. Must run AFTER the main backend has written
    // its G-buffer for this frame: gpos is the depth reference the pass reads.
    // A no-op when no micro world is loaded, so the cost with the feature off is
    // a single branch.
    //
    // `frameSlot` is the frame loop's own in-flight slot (m_frameIdx %
    // kMaxFramesInFlight). The pass takes its per-frame resources — the mesh
    // depth attachment — from that slot, because the slot is the unit the
    // frame loop's fence proves complete, so nothing the pass destroys at the
    // start of a frame can still be read by a frame in flight. Callers pass
    // their real slot; the default keeps a caller that has no slots (or no mesh
    // world) correct rather than silently sharing one.
    void record(VkCommandBuffer cmd, const RaymarchPush& push, uint32_t frameSlot = 0);

private:
    struct Ssbo {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        size_t bytes = 0;
    };
    bool uploadSsbo(Ssbo& s, const void* data, size_t bytes);
    void destroySsbo(Ssbo& s);
    void bindMicroBuffers();
    bool ensureMeshDepth(uint32_t slot, uint32_t w, uint32_t h);
    bool ensureMeshScene(uint32_t w, uint32_t h);
    bool ensureSunDepth();

    const Context* m_ctx = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    // The raster path's pipeline: same layout, same set, depth-only.
    VkPipeline m_meshPipeline = VK_NULL_HANDLE;
    // The raster path's DEPTH PREPASS: the vertex stage alone, so it writes
    // depth and can not write a G-buffer texel. `m_meshPipeline` then tests
    // EQUAL against it, which is what leaves exactly one surviving writer per
    // pixel (see the comment at its creation).
    VkPipeline m_meshDepthPipeline = VK_NULL_HANDLE;

    // Workstream B: per-frame sun-space shadow map. The posed meshes are
    // rasterized from the sun into m_sunDepthRT (a COLOR attachment), then
    // COPIED into m_sunDepth (a STORAGE image bound at binding 29). Two images
    // are required: writing an image as a colour attachment while it is also
    // bound as a storage image in the same pass is a hazard that silently drops
    // the writes - the cause of the first attempt's "clear value only" map.
    VkPipeline m_sunPipeline = VK_NULL_HANDLE;
    Image3D m_sunDepthRT {};      // R32F colour attachment (render target)
    Image3D m_sunDepthAttach {};  // D32 depth for nearest-wins
    Image3D m_sunDepth {};        // R32F storage (binding 29, shader-read)
    VkExtent2D m_sunDepthExtent { 0, 0 };
    static constexpr uint32_t kSunDepthRes = 1024;
    Buffer m_sunUbo {};           // SunShadow UBO (viewProj + params), binding 28
    bool m_sunShadowOn = false;

    // The micro worlds' own pools. These are NOT the world's SVO buffers.
    Ssbo m_grid, m_childBase, m_payload, m_handles, m_bricks, m_chunkInfo;
    // The DENSE signed-distance fields, one byte per cell, all worlds
    // concatenated. Separate from the bricks because the field has to be
    // CONTINUOUS across brick boundaries (see MicroWorld::field).
    Ssbo m_field;
    // The RASTER MESH worlds' vertices, all objects concatenated:
    // vec4(rest.xyz, bone + 1). A world's slice is addressed with the draw's
    // firstVertex, so one buffer and one pipeline serve every object.
    Ssbo m_meshVertsBuf;
    std::vector<std::vector<glm::vec4>> m_meshVerts;
    size_t m_meshWorldCount = 0;
    // Per-frame-slot depth attachment (D32, linear), kMeshSlots of them. It
    // exists only so a mesh occludes itself and the other mesh objects; the
    // world/micro boundary stays the distance comparison against gpos the
    // traced path already makes, and the main backend's depth is never touched.
    //
    // WHY SLOTS AND NOT ONE IMAGE — and what it is NOT.
    //
    // What it IS: hygiene with a latent defect behind it. Every frame CLEARS
    // this attachment, and a clear is destructive, so with one image frame
    // N+1's clear is unordered against frame N's depth tests: the in-flight
    // fence only proves the submission THREE frames back complete, and a
    // barrier cannot reach across submissions. Indexing the attachment by the
    // slot the fence DOES prove complete removes the defect by construction,
    // which is why it is here.
    //
    // What it is NOT: the fix for the mesh flicker. Measured 2026-11-12, the
    // duplicate-pose pair still differed by 4,843 px after this change, and
    // 4,518 px with `VF_TEST_SERIAL_SUBMIT=1` draining the queue after every
    // submit — so no cross-frame overlap was involved at all. The flicker was
    // LATE FRAGMENT TESTS: this pass writes the G-buffer with `imageStore`
    // side effects, so the driver cannot run the depth test before the
    // fragment shader, both walls of the (culling-off, closed) mesh execute,
    // both store, and the losing store is never un-done — so the surviving
    // pixel was whichever fragment happened to store last. `VF_MESH=0` on the
    // same camera and pose reads 0 px, which is the proof the raster pass owns
    // it. See .opencode/wiki/concepts/render-nondeterminism.md.
    static constexpr int kMeshSlots = 3; // == kMaxFramesInFlight (app/app.hpp)
    Image3D m_meshDepth[kMeshSlots] {};
    VkExtent2D m_meshDepthExtent[kMeshSlots] { {}, {}, {} };
    // A SNAPSHOT of gpos taken once per frame BEFORE the mesh draw. The mesh
    // fragment needs the world surface's distance as its depth reference, but
    // it also WRITES gpos: reading the image it writes is a read/write hazard
    // inside one raster pass, so a fragment could see a sibling's own position.
    // dot(gp - ro, rd) then differs from that sibling's t by up to a pixel
    // footprint - larger than the half-voxel the boundary test tolerates - so
    // the verdict (world pixel vs coat) depended on draw completion order and
    // flipped from run to run (measured: ~4 k pixels between two runs of the
    // same pinned pose, 96 % on the object's outline). Reading a snapshot
    // taken before the pass turns that into a plain read.
    Image3D m_meshScene {};
    VkExtent2D m_meshSceneExtent { 0, 0 };
    // The live gpos image (set by updateDescriptors): the snapshot's copy
    // source. The view is not enough - vkCmdCopyImage needs the image.
    VkImage m_gposImage = VK_NULL_HANDLE;
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