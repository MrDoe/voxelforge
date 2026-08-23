#pragma once
#include "rhi/context.hpp"
#include <vector>

namespace vf {

enum class PresentPolicy { PreferMailbox, Immediate };

class Swapchain {
public:
    bool init(const Context& ctx, uint32_t fallbackW, uint32_t fallbackH,
              PresentPolicy policy = PresentPolicy::PreferMailbox);
    void destroy();
    bool recreate(uint32_t fallbackW, uint32_t fallbackH);

    VkSwapchainKHR handle() const { return m_swapchain; }
    const VkSwapchainKHR* handlePtr() const { return &m_swapchain; }
    VkFormat format() const { return m_format; }
    const VkFormat* formatPtr() const { return &m_format; }
    VkExtent2D extent() const { return m_extent; }
    VkImage image(uint32_t i) const { return m_images[i]; }
    const std::vector<VkImageView>& imageViews() const { return m_views; }
    size_t imageCount() const { return m_images.size(); }

private:
    const Context* m_ctx = nullptr;
    PresentPolicy m_policy = PresentPolicy::PreferMailbox;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent {};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
};

} // namespace vf
