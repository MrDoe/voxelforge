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
};
static_assert(sizeof(FurWorldRow) == 240, "std140 slot size");

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

    VkDescriptorSetLayoutBinding b[16] = {};
    int n = 0;
    // 0/1/2: the cards and the two bucket arrays.
    for (uint32_t i = 0; i < 3; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                   nullptr };
    // 8/9: the terrain height texture and the coarse object volume (declared
    // by the shader itself; common_base.glsl references them by name).
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 11/12: the black environment cubemap and the BRDF LUT (common_base.glsl
    // declares them; the IBL is black in this renderer, but the descriptor
    // set has to be complete or the driver reads an unbound descriptor).
    b[n++] = { 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 13/14: the main backend's hdr and gpos. gpos is the depth reference;
    // hdr is the composited output.
    b[n++] = { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    b[n++] = { 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 22/23: the texture atlas and its table (shared detailAlbedo).
    b[n++] = { 22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 25: the explicit point lights.
    b[n++] = { 25, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };
    // 26: the coarse irradiance volume.
    b[n++] = { 26, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 30: the per-object table.
    b[n++] = { 30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
               nullptr };

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

    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/fur_overlay.comp.spv");
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
        spdlog::critical("fur: pipeline failed ({})", int(r));
        return false;
    }

    // The pool covers every binding above: STORAGE_IMAGE 8/9/13/14 (4),
    // STORAGE_BUFFER 0/1/2 (3), UNIFORM_BUFFER 23/25/30 (3),
    // COMBINED_IMAGE_SAMPLER 11/12/22/26 (4).
    VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 },
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

void FurPass::updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage)
{
    if (!m_set || !m_ctx)
        return;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, m_heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, m_objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[4] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 13, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 14, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 4, w, 0, nullptr);
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
    // No fur -> nothing to composite. This is the whole cost of the feature
    // when it is off, and it guarantees the frame is byte-identical to the
    // frame without it.
    if (m_objectCount == 0 || m_cardCount == 0 || !m_pipeline || !m_set)
        return;
    // The host-written table has to reach the device: the buffer is
    // host-visible but not host-coherent, so a plain memcpy into the mapped
    // range does not become visible without a flush.
    if (m_table.mapped)
        vmaFlushAllocation(m_ctx->allocator(), m_table.alloc, 0, VK_WHOLE_SIZE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t(push.a.z + 7) / 8, uint32_t(push.a.w + 7) / 8, 1);
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
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
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
    m_pipeline = VK_NULL_HANDLE;
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
