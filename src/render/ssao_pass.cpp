#include "render/ssao_pass.hpp"
#include <core/log.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace vf {

namespace {
std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t n = static_cast<size_t>(f.tellg()); f.seekg(0);
    std::vector<uint8_t> d(n); f.read(reinterpret_cast<char*>(d.data()), n);
    return d;
}
}

bool SSAOPass::init(const Context& ctx)
{
    m_ctx = &ctx; VkDevice dev = ctx.device();
    // 0 = AO scratch (ssao.comp writes, ssao_apply.comp reads), 1 = G-buffer
    // world pos, 2 = G-buffer normal, 3 = LDR scene, 4 = LDR out. One shared
    // set/layout serves both pipelines; each shader declares its own subset.
    VkDescriptorSetLayoutBinding b[5] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 5; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    // shared 128 B RaymarchPush + one pass-local vec4 (debug/strength/radius)
    VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(RaymarchPush) + sizeof(glm::vec4) };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount=1; pli.pSetLayouts=&m_setLayout; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS) return false;

    auto makePipeline = [&](const char* spirvName, VkPipeline* out) {
        auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/" + spirvName);
        if (spirv.empty()) return false;
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = spirv.size(); mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
        VkShaderModule mod; vkCreateShaderModule(dev, &mci, nullptr, &mod);
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.layout = m_layout; cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = mod; cpi.stage.pName = "main";
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, out);
        vkDestroyShaderModule(dev, mod, nullptr);
        return r == VK_SUCCESS;
    };
    if (!makePipeline("ssao.comp.spv", &m_pipeline)) return false;
    if (!makePipeline("ssao_apply.comp.spv", &m_applyPipeline)) return false;

    VkDescriptorPoolSize sizes[1] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 } };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool=m_pool; ai.descriptorSetCount=1; ai.pSetLayouts=&m_setLayout;
    return vkAllocateDescriptorSets(dev, &ai, &m_set) == VK_SUCCESS;
}

void SSAOPass::destroy()
{
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if (m_pool) vkDestroyDescriptorPool(dev,m_pool,nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev,m_setLayout,nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev,m_layout,nullptr);
    if (m_pipeline) vkDestroyPipeline(dev,m_pipeline,nullptr);
    if (m_applyPipeline) vkDestroyPipeline(dev,m_applyPipeline,nullptr);
}

void SSAOPass::updateDescriptors(VkImageView gposView, VkImageView gnormView,
                                 VkImageView aoView, VkImageView currentView,
                                 VkImageView outView)
{
    if (!m_set || !m_ctx) return;
    VkDescriptorImageInfo i0{ VK_NULL_HANDLE, aoView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i1{ VK_NULL_HANDLE, gposView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i2{ VK_NULL_HANDLE, gnormView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i3{ VK_NULL_HANDLE, currentView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i4{ VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[5] = {};
    w[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i0,nullptr,nullptr};
    w[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i1,nullptr,nullptr};
    w[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i2,nullptr,nullptr};
    w[3]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i3,nullptr,nullptr};
    w[4]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,4,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i4,nullptr,nullptr};
    vkUpdateDescriptorSets(m_ctx->device(),5,w,0,nullptr);
}

void SSAOPass::record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                      const RaymarchPush& push, const glm::vec4& params) const
{
    const uint32_t gx = (width + 7) / 8, gy = (height + 7) / 8;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RaymarchPush), &push);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       sizeof(RaymarchPush), sizeof(glm::vec4), &params);
    // 1) raw AO into the scratch image
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdDispatch(cmd, gx, gy, 1);
    // 2) barrier: scratch write -> apply-pass read (same dispatch domain)
    VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    // 3) denoise + apply into the LDR image
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_applyPipeline);
    vkCmdDispatch(cmd, gx, gy, 1);
}

} // namespace vf
