#include "platform/window.hpp"
#include <core/log.hpp>

namespace vf {

bool Window::init(int width, int height, const std::string& title, bool visible)
{
    glfwSetErrorCallback([](int code, const char* desc) {
        spdlog::error("GLFW error {}: {}", code, desc ? desc : "?");
    });
    if (!glfwInit()) {
        spdlog::critical("glfwInit failed");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // Automated capture runs still need an X11 window for the Vulkan surface,
    // but it must never be mapped: a visible window steals focus and the
    // pointer from whatever the user is doing while a test renders.
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        spdlog::critical("Window creation failed");
        return false;
    }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, sFramebufferResizeCb);
    // No scroll callback: the wheel belongs to the UI. ImGui installs its own
    // scroll callback and CHAINS this one, so an app-level handler would still
    // fire - the wheel used to scale the camera's movement speed from here,
    // which is why a scroll over the sidebar changed how fast the camera flew
    // and nothing on screen said so. The speed is an explicit setting now (the
    // Render panel's VIEW group); the wheel scrolls the sidebar.
    return true;
}

void Window::shutdown()
{
    if (m_window)
        glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Window::pollEvents() { glfwPollEvents(); }
void Window::waitEvents() { glfwWaitEvents(); }
bool Window::shouldClose() const { return glfwWindowShouldClose(m_window); }

void Window::sFramebufferResizeCb(GLFWwindow*, int, int) {}

glm::ivec2 Window::framebufferSize() const
{
    int w = 1, h = 1;
    glfwGetFramebufferSize(m_window, &w, &h);
    return { w, h };
}

void Window::frameBufferSize(int& w, int& h) const
{
    glm::ivec2 s = framebufferSize();
    w = s.x;
    h = s.y;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = glfwCreateWindowSurface(instance, m_window, nullptr, &surface);
    return r == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

void Window::getMouseDelta(double& dx, double& dy) const
{
    // Always report raw deltas relative to the previous query; callers decide
    // whether to consume them (e.g. only while a mouse button is held).
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(m_window, &x, &y);
    dx = x - m_lastMouseX;
    dy = y - m_lastMouseY;
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void Window::setMouseCaptured(bool captured)
{
    m_mouseCaptured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured)
        glfwGetCursorPos(m_window, &m_lastMouseX, &m_lastMouseY);
}

} // namespace vf
