#include "rhi/swapchain.hpp"
#include <core/log.hpp>
#include <algorithm>

namespace vf {

bool Swapchain::init(const Context& ctx, uint32_t fallbackW, uint32_t fallbackH,
                     PresentPolicy policy)
{
    m_ctx = &ctx;
    m_policy = policy;
    return recreate(fallbackW, fallbackH);
}

void Swapchain::destroy()
{
    for (VkImageView v : m_views)
        vkDestroyImageView(m_ctx->device(), v, nullptr);
    if (m_swapchain)
        vkDestroySwapchainKHR(m_ctx->device(), m_swapchain, nullptr);
    m_views.clear();
    m_images.clear();
    m_swapchain = VK_NULL_HANDLE;
}

bool Swapchain::recreate(uint32_t fallbackW, uint32_t fallbackH)
{
    if (!m_ctx)
        return false;
    destroy();

    VkSurfaceCapabilitiesKHR caps {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx->physicalDevice(), m_ctx->surface(), &caps);

    // --- format ----------------------------------------------------
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx->physicalDevice(), m_ctx->surface(), &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx->physicalDevice(), m_ctx->surface(), &fmtCount, formats.data());
    if (formats.empty()) {
        spdlog::critical("Surface has no supported formats");
        return false;
    }
    m_format = formats[0].format;
    for (auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            m_format = f.format;
            break;
        }
    }

    // --- present mode ----------------------------------------------
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx->physicalDevice(), m_ctx->surface(), &pmCount, nullptr);
    std::vector<VkPresentModeKHR> modes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx->physicalDevice(), m_ctx->surface(), &pmCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // guaranteed
    // NOTE: MAILBOX deadlocks after a few thousand frames on NVIDIA 580 + X11
    // (xcb present wakeup loss). IMMEDIATE is reliable -> preferred by default.
    bool mailbox = false, immediate = false;
    for (auto m : modes) {
        mailbox |= m == VK_PRESENT_MODE_MAILBOX_KHR;
        immediate |= m == VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    const bool wantImmediate = m_policy == PresentPolicy::Immediate;
    if (wantImmediate)
        presentMode = immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                : (mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : presentMode);
    else
        presentMode = mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : presentMode;

    // --- extent -----------------------------------------------------
    m_extent = caps.currentExtent;
    if (m_extent.width == UINT32_MAX || m_extent.height == UINT32_MAX || m_extent.width == 0 ||
        m_extent.height == 0) {
        m_extent.width = fallbackW;
        m_extent.height = fallbackH;
    }
    m_extent.width = std::clamp(m_extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    m_extent.height = std::clamp(m_extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount > 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_ctx->surface();
    ci.minImageCount = imageCount;
    ci.imageFormat = m_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(m_ctx->device(), &ci, nullptr, &m_swapchain) != VK_SUCCESS) {
        spdlog::critical("vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_ctx->device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(m_ctx->device(), m_swapchain, &count, m_images.data());

    m_views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &m_views[i]) != VK_SUCCESS) {
            spdlog::critical("swapchain image view {} failed", i);
            return false;
        }
    }
    spdlog::info("Swapchain: {}x{}, {} images, fmt {}", m_extent.width, m_extent.height,
                 count, static_cast<int>(m_format));
    return true;
}

} // namespace vf
