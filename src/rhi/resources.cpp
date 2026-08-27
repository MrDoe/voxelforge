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

    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = format;
    ii.extent = im.extent;
    ii.mipLevels = 1;
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

Image3D makeImage2D(const Context& ctx, uint32_t w, uint32_t h,
                     VkFormat format, VkImageUsageFlags usage)
{
    Image3D im;
    im.extent = { w, h, 1 };
    im.format = format;

    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = im.extent;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &im.img, &im.alloc, nullptr) != VK_SUCCESS) {
        spdlog::critical("vmaCreateImage 2D {}x{} failed", w, h);
        return im;
    }

    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(ctx.device(), &vi, nullptr, &im.view) != VK_SUCCESS) {
        spdlog::critical("vkCreateImageView(2D) failed");
        return im;
    }
    return im;
}

void transitionImage(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
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
