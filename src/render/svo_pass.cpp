#include "render/svo_pass.hpp"
#include <core/log.hpp>
#include <cstring>
#include <fstream>

namespace vf {

namespace {

std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::critical("Cannot open shader '{}'", path);
        return {};
    }
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> d(n);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(n));
    return d;
}

} // namespace

bool SvoPass::uploadSsbo(Ssbo& s, const void* data, size_t bytes)
{
    // zero-sized sections (e.g. a fully-solid world has no nodes) still need
    // a bindable buffer for the descriptor set - use a 4-byte dummy
    static const uint32_t dummy = 0;
    if (bytes == 0) {
        data = &dummy;
        bytes = sizeof(dummy);
    }
    Buffer staging =
        makeBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!staging.buf || !data)
        return false;
    memcpy(staging.mapped, data, bytes);

    s = {};
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    bool ok = vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &s.buf, &s.alloc, nullptr) ==
              VK_SUCCESS;

    if (ok) {
        ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy c { 0, 0, bytes };
            vkCmdCopyBuffer(cmd, staging.buf, s.buf, 1, &c);
        });
    }
    destroyBuffer(*m_ctx, staging);
    return ok;
}

void SvoPass::destroySsbo(Ssbo& s)
{
    if (s.buf)
        vmaDestroyBuffer(m_ctx->allocator(), s.buf, s.alloc);
    s = {};
}

bool SvoPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    // Note: binding 0 is intentionally absent (an unwritten binding in the
    // layout invalidates the whole descriptor set on this driver). uHdr lives
    // at 9, uGPos at 10; the shader no longer references binding 0.
    VkDescriptorSetLayoutBinding b[10] = {};
    int n = 0;
    for (uint32_t i = 1; i < 6; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

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

    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/svo_raymarch.comp.spv");
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
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("svo: pipeline failed ({})", int(r));
        return false;
    }

    VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 },
                                       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    // highlight feeds: tiny host-visible uniform buffer, bound once
    m_selection = makeBuffer(ctx, 2 * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_selection.buf || !m_selection.mapped)
        return false;
    glm::vec4 init[2] = { m_selFeed, m_hovFeed };
    memcpy(m_selection.mapped, init, sizeof(init));
    VkDescriptorBufferInfo selInfo { m_selection.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &selInfo, nullptr };
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

void SvoPass::setWorld(const std::vector<int32_t>& grid,
                       const std::vector<uint32_t>& childBase,
                       const std::vector<uint32_t>& payload,
                       const std::vector<uint32_t>& handles,
                       const std::vector<uint32_t>& bricks)
{
    destroySsbo(m_grid);
    destroySsbo(m_childBase);
    destroySsbo(m_payload);
    destroySsbo(m_handles);
    destroySsbo(m_bricks);
    uploadSsbo(m_grid, grid.data(), grid.size() * 4);
    uploadSsbo(m_childBase, childBase.data(), childBase.size() * 4);
    uploadSsbo(m_payload, payload.data(), payload.size() * 4);
    uploadSsbo(m_handles, handles.data(), handles.size() * 4);
    uploadSsbo(m_bricks, bricks.data(), bricks.size() * 4);

    // bind
    VkDescriptorBufferInfo infos[5] = {
        { m_grid.buf, 0, VK_WHOLE_SIZE },       { m_childBase.buf, 0, VK_WHOLE_SIZE },
        { m_payload.buf, 0, VK_WHOLE_SIZE },    { m_handles.buf, 0, VK_WHOLE_SIZE },
        { m_bricks.buf, 0, VK_WHOLE_SIZE },
    };
    VkWriteDescriptorSet w[5] = {};
    for (int i = 0; i < 5; ++i)
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, uint32_t(i + 1), 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 5, w, 0, nullptr);
}

void SvoPass::setHeightmapView(VkImageView view) { m_heightView = view; }
void SvoPass::setObjVolumeView(VkImageView view) { m_objVolView = view; }

void SvoPass::updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage)
{
    if (!m_set || !m_ctx)
        return;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, m_heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, m_objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[4] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 6, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 7, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 10, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 4, w, 0, nullptr);
}

void SvoPass::record(VkCommandBuffer cmd, const RaymarchPush& push)
{
    // flush the staged highlight feeds into the persistently mapped UBO
    if (m_selection.mapped) {
        glm::vec4 feeds[2] = { m_selFeed, m_hovFeed };
        memcpy(m_selection.mapped, feeds, sizeof(feeds));
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t(push.a.z + 7) / 8, uint32_t(push.a.w + 7) / 8, 1);
}

void SvoPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
    destroySsbo(m_grid);
    destroySsbo(m_childBase);
    destroySsbo(m_payload);
    destroySsbo(m_handles);
    destroySsbo(m_bricks);
    if (m_selection.buf)
        destroyBuffer(*m_ctx, m_selection);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
}

} // namespace vf
