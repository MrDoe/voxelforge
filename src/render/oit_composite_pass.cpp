#include "render/oit_composite_pass.hpp"

#include <core/log.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace vf {

namespace {
std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::critical("oit composite: cannot open shader '{}'", path);
        return {};
    }
    const size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> d(n);
    f.read(reinterpret_cast<char*>(d.data()), n);
    return d;
}
} // namespace

bool OitCompositePass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    VkDescriptorSetLayoutBinding b[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 5;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    // No push constants: the shader reads everything from its four images.
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    const auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/oit_composite.comp.spv");
    if (spirv.empty())
        return false;
    VkShaderModuleCreateInfo mci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = spirv.size();
    mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &mci, nullptr, &mod);
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                                &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("oit composite: pipeline failed ({})", int(r));
        return false;
    }

    VkDescriptorPoolSize size { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;
    return true;
}

void OitCompositePass::updateDescriptors(VkImageView accum, VkImageView reveal,
                                         VkImageView backdrop, VkImageView hdr,
                                         VkImageView base)
{
    if (!m_set || !m_ctx)
        return;
    VkDescriptorImageInfo ii[5] = {
        { VK_NULL_HANDLE, accum, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, reveal, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, backdrop, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, hdr, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, base, VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet w[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, i, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], nullptr, nullptr };
    }
    vkUpdateDescriptorSets(m_ctx->device(), 5, w, 0, nullptr);
}

void OitCompositePass::record(VkCommandBuffer cmd, uint32_t width, uint32_t height) const
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set,
                            0, nullptr);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

void OitCompositePass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
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
