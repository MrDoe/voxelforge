#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace vf {

// Thin wrapper owning instance/device/allocator + submission helpers.
class Context {
public:
    bool init(GLFWwindow* window, bool wantValidation);
    void shutdown();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VmaAllocator allocator() const { return m_allocator; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    VkSurfaceKHR surface() const { return m_surface; }

    const char* gpuName() const { return m_gpuName.c_str(); }
    bool hasValidation() const { return m_validationActive; }

    // One-off synchronous submit used for uploads / init work.
    bool immediateSubmit(std::function<void(VkCommandBuffer)>&& record) const;

private:
    bool createInstance(GLFWwindow* window, bool wantValidation);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createAllocator();
    bool createUploadContext();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    uint32_t m_graphicsFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;

    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

    mutable VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    mutable VkCommandBuffer m_uploadCmd = VK_NULL_HANDLE;
    mutable VkFence m_uploadFence = VK_NULL_HANDLE;

    std::string m_gpuName = "?";
    bool m_validationActive = false;
};

} // namespace vf
