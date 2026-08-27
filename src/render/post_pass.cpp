#include "render/post_pass.hpp"
#include <fstream>
#include <spdlog/spdlog.h>

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

bool PostPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[3] = { 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 4;
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

    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/post.comp.spv");
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
        spdlog::critical("post: pipeline failed ({})", int(r));
        return false;
    }

    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
                                      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    m_selection = makeBuffer(ctx, 2 * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_selection.buf || !m_selection.mapped)
        return false;
    glm::vec4 init[2] = { m_selFeed, m_hovFeed };
    memcpy(m_selection.mapped, init, sizeof(init));
    VkDescriptorBufferInfo selInfo { m_selection.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 3, 0, 1,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &selInfo, nullptr };
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

void PostPass::updateDescriptors(VkImageView hdrView, VkImageView gposView, VkImageView outView)
{
    if (!m_set || !m_ctx)
        return;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[3] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 2, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
}

void PostPass::record(VkCommandBuffer cmd, const RaymarchPush& push) const
{
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

void PostPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
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
