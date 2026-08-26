#pragma once
#include "rhi/context.hpp"
#include <vector>

namespace vf {

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Image3D {
    VkImage img = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent3D extent {};
    VkFormat format = VK_FORMAT_UNDEFINED;
};

Buffer makeBuffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VmaMemoryUsage memUsage, bool persistentlyMapped);
void destroyBuffer(const Context& ctx, Buffer& b);

Image3D makeImage3D(const Context& ctx, uint32_t w, uint32_t h, uint32_t d,
                    VkFormat format, VkImageUsageFlags usage);
void destroyImage3D(const Context& ctx, Image3D& im);

// Staging-buffer upload; leaves image in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
bool uploadToImage3D(const Context& ctx, Image3D& im, const void* data, size_t bytes);

// Copies a 2D region of `src` (currently in GENERAL layout) into `out` (row-major, tightly packed).
bool readbackImage2D(const Context& ctx, VkImage src, uint32_t w, uint32_t h,
                     std::vector<uint8_t>& out);

// Sync flags are the 64-bit Vk*Flags2 types: the VK_*_2 stage/access bits
// live above bit 31 and silently truncate to 0 in the legacy 32-bit types.
void transitionImage(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                     uint32_t layerCount = 1);

} // namespace vf
