// Pure-logic pin for src/voxel/collision.hpp: the camera sphere sweep and the
// walker footprint, tested against an ANALYTIC wall rather than the scene.
//
// The App binding (src/app/world/collision.cpp) is deliberately not exercised
// here: it reads the live ChunkStore and the micro fields, and the headless
// scene checks are the right place for that. These cases pin the response the
// camera and the alpaca both depend on:
//   * a blocked axis is refused,
//   * the free axis still slides,
//   * a corner can stop the move completely,
//   * an embedded start escapes instead of being welded in place,
//   * the walker cylinder refuses a wall but not thin air.
#include <voxel/collision.hpp>

#include <doctest/doctest.h>

#include <cmath>

using vf::voxel::cylinderBlocked;
using vf::voxel::kCameraRadius;
using vf::voxel::moveSphere;
using vf::voxel::SolidTest;
using vf::voxel::sphereBlocked;

namespace {

// A half-space wall at x = 0: everything with x > 0 is solid.
SolidTest wallAtX0()
{
    return [](const glm::vec3& p) { return p.x > 0.0f; };
}

bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

} // namespace

TEST_CASE("collision: a sphere overlaps a wall before its centre reaches it")
{
    const SolidTest solid = wallAtX0();
    // centre 0.3 m inside the wall is obviously blocked; the surface sample is
    // what makes a centre 0.2 m short of it blocked too (0.2 + 0.25 = 0.45).
    CHECK(sphereBlocked(solid, glm::vec3(0.3f, 0.0f, 0.0f), kCameraRadius));
    CHECK_FALSE(sphereBlocked(solid, glm::vec3(-0.4f, 0.0f, 0.0f),
                              kCameraRadius));
    CHECK(sphereBlocked(solid, glm::vec3(-0.1f, 0.0f, 0.0f), kCameraRadius));
    // A tiny radius only tests the centre-ish shell: still blocked once past.
    CHECK(sphereBlocked(solid, glm::vec3(0.01f, 0.0f, 0.0f), 0.02f));
}

TEST_CASE("collision: a head-on move stops at the wall")
{
    const SolidTest solid = wallAtX0();
    const auto r = moveSphere(solid, glm::vec3(-0.5f, 0.0f, 0.0f),
                              glm::vec3(1.0f, 0.0f, 0.0f), kCameraRadius);
    CHECK(r.hit);
    // The last accepted substep keeps the surface sample at x <= 0.
    CHECK(r.pos.x <= 0.0f);
    CHECK(r.pos.x >= -kCameraRadius - 0.001f);
    CHECK(near(r.pos.y, 0.0f));
    CHECK(near(r.pos.z, 0.0f));
}

TEST_CASE("collision: a blocked axis slides the free one")
{
    const SolidTest solid = wallAtX0();
    const auto r = moveSphere(solid, glm::vec3(-0.5f, 0.0f, 0.0f),
                              glm::vec3(1.0f, 0.0f, 0.6f), kCameraRadius);
    CHECK(r.hit);
    CHECK(r.pos.x <= 0.0f);
    // z made progress: the move glides along the wall instead of stopping.
    CHECK(r.pos.z > 0.5f);
}

TEST_CASE("collision: a corner can refuse every axis")
{
    const SolidTest solid = [](const glm::vec3& p) {
        return p.x > 0.0f || p.z > 0.0f;
    };
    const auto r = moveSphere(solid, glm::vec3(-0.5f, 0.0f, -0.5f),
                              glm::vec3(1.0f, 0.0f, 1.0f), kCameraRadius);
    CHECK(r.hit);
    CHECK(r.stopped);
    CHECK(r.pos.x <= 0.0f);
    CHECK(r.pos.z <= 0.0f);
}

TEST_CASE("collision: an embedded start is allowed to escape")
{
    const SolidTest solid = wallAtX0();
    // Already inside (a camera spawned in geometry, or the world loaded around
    // it): the delta must be honoured verbatim, not refused forever.
    const auto r = moveSphere(solid, glm::vec3(0.1f, 0.0f, 0.0f),
                              glm::vec3(1.0f, 0.0f, 0.0f), kCameraRadius);
    CHECK(r.hit);
    CHECK(near(r.pos.x, 1.1f));
}

TEST_CASE("collision: a zero delta never samples (and never moves)")
{
    const SolidTest solid = [](const glm::vec3&) { return true; };
    const auto r = moveSphere(solid, glm::vec3(3.0f, 4.0f, 5.0f),
                              glm::vec3(0.0f), kCameraRadius);
    CHECK_FALSE(r.hit);
    CHECK(near(r.pos.x, 3.0f));
    CHECK(near(r.pos.y, 4.0f));
    CHECK(near(r.pos.z, 5.0f));
}

TEST_CASE("collision: a walker footprint refuses a wall but not thin air")
{
    const SolidTest solid = wallAtX0();
    // Centre + ring at x = 0.2 + 0.2 = 0.4 crosses the wall.
    CHECK(cylinderBlocked(solid, glm::vec3(0.2f, 0.0f, 0.0f), 0.2f, 0.0f, 0.5f));
    // Well clear of it, and a zero-height band on the ground is free.
    CHECK_FALSE(
        cylinderBlocked(solid, glm::vec3(-0.5f, 0.0f, 0.0f), 0.2f, 0.0f, 0.5f));
    // A band ABOVE the wall (a bridge deck) is not this analytic solid, so it
    // is free: the test is a pure point query, the caller chooses the heights.
    const SolidTest floorOnly = [](const glm::vec3& p) { return p.y < 0.0f; };
    CHECK_FALSE(cylinderBlocked(floorOnly, glm::vec3(0.0f), 0.2f, 0.1f, 0.5f));
    CHECK(cylinderBlocked(floorOnly, glm::vec3(0.0f), 0.2f, -0.5f, 0.5f));
}
