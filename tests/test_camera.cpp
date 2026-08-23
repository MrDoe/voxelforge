#include "core/camera.hpp"
#include <doctest/doctest.h>
#include <cmath>
#include <glm/gtc/constants.hpp>

using namespace vf;

namespace {
bool approxVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
    return glm::length(a - b) < eps;
}
bool approxVec(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) < eps;
}
} // namespace

TEST_CASE("camera basis is orthonormal for many orientations")
{
    Camera cam;
    for (float yaw = -3.1f; yaw <= 3.2f; yaw += 0.7f) {
        for (float pitch = -1.5f; pitch <= 1.5f; pitch += 0.5f) {
            cam.yaw = yaw;
            cam.pitch = pitch;
            glm::vec3 f = cam.forward(), r = cam.right(), u = cam.up();
            CHECK(approxVec(glm::length(f), 1.0f));
            CHECK(approxVec(glm::length(r), 1.0f));
            CHECK(approxVec(glm::length(u), 1.0f));
            CHECK(std::abs(glm::dot(f, r)) < 1e-4);
            CHECK(std::abs(glm::dot(f, u)) < 1e-4);
            CHECK(std::abs(glm::dot(r, u)) < 1e-4);
            // right-handed: f x r == -u? verify up consistency
            CHECK(approxVec(glm::cross(r, f), u)); // matches up() definition
        }
    }
}

TEST_CASE("canonical orientations")
{
    Camera cam;
    cam.yaw = -glm::half_pi<float>();
    cam.pitch = 0.0f;
    CHECK(approxVec(cam.forward(), glm::vec3(0.0f, 0.0f, -1.0f))); // default spawn looks toward scene
    CHECK(approxVec(cam.right(), glm::vec3(1.0f, 0.0f, 0.0f)));

    cam.yaw = 0.0f;
    CHECK(approxVec(cam.forward(), glm::vec3(1.0f, 0.0f, 0.0f)));

    cam.yaw = glm::half_pi<float>();
    CHECK(approxVec(cam.forward(), glm::vec3(0.0f, 0.0f, 1.0f)));

    cam.pitch = glm::half_pi<float>() * 0.99f;
    CHECK(cam.forward().y > 0.9f); // looking up
}

TEST_CASE("look clamps pitch and rotates yaw monotonically")
{
    float yaw = 0.f, pitch = 0.f;
    Camera::applyLook(yaw, pitch, 100.f, -100.f); // drag right/up
    CHECK(yaw < 0.f);
    CHECK(pitch > 0.f);

    for (int i = 0; i < 200; ++i)
        Camera::applyLook(yaw, pitch, 0.f, 100.f);
    CHECK_LE(pitch, 1.5533f + 1e-5f);

    for (int i = 0; i < 400; ++i)
        Camera::applyLook(yaw, pitch, 0.f, -100.f);
    CHECK_GE(pitch, -1.5533f - 1e-5f);
}

TEST_CASE("computeMove normalizes and respects keys")
{
    glm::vec3 f(0, 0, -1), r(1, 0, 0);
    auto mv = [](auto... args) { return Camera::computeMove(args...); };

    CHECK(approxVec(mv(true, false, false, false, false, false, f, r), f));
    CHECK(approxVec(mv(false, true, false, false, false, false, f, r), -f));
    CHECK(approxVec(mv(false, false, true, false, false, false, f, r), -r));
    CHECK(approxVec(mv(false, false, false, true, false, false, f, r), r));

    // W+D diagonal has unit length (normalized)
    glm::vec3 diag = mv(true, false, false, true, false, false, f, r);
    CHECK(approxVec(glm::length(diag), 1.0f));
    CHECK_LT(diag.z, -0.6f);
    CHECK_GT(diag.x, 0.6f);

    // opposite keys cancel
    CHECK(approxVec(mv(true, true, false, false, false, false, f, r), glm::vec3(0.0f)));

    // vertical only affects y
    glm::vec3 upv = mv(false, false, false, false, true, false, f, r);
    CHECK(approxVec(upv, glm::vec3(0.0f, 1.0f, 0.0f)));

    // nothing pressed
    CHECK(approxVec(mv(false, false, false, false, false, false, f, r), glm::vec3(0.0f)));
}
