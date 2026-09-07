#include "render/splat_pass.hpp"
#include <core/log.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

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

VkShaderModule makeModule(VkDevice dev, const std::vector<uint8_t>& spirv)
{
    VkShaderModuleCreateInfo mci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = spirv.size();
    mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &mci, nullptr, &mod);
    return mod;
}

} // namespace

bool SplatPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    VkDescriptorSetLayoutBinding b[8] = {};
    b[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[3] = { 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                VK_SHADER_STAGE_COMPUTE_BIT),
             nullptr };
    b[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT, nullptr }; // compact indirection
    b[5] = { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // selection entries
    b[6] = { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // draw command stream
    b[7] = { 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // frustum planes
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 8;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                                VK_SHADER_STAGE_FRAGMENT_BIT |
                                                VK_SHADER_STAGE_COMPUTE_BIT),
                             0, sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    if (!createPipelines(VK_FORMAT_R16G16B16A16_SFLOAT))
        return false;

    VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
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

    m_paramsBuf = makeBuffer(ctx, 2 * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_paramsBuf.buf || !m_paramsBuf.mapped)
        return false;
    {
        glm::vec4 init[2] = { m_params, glm::vec4(m_radiusScale, 0.0f, 0.0f, 0.0f) };
        memcpy(m_paramsBuf.mapped, init, sizeof(init));
    }
    VkDescriptorBufferInfo pi3 { m_paramsBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 3, 0, 1,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &pi3, nullptr };
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    // per-frame indirect draw command buffers (one vkCmdDraw per visible
    // chunk would be ~12k API calls/frame; indirect collapses each opaque
    // pass to a single call)
    for (auto& c : m_drawCmds) {
        c = makeBuffer(ctx, kMaxChunkDraws * sizeof(VkDrawIndirectCommand),
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!c.buf || !c.mapped)
            return false;
    }
    for (auto& c : m_waterCmds) {
        c = makeBuffer(ctx, kMaxChunkDraws * sizeof(VkDrawIndirectCommand),
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!c.buf || !c.mapped)
            return false;
    }
    // compaction slots: slot -> surfel index, identity-filled at upload;
    // sized on demand in setSurfels (recreated when the set grows)
    if (!createCullPipeline())
        return false;
    for (auto& s : m_selBufs) {
        s = makeBuffer(ctx, kMaxChunkDraws * sizeof(uint32_t) * 4,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!s.buf || !s.mapped)
            return false;
    }
    m_planesBuf = makeBuffer(ctx, 6 * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_planesBuf.buf || !m_planesBuf.mapped)
        return false;
    return true;
}

bool SplatPass::createPipelines(VkFormat hdrFormat)
{
    VkDevice dev = m_ctx->device();
    auto vsSpirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.vert.spv");
    auto fsSpirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.frag.spv");
    if (vsSpirv.empty() || fsSpirv.empty())
        return false;
    VkShaderModule vsm = makeModule(dev, vsSpirv);
    VkShaderModule fsm = makeModule(dev, fsSpirv);
    if (!vsm || !fsm)
        return false;

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
    };
    VkPipelineInputAssemblyStateCreateInfo ia {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkFormat colorFmts[2] = { hdrFormat, hdrFormat };
    VkPipelineRenderingCreateInfo prc { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 2;
    prc.pColorAttachmentFormats = colorFmts;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    // No depth prepass: the main pass writes gl_FragDepth (exact plane
    // depth), which disables early-z, so a prepass cannot reduce fragment
    // cost - it only ever changed rim blending. Per-fragment marches were
    // the real cost driver; those are baked on the CPU instead.
    auto makePipe = [&](int skyMode, int passMode, bool depthTest,
                        bool depthWrite, VkCompareOp depthOp, bool blend,
                        VkBlendOp blendOp, bool stencilTest, VkCompareOp stencilOp,
                        uint32_t stencilRef, bool stencilWrite,
                        VkFormat depthFmt, VkPipeline* out) {
        VkSpecializationMapEntry entries[2] = { { 0, 0, sizeof(int32_t) },
                                                { 1, sizeof(int32_t), sizeof(int32_t) } };
        int32_t specData[2] = { skyMode, passMode };
        VkSpecializationInfo spec { 2, entries, sizeof(specData), &specData };
        VkPipelineShaderStageCreateInfo st[2] = { stages[0], stages[1] };
        st[0].pSpecializationInfo = &spec;
        st[1].pSpecializationInfo = &spec;

        VkPipelineDepthStencilStateCreateInfo ds {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
        };
        ds.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = depthOp;
        // Stencil marks core-covered pixels (core pipe) so the two rim
        // blends can tell surface-interior apart from true silhouette.
        ds.stencilTestEnable = stencilTest ? VK_TRUE : VK_FALSE;
        VkStencilOpState stencilState {};
        stencilState.failOp = VK_STENCIL_OP_KEEP;
        stencilState.passOp = stencilWrite ? VK_STENCIL_OP_REPLACE
                                           : VK_STENCIL_OP_KEEP;
        stencilState.depthFailOp = VK_STENCIL_OP_KEEP;
        stencilState.compareOp = stencilOp;
        stencilState.compareMask = 0xFF;
        stencilState.writeMask = stencilWrite ? 0xFF : 0x00;
        stencilState.reference = stencilRef;
        ds.front = stencilState;
        ds.back = stencilState;

        VkPipelineColorBlendAttachmentState cba[2] = {};
        cba[0].colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba[1].colorWriteMask = cba[0].colorWriteMask;
        // att0 (HDR): alpha blend for rims/water, lighten-only MAX for
        // interior rims (never darken settled surface); att1 (G-buffer
        // world pos) must never blend. (MAX ignores factors per spec, but
        // ONE/ONE is set anyway so both interpretations agree.)
        const bool useMax = (blendOp == VK_BLEND_OP_MAX);
        cba[0].blendEnable = blend ? VK_TRUE : VK_FALSE;
        cba[0].srcColorBlendFactor =
            useMax ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].dstColorBlendFactor =
            useMax ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].colorBlendOp = blendOp;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
        };
        cb.attachmentCount = 2;
        cb.pAttachments = cba;

        VkPipelineRenderingCreateInfo lprc = prc;
        lprc.depthAttachmentFormat = depthFmt;

        VkGraphicsPipelineCreateInfo gpi { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.pNext = &lprc;
        gpi.stageCount = 2;
        gpi.pStages = st;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &dy;
        gpi.layout = m_layout;
        // NOTE: spec points at stack `mode`, consumed synchronously by create.
        return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, out) ==
               VK_SUCCESS;
    };

    // Core pass settles opaque depth+color and marks stencil (any order:
    // all fragments opaque); rim pass splits by stencil: interior rims
    // lighten-only (MAX, never darken settled surface), silhouette rims
    // alpha-blend over sky. Water stays a single full-disk pass.
    // Rim depth test is strict LESS against the (slightly toward-camera
    // biased) core depth: same-surface overlap rims can never pass it, so
    // they can't flicker on an LEQUAL coin-flip; true silhouettes clear it
    // by centimetres and are unaffected.
    const VkFormat kDepthFmt = VK_FORMAT_D24_UNORM_S8_UINT;
    bool ok =
        makePipe(1, 0, false, false, VK_COMPARE_OP_LESS, false, VK_BLEND_OP_ADD,
                 false, VK_COMPARE_OP_ALWAYS, 0, false, VK_FORMAT_UNDEFINED,
                 &m_skyPipe) &&
        makePipe(0, 1, true, true, VK_COMPARE_OP_LESS, true, VK_BLEND_OP_ADD,
                 true, VK_COMPARE_OP_ALWAYS, 1, true, kDepthFmt, &m_corePipe) &&
        makePipe(0, 2, true, false, VK_COMPARE_OP_LESS, true, VK_BLEND_OP_MAX,
                 true, VK_COMPARE_OP_EQUAL, 1, false, kDepthFmt, &m_rimInPipe) &&
        makePipe(0, 2, true, false, VK_COMPARE_OP_LESS, true, VK_BLEND_OP_ADD,
                 true, VK_COMPARE_OP_EQUAL, 0, false, kDepthFmt, &m_rimOutPipe) &&
        makePipe(0, 0, true, false, VK_COMPARE_OP_LESS, true, VK_BLEND_OP_ADD,
                 false, VK_COMPARE_OP_ALWAYS, 0, false, kDepthFmt, &m_waterPipe);
    vkDestroyShaderModule(dev, vsm, nullptr);
    vkDestroyShaderModule(dev, fsm, nullptr);
    if (!ok)
        spdlog::critical("splat: graphics pipeline failed");
    return ok;
}

void SplatPass::setSurfels(const void* data, size_t bytes, size_t count,
                           const std::vector<uint32_t>& chunkRange, uint32_t waterStart,
                           const std::vector<uint32_t>& waterChunkRange,
                           const std::vector<uint32_t>& microStart,
                           const std::vector<uint32_t>& lod1Range,
                           const std::vector<uint32_t>& lod2Range)
{
    if (m_surfelBuf) {
        vmaDestroyBuffer(m_ctx->allocator(), m_surfelBuf, m_surfelAlloc);
        m_surfelBuf = VK_NULL_HANDLE;
        m_surfelAlloc = VK_NULL_HANDLE;
    }
    m_count = 0;
    m_waterStart = 0;
    m_chunkRange.clear();
    static const uint32_t dummy = 0;
    const void* src = (data && bytes) ? data : &dummy;
    size_t up = (data && bytes) ? bytes : sizeof(dummy);
    Buffer staging = makeBuffer(*m_ctx, up, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!staging.buf || !src)
        return;
    memcpy(staging.mapped, src, up);
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = up;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    bool ok = vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &m_surfelBuf, &m_surfelAlloc,
                              nullptr) == VK_SUCCESS;
    if (ok) {
        ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy c { 0, 0, up };
            vkCmdCopyBuffer(cmd, staging.buf, m_surfelBuf, 1, &c);
        });
    }
    destroyBuffer(*m_ctx, staging);
    if (!ok)
        return;
    VkDescriptorBufferInfo bi0 { m_surfelBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi0, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    // compaction indirection: identity (slot == surfel index). The GPU cull
    // pre-pass rewrites each entry's range per frame; with culling disabled
    // the identity keeps the VS path equivalent to the direct index.
    const size_t need = up; // bytes of the surfel stream
    if (need > m_compactBytes) {
        if (m_compactBuf.buf)
            destroyBuffer(*m_ctx, m_compactBuf);
        m_compactBuf = makeBuffer(*m_ctx, need,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false);
        m_compactBytes = m_compactBuf.buf ? need : 0;
        if (!m_compactBuf.buf)
            return;
        Buffer idStaging = makeBuffer(*m_ctx, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (idStaging.mapped) {
            auto* ids = static_cast<uint32_t*>(idStaging.mapped);
            for (size_t i = 0; i < need / 4; ++i)
                ids[i] = uint32_t(i);
            m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
                VkBufferCopy c { 0, 0, need };
                vkCmdCopyBuffer(cmd, idStaging.buf, m_compactBuf.buf, 1, &c);
            });
        }
        destroyBuffer(*m_ctx, idStaging);
    }
    VkDescriptorBufferInfo ci0 { m_compactBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wc { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 4, 0, 1,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ci0, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &wc, 0, nullptr);
    // cull-only bindings: selection entries (slot 5, set at record time),
    // command stream (slot 6 = the indirect buffers), frustum planes (slot 7)
    VkDescriptorBufferInfo selInfo { m_selBufs[0].buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo cmdInfo { m_drawCmds[0].buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo plInfo { m_planesBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wcc[3] = {};
    wcc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wcc[0].dstSet = m_set;
    wcc[0].dstBinding = 5;
    wcc[0].descriptorCount = 1;
    wcc[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wcc[0].pBufferInfo = &selInfo;
    wcc[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wcc[1].dstSet = m_set;
    wcc[1].dstBinding = 6;
    wcc[1].descriptorCount = 1;
    wcc[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wcc[1].pBufferInfo = &cmdInfo;
    wcc[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wcc[2].dstSet = m_set;
    wcc[2].dstBinding = 7;
    wcc[2].descriptorCount = 1;
    wcc[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wcc[2].pBufferInfo = &plInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 3, wcc, 0, nullptr);
    m_count = (data && bytes) ? count : 0;
    m_waterStart = (data && bytes) ? waterStart : 0;
    m_chunkRange = chunkRange;
    m_waterChunkRange = ((data && bytes) && waterChunkRange.size() == 16 * 16 * 16 + 1)
                            ? waterChunkRange
                            : std::vector<uint32_t>();
    m_microStart = ((data && bytes) && microStart.size() == 16 * 16 * 16 + 1)
                       ? microStart
                       : std::vector<uint32_t>();
    m_lod1Range = ((data && bytes) && lod1Range.size() == 16 * 16 * 16 + 1)
                      ? lod1Range
                      : std::vector<uint32_t>();
    m_lod2Range = ((data && bytes) && lod2Range.size() == 16 * 16 * 16 + 1)
                      ? lod2Range
                      : std::vector<uint32_t>();
}

bool SplatPass::createCullPipeline()
{
    VkDevice dev = m_ctx->device();
    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat_cull.comp.spv");
    if (spirv.empty())
        return false;
    VkShaderModule mod = makeModule(dev, spirv);
    if (!mod)
        return false;
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_cullPipe);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("splat: cull pipeline failed");
        return false;
    }
    return true;
}

bool SplatPass::recreateDepth(uint32_t w, uint32_t h)
{
    destroyImage3D(*m_ctx, m_depth);
    // D24+S8: depth for plane ordering, stencil to tell interior rims
    // (lighten-only blend) apart from silhouette rims (alpha blend)
    m_depth = makeImage2D(*m_ctx, w, h, VK_FORMAT_D24_UNORM_S8_UINT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                             VK_IMAGE_ASPECT_STENCIL_BIT));
    return m_depth.img != VK_NULL_HANDLE;
}

void SplatPass::updateDescriptors(VkImageView hdrView, VkImageView gposView,
                                  VkImageView heightView, VkImageView objVolView)
{
    if (!m_set || !m_ctx)
        return;
    m_hdrView = hdrView;
    m_gposView = gposView;
    m_heightView = heightView;
    m_objVolView = objVolView;
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 2, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
}

void SplatPass::frustumPlanes(const RaymarchPush& push, glm::vec4 planes[6]) const
{
    const glm::vec3 fwd = glm::vec3(push.camFwd);
    const glm::vec3 right = glm::vec3(push.camRight);
    const glm::vec3 up = glm::vec3(push.camUp);
    const float tanH = push.a.x;
    const float asp = push.a.y;
    glm::vec3 ns[6];
    // left / right / bottom / top / near / far, inward normals. Each side
    // plane contains camPos and a frustum edge; the inward normal is the
    // perpendicular pointing at the frustum interior, e.g. left plane
    // (edge = fwd - t*right) has inward normal (right*t + fwd).
    ns[0] = glm::normalize(right + fwd * (tanH * asp)); // left plane inward
    ns[1] = glm::normalize(fwd * (tanH * asp) - right); // right plane inward
    ns[2] = glm::normalize(up + fwd * tanH);            // bottom plane inward
    ns[3] = glm::normalize(fwd * tanH - up);            // top plane inward
    ns[4] = fwd;                                        // near
    ns[5] = -fwd;                                       // far
    const glm::vec3 p = glm::vec3(push.camPos);
    const float nearD = 0.1f, farD = 400.0f;
    for (int i = 0; i < 4; ++i)
        planes[i] = glm::vec4(ns[i], -glm::dot(ns[i], p));
    planes[4] = glm::vec4(ns[4], -(glm::dot(ns[4], p) + nearD));
    planes[5] = glm::vec4(ns[5], -(glm::dot(ns[5], p) - farD));
}

bool SplatPass::chunkVisible(const glm::vec4 planes[6], uint32_t chunk) const
{
    // chunk grid: 16^3 over [-51.2, +51.2], index (cx*16+cy)*16+cz
    const uint32_t cx = chunk / 256, cy = (chunk / 16) % 16, cz = chunk % 16;
    const float cs = 102.4f / 16.0f;
    const glm::vec3 mn(-51.2f + cx * cs, -51.2f + cy * cs, -51.2f + cz * cs);
    const glm::vec3 mx = mn + glm::vec3(cs);
    for (int i = 0; i < 6; ++i) {
        // p-vertex test: the corner most inside the half-space; if even it
        // is outside, the whole box is culled
        glm::vec3 v = mn;
        if (planes[i].x > 0.0f) v.x = mx.x;
        if (planes[i].y > 0.0f) v.y = mx.y;
        if (planes[i].z > 0.0f) v.z = mx.z;
        if (glm::dot(glm::vec3(planes[i]), v) + planes[i].w < 0.0f)
            return false;
    }
    return true;
}

void SplatPass::computeDraws(const RaymarchPush& push)
{
    m_cpuDraws.clear();
    m_cpuWaterDraws.clear();
    m_waterDraws = 0;
    if (m_count == 0)
        return;
    const uint32_t opaqueEnd =
        m_waterStart > 0 ? std::min(m_waterStart, uint32_t(m_count)) : uint32_t(m_count);
    if (opaqueEnd == 0)
        return;
    if (m_chunkRange.size() != 16 * 16 * 16 + 1) {
        // no chunking info (should not happen): draw everything opaque
        m_cpuDraws.push_back({ 4, opaqueEnd, 0, 0 });
        return;
    }
    glm::vec4 planes[6];
    frustumPlanes(push, planes);
    // back-to-front chunk order: with translucent Gaussian rims, far
    // must blend first so near geometry (and the sky behind silhouettes)
    // composites correctly. 4096 distance evaluations + sort per frame
    // is ~100 us; within-chunk order errors are bounded by the 6.4 m
    // chunk size and resolved by the depth test for opaque cores.
    struct Draw {
        float dist2;
        uint32_t first, count;
        bool rim; // chunk still carries rim geometry (near-field AA band)
    };
    std::vector<Draw> draws;
    draws.reserve(1024);
    const glm::vec3 camPos = glm::vec3(push.camPos);
    const bool cull = !getenv("VF_SPLAT_NOCULL");
    // Rim fade: splat.frag ramps coreD2 -> 1.0 by 40 m, so rim fragments of
    // chunks entirely beyond that distance all discard. Compare the chunk
    // AABB's nearest point to the camera (conservative: keeps rims whenever
    // any surfel could still be inside the ramp).
    float rimDist2 = m_rimDist * m_rimDist;
    if (const char* e = getenv("VF_RIM_DIST"))
        rimDist2 = float(atof(e)) * float(atof(e));
    const bool hasRimSplit = rimDist2 > 0.0f;
    // LOD ring selection: chunks past VF_LOD1 (default 20 m) draw their
    // merged-terrain LOD1 run instead of base+micro; past VF_LOD2 (60 m)
    // the LOD2 run. Object-only chunks (empty merged runs) fall back to
    // the base range. 0 disables the ring.
    float lod1Dist = 20.0f, lod2Dist = 60.0f;
    if (const char* e = getenv("VF_LOD1"))
        lod1Dist = float(atof(e));
    if (const char* e = getenv("VF_LOD2"))
        lod2Dist = float(atof(e));
    const bool hasLod1 =
        m_lod1Range.size() == 16 * 16 * 16 + 1 && lod1Dist > 0.0f;
    const bool hasLod2 =
        m_lod2Range.size() == 16 * 16 * 16 + 1 && lod2Dist > 0.0f;
    // Micro-detail cull distance: micro disks (0.04-0.09 m) are sub-pixel
    // beyond ~20 m (1-2 px at 720p) and hide inside their base footprint, so
    // distant chunks draw base only. Measured cost of 20-40 m micros:
    // ~19 ms/frame at the reference view; visual diff 0.45% pixels >10
    // (hero view). VF_MICRO_DIST=0 keeps all micros (legacy), 40 = old value.
    float microDist2 = 20.0f * 20.0f;
    if (const char* e = getenv("VF_MICRO_DIST"))
        microDist2 = float(atof(e)) * float(atof(e));
    const bool hasMicroSplit = m_microStart.size() == 16 * 16 * 16 + 1;
    const float cs = 102.4f / 16.0f;
    for (uint32_t c = 0; c < 16 * 16 * 16; ++c) {
        uint32_t first = m_chunkRange[c];
        uint32_t last = m_chunkRange[c + 1];
        if (last > opaqueEnd)
            last = opaqueEnd;
        if (last <= first)
            continue;
        if (cull && !chunkVisible(planes, c))
            continue;
        const uint32_t cx = c / 256, cy = (c / 16) % 16, cz = c % 16;
        const glm::vec3 ctr(-51.2f + (float(cx) + 0.5f) * 6.4f,
                            -51.2f + (float(cy) + 0.5f) * 6.4f,
                            -51.2f + (float(cz) + 0.5f) * 6.4f);
        const glm::vec3 d = ctr - camPos;
        const float dist2 = glm::dot(d, d);
        // nearest point of the chunk AABB: conservative rim eligibility and
        // LOD ring selection (slightly aggressive for LOD: surfel distances
        // can only be larger than the AABB nearest point)
        glm::vec3 ncp = camPos;
        if (hasRimSplit || hasLod1 || hasLod2) {
            const glm::vec3 mn(-51.2f + float(cx) * cs, -51.2f + float(cy) * cs,
                               -51.2f + float(cz) * cs);
            ncp = glm::clamp(camPos, mn, mn + glm::vec3(cs));
        }
        const float nearDist = std::sqrt(glm::dot(ncp - camPos, ncp - camPos));
        bool rim = !hasRimSplit || glm::dot(ncp - camPos, ncp - camPos) <= rimDist2;
        uint32_t split = last;
        if (hasMicroSplit)
            split = std::min(m_microStart[c], last);
        if (split < first)
            split = first;
        // LOD ring selection replaces the chunk's base terrain run; object
        // surfels ride along unmerged inside the ring (trees must never
        // vanish). The chunk's micro tail still applies on top, gated by
        // the same micro distance as base chunks.
        if (hasLod2 && nearDist >= lod2Dist &&
            m_lod2Range[c + 1] > m_lod2Range[c]) {
            draws.push_back({ dist2, m_lod2Range[c],
                              m_lod2Range[c + 1] - m_lod2Range[c], rim });
            if (hasMicroSplit && (microDist2 <= 0.0f || dist2 < microDist2)) {
                const uint32_t mf = std::min(m_microStart[c], opaqueEnd);
                const uint32_t ml = std::min(m_chunkRange[c + 1], opaqueEnd);
                if (ml > mf)
                    draws.push_back({ dist2, mf, ml - mf, rim });
            }
            continue;
        }
        if (hasLod1 && nearDist >= lod1Dist &&
            m_lod1Range[c + 1] > m_lod1Range[c]) {
            draws.push_back({ dist2, m_lod1Range[c],
                              m_lod1Range[c + 1] - m_lod1Range[c], rim });
            if (hasMicroSplit && (microDist2 <= 0.0f || dist2 < microDist2)) {
                const uint32_t mf = std::min(m_microStart[c], opaqueEnd);
                const uint32_t ml = std::min(m_chunkRange[c + 1], opaqueEnd);
                if (ml > mf)
                    draws.push_back({ dist2, mf, ml - mf, rim });
            }
            continue;
        }
        draws.push_back({ dist2, first, split > first ? split - first : 0, rim });
        // near chunks also draw their micro tail (same sort key: stable
        // sort below keeps base-then-micro order within the chunk)
        if (last > split && (microDist2 <= 0.0f || dist2 < microDist2))
            draws.push_back({ dist2, split, last - split, rim });
    }
    // stable: equal keys (base + micro of one chunk) keep insertion order
    std::stable_sort(draws.begin(), draws.end(),
                     [](const Draw& a, const Draw& b) { return a.dist2 > b.dist2; });
    m_cpuDraws.reserve(draws.size());
    // far->near order: core-only (rim=false) chunks form the contiguous
    // prefix, rim-carrying chunks the tail. First rim entry = rim passes'
    // indirect start (base + micro of one near chunk are adjacent entries).
    m_rimStart = uint32_t(draws.size());
    for (uint32_t i = 0; i < draws.size(); ++i) {
        if (draws[i].rim) {
            m_rimStart = i;
            break;
        }
    }
    for (const Draw& dr : draws) {
        if (dr.count == 0)
            continue;
        m_cpuDraws.push_back({ 4, dr.count, 0, dr.first });
    }
    if (getenv("VF_TRACE") && int(push.b.w) % 60 == 0) {
        double b[4] = {}; // <10, 10-20, 20-40, >40 m: quads per band
        uint32_t rimBase = 0;
        for (const Draw& dr : draws) {
            if (dr.count == 0)
                continue;
            const float d = std::sqrt(dr.dist2);
            double& acc = d < 10 ? b[0] : d < 20 ? b[1] : d < 40 ? b[2] : b[3];
            acc += dr.count;
            if (dr.rim)
                rimBase += dr.count;
        }
        spdlog::info(
            "splat bands: <10 {:.0f} | 10-20 {:.0f} | 20-40 {:.0f} | >40 {:.0f}"
            " | rim-eligible {:.0f}",
            b[0], b[1], b[2], b[3], double(rimBase));
    }
    // water chunks: same frustum cull, no sorting needed (single
    // blended pass, depth-tested). Off-screen lake chunks emit no
    // commands at all instead of rasterizing thousands of quads.
    if (m_waterStart > 0 && uint64_t(m_waterStart) < m_count &&
        !getenv("VF_SPLAT_NOWATERCULL")) {
        if (m_waterChunkRange.size() == 16 * 16 * 16 + 1) {
            for (uint32_t c = 0; c < 16 * 16 * 16; ++c) {
                uint32_t first = m_waterChunkRange[c];
                uint32_t last = m_waterChunkRange[c + 1];
                if (last <= first)
                    continue;
                if (cull && !chunkVisible(planes, c))
                    continue;
                m_cpuWaterDraws.push_back({ 4, last - first, 0, first });
            }
        }
    }
    if (m_cpuWaterDraws.empty() && m_waterStart > 0 &&
        uint64_t(m_waterStart) < m_count) {
        // legacy: unbucketed water draws in one call
        // (record() still gates on VF_SPLAT_NOWATER)
        m_cpuWaterDraws.push_back(
            { 4, uint32_t(m_count) - m_waterStart, 0, m_waterStart });
    }
    m_waterDraws = uint32_t(m_cpuWaterDraws.size());
}

void SplatPass::record(VkCommandBuffer cmd, const RaymarchPush& push, VkExtent2D extent)
{
    glm::vec4 params = m_params;
    params.x = m_buried ? 1.0f : 0.0f;
    if (const char* e = getenv("VF_SPLAT_CORE"))
        params.y = float(atof(e));
    if (const char* e = getenv("VF_SPLAT_EXTENT"))
        params.z = float(atof(e));
    // debug view modes (FS): 1 = flat white coverage, 2 = normal->rgb,
    // 3 = plane-depth heat. VS skips backface collapse when != 0.
    if (const char* e = getenv("VF_SPLAT_DEBUG"))
        params.w = float(atof(e));
    if (const char* e = getenv("VF_SPLAT_RADIUS"))
        m_radiusScale = glm::clamp(float(atof(e)), 0.5f, 2.0f);
    if (m_paramsBuf.mapped) {
        glm::vec4 words[2] = { params,
                               glm::vec4(m_radiusScale, 0.0f, 0.0f, 0.0f) };
        memcpy(m_paramsBuf.mapped, words, sizeof(words));
    }

    // GPU-driven cull pre-pass (before rendering scope: compute may not run
    // inside vkCmdBeginRendering). Compacts every entry to its
    // fragment-producing surfels and rewrites the entry's instanceCount in
    // the indirect command stream; the draws then run via
    // vkCmdDrawIndirectCount. Culled quads emit zero fragments today, so
    // the image is unchanged - only vertex/clipper work disappears.
    computeDraws(push);
    const uint32_t nDraws = uint32_t(m_cpuDraws.size());
    if (getenv("VF_TRACE") && int(push.b.w) % 60 == 0) {
        uint32_t nTotal = 0;
        for (const auto& d : m_cpuDraws)
            nTotal += d.instanceCount;
        spdlog::info("splat pre-cull: {} quads in {} draws (rim from {}), water {}",
                     nTotal, nDraws, m_rimStart, m_waterDraws);
    }
    const bool direct = getenv("VF_SPLAT_DIRECT") != nullptr; // legacy A/B path
    const bool gpuCull =
        !direct && !getenv("VF_NO_GPU_CULL") && nDraws > 0 && m_cullPipe &&
        m_compactBuf.buf && m_selBufs[0].buf && m_planesBuf.buf;
    if (gpuCull) {
        // active triple-buffer slot into the descriptor set
        VkDescriptorBufferInfo selInfo { m_selBufs[m_cmdSlot].buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo cmdInfo { m_drawCmds[m_cmdSlot].buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wcc[2] = {};
        wcc[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 5, 0, 1,
                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &selInfo, nullptr };
        wcc[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 6, 0, 1,
                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cmdInfo, nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 2, wcc, 0, nullptr);
        auto* sel = static_cast<uint32_t*>(m_selBufs[m_cmdSlot].mapped);
        if (sel) {
            for (uint32_t i = 0; i < nDraws; ++i) {
                sel[i * 4 + 0] = m_cpuDraws[i].firstInstance;
                sel[i * 4 + 1] = m_cpuDraws[i].instanceCount;
                sel[i * 4 + 2] = 0;
                sel[i * 4 + 3] = 0;
            }
        }
        glm::vec4 planes[6];
        frustumPlanes(push, planes);
        if (m_planesBuf.mapped)
            memcpy(m_planesBuf.mapped, planes, sizeof(planes));
        // host-written selection/planes -> compute reads
        VkMemoryBarrier2 hb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        hb.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hb.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        hb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_UNIFORM_READ_BIT;
        VkDependencyInfo hdi { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        hdi.memoryBarrierCount = 1;
        hdi.pMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &hdi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1,
                                &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout,
                           VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT |
                                              VK_SHADER_STAGE_COMPUTE_BIT),
                           0, sizeof(push), &push);
        vkCmdDispatch(cmd, nDraws, 1, 1);
        // cull writes (command stream + compaction) -> indirect + VS reads
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    VkRenderingAttachmentInfo colors[2] = {};
    colors[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colors[0].imageView = m_hdrView;
    colors[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colors[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colors[1].imageView = m_gposView;
    colors[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depth.imageView = m_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    // stencil shares the depth image: same view/layout, cleared together
    // (loadOp CLEAR above zeroes it; DONT_CARE store)
    ri.pStencilAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    VkRect2D sc { { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_set,
                            0, nullptr);
    vkCmdPushConstants(cmd, m_layout,
                       VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                          VK_SHADER_STAGE_FRAGMENT_BIT),
                       0, sizeof(push), &push);

    // sky first (no depth): every pixel gets sky + hitType 0
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // opaque cores first (any order: every fragment opaque, depth settles
    // the watertight surface and marks stencil), then rims split by stencil:
    // interior rims lighten-only (MAX: soft edges that can never darken
    // settled surface), silhouette rims alpha-blended over sky.
    constexpr VkDeviceSize stride = sizeof(VkDrawIndirectCommand);
    // drawOpaque draws [from, nDraws); the core pipe uses 0 (all chunks),
    // the rim pipes start at m_rimStart (far chunks are core-only: their
    // rim fragments would all discard beyond the 40 m coreD2 ramp).
    // With the GPU cull pre-pass active the command stream is GPU-written
    // (instanceCount per entry = compacted count), so plain indirect draws
    // consume the compacted instance counts directly.
    auto drawOpaque = [&](VkPipeline pipe, uint32_t from) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (direct) {
            for (uint32_t i = from; i < nDraws; ++i) {
                const auto& d = m_cpuDraws[i];
                vkCmdDraw(cmd, d.vertexCount, d.instanceCount, d.firstVertex,
                          d.firstInstance);
            }
        } else {
            vkCmdDrawIndirect(cmd, m_drawCmds[m_cmdSlot].buf,
                              VkDeviceSize(from) * stride, nDraws - from, stride);
        }
    };
    if (!direct && !gpuCull && (nDraws > 0 || m_waterDraws > 0) &&
        !getenv("VF_NO_INDIRECT_BARRIER")) {
        // host-written commands -> indirect-command read
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        auto* dst =
            static_cast<VkDrawIndirectCommand*>(m_drawCmds[m_cmdSlot].mapped);
        auto* wdst =
            static_cast<VkDrawIndirectCommand*>(m_waterCmds[m_cmdSlot].mapped);
        if (dst && m_cpuDraws.size() <= kMaxChunkDraws)
            memcpy(dst, m_cpuDraws.data(), m_cpuDraws.size() * stride);
        if (wdst && m_cpuWaterDraws.size() <= kMaxChunkDraws)
            memcpy(wdst, m_cpuWaterDraws.data(), m_cpuWaterDraws.size() * stride);
    } else if (gpuCull && m_waterDraws > 0) {
        // cull mode: opaque command stream is GPU-written; water stream
        // stays CPU-written and still needs its own host->indirect barrier
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        auto* wdst =
            static_cast<VkDrawIndirectCommand*>(m_waterCmds[m_cmdSlot].mapped);
        if (wdst && m_cpuWaterDraws.size() <= kMaxChunkDraws)
            memcpy(wdst, m_cpuWaterDraws.data(), m_cpuWaterDraws.size() * stride);
    }
    if (nDraws > 0) {
        if (!getenv("VF_SPLAT_NORIM"))
            drawOpaque(m_corePipe, 0);
        // Rim passes draw only the near tail (rim-carrying chunks). The
        // env names are swapped vs the pipes they skip (historical quirk):
        // VF_SPLAT_NOCORE skips the rim pipes, VF_SPLAT_NORIM the core pipe.
        if (!getenv("VF_SPLAT_NOCORE") && m_rimStart < nDraws) {
            drawOpaque(m_rimInPipe, m_rimStart);
            drawOpaque(m_rimOutPipe, m_rimStart);
        }
    }
    // water surfels: blended over, depth-tested, no depth write.
    // Culled per chunk like opaque (off-screen lake chunks emit nothing).
    if (!getenv("VF_SPLAT_NOWATER") && m_waterDraws > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipe);
        if (direct) {
            for (const auto& d : m_cpuWaterDraws)
                vkCmdDraw(cmd, d.vertexCount, d.instanceCount, d.firstVertex,
                          d.firstInstance);
        } else {
            vkCmdDrawIndirect(cmd, m_waterCmds[m_cmdSlot].buf, 0, m_waterDraws, stride);
        }
    }
    m_cmdSlot = (m_cmdSlot + 1) % 3;

    vkCmdEndRendering(cmd);
}

void SplatPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
    if (m_surfelBuf)
        vmaDestroyBuffer(m_ctx->allocator(), m_surfelBuf, m_surfelAlloc);
    if (m_paramsBuf.buf)
        destroyBuffer(*m_ctx, m_paramsBuf);
    for (auto& c : m_drawCmds)
        if (c.buf)
            destroyBuffer(*m_ctx, c);
    for (auto& c : m_waterCmds)
        if (c.buf)
            destroyBuffer(*m_ctx, c);
    destroyImage3D(*m_ctx, m_depth);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_skyPipe)
        vkDestroyPipeline(dev, m_skyPipe, nullptr);
    if (m_corePipe)
        vkDestroyPipeline(dev, m_corePipe, nullptr);
    if (m_rimInPipe)
        vkDestroyPipeline(dev, m_rimInPipe, nullptr);
    if (m_rimOutPipe)
        vkDestroyPipeline(dev, m_rimOutPipe, nullptr);
    if (m_waterPipe)
        vkDestroyPipeline(dev, m_waterPipe, nullptr);
    m_surfelBuf = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_skyPipe = m_corePipe = m_rimInPipe = m_rimOutPipe = m_waterPipe = VK_NULL_HANDLE;
}

} // namespace vf
