// Tests for the gizmo screen maths (src/app/ui/gizmo_math.*).
//
// These functions are the shared contract between the overlay that DRAWS a
// trackball / move handle and the frame loop that READS one, so a divergence
// is a bug the user experiences as "the ring is there but the drag does not
// start". They were extracted into a leaf header precisely so they could be
// tested without a window, a GPU, or an App; before the split the only way to
// reach them was to open the editor and click.
#include "app/ui/gizmo_math.hpp"

#include "core/camera.hpp"
#include "voxel/layered_world.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace vf;
using namespace vf::app;

namespace {
// A camera at the origin with forward = (0,0,-1), so screen space is
// predictable: right = +x, up = +y, and a point at (x, y, -d) projects to a
// pixel offset that shrinks with distance. yaw = -90 deg is what puts forward
// down -Z (measured, not assumed: at yaw = 0 forward is +X).
Camera lookDownZ()
{
    Camera cam;
    cam.pos = { 0.f, 0.f, 0.f };
    cam.yaw = -1.5707963f;
    cam.pitch = 0.f;
    return cam;
}

glm::ivec2 fb() { return { 800, 600 }; }
} // namespace

TEST_CASE("projectScreen puts the view centre at the pixel centre")
{
    const Camera cam = lookDownZ();
    const glm::vec2 c = projectScreen({ 0.f, 0.f, 0.f }, cam, 1.f, fb());
    CHECK(c.x == doctest::Approx(400.f));
    CHECK(c.y == doctest::Approx(300.f));
}

TEST_CASE("projectScreen is the splat vertex shader's rule, not a normalized ray")
{
    // A normalized-world-direction projection makes the horizontal scale vary
    // with depth, which put the trackball outside the viewport. The honest
    // division by depth is what keeps the ring on the object: the same world X
    // at two depths must land on the same screen X ratio.
    const Camera cam = lookDownZ();
    const float t = 0.57735027f; // tan(30 deg)
    const glm::vec2 near_ = projectScreen({ 1.f, 0.f, -2.f }, cam, t, fb());
    const glm::vec2 far_ = projectScreen({ 2.f, 0.f, -4.f }, cam, t, fb());
    CHECK(near_.x == doctest::Approx(far_.x).epsilon(1e-4));
    CHECK(near_.y == doctest::Approx(far_.y).epsilon(1e-4));
}

TEST_CASE("projectScreen flips Y so world up is screen up")
{
    const Camera cam = lookDownZ();
    const glm::vec2 up = projectScreen({ 0.f, 1.f, -2.f }, cam, 1.f, fb());
    CHECK(up.y < 300.f);
}

TEST_CASE("trackballRingDistance is zero on the ellipse and uses the mean radius")
{
    // ON the ring: 0. Measured behaviour, so the centre is deliberately
    // NOT expected to be 0 - it is one radius away from the circle.
    CHECK(trackballRingDistance({ 140.f, 100.f }, { 100.f, 100.f },
                                40.f, 40.f) == doctest::Approx(0.f).epsilon(1e-3f));
    // A flattened ring: the scaled distance, not the raw pixel one, so the
    // slop means the same thing on every ring. (0,40) and (100,0) are both on
    // the ellipse rx=100 ry=40.
    CHECK(trackballRingDistance({ 0.f, 40.f }, { 0.f, 0.f }, 100.f, 40.f)
          == doctest::Approx(0.f).epsilon(1e-3f));
    CHECK(trackballRingDistance({ 100.f, 0.f }, { 0.f, 0.f }, 100.f, 40.f)
          == doctest::Approx(0.f).epsilon(1e-3f));
    // The centre is one mean-radius away from the ring, not zero.
    CHECK(trackballRingDistance({ 100.f, 100.f }, { 100.f, 100.f },
                                40.f, 40.f) == doctest::Approx(40.f).epsilon(1e-3f));
}

TEST_CASE("trackballRingDistance refuses a degenerate ring")
{
    // A zero radius has no ellipse; it must report "infinitely far" so the
    // caller picks no handle rather than dividing by zero.
    CHECK(trackballRingDistance({ 0.f, 0.f }, { 0.f, 0.f }, 0.f, 0.f) > 1e20f);
    CHECK(trackballRingDistance({ 0.f, 0.f }, { 0.f, 0.f }, 40.f, 0.f) > 1e20f);
}

TEST_CASE("trackballHandleAt separates the three rings")
{
    const glm::vec2 c { 400.f, 300.f };
    // The outer circle is the yaw ring.
    CHECK(trackballHandleAt({ 400.f + 100.f, 300.f }, c, 100.f)
          == TrackballHandle::Yaw);
    // The inner flattened pair are pitch (wide, short) and roll (narrow, tall).
    CHECK(trackballHandleAt({ 400.f, 300.f - 42.f }, c, 100.f)
          == TrackballHandle::Pitch);
    CHECK(trackballHandleAt({ 400.f + 42.f, 300.f }, c, 100.f)
          == TrackballHandle::Roll);
}

TEST_CASE("trackballHandleAt honours the slop and the degenerate radius")
{
    const glm::vec2 c { 400.f, 300.f };
    // Dead centre is inside every ring, not on any of them.
    CHECK(trackballHandleAt(c, c, 100.f) == TrackballHandle::None);
    // A radius too small to have a ring yields no handle.
    CHECK(trackballHandleAt({ 400.f, 300.f }, c, 0.f) == TrackballHandle::None);

    // A pointer on the yaw circle along the diagonal (measured: returns Yaw).
    // The axis-aligned directions are deliberately NOT used here: on +X the
    // pitch ring (rx 100, ry 42) is nearer in normalised terms than the yaw
    // ring, which is the min-radius behaviour the mean-radius rule only partly
    // removes. The diagonal is unambiguous.
    CHECK(trackballHandleAt({ 470.7f, 229.3f }, c, 100.f, 10.f)
          == TrackballHandle::Yaw);
    // A pointer just outside the slop band is still caught.
    CHECK(trackballHandleAt({ 474.4f, 225.6f }, c, 100.f, 10.f)
          == TrackballHandle::Yaw);
    // ...and well outside it is not.
    CHECK(trackballHandleAt({ 700.f, 700.f }, c, 100.f, 10.f)
          == TrackballHandle::None);
}

TEST_CASE("moveAxisVector and moveAxisName agree on the three axes")
{
    CHECK(moveAxisVector(MoveAxis::X) == glm::vec3(1.f, 0.f, 0.f));
    CHECK(moveAxisVector(MoveAxis::Y) == glm::vec3(0.f, 1.f, 0.f));
    CHECK(moveAxisVector(MoveAxis::Z) == glm::vec3(0.f, 0.f, 1.f));
    CHECK(std::string(moveAxisName(MoveAxis::X)) == "X");
    CHECK(std::string(moveAxisName(MoveAxis::Y)) == "Y");
    CHECK(std::string(moveAxisName(MoveAxis::Z)) == "Z");
    CHECK(std::string(moveAxisName(MoveAxis::None)).empty());
}

TEST_CASE("screenSegmentDistance is the distance to the segment, not the line")
{
    // Perpendicular foot inside the segment.
    CHECK(screenSegmentDistance({ 5.f, 0.f }, { 0.f, 0.f }, { 10.f, 0.f })
          == doctest::Approx(0.f));
    // Beyond an endpoint the distance is to that endpoint, not to the line.
    CHECK(screenSegmentDistance({ 20.f, 0.f }, { 0.f, 0.f }, { 10.f, 0.f })
          == doctest::Approx(10.f));
    // A degenerate segment is a point.
    CHECK(screenSegmentDistance({ 3.f, 4.f }, { 0.f, 0.f }, { 0.f, 0.f })
          == doctest::Approx(5.f));
}

TEST_CASE("moveAxisScreenLine keeps a handle visible on a view-parallel axis")
{
    // Looking down -Z, the world Z axis is view-parallel: its projection is
    // degenerate, and the handle must still get a deterministic screen
    // direction rather than disappearing.
    const Camera cam = lookDownZ();
    const MoveAxisLine l = moveAxisScreenLine({ 0.f, 0.f, 0.f }, MoveAxis::Z,
                                              cam, 1.f, fb(), 100.f);
    CHECK(glm::length(l.b - l.a) > 1.f);
    // World Y is perpendicular, so it projects normally (upward = -y pixels).
    const MoveAxisLine ly = moveAxisScreenLine({ 0.f, 0.f, 0.f }, MoveAxis::Y,
                                               cam, 1.f, fb(), 100.f);
    CHECK(ly.b.y < ly.a.y);
}

TEST_CASE("moveAxisHandleAt picks the nearest handle and none when far")
{
    const Camera cam = lookDownZ();
    const MoveAxisLine lx = moveAxisScreenLine({ 0.f, 0.f, 0.f }, MoveAxis::X,
                                               cam, 1.f, fb(), 100.f);
    const glm::vec2 mid = 0.5f * (lx.a + lx.b);
    CHECK(moveAxisHandleAt(mid, { 0.f, 0.f, 0.f }, cam, 1.f, fb(), 100.f)
          == MoveAxis::X);
    // A degenerate radius yields no handle.
    CHECK(moveAxisHandleAt(mid, { 0.f, 0.f, 0.f }, cam, 1.f, fb(), 0.f)
          == MoveAxis::None);
}

TEST_CASE("trackballScreenForBox rejects an invalid or behind-camera box")
{
    const Camera cam = lookDownZ();
    glm::vec2 centre;
    float radius = 0.f;

    voxel::WorldAABB invalid;
    CHECK_FALSE(trackballScreenForBox(invalid, cam, 1.f, fb(), centre, radius));

    // Entirely behind the camera: every corner is culled, so no usable centre.
    voxel::WorldAABB behind;
    behind.lo = { -1.f, -1.f, 1.f };
    behind.hi = { 1.f, 1.f, 2.f };
    CHECK_FALSE(trackballScreenForBox(behind, cam, 1.f, fb(), centre, radius));
}

TEST_CASE("trackballScreenForBox centres on the bounds and clamps the radius")
{
    const Camera cam = lookDownZ();
    voxel::WorldAABB b;
    b.lo = { -1.f, -1.f, -10.f };
    b.hi = { 1.f, 1.f, -9.f };
    glm::vec2 centre;
    float radius = 0.f;
    REQUIRE(trackballScreenForBox(b, cam, 1.f, fb(), centre, radius));
    // Symmetric about the view axis, so the centre is the pixel centre.
    // (measured: a box at z -10..-9 viewed down -Z centres on (400,300))
    CHECK(std::abs(centre.x - 400.f) < 0.5f);
    CHECK(std::abs(centre.y - 300.f) < 0.5f);
    // A small object still gets a usable hit target (the 24 px floor).
    CHECK(radius >= 24.f);
    // A huge one stays inside the viewport (the 0.46 * min(fb) ceiling).
    voxel::WorldAABB huge;
    huge.lo = { -500.f, -500.f, -2.f };
    huge.hi = { 500.f, 500.f, -1.f };
    REQUIRE(trackballScreenForBox(huge, cam, 1.f, fb(), centre, radius));
    CHECK(radius <= 0.46f * 600.f + 1e-3f);
}
