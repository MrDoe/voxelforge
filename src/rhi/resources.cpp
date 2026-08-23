#include "rhi/resources.hpp"
#include <core/log.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace vf {

Buffer makeBuffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VmaMemoryUsage memUsage, bool persistentlyMapped)
{
    Buffer b;
    b.size = size;

    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai {};
    ai.usage = memUsage;
    if (persistentlyMapped)
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo out {};
    if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buf, &b.alloc, &out) != VK_SUCCESS) {
        spdlog::critical("vmaCreateBuffer({} bytes) failed", size);
        return b;
    }
    b.mapped = persistentlyMapped ? out.pMappedData : nullptr;
    return b;
}

void destroyBuffer(const Context& ctx, Buffer& b)
{
    if (b.buf)
        vmaDestroyBuffer(ctx.allocator(), b.buf, b.alloc);
    b = {};
}

Image3D makeImage3D(const Context& ctx, uint32_t w, uint32_t h, uint32_t d,
                    VkFormat format, VkImageUsageFlags usage)
{
    Image3D im;
    im.extent = { w, h, d };
    im.format = format;

    uint32_t mips = 1;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        uint32_t md = std::min({w, h, d});
        while ((md >>= 1) > 0) ++mips;
        mips = std::min(mips, 9u);
    }
    im.mipLevels = mips;

    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = format;
    ii.extent = im.extent;
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &im.img, &im.alloc, nullptr) != VK_SUCCESS) {
        spdlog::critical("vmaCreateImage 3D {}x{}x{} failed", w, h, d);
        return im;
    }

    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(ctx.device(), &vi, nullptr, &im.view) != VK_SUCCESS) {
        spdlog::critical("vkCreateImageView(3D) failed");
        return im;
    }
    return im;
}

void destroyImage3D(const Context& ctx, Image3D& im)
{
    if (!ctx.device())
        return;
    if (im.view)
        vkDestroyImageView(ctx.device(), im.view, nullptr);
    if (im.img)
        vmaDestroyImage(ctx.allocator(), im.img, im.alloc);
    im = {};
}

VkSampler makeSampler(const Context& ctx, VkFilter filter, VkSamplerAddressMode mode)
{
    VkSamplerCreateInfo si { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = filter;
    si.minFilter = filter;
    si.mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = mode;
    si.addressModeV = mode;
    si.addressModeW = mode;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = 4.0f;
    si.maxLod = 12.0f;

    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(ctx.device(), &si, nullptr, &s);
    return s;
}

void destroySampler(const Context& ctx, VkSampler s)
{
    if (s && ctx.device())
        vkDestroySampler(ctx.device(), s, nullptr);
}

void transitionImage(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess,
                     uint32_t layerCount)
{
    VkImageMemoryBarrier2 b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, layerCount };

    VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool uploadToImage3D(const Context& ctx, Image3D& im, const void* data, size_t bytes)
{
    if (im.mipLevels == 1) {
        Buffer staging = makeBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!staging.buf) return false;
        memcpy(staging.mapped, data, bytes);
        bool ok = ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            transitionImage(cmd, im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = im.extent;
            vkCmdCopyBufferToImage(cmd, staging.buf, im.img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transitionImage(cmd, im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        destroyBuffer(ctx, staging);
        return ok;
    }

    // mipped upload: generate CPU mip chain then copy each level
    uint32_t w = im.extent.width, h = im.extent.height, d = im.extent.depth;
    size_t bpp = bytes / (size_t(w) * h * d);
    std::vector<std::vector<uint8_t>> mips;
    mips.reserve(im.mipLevels);
    mips.emplace_back(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + bytes);
    for (uint32_t l = 1; l < im.mipLevels; ++l) {
        uint32_t pw = std::max(1u, w >> (l - 1)), ph = std::max(1u, h >> (l - 1)),
                 pd = std::max(1u, d >> (l - 1));
        uint32_t cw = std::max(1u, w >> l), ch = std::max(1u, h >> l),
                 cd = std::max(1u, d >> l);
        const auto& prev = mips[l - 1];
        std::vector<uint8_t> cur(size_t(cw) * ch * cd * bpp, 0);
        for (uint32_t z = 0; z < cd; ++z)
            for (uint32_t y = 0; y < ch; ++y)
                for (uint32_t x = 0; x < cw; ++x) {
                    if (bpp == 1) {
                        int8_t best = 127;
                        int bestAbs = 128;
                        for (uint32_t dz = 0; dz < 2; ++dz)
                            for (uint32_t dy = 0; dy < 2; ++dy)
                                for (uint32_t dx = 0; dx < 2; ++dx) {
                                    uint32_t sx = std::min(2 * x + dx, pw - 1);
                                    uint32_t sy = std::min(2 * y + dy, ph - 1);
                                    uint32_t sz = std::min(2 * z + dz, pd - 1);
                                    int8_t v = reinterpret_cast<const int8_t*>(prev.data())[
                                        (size_t(sz) * ph + sy) * pw + sx];
                                    int a = std::abs(int(v));
                                    if (a < bestAbs) {
                                        bestAbs = a;
                                        best = v;
                                    }
                                }
                        cur[(size_t(z) * ch + y) * cw + x] = reinterpret_cast<uint8_t*>(&best)[0];
                    } else {
                        int sum[4] = {0, 0, 0, 0};
                        int cnt = 0;
                        for (uint32_t dz = 0; dz < 2; ++dz)
                            for (uint32_t dy = 0; dy < 2; ++dy)
                                for (uint32_t dx = 0; dx < 2; ++dx) {
                                    uint32_t sx = std::min(2 * x + dx, pw - 1);
                                    uint32_t sy = std::min(2 * y + dy, ph - 1);
                                    uint32_t sz = std::min(2 * z + dz, pd - 1);
                                    size_t idx = ((size_t(sz) * ph + sy) * pw + sx) * bpp;
                                    for (size_t c = 0; c < bpp; ++c) sum[c] += prev[idx + c];
                                    ++cnt;
                                }
                        size_t o = ((size_t(z) * ch + y) * cw + x) * bpp;
                        for (size_t c = 0; c < bpp; ++c) cur[o + c] = uint8_t(sum[c] / cnt);
                    }
                }
        mips.emplace_back(std::move(cur));
    }

    std::vector<Buffer> stagings;
    stagings.reserve(im.mipLevels);
    for (auto& m : mips) {
        Buffer st = makeBuffer(ctx, m.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!st.buf) {
            for (auto& s : stagings) destroyBuffer(ctx, s);
            return false;
        }
        memcpy(st.mapped, m.data(), m.size());
        stagings.push_back(st);
    }

    bool ok = ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = im.img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im.mipLevels, 0, 1 };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        for (uint32_t l = 0; l < im.mipLevels; ++l) {
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
            region.imageExtent = { std::max(1u, w >> l), std::max(1u, h >> l),
                                   std::max(1u, d >> l) };
            vkCmdCopyBufferToImage(cmd, stagings[l].buf, im.img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        transitionImage(cmd, im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    for (auto& s : stagings) destroyBuffer(ctx, s);
    return ok;
}

bool readbackImage2D(const Context& ctx, VkImage src, uint32_t w, uint32_t h,
                     std::vector<uint8_t>& out)
{
    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    out.resize(static_cast<size_t>(bytes));

    Buffer dst = makeBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!dst.buf)
        return false;

    bool ok = ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImage(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst.buf, 1, &region);

        transitionImage(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    });

    if (ok)
        memcpy(out.data(), dst.mapped, static_cast<size_t>(bytes));
    destroyBuffer(ctx, dst);
    return ok;
}

} // namespace vf
