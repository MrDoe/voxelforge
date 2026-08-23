#include "render/taa_pass.hpp"
#include <fstream>
#include <spdlog/spdlog.h>

namespace vf {

namespace {
std::vector<uint8_t> loadSpirv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { spdlog::critical("Cannot open shader '{}'", path); return {}; }
    size_t n = static_cast<size_t>(f.tellg()); f.seekg(0);
    std::vector<uint8_t> d(n); f.read(reinterpret_cast<char*>(d.data()), std::streamsize(n));
    return d;
}
}

bool TaaPass::init(const Context& ctx) {
    m_ctx = &ctx;
    VkDevice dev = ctx.device();
    VkDescriptorSetLayoutBinding b[3] = {};
    for (int i=0;i<3;++i) b[i] = { uint32_t(i), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)*4 };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount=1; pli.pSetLayouts=&m_setLayout; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS) return false;
    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/taa_resolve.comp.spv");
    if (spirv.empty()) return false;
    VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = spirv.size(); mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule mod; vkCreateShaderModule(dev, &mci, nullptr, &mod);
    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = mod; cpi.stage.pName = "main";
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[1] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 } };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool=m_pool; ai.descriptorSetCount=1; ai.pSetLayouts=&m_setLayout;
    return vkAllocateDescriptorSets(dev, &ai, &m_set) == VK_SUCCESS;
}

void TaaPass::destroy() {
    if (!m_ctx) return;
    VkDevice dev=m_ctx->device();
    if (m_pool) vkDestroyDescriptorPool(dev,m_pool,nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev,m_setLayout,nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev,m_layout,nullptr);
    if (m_pipeline) vkDestroyPipeline(dev,m_pipeline,nullptr);
    m_pool=nullptr; m_setLayout=nullptr; m_layout=nullptr; m_pipeline=nullptr;
}

void TaaPass::updateDescriptors(VkImageView cur, VkImageView hist, VkImageView out) {
    if (!m_set || !m_ctx) return;
    VkDescriptorImageInfo i0{ VK_NULL_HANDLE, cur, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i1{ VK_NULL_HANDLE, hist, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo i2{ VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[3]={};
    w[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i0,nullptr,nullptr};
    w[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i1,nullptr,nullptr};
    w[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,m_set,2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&i2,nullptr,nullptr};
    vkUpdateDescriptorSets(m_ctx->device(),3,w,0,nullptr);
}

void TaaPass::record(VkCommandBuffer cmd, uint32_t w, uint32_t h, float blend, bool first) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
    struct PC { float invW, invH, blend, first; } pc{ 1.0f/float(w), 1.0f/float(h), blend, first?1.0f:0.0f };
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (w+7)/8, (h+7)/8, 1);
}

} // namespace vf
