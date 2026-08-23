#include "platform/window.hpp"
#include <core/log.hpp>

namespace vf {

bool Window::init(int width, int height, const std::string& title)
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
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        spdlog::critical("Window creation failed");
        return false;
    }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, sFramebufferResizeCb);
    glfwSetScrollCallback(m_window, [](GLFWwindow* w, double /*x*/, double y) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        self->m_scrollAccum += y;
    });
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

double Window::scrollDelta() const
{
    double d = m_scrollAccum;
    m_scrollAccum = 0.0;
    return d;
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
