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
    if (ok)
        s.bytes = bytes;
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
    VkDescriptorSetLayoutBinding b[11] = {};
    int n = 0;
    for (uint32_t i = 1; i < 6; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[n++] = { 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

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
                                       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
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

void SvoPass::bindWorldBuffers()
{
    VkDescriptorBufferInfo infos[6] = {
        { m_grid.buf, 0, VK_WHOLE_SIZE },    { m_childBase.buf, 0, VK_WHOLE_SIZE },
        { m_payload.buf, 0, VK_WHOLE_SIZE }, { m_handles.buf, 0, VK_WHOLE_SIZE },
        { m_bricks.buf, 0, VK_WHOLE_SIZE },  { m_chunkInfo.buf, 0, VK_WHOLE_SIZE },
    };
    const uint32_t bindings[6] = { 1, 2, 3, 4, 5, 11 };
    VkWriteDescriptorSet w[6] = {};
    for (int i = 0; i < 6; ++i)
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, bindings[i], 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 6, w, 0, nullptr);
}

bool SvoPass::uploadRange(Ssbo& s, size_t offset, const void* data, size_t bytes)
{
    if (!s.buf || !data || bytes == 0)
        return false;
    if (offset + bytes > s.bytes) {
        spdlog::error("svo uploadRange out of bounds: {}+{} > {}", offset, bytes, s.bytes);
        return false;
    }
    Buffer staging = makeBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!staging.buf || !staging.mapped)
        return false;
    memcpy(staging.mapped, data, bytes);
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, offset, bytes };
        vkCmdCopyBuffer(cmd, staging.buf, s.buf, 1, &c);
    });
    destroyBuffer(*m_ctx, staging);
    return ok;
}

bool SvoPass::growSsbo(Ssbo& s, size_t minBytes)
{
    if (!s.buf)
        return false;
    size_t newBytes = std::max<size_t>(s.bytes + s.bytes / 2, size_t(4096));
    if (newBytes < minBytes)
        newBytes = minBytes;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = newBytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBuffer nb = VK_NULL_HANDLE;
    VmaAllocation na = VK_NULL_HANDLE;
    if (vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &nb, &na, nullptr) != VK_SUCCESS)
        return false;
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, 0, s.bytes };
        vkCmdCopyBuffer(cmd, s.buf, nb, 1, &c);
    });
    if (!ok) {
        vmaDestroyBuffer(m_ctx->allocator(), nb, na);
        return false;
    }
    vmaDestroyBuffer(m_ctx->allocator(), s.buf, s.alloc);
    s.buf = nb;
    s.alloc = na;
    s.bytes = newBytes;
    return true;
}

void SvoPass::setWorld(const voxel::GpuWorld& world)
{
    destroySsbo(m_grid);
    destroySsbo(m_childBase);
    destroySsbo(m_payload);
    destroySsbo(m_handles);
    destroySsbo(m_bricks);
    destroySsbo(m_chunkInfo);
    uploadSsbo(m_grid, world.chunkGrid.data(), world.chunkGrid.size() * 4);
    uploadSsbo(m_childBase, world.childBase.data(), world.childBase.size() * 4);
    uploadSsbo(m_payload, world.payload.data(), world.payload.size() * 4);
    uploadSsbo(m_handles, world.handles.data(), world.handles.size() * 4);
    uploadSsbo(m_bricks, world.bricks.data(), world.bricks.size() * 4);
    static_assert(sizeof(voxel::GpuChunkInfo) == 16, "uvec4 layout");
    uploadSsbo(m_chunkInfo, world.chunkInfo.data(), world.chunkInfo.size() * 16);
    bindWorldBuffers();

    // CPU mirror + per-chunk capacities for live patching. The freshly merged
    // layout is packed, so a chunk's slot size is the distance to the next
    // non-empty chunk's base; a pool that grows relocates into the free tail.
    m_info = world.chunkInfo;
    const size_t n = m_info.size();
    m_capPayload.assign(n, 0);
    m_capHandles.assign(n, 0);
    m_capBricks.assign(n, 0);
    const uint32_t payloadEnd = uint32_t(world.payload.size());
    const uint32_t handlesEnd = uint32_t(world.handles.size());
    const uint32_t bricksEnd = uint32_t(world.bricks.size() / voxel::BRICK_WORDS);
    for (size_t i = 0; i < n; ++i) {
        if (i >= world.chunkGrid.size() || world.chunkGrid[i] < 0)
            continue;
        auto nextOf = [&](auto baseFn, uint32_t end) {
            for (size_t j = i + 1; j < n; ++j)
                if (world.chunkGrid[j] >= 0)
                    return baseFn(m_info[j]);
            return end;
        };
        m_capPayload[i] = nextOf([](const voxel::GpuChunkInfo& g) { return g.nodeBase; },
                                 payloadEnd) -
                          m_info[i].nodeBase;
        m_capHandles[i] = nextOf([](const voxel::GpuChunkInfo& g) { return g.childBase; },
                                 handlesEnd) -
                          m_info[i].childBase;
        m_capBricks[i] = nextOf([](const voxel::GpuChunkInfo& g) { return g.brickBase; },
                                bricksEnd) -
                         m_info[i].brickBase;
    }
    m_highPayload = payloadEnd;
    m_highHandles = handlesEnd;
    m_highBricks = bricksEnd;
}

void SvoPass::patchChunk(uint32_t chunk, const voxel::ChunkPool& pool)
{
    if (!m_ctx || chunk >= m_info.size() || !m_grid.buf)
        return;
    vkDeviceWaitIdle(m_ctx->device());
    const uint32_t needNodes = uint32_t(pool.payload.size());
    const uint32_t needHandles = uint32_t(pool.handles.size());
    const uint32_t needBricks = uint32_t(pool.bricks.size() / voxel::BRICK_WORDS);
    const bool empty = pool.root < 0;

    if (empty) {
        m_info[chunk] = {};
        m_capPayload[chunk] = m_capHandles[chunk] = m_capBricks[chunk] = 0;
    } else if (needNodes <= m_capPayload[chunk] && needHandles <= m_capHandles[chunk] &&
               needBricks <= m_capBricks[chunk]) {
        // fits the existing slot: overwrite in place at the same bases
    } else {
        // relocate the chunk into the free tail with slack
        const uint32_t capP = needNodes + std::max<uint32_t>(64, needNodes / 8);
        const uint32_t capH = needHandles + std::max<uint32_t>(64, needHandles / 8);
        const uint32_t capB = needBricks + std::max<uint32_t>(64, needBricks / 8);
        // payload and childBase share the node index space, both need room
        if (m_highPayload + capP > m_payload.bytes / 4) {
            if (!growSsbo(m_payload, size_t(m_highPayload + capP) * 4))
                return;
        }
        if (m_highPayload + capP > m_childBase.bytes / 4) {
            if (!growSsbo(m_childBase, size_t(m_highPayload + capP) * 4))
                return;
        }
        if (m_highHandles + capH > m_handles.bytes / 4) {
            if (!growSsbo(m_handles, size_t(m_highHandles + capH) * 4))
                return;
        }
        // bricks are measured in BRICK_WORDS, the buffer in bytes
        constexpr size_t kBrickBytes = size_t(voxel::BRICK_WORDS) * 4;
        if (size_t(m_highBricks + capB) * kBrickBytes > m_bricks.bytes) {
            if (!growSsbo(m_bricks, size_t(m_highBricks + capB) * kBrickBytes))
                return;
        }
        m_info[chunk].nodeBase = m_highPayload;
        m_info[chunk].childBase = m_highHandles;
        m_info[chunk].brickBase = m_highBricks;
        m_capPayload[chunk] = capP;
        m_capHandles[chunk] = capH;
        m_capBricks[chunk] = capB;
        m_highPayload += capP;
        m_highHandles += capH;
        m_highBricks += capB;
        bindWorldBuffers(); // any grow replaced a buffer
    }

    if (!empty) {
        const voxel::GpuChunkInfo& b = m_info[chunk];
        if (needNodes) {
            uploadRange(m_childBase, size_t(b.nodeBase) * 4, pool.childBase.data(),
                        needNodes * 4);
            uploadRange(m_payload, size_t(b.nodeBase) * 4, pool.payload.data(),
                        needNodes * 4);
        }
        if (needHandles)
            uploadRange(m_handles, size_t(b.childBase) * 4, pool.handles.data(),
                        needHandles * 4);
        if (needBricks)
            uploadRange(m_bricks, size_t(b.brickBase) * voxel::BRICK_WORDS * 4,
                        pool.bricks.data(), size_t(needBricks) * voxel::BRICK_WORDS * 4);
    }
    const int32_t root = empty ? -1 : int32_t(uint32_t(pool.root));
    if (getenv("VF_TRACE"))
        spdlog::info("svo patch chunk {}: root {} nodes {} handles {} bricks {} "
                     "bases ({},{},{}) caps ({},{},{}) high ({},{},{})",
                     chunk, root, needNodes, needHandles, needBricks,
                     m_info[chunk].nodeBase, m_info[chunk].childBase,
                     m_info[chunk].brickBase, m_capPayload[chunk], m_capHandles[chunk],
                     m_capBricks[chunk], m_highPayload, m_highHandles, m_highBricks);
    uploadRange(m_grid, size_t(chunk) * 4, &root, 4);
    uploadRange(m_chunkInfo, size_t(chunk) * 16, &m_info[chunk], 16);
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
    destroySsbo(m_chunkInfo);
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
