#include "render/splat_pass.hpp"
#include <core/log.hpp>
#include <algorithm>
#include <cstdio>
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

    VkDescriptorSetLayoutBinding b[13] = {};
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
    b[8] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // uHizWrite (Hi-Z build)
    b[9] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // uHizRead (Hi-Z build)
    b[10] = { 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
              VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // uHizSampled (cull)
    b[11] = { 11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
              VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // OcclParams
    b[12] = { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
              VK_SHADER_STAGE_COMPUTE_BIT, nullptr }; // uDepth (Hi-Z mip0)
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 13;
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

    VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 },
                                          { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
                                          { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
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
    // occlusion parameters (occlEnabled + hizNumMips)
    m_occlBuf = makeBuffer(ctx, 2 * sizeof(int),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_occlBuf.buf || !m_occlBuf.mapped)
        return false;
    {
        int oc[2] = { 0, 0 }; // occlEnabled=0, hizNumMips=0
        memcpy(m_occlBuf.mapped, oc, sizeof(oc));
    }
    VkDescriptorBufferInfo ocInfo { m_occlBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet woc { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 11, 0, 1,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ocInfo, nullptr };
    vkUpdateDescriptorSets(dev, 1, &woc, 0, nullptr);
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
    if (!initTileResources(ctx))
        return false;
    return true;
}

// ---- tile splat path resources -------------------------------------------
// One descriptor set serves all tile compute pipelines (bindings cover the
// union; static bindings are written once here, per-reload images in
// setSurfels/updateDescriptors, per-frame slots in recordTile).
bool SplatPass::initTileResources(const Context& ctx)
{
    VkDevice dev = ctx.device();
    auto ssbo = [&](uint32_t b) {
        VkDescriptorSetLayoutBinding x {};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return x;
    };
    auto ubo = [&](uint32_t b) {
        VkDescriptorSetLayoutBinding x {};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return x;
    };
    auto simage = [&](uint32_t b) {
        VkDescriptorSetLayoutBinding x {};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return x;
    };
    auto sampler = [&](uint32_t b) {
        VkDescriptorSetLayoutBinding x {};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return x;
    };
    VkDescriptorSetLayoutBinding tb[21] = {
        ssbo(0), simage(1), simage(2), ubo(3), ssbo(4), ssbo(5), ssbo(6),
        ssbo(7), ubo(8), ssbo(9), ssbo(10), sampler(11), sampler(12),
        ssbo(13), simage(14), simage(15), ssbo(16), ssbo(17), ssbo(18),
        ssbo(19), ssbo(20)
    };
    // live bindings: 0 surfels, 1 height, 2 objvol, 3 splatUBO, 4 sel,
    // 5 counts/bases matrix, 6 tile offsets, 7 tile ends, 8 frame UBO,
    // 9 tile totals, 10 dup vals, 11/12 IBL (black parity), 14/15 hdr/gpos,
    // 20 slot-tripled dup total
    VkDescriptorSetLayoutCreateInfo tli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    tli.bindingCount = 21;
    tli.pBindings = tb;
    if (vkCreateDescriptorSetLayout(dev, &tli, nullptr, &m_tileSetLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange tpc[1] = {};
    tpc[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    tpc[0].offset = 0;
    tpc[0].size = sizeof(RaymarchPush);
    VkPipelineLayoutCreateInfo tpli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    tpli.setLayoutCount = 1;
    tpli.pSetLayouts = &m_tileSetLayout;
    tpli.pushConstantRangeCount = 1;
    tpli.pPushConstantRanges = tpc;
    if (vkCreatePipelineLayout(dev, &tpli, nullptr, &m_tileLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize tps[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 13 },
                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 },
                                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
                                    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo tpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    tpi.maxSets = 1;
    tpi.poolSizeCount = 4;
    tpi.pPoolSizes = tps;
    if (vkCreateDescriptorPool(dev, &tpi, nullptr, &m_tilePool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo tai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    tai.descriptorPool = m_tilePool;
    tai.descriptorSetCount = 1;
    tai.pSetLayouts = &m_tileSetLayout;
    if (vkAllocateDescriptorSets(dev, &tai, &m_tileSet) != VK_SUCCESS)
        return false;

    auto makeCompute = [&](const char* name, VkPipeline* out,
                           const int32_t* specIds = nullptr,
                           const int32_t* specVals = nullptr, uint32_t nSpec = 0) {
        auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/" + name);
        if (spirv.empty())
            return false;
        VkShaderModule mod = makeModule(dev, spirv);
        if (!mod)
            return false;
        VkSpecializationMapEntry entries[3] = { { 0, 0, sizeof(int32_t) },
                                                { 1, sizeof(int32_t), sizeof(int32_t) },
                                                { 2, 2 * sizeof(int32_t), sizeof(int32_t) } };
        VkSpecializationInfo spec { nSpec, entries, nSpec * sizeof(int32_t), specVals };
        VkPipelineShaderStageCreateInfo st { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = mod;
        st.pName = "main";
        if (nSpec > 0)
            st.pSpecializationInfo = &spec;
        (void)specIds;
        VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.layout = m_tileLayout;
        cpi.stage = st;
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, out);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (r != VK_SUCCESS) {
            spdlog::critical("splat: tile pipeline '{}' failed", name);
            return false;
        }
        return true;
    };
    int32_t zero = 0, one = 1;
    if (!makeCompute("splat_tile_bin.comp.spv", &m_tileBinCountPipe, nullptr, &zero, 1) ||
        !makeCompute("splat_tile_bin.comp.spv", &m_tileBinFillPipe, nullptr, &one, 1) ||
        !makeCompute("splat_tile_scan.comp.spv", &m_tileScanPipe, nullptr, &zero, 1) ||
        !makeCompute("splat_tile_base.comp.spv", &m_tileTotalsPipe, nullptr, &zero, 1) ||
        !makeCompute("splat_tile_base.comp.spv", &m_tileBasePipe, nullptr, &one, 1) ||
        !makeCompute("splat_tile_render.comp.spv", &m_tileRenderPipe))
        return false;

    auto devBuf = [&](VkDeviceSize bytes) {
        return makeBuffer(ctx, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false);
    };
    auto devFillBuf = [&](VkDeviceSize bytes) {
        return makeBuffer(ctx, bytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false);
    };
    m_tileTotals = devFillBuf(kMaxTiles * sizeof(uint32_t));
    m_tileOffsets = devBuf(kMaxTiles * sizeof(uint32_t));
    m_tileCursor = devBuf(kMaxTiles * sizeof(uint32_t));
    if (!m_tileTotals.buf || !m_tileOffsets.buf || !m_tileCursor.buf)
        return false;
    // dup stream capacity: measured hero totals x headroom (tuned via
    // VF_TILE_STATS); overflow falls back to the forward path for the rest
    // of the session (flag checked by the tile render + record)
    m_dupCap = 12 * 1024 * 1024;
    m_dupVals = devBuf(m_dupCap * sizeof(uint32_t));
    if (!m_dupVals.buf)
        return false;
    m_tileFrame = makeBuffer(ctx, 2 * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    // slot-tripled totals: each frame slot owns 5 u32 (total + dx + overflow)
    // so record() can safely read the fence-complete value from 3 frames ago
    // for stats + the session fallback
    m_tileTotal = makeBuffer(ctx, 3 * 5 * sizeof(uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_tileFrame.buf || !m_tileFrame.mapped || !m_tileTotal.buf || !m_tileTotal.mapped)
        return false;
    memset(m_tileTotal.mapped, 0, 3 * 5 * sizeof(uint32_t));

    // parity images: the forward path leaves IBL bindings 11/12 unbound
    // (reads return zero on this driver); the tile set gets explicit black
    // images so the tile math matches bit-for-bit
    if (!createBlackEnv(ctx))
        return false;

    // static bindings (per-reload images + per-frame slots are rebound later;
    // the counts matrix (binding 5) is created on demand in recreateDepth)
    auto bufInfo = [&](Buffer& b) {
        VkDescriptorBufferInfo i { b.buf, 0, VK_WHOLE_SIZE };
        return i;
    };
    VkDescriptorBufferInfo bi[] = { bufInfo(m_tileOffsets), bufInfo(m_tileCursor),
                                    bufInfo(m_tileFrame),  bufInfo(m_tileTotals),
                                    bufInfo(m_dupVals),    bufInfo(m_tileTotal),
                                    bufInfo(m_paramsBuf) };
    const uint32_t bb[] = { 6, 7, 8, 9, 10, 20, 3 };
    VkWriteDescriptorSet w[7] = {};
    for (int k = 0; k < 7; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = m_tileSet;
        w[k].dstBinding = bb[k];
        w[k].descriptorCount = 1;
        w[k].descriptorType = (bb[k] == 8 || bb[k] == 3)
                                  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                  : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[k].pBufferInfo = &bi[k];
    }
    vkUpdateDescriptorSets(dev, 7, w, 0, nullptr);
    VkDescriptorImageInfo cube { m_blackSampler, m_blackCube.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo lut { m_blackSampler, m_blackLut.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wi[2] = {};
    wi[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wi[0].dstSet = m_tileSet;
    wi[0].dstBinding = 11;
    wi[0].descriptorCount = 1;
    wi[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wi[0].pImageInfo = &cube;
    wi[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wi[1].dstSet = m_tileSet;
    wi[1].dstBinding = 12;
    wi[1].descriptorCount = 1;
    wi[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wi[1].pImageInfo = &lut;
    vkUpdateDescriptorSets(dev, 2, wi, 0, nullptr);

    m_tileReady = true;
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

    // Depth prepass (PASS_MODE=3): writes the same depth as the core
    // pass so the Hi-Z pyramid sees identical depths. No shading, no
    // colour output (colourWriteMask=0). The core pass then re-writes
    // depth, but the prepass culls invisible surfels before that.
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
    // lighten-only (MAX: soft edges that can never darken settled surface), silhouette rims
    // alpha-blended over sky. Water stays a single full-disk pass.
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
                 false, VK_COMPARE_OP_ALWAYS, 0, false, kDepthFmt, &m_waterPipe) &&
        // Depth-only prepass (PASS_MODE=3): depth test+write, no colour.
        makePipe(0, 3, true, true, VK_COMPARE_OP_LESS, false, VK_BLEND_OP_ADD,
                 false, VK_COMPARE_OP_ALWAYS, 0, false, kDepthFmt, &m_prepassPipe);
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
    if (m_tileReady) {
        VkDescriptorBufferInfo tbi { m_surfelBuf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet tw { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                  m_tileSet, 0, 0, 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &tbi,
                                  nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 1, &tw, 0, nullptr);
    }
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
    // command stream (slot 6 = the indirect buffers), frustum planes (slot 7),
    // occlusion parameters (slot 11)
    VkDescriptorBufferInfo selInfo { m_selBufs[0].buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo cmdInfo { m_drawCmds[0].buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo plInfo { m_planesBuf.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo ocInfo { m_occlBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wcc[4] = {};
    wcc[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 5, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &selInfo, nullptr };
    wcc[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 6, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &cmdInfo, nullptr };
    wcc[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 7, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &plInfo, nullptr };
    wcc[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 11, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ocInfo, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 4, wcc, 0, nullptr);
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

// Parity images for the tile set's IBL bindings (11/12): the forward
// graphics pipelines never bind them (reads return zero on this driver),
// so the tile path gets explicit black images to match bit-for-bit.
bool SplatPass::createBlackEnv(const Context& ctx)
{
    VkDevice dev = ctx.device();
    // 1x1 black 2D LUT via the shared helpers
    m_blackLut = makeImage2D(ctx, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    if (!m_blackLut.img)
        return false;
    static const uint64_t zero64[2] = { 0, 0 };
    if (!uploadToImage3D(ctx, m_blackLut, zero64, sizeof(zero64)))
        return false;
    // 1x1x6 black cubemap (raw Vulkan: layer count + cube view)
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
    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &m_blackCube.img,
                       &m_blackCube.alloc, nullptr) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_blackCube.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    if (vkCreateImageView(dev, &vi, nullptr, &m_blackCube.view) != VK_SUCCESS)
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
            vkCmdClearColorImage(cmd, m_blackCube.img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zv, 1, &range);
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
    return vkCreateSampler(dev, &sci, nullptr, &m_blackSampler) == VK_SUCCESS;
}

// ---- tile splat path ------------------------------------------------------
// Bin -> scan -> fill -> radix-sort x4 -> register-blend, all compute.
// computeDraws() already produced the far->near opaque entries
// (m_cpuDraws) plus the unsorted water entries (m_cpuWaterDraws); the tile
// submission concatenates both (water pass bit = 1, after opaque).
void SplatPass::recordTile(VkCommandBuffer cmd, const RaymarchPush& push,
                           VkExtent2D extent, uint32_t nDraws)
{
    const uint32_t W = extent.width, H = extent.height;
    const uint32_t tilesX = (W + kTilePx - 1) / kTilePx;
    const uint32_t tilesY = (H + kTilePx - 1) / kTilePx;
    const uint32_t nTiles = tilesX * tilesY;
    const uint32_t nEntries = nDraws + m_waterDraws;

    // stale slot read (3 frames old, fence-complete): dup total for stats,
    // overflow flag for the session fallback. The buffer is host-visible
    // but GPU-written, so invalidate first (no-op when coherent).
    vmaInvalidateAllocation(m_ctx->allocator(), m_tileTotal.alloc, 0, VK_WHOLE_SIZE);
    auto* totals = static_cast<uint32_t*>(m_tileTotal.mapped);
    const uint32_t staleTotal = totals ? totals[m_cmdSlot * 5 + 0] : 0;
    const uint32_t staleOverflow = totals ? totals[m_cmdSlot * 5 + 4] : 0;
    if (staleOverflow && !m_tileDisabled) {
        m_tileDisabled = true;
        spdlog::warn("splat tile: dup stream overflow (>{}M) - forward fallback",
                     m_dupCap / (1024 * 1024));
    }
    if (getenv("VF_TRACE") && int(push.b.w) % 60 == 0)
        spdlog::info("splat tile: {} entries ({} opaque + {} water), {}x{} tiles, "
                     "dups {} (cap {}M){}",
                     nEntries, nDraws, m_waterDraws, tilesX, tilesY, staleTotal,
                     m_dupCap / (1024 * 1024),
                     m_tileDisabled ? " [FALLBACK]" : "");
    ++m_tileFrames;

    // active selection slot into the tile set
    VkDescriptorBufferInfo selInfo { m_selBufs[m_cmdSlot].buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet ws { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tileSet,
                              4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              nullptr, &selInfo, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &ws, 0, nullptr);
    auto* sel = static_cast<uint32_t*>(m_selBufs[m_cmdSlot].mapped);
    if (sel) {
        for (uint32_t i = 0; i < nDraws; ++i) {
            sel[i * 4 + 0] = m_cpuDraws[i].firstInstance;
            sel[i * 4 + 1] = m_cpuDraws[i].instanceCount;
            sel[i * 4 + 2] = 0; // opaque pass bit
            sel[i * 4 + 3] = 0;
        }
        for (uint32_t k = 0; k < m_waterDraws; ++k) {
            const uint32_t i = nDraws + k;
            sel[i * 4 + 0] = m_cpuWaterDraws[k].firstInstance;
            sel[i * 4 + 1] = m_cpuWaterDraws[k].instanceCount;
            sel[i * 4 + 2] = 1; // water pass bit
            sel[i * 4 + 3] = 0;
        }
    }
    if (m_tileFrame.mapped) {
        auto* f = static_cast<glm::vec4*>(m_tileFrame.mapped);
        f[0] = glm::vec4(float(tilesX), float(tilesY), float(nEntries), float(nTiles));
        f[1] = glm::vec4(float(m_dupCap), float(m_cmdSlot), 0.0f, 0.0f);
    }

    auto cbar = [&](VkAccessFlags2 srcA, VkAccessFlags2 dstA,
                    VkPipelineStageFlags2 srcS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VkPipelineStageFlags2 dstS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = srcS;
        mb.srcAccessMask = srcA;
        mb.dstStageMask = dstS;
        mb.dstAccessMask = dstA;
        VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    // host-written selection/frame -> compute reads
    cbar(VK_ACCESS_2_HOST_WRITE_BIT,
         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT,
         VK_PIPELINE_STAGE_2_HOST_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tileLayout, 0, 1,
                            &m_tileSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_tileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    auto bindCompute = [&](VkPipeline pipe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_tileLayout, 0, 1,
                                &m_tileSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_tileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
    };

    // sky first (same fullscreen triangle as the forward path; the tile
    // pass initializes per-pixel state from the settled sky)
    {
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
        VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, extent };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = colors;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp { 0.0f, 0.0f, float(extent.width), float(extent.height),
                        0.0f, 1.0f };
        VkRect2D sc { { 0, 0 }, extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1,
                                &m_set, 0, nullptr);
        vkCmdPushConstants(cmd, m_layout,
                           VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT),
                           0, sizeof(push), &push);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    if (nEntries == 0) {
        m_cmdSlot = (m_cmdSlot + 1) % 3;
        return; // sky only
    }

    // bin counts
    // zero the live (tile x entry) slice of the counts matrix
    vkCmdFillBuffer(cmd, m_tileCounts.buf, 0,
                    VkDeviceSize(nTiles) * nEntries * 4, 0);
    cbar(VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    bindCompute(m_tileBinCountPipe);
    vkCmdDispatch(cmd, nEntries, 1, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // per-tile totals from the counts matrix
    bindCompute(m_tileTotalsPipe);
    vkCmdDispatch(cmd, (nTiles + 63) / 64, 1, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // tile offsets + ends + total (+ overflow flag)
    bindCompute(m_tileScanPipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // counts -> exclusive entry-prefix bases (overwrites the matrix in place)
    bindCompute(m_tileBasePipe);
    vkCmdDispatch(cmd, (nTiles + 63) / 64, 1, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // fill dups at base[tile][entry] + localIdx (no atomics, exact order)
    bindCompute(m_tileBinFillPipe);
    vkCmdDispatch(cmd, nEntries, 1, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // unified register blend
    bindCompute(m_tileRenderPipe);
    vkCmdDispatch(cmd, tilesX, tilesY, 1);
    cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    m_cmdSlot = (m_cmdSlot + 1) % 3;
}

bool SplatPass::createDepthResources(uint32_t w, uint32_t h)
{
    // Hi-Z pyramid (R32F, all mips)
    uint32_t nm = 1; { uint32_t pw=w,ph=h; while(pw>1||ph>1){pw=(pw+1)/2;ph=(ph+1)/2;++nm;} }
    destroyImage3D(*m_ctx, m_hiz);
    for (int i = 0; i < nm && i < kMaxHiZMips; ++i)
        m_hizViews[i] = VK_NULL_HANDLE;
    m_hizSampledView = VK_NULL_HANDLE;
    m_hizSampler = VK_NULL_HANDLE;
    m_hizNumMips = nm;
    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.extent = {w, h, 1};
    ii.mipLevels = nm;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &m_hiz.img, &m_hiz.alloc, nullptr) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_hiz.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, nm, 0, 1 };
    if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &m_hiz.view) != VK_SUCCESS)
        return false;
    m_hiz.format = VK_FORMAT_R32_SFLOAT;
    m_hiz.extent = {w, h, 1};
    // per-mip storage views
    for (uint32_t i = 0; i < nm; ++i) {
        VkImageViewCreateInfo vii { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vii.image = m_hiz.img; vii.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vii.format = VK_FORMAT_R32_SFLOAT;
        vii.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1 };
        if (vkCreateImageView(m_ctx->device(), &vii, nullptr, &m_hizViews[i]) != VK_SUCCESS)
            return false;
    }
    // sampled view covering all mips
    if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &m_hizSampledView) != VK_SUCCESS)
        return false;
    VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = float(nm);
    if (vkCreateSampler(m_ctx->device(), &sci, nullptr, &m_hizSampler) != VK_SUCCESS)
        return false;
    // depth sampler for the Hi-Z mip0 source
    if (m_depthSampler) {
        vkDestroySampler(m_ctx->device(), m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }
    VkSamplerCreateInfo dsci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    dsci.magFilter = VK_FILTER_NEAREST;
    dsci.minFilter = VK_FILTER_NEAREST;
    dsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    dsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    dsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    dsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    dsci.compareEnable = VK_FALSE;
    dsci.maxLod = 0.0f;
    if (vkCreateSampler(m_ctx->device(), &dsci, nullptr, &m_depthSampler) != VK_SUCCESS)
        return false;
    // depth image: add SAMPLED usage for the Hi-Z mip0 source
    destroyImage3D(*m_ctx, m_depth);
    m_depth = makeImage2D(*m_ctx, w, h, VK_FORMAT_D24_UNORM_S8_UINT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT,
                          VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                             VK_IMAGE_ASPECT_STENCIL_BIT));
    if (m_depth.img == VK_NULL_HANDLE)
        return false;
    // (tile x entry) counts matrix for the tile path
    const uint32_t nTiles = ((w + kTilePx - 1) / kTilePx) *
                            ((h + kTilePx - 1) / kTilePx);
    const VkDeviceSize bytes = VkDeviceSize(nTiles) * kMaxChunkDraws * 4;
    if (bytes != m_tileCountsBytes) {
        if (m_tileCounts.buf)
            destroyBuffer(*m_ctx, m_tileCounts);
        m_tileCounts = makeBuffer(*m_ctx, bytes,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false);
        m_tileCountsBytes = m_tileCounts.buf ? bytes : 0;
        if (!m_tileCounts.buf)
            return false;
        VkDescriptorBufferInfo ci { m_tileCounts.buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                 m_tileSet, 5, 0, 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ci,
                                 nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
    return true;
}

bool SplatPass::recreateDepth(uint32_t w, uint32_t h)
{
    destroyImage3D(*m_ctx, m_depth);
    // D24+S8: depth for plane ordering, stencil to tell interior rims
    // (lighten-only blend) apart from silhouette rims (alpha blend)
    // NOTE: also VK_IMAGE_USAGE_SAMPLED_BIT for the Hi-Z mip0 source
    m_depth = makeImage2D(*m_ctx, w, h, VK_FORMAT_D24_UNORM_S8_UINT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT,
                          VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                             VK_IMAGE_ASPECT_STENCIL_BIT));
    if (m_depth.img == VK_NULL_HANDLE)
        return false;
    return createDepthResources(w, h);
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
    if (m_tileReady) {
        // tile set mirrors the field images + the HDR/G-buffer targets
        // (bindings 1/2/14/15); surfel buffer follows in setSurfels
        VkDescriptorImageInfo thi { VK_NULL_HANDLE, heightView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo toi { VK_NULL_HANDLE, objVolView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo hhi { VK_NULL_HANDLE, hdrView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo ggi { VK_NULL_HANDLE, gposView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet tw[4] = {};
        tw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[0].dstSet = m_tileSet;
        tw[0].dstBinding = 1;
        tw[0].descriptorCount = 1;
        tw[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tw[0].pImageInfo = &thi;
        tw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[1].dstSet = m_tileSet;
        tw[1].dstBinding = 2;
        tw[1].descriptorCount = 1;
        tw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tw[1].pImageInfo = &toi;
        tw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[2].dstSet = m_tileSet;
        tw[2].dstBinding = 14;
        tw[2].descriptorCount = 1;
        tw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tw[2].pImageInfo = &hhi;
        tw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[3].dstSet = m_tileSet;
        tw[3].dstBinding = 15;
        tw[3].descriptorCount = 1;
        tw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tw[3].pImageInfo = &ggi;
        vkUpdateDescriptorSets(m_ctx->device(), 4, tw, 0, nullptr);
    }
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
    const bool direct = getenv("VF_SPLAT_DIRECT") != nullptr;
    const bool gpuCull =
        !direct && !getenv("VF_NO_GPU_CULL") && nDraws > 0 && m_cullPipe &&
        m_compactBuf.buf && m_selBufs[0].buf && m_planesBuf.buf;
    // Tile path (VF_TILE=1): bin + sort + register-blend instead of the
    // three forward raster passes. Debug views (VF_SPLAT_DEBUG) stay on the
    // forward path; oversized extents fall back (tile table is fixed-size).
    bool tile = m_tileReady && !m_tileDisabled && !direct;
    if (const char* e = getenv("VF_TILE"))
        tile = tile && atoi(e) != 0;
    else
        tile = false; // default off until per-scene parity is proven
    if (getenv("VF_SPLAT_DEBUG"))
        tile = false;
    {
        const uint32_t tx = (extent.width + kTilePx - 1) / kTilePx;
        const uint32_t ty = (extent.height + kTilePx - 1) / kTilePx;
        if (uint64_t(tx) * ty > kMaxTiles)
            tile = false;
    }
    if (tile) {
        if (getenv("VF_TRACE") && int(push.b.w) % 60 == 0)
            spdlog::info("splat: tile path ({} opaque + {} water entries)",
                         nDraws, m_waterDraws);
        recordTile(cmd, push, extent, nDraws);
        return;
    }
    constexpr VkDeviceSize stride = sizeof(VkDrawIndirectCommand);
    // ---- occlusion prepass + Hi-Z build ----
    const bool doOccl = nDraws > 0 && m_prepassPipe && m_hiz.img != VK_NULL_HANDLE
                        && m_occlBuf.buf && m_depth.img != VK_NULL_HANDLE;
    if (doOccl) {
        // depth is DEPTH_ATTACHMENT_OPTIMAL (set by main.cpp)
        // transition Hi-Z to GENERAL for storage writes
        vf::transitionImage(cmd, m_hiz.img, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        // === PREPASS: depth-only, replicates core pass depth exactly ===
        {
            VkRenderingAttachmentInfo colrs[2] = {};
            colrs[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colrs[0].imageView = m_hdrView;
            colrs[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            colrs[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colrs[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colrs[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colrs[1].imageView = m_gposView;
            colrs[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            colrs[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colrs[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo dp { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            dp.imageView = m_depth.view;
            dp.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dp.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            dp.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            dp.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, extent };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 2;
            ri.pColorAttachments = colrs;
            ri.pDepthAttachment = &dp;
            ri.pStencilAttachment = &dp;
            vkCmdBeginRendering(cmd, &ri);
            VkViewport vp { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
            VkRect2D sc { { 0, 0 }, extent };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1,
                                    &m_set, 0, nullptr);
            vkCmdPushConstants(cmd, m_layout,
                               VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                                  VK_SHADER_STAGE_FRAGMENT_BIT),
                               0, sizeof(push), &push);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prepassPipe);
            if (direct) {
                for (uint32_t i = 0; i < nDraws; ++i) {
                    const auto& d = m_cpuDraws[i];
                    vkCmdDraw(cmd, d.vertexCount, d.instanceCount, d.firstVertex, d.firstInstance);
                }
            } else {
                vkCmdDrawIndirect(cmd, m_drawCmds[m_cmdSlot].buf, 0, nDraws, stride);
            }
            vkCmdEndRendering(cmd);
        }
        // === Hi-Z build ===
        // depth: DEPTH_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        vf::transitionImage(cmd, m_depth.img,
                             VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT),
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT);
        // barrier: prepass depth write -> Hi-Z read
        VkMemoryBarrier2 mb0 { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb0.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        mb0.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb0.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb0.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        { VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
          di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb0;
          vkCmdPipelineBarrier2(cmd, &di); }
        auto cbar = [&](VkAccessFlags2 srcA, VkAccessFlags2 dstA,
                         VkPipelineStageFlags2 srcS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VkPipelineStageFlags2 dstS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = srcS; mb.srcAccessMask = srcA;
            mb.dstStageMask = dstS; mb.dstAccessMask = dstA;
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        };
        // bind uDepth (binding 12) once: depth sampler
        VkDescriptorImageInfo depthDi { VkSampler(), m_depth.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wDepth { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 12, 0, 1,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthDi, nullptr };
        for (uint32_t mi = 0; mi < uint32_t(m_hizNumMips); ++mi) {
            VkDescriptorImageInfo prevDi, curDi;
            if (mi == 0) {
                // uDepth is bound at binding 12; binding 9 (uHizRead) unused for mip0
            } else {
                prevDi = { VkSampler(), m_hizViews[mi - 1], VK_IMAGE_LAYOUT_GENERAL };
            }
            curDi = { VkSampler(), m_hizViews[mi], VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w8 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &curDi, nullptr };
            VkWriteDescriptorSet w9 {};
            if (mi > 0) {
                w9 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &prevDi, nullptr };
            }
            if (mi == 0) {
                vkUpdateDescriptorSets(m_ctx->device(), 1, &wDepth, 0, nullptr);
                vkUpdateDescriptorSets(m_ctx->device(), 1, &w8, 0, nullptr);
            } else {
                vkUpdateDescriptorSets(m_ctx->device(), 1, &w8, 0, nullptr);
                VkWriteDescriptorSet w9c { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
                                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &prevDi, nullptr };
                vkUpdateDescriptorSets(m_ctx->device(), 1, &w9c, 0, nullptr);
            }
            vkCmdDispatch(cmd, (uint32_t(m_hiz.extent.width) + 7) / 8,
                          (uint32_t(m_hiz.extent.height) + 7) / 8, 1);
            if (mi + 1 < uint32_t(m_hizNumMips))
                cbar(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
        // depth: SHADER_READ_ONLY_OPTIMAL -> DEPTH_ATTACHMENT_OPTIMAL
        vf::transitionImage(cmd, m_depth.img,
                             VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT),
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        // Hi-Z GENERAL -> SHADER_READ_ONLY for the cull
        vf::transitionImage(cmd, m_hiz.img, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_READ_BIT);
    }

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
    // stride defined above
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
    if (m_cullPipe)
        vkDestroyPipeline(dev, m_cullPipe, nullptr);
    if (m_compactBuf.buf)
        destroyBuffer(*m_ctx, m_compactBuf);
    for (auto& c : m_selBufs)
        if (c.buf)
            destroyBuffer(*m_ctx, c);
    if (m_planesBuf.buf)
        destroyBuffer(*m_ctx, m_planesBuf);
    // tile path resources
    if (m_tileBinCountPipe)
        vkDestroyPipeline(dev, m_tileBinCountPipe, nullptr);
    if (m_tileBinFillPipe)
        vkDestroyPipeline(dev, m_tileBinFillPipe, nullptr);
    if (m_tileScanPipe)
        vkDestroyPipeline(dev, m_tileScanPipe, nullptr);
    if (m_tileTotalsPipe)
        vkDestroyPipeline(dev, m_tileTotalsPipe, nullptr);
    if (m_tileBasePipe)
        vkDestroyPipeline(dev, m_tileBasePipe, nullptr);
    if (m_tileRenderPipe)
        vkDestroyPipeline(dev, m_tileRenderPipe, nullptr);
    for (Buffer* b : { &m_tileCounts, &m_tileTotals, &m_tileOffsets, &m_tileCursor,
                       &m_tileFrame, &m_dupVals, &m_tileTotal }) {
        if (b->buf)
            destroyBuffer(*m_ctx, *b);
    }
    destroyImage3D(*m_ctx, m_blackCube);
    destroyImage3D(*m_ctx, m_blackLut);
    if (m_blackSampler)
        vkDestroySampler(dev, m_blackSampler, nullptr);
    if (m_tilePool)
        vkDestroyDescriptorPool(dev, m_tilePool, nullptr);
    if (m_tileSetLayout)
        vkDestroyDescriptorSetLayout(dev, m_tileSetLayout, nullptr);
    if (m_tileLayout)
        vkDestroyPipelineLayout(dev, m_tileLayout, nullptr);
    m_surfelBuf = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_skyPipe = m_corePipe = m_rimInPipe = m_rimOutPipe = m_waterPipe = VK_NULL_HANDLE;
}

} // namespace vf
