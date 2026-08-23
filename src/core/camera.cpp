#include "core/camera.hpp"
#include "platform/window.hpp"
#include <algorithm>
#include <cmath>

namespace vf {

glm::vec3 Camera::forward() const
{
    float cp = cosf(pitch), sp = sinf(pitch);
    return glm::normalize(glm::vec3(cp * cosf(yaw), sp, cp * sinf(yaw)));
}

glm::vec3 Camera::right() const
{
    return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0)));
}

glm::vec3 Camera::up() const
{
    return glm::cross(right(), forward());
}

void Camera::applyLook(float& yaw, float& pitch, float dxPx, float dyPx)
{
    constexpr float kSensitivity = 0.0022f;
    yaw -= dxPx * kSensitivity;
    pitch = std::clamp(pitch - dyPx * kSensitivity, -1.5533f, 1.5533f);
}

glm::vec3 Camera::computeMove(bool fwdKey, bool backKey, bool leftKey, bool rightKey,
                              bool upKey, bool downKey, const glm::vec3& f,
                              const glm::vec3& r)
{
    glm::vec3 move {};
    if (fwdKey)
        move += f;
    if (backKey)
        move -= f;
    if (rightKey)
        move += r;
    if (leftKey)
        move -= r;
    if (upKey)
        move += glm::vec3(0, 1, 0);
    if (downKey)
        move -= glm::vec3(0, 1, 0);
    if (glm::dot(move, move) > 0.0f)
        return glm::normalize(move);
    return move;
}

void Camera::update(const Window& win, float dt)
{
    constexpr float kBoost = 3.5f;
    constexpr float kSlow = 0.25f;

    // Sample cursor every frame so the internal "last position" never goes
    // stale; otherwise the first frame after pressing RMB applies the whole
    // idle drift as one huge look delta.
    double dx = 0.0, dy = 0.0;
    win.getMouseDelta(dx, dy);
    if (win.mouseDown(GLFW_MOUSE_BUTTON_RIGHT))
        applyLook(yaw, pitch, float(dx), float(dy));

    float s = speed;
    if (win.keyPressed(GLFW_KEY_LEFT_SHIFT))
        s *= kBoost;
    if (win.keyPressed(GLFW_KEY_LEFT_CONTROL))
        s *= kSlow;

    glm::vec3 move = computeMove(win.keyPressed(GLFW_KEY_W), win.keyPressed(GLFW_KEY_S),
                                 win.keyPressed(GLFW_KEY_A), win.keyPressed(GLFW_KEY_D),
                                 win.keyPressed(GLFW_KEY_E), win.keyPressed(GLFW_KEY_Q),
                                 forward(), right());
    pos += move * s * dt;

    speed *= expf(float(-win.scrollDelta()) * 0.15f);
}

} // namespace vf
