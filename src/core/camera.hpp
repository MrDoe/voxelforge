#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace vf {



class Camera {
public:
    glm::vec3 pos { 0.0f, 1.4f, 7.0f };
    float yaw = -glm::half_pi<float>(); // yaw 0 => +X, -90deg => -Z
    float pitch = -0.12f;
    float speed = 4.0f;

    void update(const class Window& win, float dt);

    // Pure helpers (unit-tested):
    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    static void applyLook(float& yaw, float& pitch, float dxPx, float dyPx);
    static glm::vec3 computeMove(bool fwd, bool back, bool left, bool rightIn,
                                 bool up, bool down,
                                 const glm::vec3& f, const glm::vec3& r);
};

} // namespace vf
