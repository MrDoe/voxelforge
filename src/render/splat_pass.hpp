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
    // waterChunkRange optionally buckets the trailing water surfels per chunk
    // (GRID_N^3 + 1 absolute offsets, like chunkRange) so off-screen water is
    // culled per frame; empty = draw all water in one call (legacy).
    // microStart optionally splits each chunk into [base, micro) (same
    // GRID_N^3 + 1 layout, absolute offsets); empty = no split. Distant
    // chunks skip their sub-pixel micro range (VF_MICRO_DIST, default 40 m).
    // lod1Range/lod2Range hold the merged-terrain LOD ring runs per chunk
    // (same layout); draw-time selection picks a ring by chunk distance
    // (VF_LOD1/VF_LOD2, default 20/60 m) with base-range fallback for
    // object-only chunks.
    void setSurfels(const void* data, size_t bytes, size_t count,
                    const std::vector<uint32_t>& chunkRange, uint32_t waterStart,
                    const std::vector<uint32_t>& waterChunkRange = {},
                    const std::vector<uint32_t>& microStart = {},
                    const std::vector<uint32_t>& lod1Range = {},
                    const std::vector<uint32_t>& lod2Range = {});
    // Depth target follows the offscreen extent (D32_SFLOAT).
    bool recreateDepth(uint32_t w, uint32_t h);
    void updateDescriptors(VkImageView hdrView, VkImageView gposView,
                           VkImageView heightView, VkImageView objVolView);

    // Kernel tuning: y = opaque-core threshold, z = quad half-size.
    // x = buried flag, managed via setBuried (not setParams).
    void setParams(float core, float extent)
    {
        m_params.y = core;
        m_params.z = extent;
    }
    // Runtime disk-radius multiplier, hotkeys [/] (0.5..2.0, default 1.0):
    // grows/shrinks every splat disk for coverage-vs-crispness tuning.
    void setRadiusScale(float s) { m_radiusScale = glm::clamp(s, 0.5f, 2.0f); }
    float radiusScale() const { return m_radiusScale; }
    // Camera embedded inside solid geometry (probed on the CPU per frame):
    // disables backface collapse so the surrounding shell renders instead
    // of flashing sky. Clear otherwise (fast exterior path).
    void setBuried(bool buried) { m_buried = buried; }

    // Record sky + opaque chunks + water. Assumes hdr/gpos already in
    // GENERAL and m_depth in DEPTH_ATTACHMENT_OPTIMAL (App transitions).
    void record(VkCommandBuffer cmd, const RaymarchPush& push, VkExtent2D extent);

    size_t surfelCount() const { return m_count; }
    uint32_t waterStart() const { return m_waterStart; }
    const Image3D& depthImage() const { return m_depth; }

private:
    bool createPipelines(VkFormat hdrFormat);
    bool createCullPipeline();
    void frustumPlanes(const RaymarchPush& push, glm::vec4 planes[6]) const;
    bool chunkVisible(const glm::vec4 planes[6], uint32_t chunk) const;
    // one instanced-quad draw per visible opaque chunk (shared by the depth
    // prepass and the shading pass)
    // Computes the frustum-culled, back-to-front sorted opaque chunk draws
    // plus the culled water-chunk draws into m_cpuDraws/m_cpuWaterDraws.
    // Chunks whose AABB nearest point lies beyond the rim fade distance
    // (m_rimDist, default 42 m) emit core-only commands: splat.frag ramps
    // coreD2 -> 1.0 by 40 m, so those rim fragments would all discard.
    // record() executes the core pipe over all draws and the two rim pipes
    // over the contiguous near tail [m_rimStart, end) (sort order is
    // far->near, so the near tail is one contiguous indirect range).
    // record() then executes them either as 3 indirect draws (default) or,
    // with VF_SPLAT_DIRECT=1, as one vkCmdDraw per chunk (A/B benchmark).
    void computeDraws(const RaymarchPush& push);

    const Context* m_ctx = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkPipeline m_skyPipe = VK_NULL_HANDLE;
    VkPipeline m_corePipe = VK_NULL_HANDLE;
    VkPipeline m_rimInPipe = VK_NULL_HANDLE;
    VkPipeline m_rimOutPipe = VK_NULL_HANDLE;
    VkPipeline m_waterPipe = VK_NULL_HANDLE;
    // GPU-driven cull pre-pass (splat_cull.comp): compacts each draw
    // entry's surfels to fragment-producing ones and rewrites the entry's
    // instanceCount in the draw command stream; the graphics passes draw
    // the compacted stream via plain indirect draws. VF_NO_GPU_CULL=1
    // falls back to CPU counts (identity compaction -> identical image;
    // both paths are visually exact).
    VkPipeline m_cullPipe = VK_NULL_HANDLE;
    Buffer m_compactBuf {};  // slot -> surfel index (identity at upload)
    size_t m_compactBytes = 0;
    Buffer m_selBufs[3] {};  // per-frame selection entries {first, count, 0, 0}
    Buffer m_planesBuf {};   // per-frame frustum planes (inward normals)

    VkBuffer m_surfelBuf = VK_NULL_HANDLE;
    VmaAllocation m_surfelAlloc = VK_NULL_HANDLE;
    Image3D m_depth {};
    Buffer m_paramsBuf {}; // persistently mapped 2xvec4 kernel-tuning UBO
    // Triple-buffered indirect draw commands (host-visible, CPU-filled per
    // frame; cycled in lockstep with the frame slots so the GPU never reads
    // a buffer the CPU is currently writing).
    static constexpr uint32_t kMaxChunkDraws = 16 * 16 * 16;
    Buffer m_drawCmds[3] {};
    uint32_t m_cmdSlot = 0;
    // Water indirect commands (same triple-buffering); filled alongside the
    // opaque commands so off-screen lake chunks are never rasterized.
    Buffer m_waterCmds[3] {};
    uint32_t m_waterDraws = 0;
    // CPU-side culled draw lists (rebuilt per frame, executed direct or
    // copied into the indirect buffers above).
    std::vector<VkDrawIndirectCommand> m_cpuDraws;
    std::vector<VkDrawIndirectCommand> m_cpuWaterDraws;
    // First index in m_cpuDraws whose chunk still carries rim geometry
    // (nearest-point distance <= m_rimDist). The far->near sort keeps all
    // core-only chunks in the contiguous prefix; rim passes draw
    // [m_rimStart, nDraws) only. == m_cpuDraws.size() when nothing has rim.
    uint32_t m_rimStart = 0;
    // Rim fade distance (m): chunks whose AABB is entirely beyond it draw
    // core only. 42 m > the 40 m coreD2->1.0 ramp end in splat.frag, so the
    // skip is exact. 0 disables the split (legacy: all chunks keep rims).
    float m_rimDist = 42.0f;
    glm::vec4 m_params { 0.0f, 0.55f, 1.02f, 0.0f };
    float m_radiusScale = 1.0f;
    bool m_buried = false;

    size_t m_count = 0;
    uint32_t m_waterStart = 0;
    std::vector<uint32_t> m_chunkRange;
    std::vector<uint32_t> m_waterChunkRange; // GRID_N^3 + 1 absolute offsets, or empty
    std::vector<uint32_t> m_microStart;      // per-chunk base/micro split, or empty
    std::vector<uint32_t> m_lod1Range;       // merged-terrain LOD rings, or empty
    std::vector<uint32_t> m_lod2Range;
    VkImageView m_hdrView = VK_NULL_HANDLE;
    VkImageView m_gposView = VK_NULL_HANDLE;
    VkImageView m_heightView = VK_NULL_HANDLE;
    VkImageView m_objVolView = VK_NULL_HANDLE;
};

} // namespace vf
