#include "render/splat_pass.hpp"
#include "rhi/resources.hpp"
#include <core/log.hpp>
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

bool SplatPass::init(const Context& ctx, VkFormat colorFormat, const SplatVertexData& data)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();
    m_count = data.posRadius.size();
    if (m_count == 0) {
        spdlog::warn("splat: no splats generated");
        return false;
    }

    // interleaved SSBO: posRadius[i], colors[i]
    std::vector<glm::vec4> packed(m_count * 2);
    for (size_t i = 0; i < m_count; ++i) {
        packed[2 * i + 0] = data.posRadius[i];
        packed[2 * i + 1] = data.colors[i];
    }

    Buffer staging =
        makeBuffer(ctx, packed.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    memcpy(staging.mapped, packed.data(), packed.size() * sizeof(glm::vec4));

    m_buf = makeBuffer(ctx, packed.size() * sizeof(glm::vec4),
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false)
                .buf;
    if (!m_buf) {
        spdlog::critical("splat ssbo alloc failed");
        return false;
    }
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, 0, packed.size() * sizeof(glm::vec4) };
        vkCmdCopyBuffer(cmd, staging.buf, m_buf, 1, &c);
    });
    destroyBuffer(ctx, staging);

    // descriptor set layout + pool + set
    VkDescriptorSetLayoutBinding b { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize ps { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    VkDescriptorBufferInfo bi { m_buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr };
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    // shaders
    auto vs = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.vert.spv");
    auto fs = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.frag.spv");
    if (vs.empty() || fs.empty())
        return false;
    VkShaderModule vsm = createModule(dev, vs);
    VkShaderModule fsm = createModule(dev, fs);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    }; // fetch in-shader

    VkPipelineInputAssemblyStateCreateInfo ia {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo vp { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    }; // no depth buffer: painter-style blending only

    VkPipelineColorBlendAttachmentState cba {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;               // premultiplied alpha
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prc { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 1;
    prc.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo gpi { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.pNext = &prc;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dy;
    gpi.layout = m_layout;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, vsm, nullptr);
    vkDestroyShaderModule(dev, fsm, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("splat: graphics pipeline failed ({})", int(r));
        return false;
    }
    spdlog::info("SplatPass ready: {} surface splats", m_count);
    return true;
}

void SplatPass::record(VkCommandBuffer cmd, VkExtent2D extent, const RaymarchPush& push) const
{
    VkClearValue clear {};
    clear.color = { { 0.30f, 0.42f, 0.60f, 1.0f } }; // sky-ish backdrop

    VkRenderingAttachmentInfo att { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    att.imageView = m_outView;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue = clear;

    VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;

    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    VkRect2D sc { { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, uint32_t(m_count), 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void SplatPass::destroy()
{
    VkDevice dev = m_ctx ? m_ctx->device() : VK_NULL_HANDLE;
    if (!dev)
        return;
    if (m_buf)
        vmaDestroyBuffer(m_ctx->allocator(), m_buf, m_alloc);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
    m_buf = VK_NULL_HANDLE;
    m_alloc = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
}

} // namespace vf
