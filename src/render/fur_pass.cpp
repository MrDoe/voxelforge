// fur_pass.cpp - upload and draw the fur cards over the micro objects.
#include "render/fur_pass.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

namespace vf {

namespace {

// Same per-file helper the other passes carry.
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

// The per-object row of the shader's table. It mirrors the FurWorld struct in
// fur_overlay.comp: ten vec4s.
constexpr int kFurMaxWorlds = 16;
struct alignas(16) FurBoneRow {
    float a[4];
    float b[4];
    float xform[4];
    float r0[4];
    float r1[4];
    float r2[4];
};
struct alignas(16) FurWorldRow {
    float cards[4];   // x = the first card, y = the card count
    float grid[4];    // xyz = the grid's local origin, w = the cell size
    float gdim[4];    // xyz = the grid's dims, w = the cellStart base
    float items[4];   // x = the cellItems base
    float preview[4]; // xyz = the Move preview offset, w = active
    float rot0[4];
    float rot1[4];
    float rot2[4];    // the live rotation preview's matrix (row-major)
    float pivot[4];   // xyz = the pivot (world m), w = active
    float world[4];   // xyz = the object's world AABB min
    // The autonomous BEHAVIOUR's transform: the offset + an active flag, the
    // rotation matrix and the pivot. The coat has to follow it, or it hangs
    // in the air where the animal used to be.
    float anim[4];
    float arot0[4];
    float arot1[4];
    float arot2[4];
    float apivot[4];
    float bones[4];
    FurBoneRow bone[12];
};
static_assert(sizeof(FurWorldRow) == 1408, "std140 slot size");

}  // namespace

bool FurPass::uploadSsbo(Ssbo& s, const void* data, size_t bytes)
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

void FurPass::destroySsbo(Ssbo& s)
{
    if (s.buf)
        vmaDestroyBuffer(m_ctx->allocator(), s.buf, s.alloc);
    s.buf = VK_NULL_HANDLE;
    s.alloc = VK_NULL_HANDLE;
    s.bytes = 0;
}

bool FurPass::setObjects(const std::vector<voxel::FurObject>& objects)
{
    m_objectCount = 0;
    m_cardCount = 0;
    if (objects.empty()) {
        destroySsbo(m_cards);
        destroySsbo(m_cellStart);
        destroySsbo(m_cellItems);
        if (m_table.mapped)
            memset(m_table.mapped, 0, sizeof(FurWorldRow) * kFurMaxWorlds + 16);
        return true;
    }
    const size_t n = std::min(objects.size(), size_t(kFurMaxWorlds));
    if (objects.size() > size_t(kFurMaxWorlds))
        spdlog::error("fur: {} objects exceeds the {}-object limit; the extra are "
                      "dropped",
                      objects.size(), kFurMaxWorlds);

    // The cards and the two bucket arrays are CONCATENATED, and each object's
    // row carries the base it starts at.
    std::vector<voxel::FurCard> cards;
    std::vector<uint32_t> cellStart, cellItems;
    std::vector<FurWorldRow> rows;
    rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const voxel::FurObject& o = objects[i];
        if (o.cards.empty())
            continue;
        FurWorldRow row {};
        row.cards[0] = float(cards.size());
        row.cards[1] = float(o.cards.size());
        row.grid[0] = o.grid.origin.x;
        row.grid[1] = o.grid.origin.y;
        row.grid[2] = o.grid.origin.z;
        row.grid[3] = o.grid.cell.x;
        row.gdim[0] = float(o.grid.dim.x);
        row.gdim[1] = float(o.grid.dim.y);
        row.gdim[2] = float(o.grid.dim.z);
        row.gdim[3] = float(cellStart.size());
        row.items[0] = float(cellItems.size());
        row.world[0] = o.worldMin.x;
        row.world[1] = o.worldMin.y;
        row.world[2] = o.worldMin.z;
        // The live preview rides along with the object, so a REBUILD (the
        // coat's parameters changed) does not drop a preview that is in
        // flight: the Move offset and the rotation preview are the object's
        // state, not the cards'.
        row.preview[0] = o.preview.x;
        row.preview[1] = o.preview.y;
        row.preview[2] = o.preview.z;
        row.preview[3] = o.previewActive ? 1.0f : 0.0f;
        if (o.rotActive) {
            row.rot0[0] = o.rot[0][0]; row.rot0[1] = o.rot[0][1]; row.rot0[2] = o.rot[0][2];
            row.rot1[0] = o.rot[1][0]; row.rot1[1] = o.rot[1][1]; row.rot1[2] = o.rot[1][2];
            row.rot2[0] = o.rot[2][0]; row.rot2[1] = o.rot[2][1]; row.rot2[2] = o.rot[2][2];
            row.pivot[0] = o.pivot.x;
            row.pivot[1] = o.pivot.y;
            row.pivot[2] = o.pivot.z;
            row.pivot[3] = 1.0f;
        } else {
            // The identity rotation is the neutral pose, and pivot.w = 0 is
            // what tells the shader there is nothing to rotate.
            row.rot0[0] = row.rot1[1] = row.rot2[2] = 1.0f;
            row.pivot[3] = 0.0f;
        }
        // The skeleton: segments and count are static (the rig is fixed per
        // field); the animated rotations/xforms are uploaded per frame by
        // setBoneRotations.
        row.bones[0] = float(o.boneCount);
        for (int bi = 0; bi < o.boneCount && bi < 12; ++bi) {
            row.bone[bi].a[0] = o.boneSegA[size_t(bi)].x;
            row.bone[bi].a[1] = o.boneSegA[size_t(bi)].y;
            row.bone[bi].a[2] = o.boneSegA[size_t(bi)].z;
            row.bone[bi].a[3] = (bi < (int)o.boneRadius.size()) ? o.boneRadius[size_t(bi)] : 0.f;
            row.bone[bi].b[0] = o.boneSegB[size_t(bi)].x;
            row.bone[bi].b[1] = o.boneSegB[size_t(bi)].y;
            row.bone[bi].b[2] = o.boneSegB[size_t(bi)].z;
            row.bone[bi].b[3] = 0.f;
            row.bone[bi].xform[0] = 0.f;
            row.bone[bi].xform[1] = 0.f;
            row.bone[bi].xform[2] = 0.f;
            row.bone[bi].xform[3] = 0.f;
            row.bone[bi].r0[0] = 1.f; row.bone[bi].r0[1] = 0.f; row.bone[bi].r0[2] = 0.f; row.bone[bi].r0[3] = 0.f;
            row.bone[bi].r1[0] = 0.f; row.bone[bi].r1[1] = 1.f; row.bone[bi].r1[2] = 0.f; row.bone[bi].r1[3] = 0.f;
            row.bone[bi].r2[0] = 0.f; row.bone[bi].r2[1] = 0.f; row.bone[bi].r2[2] = 1.f; row.bone[bi].r2[3] = 0.f;
        }
        // The behaviour's transform starts inactive: the identity rotation
        // and no offset, so a coat that is not animated sits exactly where
        // the field does.
        row.arot0[0] = row.arot1[1] = row.arot2[2] = 1.0f;
        row.anim[3] = 0.0f;
        cards.insert(cards.end(), o.cards.begin(), o.cards.end());
        cellStart.insert(cellStart.end(), o.cellStart.begin(), o.cellStart.end());
        cellItems.insert(cellItems.end(), o.cellItems.begin(), o.cellItems.end());
        rows.push_back(row);
    }
    m_objectCount = rows.size();
    m_cardCount = cards.size();
    if (m_objectCount == 0)
        return true;

    if (!uploadSsbo(m_cards, cards.data(), cards.size() * sizeof(voxel::FurCard)) ||
        !uploadSsbo(m_cellStart, cellStart.data(), cellStart.size() * 4) ||
        !uploadSsbo(m_cellItems, cellItems.data(), cellItems.size() * 4)) {
        spdlog::error("fur: failed to upload the card buffers");
        m_objectCount = 0;
        return false;
    }

    // The three buffers into the descriptor set.
    {
        const VkDescriptorBufferInfo infos[3] = {
            { m_cards.buf, 0, VK_WHOLE_SIZE },
            { m_cellStart.buf, 0, VK_WHOLE_SIZE },
            { m_cellItems.buf, 0, VK_WHOLE_SIZE },
        };
        const uint32_t bindings[3] = { 0, 1, 2 };
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 3; ++i)
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, bindings[i], 0,
                     1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
    }

    if (m_table.mapped) {
        memset(m_table.mapped, 0, sizeof(FurWorldRow) * kFurMaxWorlds + 16);
        memcpy(m_table.mapped, rows.data(), sizeof(FurWorldRow) * rows.size());
        int32_t* count = reinterpret_cast<int32_t*>(
            static_cast<uint8_t*>(m_table.mapped) + sizeof(FurWorldRow) * kFurMaxWorlds);
        *count = int32_t(rows.size());
    }
    spdlog::info("fur: {} object(s), {} cards, {:.1f} KB gpu", rows.size(), cards.size(),
                 double(gpuBytes()) / 1024.0);
    return true;
}

void FurPass::setWorldPreview(size_t worldIndex, glm::vec3 offset, bool active)
{
    if (!m_table.mapped || worldIndex >= size_t(m_objectCount))
        return;
    auto* rows = reinterpret_cast<FurWorldRow*>(m_table.mapped);
    FurWorldRow& row = rows[worldIndex];
    row.preview[0] = offset.x;
    row.preview[1] = offset.y;
    row.preview[2] = offset.z;
    row.preview[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);
}

void FurPass::setWorldRotation(size_t worldIndex, glm::mat3 R, glm::vec3 pivot, bool active)
{
    if (!m_table.mapped || worldIndex >= size_t(m_objectCount))
        return;
    auto* rows = reinterpret_cast<FurWorldRow*>(m_table.mapped);
    FurWorldRow& row = rows[worldIndex];
    // Row-major, like the micro overlay's rotation preview.
    row.rot0[0] = R[0][0]; row.rot0[1] = R[0][1]; row.rot0[2] = R[0][2];
    row.rot1[0] = R[1][0]; row.rot1[1] = R[1][1]; row.rot1[2] = R[1][2];
    row.rot2[0] = R[2][0]; row.rot2[1] = R[2][1]; row.rot2[2] = R[2][2];
    row.pivot[0] = pivot.x;
    row.pivot[1] = pivot.y;
    row.pivot[2] = pivot.z;
    row.pivot[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);
}

// The behaviour's transform, mirrored from the micro overlay so the coat and
// the body are always in the same pose. The overlay is the authority (it owns
// the behaviour), and the fur pass has to follow it frame by frame.
void FurPass::setWorldAnimation(size_t worldIndex, glm::vec3 offset, glm::mat3 R,
                                glm::vec3 pivot, bool active)
{
    if (!m_table.mapped || worldIndex >= size_t(m_objectCount))
        return;
    auto* rows = reinterpret_cast<FurWorldRow*>(m_table.mapped);
    FurWorldRow& row = rows[worldIndex];
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
    vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);
}

void FurPass::setBoneRotations(size_t worldIndex,
                              const std::vector<glm::mat3>& rot,
                              const std::vector<glm::vec3>& xform)
{
    if (!m_table.mapped || worldIndex >= size_t(m_objectCount))
        return;
    auto* rows = reinterpret_cast<FurWorldRow*>(m_table.mapped);
    FurWorldRow& row = rows[worldIndex];
    const int count = int(row.bones[0]);
    if (count <= 0 || int(rot.size()) < count)
        return;
    for (int i = 0; i < count; ++i) {
        const glm::mat3& R = rot[size_t(i)];
        FurBoneRow& br = row.bone[i];
        br.r0[0] = R[0][0]; br.r0[1] = R[0][1]; br.r0[2] = R[0][2];
        br.r1[0] = R[1][0]; br.r1[1] = R[1][1]; br.r1[2] = R[1][2];
        br.r2[0] = R[2][0]; br.r2[1] = R[2][1]; br.r2[2] = R[2][2];
        if (size_t(i) < xform.size()) {
            br.xform[0] = xform[size_t(i)].x;
            br.xform[1] = xform[size_t(i)].y;
            br.xform[2] = xform[size_t(i)].z;
        }
    }
    vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);
}
size_t FurPass::gpuBytes() const
{
    return m_cards.bytes + m_cellStart.bytes + m_cellItems.bytes;
}

bool FurPass::createBlackEnv(const Context& ctx)
{
    // A 1x1 black LUT and a 1x1x6 black cubemap. The environment is black
    // everywhere in this renderer (the splat pass creates the same pair), so
    // the IBL terms contribute nothing; they exist only so the descriptor set
    // is complete, because common_base.glsl declares both resources and
    // glslang emits the declarations whether the shader uses them or not.
    m_blackLut = makeImage2D(ctx, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    if (!m_blackLut.img)
        return false;
    static const uint64_t zero64[2] = { 0, 0 };
    if (!uploadToImage3D(ctx, m_blackLut, zero64, sizeof(zero64)))
        return false;

    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = { 1, 1, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 6;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    m_blackCube.extent = { 1, 1, 1 };
    m_blackCube.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &m_blackCube.img, &m_blackCube.alloc,
                       nullptr) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_blackCube.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    if (vkCreateImageView(ctx.device(), &vi, nullptr, &m_blackCube.view) != VK_SUCCESS)
        return false;
    if (!ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            vf::transitionImage(cmd, m_blackCube.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue zv {};
            VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
            vkCmdClearColorImage(cmd, m_blackCube.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zv, 1, &range);
            vf::transitionImage(cmd, m_blackCube.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_READ_BIT);
        }))
        return false;
    VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 5.0f;
    return vkCreateSampler(ctx.device(), &sci, nullptr, &m_blackSampler) == VK_SUCCESS;
}

bool FurPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();
    if (!createBlackEnv(ctx))
        return false;

    VkDescriptorSetLayoutBinding b[19] = {};
    int n = 0;
    // Every binding is declared for all three stages: the same set feeds the
    // vertex+fragment draw and the composite dispatch, so a per-stage split
    // would only duplicate the layout.
    const VkShaderStageFlags gfx = VkShaderStageFlags(
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT);
    // 0/1/2: the cards and the two bucket arrays (the raster path reads only
    // the cards; the bucket arrays stay bound because the CPU still uploads
    // them and a future LOD may want them).
    for (uint32_t i = 0; i < 3; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, gfx, nullptr };
    // 8/9: the terrain height texture and the coarse object volume (declared
    // by the shader itself; common_base.glsl references them by name).
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    // 11/12: the black environment cubemap and the BRDF LUT (common_base.glsl
    // declares them; the IBL is black in this renderer, but the descriptor
    // set has to be complete or the driver reads an unbound descriptor).
    b[n++] = { 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    b[n++] = { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    // 13: the composited HDR the fur is mixed into. 14: gpos, the depth
    // reference the fragment stage discards against.
    b[n++] = { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    b[n++] = { 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    // 22/23: the texture atlas and its table (shared detailAlbedo).
    b[n++] = { 22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    b[n++] = { 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    // 25: the explicit point lights. 26: the coarse irradiance volume.
    b[n++] = { 25, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    b[n++] = { 26, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    // 30: the per-object table (cards/grid ranges, the transforms, the bones).
    b[n++] = { 30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    // 40-43: the raster accumulation targets the composite stage folds into
    // HDR (alpha, alpha*light, alpha*ao, alpha*mat | alpha*p | alpha*n | alpha*alb).
    for (uint32_t i = 40; i <= 44; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };

    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = n;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { gfx, 0, sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    auto makeModule = [&](const char* file, VkShaderModule* out) {
        auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/" + file);
        if (spirv.empty())
            return false;
        VkShaderModuleCreateInfo mci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mci.codeSize = spirv.size();
        mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
        return vkCreateShaderModule(dev, &mci, nullptr, out) == VK_SUCCESS;
    };

    // ---- the raster pipeline: one instanced quad per card -----------------
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, cs = VK_NULL_HANDLE;
    if (!makeModule("fur.vert.spv", &vs) || !makeModule("fur.frag.spv", &fs) ||
        !makeModule("fur_composite.comp.spv", &cs)) {
        spdlog::critical("fur: shader module load failed");
        return false;
    }

    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };
        VkPipelineInputAssemblyStateCreateInfo ia {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
        };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
        };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
        };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // A card is TWO-SIDED: its billboarded plane faces the camera by
        // construction, but a strand seen end-on flips the winding.
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
        };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // The accumulation targets are ADDITIVE: every card adds its own
        // alpha-weighted terms, and the composite turns the sums into the
        // many-layer coverage. Order does not matter, so the cards need no
        // sorting (the ray version had no ordering either).
        const VkFormat accFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkPipelineRenderingCreateInfo prc {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
        };
        VkFormat fmts[kAccCount] = { accFmt, accFmt, accFmt, accFmt, accFmt };
        prc.colorAttachmentCount = kAccCount;
        prc.pColorAttachmentFormats = fmts;

        VkPipelineColorBlendAttachmentState cba[kAccCount] = {};
        for (int i = 0; i < kAccCount; ++i) {
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].colorBlendOp = VK_BLEND_OP_ADD;
            cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].alphaBlendOp = VK_BLEND_OP_ADD;
            cba[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
        };
        cb.attachmentCount = kAccCount;
        cb.pAttachments = cba;

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
        };
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        };
        gpi.pNext = &prc;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &dy;
        gpi.layout = m_layout;
        const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi,
                                                     nullptr, &m_drawPipe);
        if (r != VK_SUCCESS) {
            spdlog::critical("fur: raster pipeline failed ({})", int(r));
            vkDestroyShaderModule(dev, vs, nullptr);
            vkDestroyShaderModule(dev, fs, nullptr);
            vkDestroyShaderModule(dev, cs, nullptr);
            return false;
        }
    }

    // ---- the composite pipeline: sums -> the lit pixel --------------------
    {
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.layout = m_layout;
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = cs;
        cpi.stage.pName = "main";
        const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                                    &m_compPipe);
        if (r != VK_SUCCESS) {
            spdlog::critical("fur: composite pipeline failed ({})", int(r));
            vkDestroyShaderModule(dev, vs, nullptr);
            vkDestroyShaderModule(dev, fs, nullptr);
            vkDestroyShaderModule(dev, cs, nullptr);
            return false;
        }
    }
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    vkDestroyShaderModule(dev, cs, nullptr);

    // The pool covers every binding above: STORAGE_IMAGE 8/9/13/14 +40-43 (8),
    // STORAGE_BUFFER 0/1/2 (3), UNIFORM_BUFFER 23/25/30 (3),
    // COMBINED_IMAGE_SAMPLER 11/12/22/26 (4).
    VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9 },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
                                       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
                                       { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 } };
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

    // The per-object table: 16 rows of 160 B + the 16 B count word.
    m_table = makeBuffer(ctx, sizeof(FurWorldRow) * kFurMaxWorlds + 16,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_table.buf || !m_table.mapped)
        return false;
    memset(m_table.mapped, 0, sizeof(FurWorldRow) * kFurMaxWorlds + 16);
    VkDescriptorBufferInfo tableInfo { m_table.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wt { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 30, 0, 1,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &tableInfo, nullptr };
    vkUpdateDescriptorSets(dev, 1, &wt, 0, nullptr);

    // The black environment, into 11 and 12.
    {
        const VkDescriptorImageInfo cube { m_blackSampler, m_blackCube.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo lut { m_blackSampler, m_blackLut.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[2] = {};
        w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 11, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cube, nullptr, nullptr };
        w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 12, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &lut, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
    return true;
}

// The raster path's accumulation targets. They follow the render extent, so
// they are (re)created whenever the swapchain size changes.
bool FurPass::ensureAccumulators(uint32_t w, uint32_t h)
{
    if (!m_ctx || w == 0 || h == 0)
        return false;
    if (m_acc[0].img && m_accExtent.width == w && m_accExtent.height == h)
        return true;
    for (int i = 0; i < kAccCount; ++i) {
        destroyImage3D(*m_ctx, m_acc[i]);
        m_acc[i] = makeImage2D(*m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_acc[i].img) {
            spdlog::critical("fur: accumulator image {}x{} failed", w, h);
            return false;
        }
    }
    m_accExtent = { w, h };
    // Leave them in GENERAL once, here, so the per-frame path never has to
    // pretend the contents are undefined: the render pass CLEARS them anyway,
    // but a GENERAL->GENERAL transition with real stage masks is what orders
    // frame N's CLEAR against frame N-1's composite READ of the same images.
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        for (int i = 0; i < kAccCount; ++i)
            vf::transitionImage(cmd, m_acc[i].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    return true;
}
void FurPass::updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage)
{
    if (!m_set || !m_ctx)
        return;
    if (!ensureAccumulators(hdrImage.extent.width, hdrImage.extent.height))
        return;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, m_heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, m_objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo acc[kAccCount];
    for (int i = 0; i < kAccCount; ++i)
        acc[i] = { VK_NULL_HANDLE, m_acc[i].view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[9] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 13, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 14, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    for (int i = 0; i < kAccCount; ++i)
        w[4 + i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set,
                     uint32_t(40 + i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     &acc[i], nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 9, w, 0, nullptr);
}

void FurPass::setTexAtlas(VkImageView view, VkSampler sampler, VkBuffer tableUbo)
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

void FurPass::setIrrVolView(VkImageView view)
{
    if (!m_ctx || view == VK_NULL_HANDLE)
        return;
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
            spdlog::error("fur: irradiance sampler creation failed (binding 26 unbound)");
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

void FurPass::setLights(const vf::voxel::LightUBO& lights)
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

void FurPass::record(VkCommandBuffer cmd, const RaymarchPush& push)
{
    // No fur -> nothing to draw and nothing to composite. This is the whole
    // cost of the feature when it is off, and it guarantees the frame is
    // byte-identical to the frame without it.
    if (m_objectCount == 0 || m_cardCount == 0 || !m_drawPipe || !m_compPipe || !m_set)
        return;
    if (!m_acc[0].img)
        return;
    const uint32_t W = uint32_t(push.a.z);
    const uint32_t H = uint32_t(push.a.w);
    if (W == 0 || H == 0 || m_accExtent.width != W || m_accExtent.height != H)
        return; // the accumulation targets are stale until updateDescriptors runs

    // The screen rectangle the coat can touch. It is the objects' REAL screen
    // bounds - not the frustum's own half-size, which is what this used to
    // project and always clipped to the whole image.
    int rectMinX = INT_MAX, rectMinY = INT_MAX, rectMaxX = 0, rectMaxY = 0;
    {
        const float tanHalf = push.a.x;
        const float aspect = push.a.y;
        auto* rows = reinterpret_cast<FurWorldRow*>(m_table.mapped);
        for (size_t i = 0; i < m_objectCount; ++i) {
            const FurWorldRow& r = rows[i];
            if (r.cards[1] <= 0.0f)
                continue;
            const glm::vec3 half = 0.5f * glm::vec3(r.gdim[0], r.gdim[1], r.gdim[2]) *
                                   r.grid[3];
            glm::vec3 c = glm::vec3(r.world[0], r.world[1], r.world[2]) +
                          glm::vec3(r.grid[0], r.grid[1], r.grid[2]) + half;
            // The coat hangs off a skeleton: a swinging bone carries cards
            // OUTSIDE the rest AABB, so the bound is the AABB plus the reach
            // of one bone. Conservative, and still a fraction of the screen.
            float radius = glm::length(half) + 0.30f;
            // Mirror the shader's transform order exactly (furToWorld): the
            // rotation preview about its pivot, then the behaviour about its
            // own, then the Move preview's translation.
            auto rowMat = [](const float* a, const float* b2, const float* c2) {
                glm::mat3 M;
                M[0] = glm::vec3(a[0], a[1], a[2]);
                M[1] = glm::vec3(b2[0], b2[1], b2[2]);
                M[2] = glm::vec3(c2[0], c2[1], c2[2]);
                return M;
            };
            if (r.pivot[3] > 0.5f) {
                const glm::vec3 pv(r.pivot[0], r.pivot[1], r.pivot[2]);
                c = pv + rowMat(r.rot0, r.rot1, r.rot2) * (c - pv);
            }
            if (r.anim[3] > 0.5f) {
                const glm::vec3 pv(r.apivot[0], r.apivot[1], r.apivot[2]);
                c = pv + rowMat(r.arot0, r.arot1, r.arot2) * (c - pv) +
                    glm::vec3(r.anim[0], r.anim[1], r.anim[2]);
            }
            if (r.preview[3] > 0.5f)
                c += glm::vec3(r.preview[0], r.preview[1], r.preview[2]);
            const glm::vec3 d = c - glm::vec3(push.camPos);
            const float z = glm::dot(d, glm::vec3(push.camFwd));
            if (z < -radius)
                continue; // fully behind
            if (z > 60.0f + radius)
                continue; // too far (sub-pixel fur)
            const float x = glm::dot(d, glm::vec3(push.camRight));
            const float y = glm::dot(d, glm::vec3(push.camUp));
            const float halfX = tanHalf * aspect * std::max(z, 0.1f) + radius;
            const float halfY = tanHalf * std::max(z, 0.1f) + radius;
            if (std::fabs(x) > halfX || std::fabs(y) > halfY)
                continue;
            // Project the bounding sphere's four screen extremes. The NEAREST
            // depth it can reach is used for the projection so a sphere that
            // straddles the camera does not collapse the rectangle.
            const float zn = std::max(z - radius, 0.1f);
            for (int s = 0; s < 4; ++s) {
                const float px = x + ((s & 1) ? radius : -radius);
                const float py = y + ((s & 2) ? radius : -radius);
                const int sx = (int)((px / (tanHalf * aspect * zn) * 0.5f + 0.5f) * float(W));
                const int sy = (int)((-py / (tanHalf * zn) * 0.5f + 0.5f) * float(H));
                rectMinX = std::min(rectMinX, sx - 16);
                rectMinY = std::min(rectMinY, sy - 16);
                rectMaxX = std::max(rectMaxX, sx + 16);
                rectMaxY = std::max(rectMaxY, sy + 16);
            }
        }
        if (rectMaxX <= rectMinX || rectMaxY <= rectMinY)
            return; // no object is on screen
        rectMinX = std::max(0, std::min(rectMinX, int(W) - 1));
        rectMinY = std::max(0, std::min(rectMinY, int(H) - 1));
        rectMaxX = std::max(rectMinX + 1, std::min(rectMaxX, int(W)));
        rectMaxY = std::max(rectMinY + 1, std::min(rectMaxY, int(H)));
    }

    // The host-written table has to reach the device: the buffer is
    // host-visible but not host-coherent, so a plain memcpy into the mapped
    // range does not become visible without a flush.
    if (m_table.mapped)
        vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);

    // The FRAGMENT stage now reads the G-buffer (the depth reference), which
    // the micro overlay wrote with a COMPUTE dispatch. The caller's barrier
    // after that dispatch only makes the write visible to COMPUTE, so without
    // this the depth test races the overlay - invisible on a slow frame,
    // wrong on a fast one. Self-contained so the frame files stay agnostic.
    {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // The accumulation targets are CLEARED by the pass itself, but the CLEAR
    // still has to be ordered after the PREVIOUS frame's composite READ of
    // these same images (they are single-buffered across frames in flight):
    // a write-after-read hazard that shows up as a torn coat only when the
    // frame is fast enough to overlap.
    for (int i = 0; i < kAccCount; ++i)
        vf::transitionImage(cmd, m_acc[i].img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    // ---- the cards: one instanced quad each ------------------------------
    {
        const VkRect2D rect { { rectMinX, rectMinY },
                              { uint32_t(rectMaxX - rectMinX),
                                uint32_t(rectMaxY - rectMinY) } };
        VkRenderingAttachmentInfo colors[kAccCount] = {};
        for (int i = 0; i < kAccCount; ++i) {
            colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colors[i].imageView = m_acc[i].view;
            colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colors[i].clearValue = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        }
        VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
        // The rectangle, not the whole frame: the clear follows the render
        // area, so a small object does not clear four full-screen targets.
        ri.renderArea = rect;
        ri.layerCount = 1;
        ri.colorAttachmentCount = kAccCount;
        ri.pColorAttachments = colors;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1,
                                &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout,
                           VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT),
                           0, sizeof(RaymarchPush), &push);
        // 4 vertices (a strip) per card, one instance per card, EVERY object
        // in one draw: the vertex shader recovers the owner from the card
        // ranges in the table.
        vkCmdDraw(cmd, 4, uint32_t(m_cardCount), 0, 0);
        vkCmdEndRendering(cmd);
    }

    // The accumulated sums are read by the composite dispatch.
    {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    // ---- the composite: sums -> the lit pixel ----------------------------
    RaymarchPush cp = push;
    cp.b.z = float(rectMinX);
    cp.b.w = float(rectMinY);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set,
                            0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
    vkCmdDispatch(cmd, uint32_t(rectMaxX - rectMinX + 7) / 8,
                  uint32_t(rectMaxY - rectMinY + 7) / 8, 1);
}

void FurPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
    destroySsbo(m_cards);
    destroySsbo(m_cellStart);
    destroySsbo(m_cellItems);
    if (m_irrSampler)
        vkDestroySampler(dev, m_irrSampler, nullptr);
    if (m_blackSampler)
        vkDestroySampler(dev, m_blackSampler, nullptr);
    if (m_drawPipe)
        vkDestroyPipeline(dev, m_drawPipe, nullptr);
    if (m_compPipe)
        vkDestroyPipeline(dev, m_compPipe, nullptr);
    for (int i = 0; i < kAccCount; ++i)
        destroyImage3D(*m_ctx, m_acc[i]);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    destroyBuffer(*m_ctx, m_table);
    destroyBuffer(*m_ctx, m_lightsBuf);
    destroyImage3D(*m_ctx, m_blackCube);
    destroyImage3D(*m_ctx, m_blackLut);
    m_drawPipe = VK_NULL_HANDLE;
    m_compPipe = VK_NULL_HANDLE;
    m_accExtent = { 0, 0 };
    m_layout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_irrSampler = VK_NULL_HANDLE;
    m_blackSampler = VK_NULL_HANDLE;
    m_objectCount = 0;
    m_cardCount = 0;
}

}  // namespace vf
