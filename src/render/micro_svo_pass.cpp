// micro_svo_pass.cpp - upload and draw the SEPARATE micro-voxel SVO worlds.
#include "render/micro_svo_pass.hpp"
#include "render/svo_pass.hpp"
#include "voxel/world.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>

namespace vf {

namespace {

// Same per-file helper the other passes carry (each render translation unit has
// its own; it is four lines and not worth a header).
std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::critical("Cannot open shader '{}'", path);
        return {};
    }
    const size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> d(n);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(n));
    return d;
}

// Mirrors the std140 MicroWorld struct in micro_overlay.comp: geo, base, lo, hi
// - four 16-byte slots per world. kMicroMaxWorlds is the shader's table size.
constexpr int kMicroMaxWorlds = 16;
// One bone: the segment it occupies, and its CHAINED transform - a rigid
// one (R*p + t), because the composition of two pivot-rotations is not a
// pivot-rotation, and a child that turns about a rest joint while the parent
// moves tears at the seam.
struct alignas(16) MicroBoneRow {
    float a[4];       // the segment's start
    float b[4];       // the segment's end
    float xform[4];   // the chained transform's translation (xyz)
    float r0[4];      // the chained transform's rotation (row-major)
    float r1[4];
    float r2[4];
};

struct alignas(16) MicroWorldRow {
    // geo  = voxel, gridN, chunkM, dimY
    // base = gridBase, chunkInfoBase, dimX, dimZ   <- authored cell dims
    // lo/hi = world AABB min/max. lo.w carries the world's OFFSET into the
    // dense field buffer (in cells); hi.w is unused.
    // The cell dims matter: a chunk spans 16 cells, so the outermost chunks of
    // a small object reach past the authored grid. Those cells exist in the
    // brick but have no record behind them, and the shader must clamp its cell
    // index to the grid or it samples a filler cell - which reads as grass
    // material and turns a pale ceramic vase green.
    // preview = the live preview offset (xyz) + an active flag (w): a staged
    // Move delta for the selected micro object. The field stays where it was
    // baked and the RENDERED position is offset, so a drag is instant and
    // costs no re-bake.
    // rot0/rot1/rot2 = the live ROTATION preview's matrix (row-major) and
    // pivot = the pivot (xyz, world m) + an active flag (w). Same idea: the
    // field, the material lookup and the octree stay in the BAKED frame and
    // the RENDERED position is rotated out of it. A micro object has no world
    // owner id, so the splat pass's rotate preview cannot show it - this is
    // its only live rotation.
    // anim = the AUTONOMOUS BEHAVIOUR's offset (xyz) + an active flag (w);
    // arot0..2 = its rotation matrix (row-major) and apivot = its pivot
    // (xyz, world m). Same contract as the rotation preview: the field stays
    // in the baked frame and the transform is applied when it is sampled, so
    // a walk costs no re-bake.
    float geo[4];
    uint32_t base[4];
    float lo[4];
    float hi[4];
    float preview[4];
    float rot0[4];
    float rot1[4];
    float rot2[4];
    float pivot[4];
    float anim[4];
    float arot0[4];
    float arot1[4];
    float arot2[4];
    float apivot[4];
    // bones = the SKELETON: x = the bone count. The bones follow: their
    // SEGMENT and JOINT never change, and the gait rewrites only their
    // rotations.
    float bones[4];
    MicroBoneRow bone[8];
};
static_assert(sizeof(MicroWorldRow) == 1008, "std140 slot size");

} // namespace

bool MicroSvoPass::uploadSsbo(Ssbo& s, const void* data, size_t bytes)
{
    destroySsbo(s);
    if (!data || bytes == 0)
        return true;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &s.buf, &s.alloc, nullptr) !=
        VK_SUCCESS)
        return false;
    s.bytes = bytes;
    Buffer staging = makeBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!staging.buf || !staging.mapped)
        return false;
    memcpy(staging.mapped, data, bytes);
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, 0, bytes };
        vkCmdCopyBuffer(cmd, staging.buf, s.buf, 1, &c);
    });
    destroyBuffer(*m_ctx, staging);
    return ok;
}

void MicroSvoPass::destroySsbo(Ssbo& s)
{
    if (s.buf)
        vmaDestroyBuffer(m_ctx->allocator(), s.buf, s.alloc);
    s.buf = VK_NULL_HANDLE;
    s.alloc = VK_NULL_HANDLE;
    s.bytes = 0;
}

void MicroSvoPass::bindMicroBuffers()
{
    VkDescriptorBufferInfo infos[7] = {
        { m_grid.buf, 0, VK_WHOLE_SIZE },    { m_childBase.buf, 0, VK_WHOLE_SIZE },
        { m_payload.buf, 0, VK_WHOLE_SIZE }, { m_handles.buf, 0, VK_WHOLE_SIZE },
        { m_bricks.buf, 0, VK_WHOLE_SIZE },  { m_chunkInfo.buf, 0, VK_WHOLE_SIZE },
        { m_field.buf, 0, VK_WHOLE_SIZE },
    };
    const uint32_t bindings[7] = { 1, 2, 3, 4, 5, 6, 10 };
    VkWriteDescriptorSet w[7] = {};
    for (int i = 0; i < 7; ++i)
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, bindings[i], 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 7, w, 0, nullptr);
}

bool MicroSvoPass::setMicroWorlds(const std::vector<voxel::MicroWorld>& worlds)
{
    m_worldCount = 0;
    if (worlds.empty()) {
        destroySsbo(m_grid);
        destroySsbo(m_childBase);
        destroySsbo(m_payload);
        destroySsbo(m_handles);
        destroySsbo(m_bricks);
        destroySsbo(m_chunkInfo);
        destroySsbo(m_field);
        if (m_worldTable.mapped)
            memset(m_worldTable.mapped, 0, sizeof(int32_t) * 4);
        return true;
    }
    if (int(worlds.size()) > kMicroMaxWorlds) {
        spdlog::error("micro: {} worlds exceeds the {}-world limit; the extra are "
                      "dropped",
                      worlds.size(), kMicroMaxWorlds);
    }
    const size_t n = std::min(worlds.size(), size_t(kMicroMaxWorlds));

    // Concatenate. Each world's chunk bases are REBASED onto the accumulated
    // arrays here, so the shader reaches every world's nodes/bricks with one
    // flat index and only needs the grid + chunkInfo bases from the table.
    std::vector<int32_t> grid;
    std::vector<voxel::GpuChunkInfo> info;
    std::vector<uint32_t> childBase, payload, handles, bricks;
    std::vector<uint8_t> field;
    std::vector<MicroWorldRow> rows;
    rows.reserve(n);
    for (size_t w = 0; w < n; ++w) {
        const voxel::MicroWorld& s = worlds[w];
        if (s.chunkGrid.empty())
            continue;
        MicroWorldRow row {};
        row.geo[0] = s.voxel;
        row.geo[1] = float(s.gridN);
        row.geo[2] = s.chunkM;
        row.geo[3] = float(s.fieldDim.y);
        row.base[2] = uint32_t(s.fieldDim.x);
        row.base[3] = uint32_t(s.fieldDim.z);
        row.base[0] = uint32_t(grid.size());
        row.base[1] = uint32_t(info.size());
        // The dense field is one contiguous array; each world's slice starts
        // where the previous one ended. The offset is padded to a multiple of
        // four because the shader reads the field as bytes out of a uint
        // array (std430 packs uints four to a block).
        while (field.size() % 4)
            field.push_back(0);
        row.lo[3] = float(field.size());
        field.insert(field.end(), s.field.begin(), s.field.end());
        for (int i = 0; i < 3; ++i) {
            row.lo[i] = s.worldMin[i];
            row.hi[i] = s.worldMax[i];
        }
        row.preview[3] = 0.0f; // no live preview until one is set
        // The bones: their segments and joints (static) and the identity
        // rotation, which the gait then drives.
        row.bones[0] = float(s.rig.bones.size());
        for (size_t bi = 0; bi < s.rig.bones.size() && bi < 8; ++bi) {
            const auto& b = s.rig.bones[bi];
            MicroBoneRow& br = row.bone[bi];
            br.a[0] = b.a.x; br.a[1] = b.a.y; br.a[2] = b.a.z;
            br.b[0] = b.b.x; br.b[1] = b.b.y; br.b[2] = b.b.z;
            // The transform's translation: the bone's local rotation is
            // about its own joint, so the translation carries the joint
            // (the app recomposes it every frame with the parent's).
            br.xform[0] = b.a.x; br.xform[1] = b.a.y; br.xform[2] = b.a.z;
            br.r0[0] = 1.f;
            br.r1[1] = 1.f;
            br.r2[2] = 1.f;
        }
        // The rotation preview starts as the identity, inactive: pivot.w = 0
        // is what tells the shader there is nothing to rotate.
        row.rot0[0] = row.rot1[1] = row.rot2[2] = 1.0f;
        row.pivot[3] = 0.0f;
        // The behaviour's transform starts as the identity, inactive.
        row.arot0[0] = row.arot1[1] = row.arot2[2] = 1.0f;
        row.anim[3] = 0.0f;

        const uint32_t nodeOff = uint32_t(payload.size());
        const uint32_t handleOff = uint32_t(handles.size());
        const uint32_t brickOff = uint32_t(bricks.size() / voxel::BRICK_WORDS);
        for (voxel::GpuChunkInfo g : s.chunkInfo) {
            g.nodeBase += nodeOff;
            g.childBase += handleOff;
            g.brickBase += brickOff;
            info.push_back(g);
        }
        grid.insert(grid.end(), s.chunkGrid.begin(), s.chunkGrid.end());
        childBase.insert(childBase.end(), s.childBase.begin(), s.childBase.end());
        payload.insert(payload.end(), s.payload.begin(), s.payload.end());
        handles.insert(handles.end(), s.handles.begin(), s.handles.end());
        bricks.insert(bricks.end(), s.bricks.begin(), s.bricks.end());
        rows.push_back(row);
    }
    m_worldCount = rows.size();

    if (!uploadSsbo(m_grid, grid.data(), grid.size() * 4) ||
        !uploadSsbo(m_childBase, childBase.data(), childBase.size() * 4) ||
        !uploadSsbo(m_payload, payload.data(), payload.size() * 4) ||
        !uploadSsbo(m_handles, handles.data(), handles.size() * 4) ||
        !uploadSsbo(m_bricks, bricks.data(), bricks.size() * 4) ||
        !uploadSsbo(m_chunkInfo, info.data(), info.size() * 16) ||
        !uploadSsbo(m_field, field.data(), field.size())) {
        spdlog::error("micro: failed to upload one of the world buffers");
        m_worldCount = 0;
        return false;
    }
    bindMicroBuffers();

    // world table: rows then the count word
    if (m_worldTable.mapped) {
        std::memset(m_worldTable.mapped, 0, sizeof(MicroWorldRow) * kMicroMaxWorlds);
        memcpy(m_worldTable.mapped, rows.data(), sizeof(MicroWorldRow) * rows.size());
        int32_t* count = reinterpret_cast<int32_t*>(
            static_cast<uint8_t*>(m_worldTable.mapped) +
            sizeof(MicroWorldRow) * kMicroMaxWorlds);
        *count = int32_t(rows.size());
    }

    size_t totalBricks = 0;
    for (const auto& s : worlds)
        totalBricks += s.brickCount();
    spdlog::info("micro: {} world(s), {} bricks, {} grid chunks, {:.1f} KB gpu",
                 rows.size(), totalBricks, grid.size(),
                 double(gpuBytes()) / 1024.0);
    return true;
}

void MicroSvoPass::setWorldPreview(size_t worldIndex, glm::vec3 offset, bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    row.preview[0] = offset.x;
    row.preview[1] = offset.y;
    row.preview[2] = offset.z;
    row.preview[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setWorldRotation(size_t worldIndex, glm::mat3 R, glm::vec3 pivot,
                                    bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    // Row-major, like the splat pass's rotate preview (uRotRow0..2).
    row.rot0[0] = R[0][0]; row.rot0[1] = R[0][1]; row.rot0[2] = R[0][2];
    row.rot1[0] = R[1][0]; row.rot1[1] = R[1][1]; row.rot1[2] = R[1][2];
    row.rot2[0] = R[2][0]; row.rot2[1] = R[2][1]; row.rot2[2] = R[2][2];
    row.pivot[0] = pivot.x;
    row.pivot[1] = pivot.y;
    row.pivot[2] = pivot.z;
    row.pivot[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setBoneRotations(size_t worldIndex,
                                    const std::vector<glm::mat3>& rot,
                                    const std::vector<glm::vec3>& xform)
{
    // The gait rewrites the bones' rotations every frame. Only the matrix
    // part of each bone changes, so the world table is edited in place (the
    // segments and the joints stay as they were uploaded).
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    const int count = int(row.bones[0]);
    if (count <= 0 || int(rot.size()) < count)
        return;
    for (int i = 0; i < count; ++i) {
        const glm::mat3& R = rot[size_t(i)];
        MicroBoneRow& br = row.bone[i];
        br.r0[0] = R[0][0]; br.r0[1] = R[0][1]; br.r0[2] = R[0][2];
        br.r1[0] = R[1][0]; br.r1[1] = R[1][1]; br.r1[2] = R[1][2];
        br.r2[0] = R[2][0]; br.r2[1] = R[2][1]; br.r2[2] = R[2][2];
        if (size_t(i) < xform.size()) {
            br.xform[0] = xform[size_t(i)].x;
            br.xform[1] = xform[size_t(i)].y;
            br.xform[2] = xform[size_t(i)].z;
        }
    }
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setWorldAnimation(size_t worldIndex, glm::vec3 offset, glm::mat3 R,
                                     glm::vec3 pivot, bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    row.anim[0] = offset.x;
    row.anim[1] = offset.y;
    row.anim[2] = offset.z;
    row.anim[3] = active ? 1.0f : 0.0f;
    row.arot0[0] = R[0][0]; row.arot0[1] = R[0][1]; row.arot0[2] = R[0][2];
    row.arot1[0] = R[1][0]; row.arot1[1] = R[1][1]; row.arot1[2] = R[1][2];
    row.arot2[0] = R[2][0]; row.arot2[1] = R[2][1]; row.arot2[2] = R[2][2];
    row.apivot[0] = pivot.x;
    row.apivot[1] = pivot.y;
    row.apivot[2] = pivot.z;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

size_t MicroSvoPass::gpuBytes() const
{
    return m_grid.bytes + m_childBase.bytes + m_payload.bytes + m_handles.bytes +
           m_bricks.bytes + m_chunkInfo.bytes + m_field.bytes;
}

bool MicroSvoPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    // Binding 0 is intentionally absent (an unwritten binding invalidates the
    // whole set on this driver). b[] count is load-bearing: one more entry than
    // this overflows the array with no error anywhere.
    VkDescriptorSetLayoutBinding b[18] = {};
    int n = 0;
    // 1-6: the micro worlds' own six pools (separate from the world's SVO set).
    for (uint32_t i = 1; i < 7; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 7: per-world scale table.
    b[n++] = { 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 10: the dense signed-distance fields (one byte per cell, all worlds
    // concatenated). Separate from the bricks so the field is continuous
    // across brick boundaries - see MicroWorld::field.
    b[n++] = { 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 8/9: terrain height texture + coarse object volume (declared by the micro
    // shader itself; shared_model includes reference them by name).
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 12/13/14: the main backend's hdr/gpos/gnorm, read-modify-write. gpos is
    // the depth reference; the others are the composited output.
    b[n++] = { 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    b[n++] = { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    b[n++] = { 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // atlas sampler + table (shared detailAlbedo; same binding numbers as the
    // other backends so common_base.glsl's declaration is reused as-is).
    b[n++] = { 22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // explicit point lights (shared with the other backends)
    b[n++] = { 25, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // coarse irradiance volume (same binding as the other sets)
    b[n++] = { 26, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = n;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/micro_overlay.comp.spv");
    if (spirv.empty())
        return false;
    VkShaderModuleCreateInfo mci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = spirv.size();
    mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule mod;
    vkCreateShaderModule(dev, &mci, nullptr, &mod);
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    const VkResult r =
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("micro: pipeline failed ({})", int(r));
        return false;
    }

    // Pool sizes cover every binding above: STORAGE_IMAGE 8/9/12/13/14 (5),
    // STORAGE_BUFFER 1-6 + 10 (7), UNIFORM_BUFFER 7/23/25 (3),
    // COMBINED_IMAGE_SAMPLER 22/26 (2).
    VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 },
                                       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
                                       { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    // world table: 16 rows of 64 B + the 16 B count word
    m_worldTable = makeBuffer(ctx, sizeof(MicroWorldRow) * kMicroMaxWorlds + 16,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_worldTable.buf || !m_worldTable.mapped)
        return false;
    memset(m_worldTable.mapped, 0, sizeof(MicroWorldRow) * kMicroMaxWorlds + 16);
    VkDescriptorBufferInfo wtInfo { m_worldTable.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wwt { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 7, 0,
                               1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &wtInfo,
                               nullptr };
    vkUpdateDescriptorSets(dev, 1, &wwt, 0, nullptr);
    return true;
}

void MicroSvoPass::updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage,
                                     const Image3D& gnormImage)
{
    if (!m_set || !m_ctx)
        return;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo ni { VK_NULL_HANDLE, gnormImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, m_heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, m_objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[5] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 12, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 13, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 14, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ni, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 5, w, 0, nullptr);
}

void MicroSvoPass::setTexAtlas(VkImageView view, VkSampler sampler, VkBuffer tableUbo)
{
    if (!m_ctx || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
        tableUbo == VK_NULL_HANDLE)
        return;
    const VkDescriptorImageInfo img { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorBufferInfo buf { tableUbo, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 22, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 23, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buf, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
}

void MicroSvoPass::setIrrVolView(VkImageView view)
{
    if (!m_ctx || view == VK_NULL_HANDLE)
        return;
    m_irrView = view;
    if (!m_irrSampler) {
        VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        if (vkCreateSampler(m_ctx->device(), &sci, nullptr, &m_irrSampler) != VK_SUCCESS) {
            spdlog::error("micro: irradiance sampler creation failed (binding 26 unbound)");
            m_irrSampler = VK_NULL_HANDLE;
            return;
        }
    }
    const VkDescriptorImageInfo img { m_irrSampler, view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 26, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img, nullptr,
                             nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
}

void MicroSvoPass::setLights(const vf::voxel::LightUBO& lights)
{
    if (!m_ctx)
        return;
    if (!m_lightsBuf.buf)
        m_lightsBuf = makeBuffer(*m_ctx, sizeof(vf::voxel::LightUBO),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_lightsBuf.buf || !m_lightsBuf.mapped)
        return;
    memcpy(m_lightsBuf.mapped, &lights, sizeof(lights));
    if (m_set) {
        VkDescriptorBufferInfo info { m_lightsBuf.buf, 0, sizeof(lights) };
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 25, 0,
                                 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &info,
                                 nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
}

void MicroSvoPass::record(VkCommandBuffer cmd, const RaymarchPush& push)
{
    // No micro worlds loaded -> nothing to composite. This is the whole cost of
    // the feature when it is off, and it guarantees the main render path is
    // byte-identical to before.
    if (m_worldCount == 0 || !m_pipeline || !m_set)
        return;
    // Flush the host-written world table before the GPU reads it. makeBuffer
    // allocates with VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    // which on a discrete GPU is host-visible but NOT host-coherent, so a
    // plain memcpy into the mapped range does not reach the device. The
    // symptom is silent and confusing: the dispatch runs and every early-out
    // passes, but `uWorldInfo.x` still reads 0, so the shader returns before
    // tracing anything and the pass renders nothing at all.
    if (m_worldTable.mapped)
        vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t(push.a.z + 7) / 8, uint32_t(push.a.w + 7) / 8, 1);
}

void MicroSvoPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
    destroySsbo(m_grid);
    destroySsbo(m_childBase);
    destroySsbo(m_payload);
    destroySsbo(m_handles);
    destroySsbo(m_bricks);
    destroySsbo(m_chunkInfo);
    destroySsbo(m_field);
    if (m_irrSampler)
        vkDestroySampler(dev, m_irrSampler, nullptr);
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    destroyBuffer(*m_ctx, m_worldTable);
    destroyBuffer(*m_ctx, m_lightsBuf);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_irrSampler = VK_NULL_HANDLE;
    m_worldCount = 0;
}

} // namespace vf