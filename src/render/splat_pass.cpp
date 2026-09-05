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

    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[3] = { 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
             nullptr };
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 4;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                                VK_SHADER_STAGE_FRAGMENT_BIT),
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

    m_paramsBuf = makeBuffer(ctx, sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_paramsBuf.buf || !m_paramsBuf.mapped)
        return false;
    memcpy(m_paramsBuf.mapped, &m_params, sizeof(m_params));
    VkDescriptorBufferInfo pi3 { m_paramsBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 3, 0, 1,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &pi3, nullptr };
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
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
    auto makePipe = [&](int skyMode, bool depthTest,
                        bool depthWrite, VkCompareOp depthOp, bool blend,
                        VkFormat depthFmt, VkPipeline* out) {
        VkSpecializationMapEntry entry { 0, 0, sizeof(int32_t) };
        int32_t mode = skyMode;
        VkSpecializationInfo spec { 1, &entry, sizeof(mode), &mode };
        VkPipelineShaderStageCreateInfo st[2] = { stages[0], stages[1] };
        st[0].pSpecializationInfo = &spec;
        st[1].pSpecializationInfo = &spec;

        VkPipelineDepthStencilStateCreateInfo ds {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
        };
        ds.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = depthOp;

        VkPipelineColorBlendAttachmentState cba[2] = {};
        cba[0].colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba[1].colorWriteMask = cba[0].colorWriteMask;
        // att0 (HDR): alpha blend for the AA annulus / water; att1 (G-buffer
        // world pos) must never blend.
        cba[0].blendEnable = blend ? VK_TRUE : VK_FALSE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
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

    bool ok =
        makePipe(1, false, false, VK_COMPARE_OP_LESS, false, VK_FORMAT_UNDEFINED,
                 &m_skyPipe) &&
        makePipe(0, true, true, VK_COMPARE_OP_LESS, true, VK_FORMAT_D32_SFLOAT,
                 &m_opaquePipe) &&
        makePipe(0, true, false, VK_COMPARE_OP_LESS, true, VK_FORMAT_D32_SFLOAT,
                 &m_waterPipe);
    vkDestroyShaderModule(dev, vsm, nullptr);
    vkDestroyShaderModule(dev, fsm, nullptr);
    if (!ok)
        spdlog::critical("splat: graphics pipeline failed");
    return ok;
}

void SplatPass::setSurfels(const void* data, size_t bytes, size_t count,
                           const std::vector<uint32_t>& chunkRange, uint32_t waterStart)
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
    m_count = (data && bytes) ? count : 0;
    m_waterStart = (data && bytes) ? waterStart : 0;
    m_chunkRange = chunkRange;
}

bool SplatPass::recreateDepth(uint32_t w, uint32_t h)
{
    destroyImage3D(*m_ctx, m_depth);
    m_depth = makeImage2D(*m_ctx, w, h, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
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

void SplatPass::drawChunks(VkCommandBuffer cmd, const RaymarchPush& push) const
{
    if (m_count == 0)
        return;
    const uint32_t opaqueEnd =
        m_waterStart > 0 ? std::min(m_waterStart, uint32_t(m_count)) : uint32_t(m_count);
    if (opaqueEnd == 0)
        return;
    if (m_chunkRange.size() == 16 * 16 * 16 + 1) {
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
        };
        std::vector<Draw> draws;
        draws.reserve(512);
        const glm::vec3 camPos = glm::vec3(push.camPos);
        const bool cull = !getenv("VF_SPLAT_NOCULL");
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
            draws.push_back({ glm::dot(d, d), first, last - first });
        }
        std::sort(draws.begin(), draws.end(),
                  [](const Draw& a, const Draw& b) { return a.dist2 > b.dist2; });
        for (const Draw& dr : draws)
            vkCmdDraw(cmd, 4, dr.count, 0, dr.first);
    } else {
        // no chunking info (should not happen): draw everything opaque
        vkCmdDraw(cmd, 4, opaqueEnd, 0, 0);
    }
}

void SplatPass::record(VkCommandBuffer cmd, const RaymarchPush& push, VkExtent2D extent)
{
    glm::vec4 params = m_params;
    if (const char* e = getenv("VF_SPLAT_CORE"))
        params.y = float(atof(e));
    if (const char* e = getenv("VF_SPLAT_EXTENT"))
        params.z = float(atof(e));
    // debug view modes (FS): 1 = flat white coverage, 2 = normal->rgb,
    // 3 = plane-depth heat. VS skips backface collapse when != 0.
    if (const char* e = getenv("VF_SPLAT_DEBUG"))
        params.w = float(atof(e));
    if (m_paramsBuf.mapped)
        memcpy(m_paramsBuf.mapped, &params, sizeof(params));

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

    // opaque surfels, one draw per visible chunk (depth test + write with
    // per-fragment plane depth: nearest disk wins, rims blend over it)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_opaquePipe);
    drawChunks(cmd, push);

    // water surfels: blended over, depth-tested, no depth write
    if (!getenv("VF_SPLAT_NOWATER") && m_waterStart > 0 && uint64_t(m_waterStart) < m_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipe);
        vkCmdDraw(cmd, 4, uint32_t(m_count) - m_waterStart, 0, m_waterStart);
    }

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
    destroyImage3D(*m_ctx, m_depth);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_skyPipe)
        vkDestroyPipeline(dev, m_skyPipe, nullptr);
    if (m_opaquePipe)
        vkDestroyPipeline(dev, m_opaquePipe, nullptr);
    if (m_waterPipe)
        vkDestroyPipeline(dev, m_waterPipe, nullptr);
    m_surfelBuf = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_skyPipe = m_opaquePipe = m_waterPipe = VK_NULL_HANDLE;
}

} // namespace vf
