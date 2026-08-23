#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <string>

namespace vf {

class Window {
public:
    bool init(int width, int height, const std::string& title);
    void shutdown();
    void pollEvents();
    void waitEvents();

    bool shouldClose() const;
    void frameBufferSize(int& w, int& h) const;
    VkSurfaceKHR createSurface(VkInstance instance) const;

    // input helpers -------------------------------------------------
    bool keyPressed(int key) const { return glfwGetKey(m_window, key) == GLFW_PRESS; }
    bool mouseDown(int button) const { return glfwGetMouseButton(m_window, button) == GLFW_PRESS; }
    void getMouseDelta(double& dx, double& dy) const;
    double scrollDelta() const;
    glm::ivec2 framebufferSize() const;

    bool resized() const { return m_resized; }
    void clearResized() { m_resized = false; }

    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return m_mouseCaptured; }

    GLFWwindow* handle() const { return m_window; }

private:
    static void sFramebufferResizeCb(GLFWwindow* w, int width, int height);

    GLFWwindow* m_window = nullptr;
    bool m_resized = false;
    bool m_mouseCaptured = false;
    mutable double m_lastMouseX = 0.0, m_lastMouseY = 0.0;
    mutable double m_scrollAccum = 0.0;
};

} // namespace vf
