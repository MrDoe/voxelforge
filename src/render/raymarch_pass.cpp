#include "render/raymarch_pass.hpp"
#include <core/log.hpp>
#include <fstream>
#include <filesystem>

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
    std::vector<uint8_t> data(n);
    f.read(reinterpret_cast<char*>(data.data()), std::streamsize(n));
    return data;
}

VkShaderModule createModule(VkDevice dev, const std::vector<uint8_t>& spirv)
{
    VkShaderModuleCreateInfo ci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

} // namespace

bool RaymarchPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    // --- descriptor set layout -----------------------------------------
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) {
        spdlog::critical("raymarch: descriptor layout failed");
        return false;
    }

    // --- push layout ----------------------------------------------------
    VkPushConstantRange pc { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS) {
        spdlog::critical("raymarch: pipeline layout failed");
        return false;
    }

    // --- compute pipeline ------------------------------------------------
    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/raymarch.comp.spv");
    if (spirv.empty())
        return false;
    VkShaderModule mod = createModule(dev, spirv);
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_pipeline) !=
        VK_SUCCESS) {
        spdlog::critical("raymarch: compute pipeline failed");
        vkDestroyShaderModule(dev, mod, nullptr);
        return false;
    }
    vkDestroyShaderModule(dev, mod, nullptr);

    // --- pool & set -------------------------------------------------------
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
    };
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
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS) {
        spdlog::critical("raymarch: alloc descriptor set failed");
        return false;
    }
    return true;
}

void RaymarchPass::updateDescriptors(const Image3D& sdfVol, VkSampler sdfSampler,
                                     const Image3D& albedoVol, VkSampler albedoSampler,
                                     const Image3D& outImage)
{
    VkDescriptorImageInfo s0 { sdfSampler, sdfVol.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo s1 { albedoSampler, albedoVol.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo s2 { VK_NULL_HANDLE, outImage.view, VK_IMAGE_LAYOUT_GENERAL };

    VkWriteDescriptorSet w[3] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 0, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &s0, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &s1, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 2, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &s2, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 3, w, 0, nullptr);
}

void RaymarchPass::record(VkCommandBuffer cmd, const RaymarchPush& push) const
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t(push.a.z + 7) / 8, uint32_t(push.a.w + 7) / 8, 1);
}

void RaymarchPass::destroy()
{
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
