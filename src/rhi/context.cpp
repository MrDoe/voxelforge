#include "rhi/context.hpp"
#include "platform/window.hpp"
#include <core/log.hpp>
#include <algorithm>
#include <cstring>
#include <set>

namespace vf {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        spdlog::error("[vulkan] {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        spdlog::warn("[vulkan] {}", data->pMessage);
    else
        spdlog::debug("[vulkan] {}", data->pMessage);
    return VK_FALSE;
}

static bool hasLayer(const std::vector<VkLayerProperties>& layers, const char* name)
{
    for (auto& l : layers)
        if (!strcmp(l.layerName, name))
            return true;
    return false;
}

bool Context::init(GLFWwindow* window, bool wantValidation)
{
    if (!createInstance(window, wantValidation))
        return false;
    if (!pickPhysicalDevice())
        return false;
    if (!createDevice())
        return false;
    if (!createAllocator())
        return false;
    if (!createUploadContext())
        return false;
    spdlog::info("Vulkan context ready on '{}'", m_gpuName);
    return true;
}

bool Context::createInstance(GLFWwindow* window, bool wantValidation)
{
    if (!glfwVulkanSupported()) {
        spdlog::critical("glfwVulkanSupported() == false - no loader/ICDs?");
        return false;
    }

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    std::vector<const char*> layers;
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    m_validationActive = wantValidation && hasLayer(availableLayers, validationLayer);
    if (m_validationActive) {
        layers.push_back(validationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else if (wantValidation) {
        spdlog::warn("Validation requested but layer '{}' not present", validationLayer);
    }

    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Voxelforge";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "voxelforge";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS) {
        spdlog::critical("vkCreateInstance failed");
        return false;
    }

    if (m_validationActive) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT dbg { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = debugCallback;
        create(m_instance, &dbg, nullptr, &m_debugMessenger);
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &surface) != VK_SUCCESS) {
        spdlog::critical("glfwCreateWindowSurface failed");
        return false;
    }
    m_surface = surface;
    return true;
}

bool Context::pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        spdlog::critical("No Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, queues.data());

        int graphicsComputeFamily = -1;
        for (uint32_t i = 0; i < qCount; ++i) {
            VkFlags f = queues[i].queueFlags;
            if ((f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT)) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, m_surface, &present);
                if (present) {
                    graphicsComputeFamily = static_cast<int>(i);
                    break;
                }
            }
        }
        if (graphicsComputeFamily < 0)
            continue;

        int score = 10;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 100;

        if (score > bestScore) {
            bestScore = score;
            m_physical = pd;
            m_graphicsFamily = static_cast<uint32_t>(graphicsComputeFamily);
            m_gpuName = props.deviceName;
        }
    }
    if (m_physical == VK_NULL_HANDLE) {
        spdlog::critical("No suitable GPU with graphics+compute+present");
        return false;
    }
    return true;
}

bool Context::createDevice()
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = m_graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f12.pNext = &f13;

    VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.samplerAnisotropy = VK_TRUE;

    const char* exts[] = { "VK_KHR_swapchain" };

    VkDeviceCreateInfo ci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;

    VkResult devRes = vkCreateDevice(m_physical, &ci, nullptr, &m_device);
    if (devRes != VK_SUCCESS) {
        spdlog::critical("vkCreateDevice failed on '{}' with VkResult {}",
                         m_gpuName, int(devRes));
        return false;
    }
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    return true;
}

bool Context::createAllocator()
{
    VmaAllocatorCreateInfo ci {};
    ci.instance = m_instance;
    ci.physicalDevice = m_physical;
    ci.device = m_device;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    return vmaCreateAllocator(&ci, &m_allocator) == VK_SUCCESS;
}

bool Context::createUploadContext()
{
    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_graphicsFamily;
    if (vkCreateCommandPool(m_device, &pci, nullptr, &m_uploadPool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = m_uploadPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &ai, &m_uploadCmd) != VK_SUCCESS)
        return false;

    VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return vkCreateFence(m_device, &fi, nullptr, &m_uploadFence) == VK_SUCCESS;
}

bool Context::immediateSubmit(std::function<void(VkCommandBuffer)>&& record) const
{
    vkResetFences(m_device, 1, &m_uploadFence);
    vkResetCommandBuffer(m_uploadCmd, 0);

    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_uploadCmd, &bi);
    record(m_uploadCmd);
    vkEndCommandBuffer(m_uploadCmd);

    VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_uploadCmd;
    vkQueueSubmit(m_graphicsQueue, 1, &si, m_uploadFence);
    return vkWaitForFences(m_device, 1, &m_uploadFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

void Context::shutdown()
{
    if (!m_device)
        return;
    vkDeviceWaitIdle(m_device);
    if (m_uploadFence)
        vkDestroyFence(m_device, m_uploadFence, nullptr);
    if (m_uploadPool)
        vkDestroyCommandPool(m_device, m_uploadPool, nullptr);
    if (m_allocator)
        vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    if (m_debugMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(m_instance, m_debugMessenger, nullptr);
    }
    if (m_surface)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
}

} // namespace vf
