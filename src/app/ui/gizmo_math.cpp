#include "app/ui/gizmo_math.hpp"

#include "core/camera.hpp"
#include "voxel/layered_world.hpp"

#include <algorithm>
#include <cmath>

namespace vf {
namespace app {

// Project a world point to viewport pixels (perspective, same rule the splat
// VS uses). Callers only feed objects the camera is looking at and skip
// behind-camera AABB corners.
glm::vec2 projectScreen(const glm::vec3& w, const vf::Camera& cam,
                        float tanHalfFov, const glm::ivec2& fb)
{
    // Match splat.vert's view-space projection exactly. Using a normalized
    // world direction here (the old implementation) changes the horizontal
    // scale with depth and put the trackball outside the viewport.
    const glm::vec3 rel = w - cam.pos;
    const float depth = std::max(glm::dot(rel, cam.forward()), 1e-3f);
    const float aspect = float(fb.x) / float(fb.y);
    const float ndcX = glm::dot(rel, cam.right()) /
                       (depth * tanHalfFov * aspect);
    const float ndcY = -glm::dot(rel, cam.up()) / (depth * tanHalfFov);
    return { 0.5f * float(fb.x) * (1.f + ndcX),
             0.5f * float(fb.y) * (1.f + ndcY) };
}

float trackballRingDistance(const glm::vec2& p, const glm::vec2& centre,
                            float radiusX, float radiusY)
{
    if (radiusX < 1e-3f || radiusY < 1e-3f)
        return 1e30f;
    const glm::vec2 d = (p - centre) / glm::vec2(radiusX, radiusY);
    // Use the mean pixel scale rather than the smaller radius. The previous
    // min-radius bias made one inner ellipse swallow clicks intended for the
    // other handles, especially near their intersections.
    return std::abs(glm::length(d) - 1.f) *
           0.5f * (radiusX + radiusY);
}

TrackballHandle trackballHandleAt(const glm::vec2& p,
                                  const glm::vec2& centre, float radius,
                                  float slop)
{
    if (radius < 1e-3f)
        return TrackballHandle::None;
    float best = slop;
    TrackballHandle hit = TrackballHandle::None;
    auto consider = [&](TrackballHandle handle, float rx, float ry) {
        const float d = trackballRingDistance(p, centre, rx, ry);
        if (d < best) {
            best = d;
            hit = handle;
        }
    };
    consider(TrackballHandle::Yaw, radius, radius);
    consider(TrackballHandle::Pitch, radius, radius * 0.42f);
    consider(TrackballHandle::Roll, radius * 0.42f, radius);
    return hit;
}

glm::vec3 moveAxisVector(MoveAxis axis)
{
    switch (axis) {
    case MoveAxis::Y: return glm::vec3(0.f, 1.f, 0.f);
    case MoveAxis::Z: return glm::vec3(0.f, 0.f, 1.f);
    case MoveAxis::None:
    case MoveAxis::X: break;
    }
    return glm::vec3(1.f, 0.f, 0.f);
}

const char* moveAxisName(MoveAxis axis)
{
    switch (axis) {
    case MoveAxis::X: return "X";
    case MoveAxis::Y: return "Y";
    case MoveAxis::Z: return "Z";
    case MoveAxis::None: break;
    }
    return "";
}

MoveAxisLine moveAxisScreenLine(const glm::vec3& worldCentre, MoveAxis axis,
                                const vf::Camera& cam, float tanHalfFov,
                                const glm::ivec2& fb, float radius)
{
    const glm::vec2 p0 = projectScreen(worldCentre, cam, tanHalfFov, fb);
    const glm::vec3 v = moveAxisVector(axis);
    glm::vec2 dir = projectScreen(worldCentre + v * 0.75f, cam, tanHalfFov, fb) - p0;
    if (glm::length(dir) < 1e-3f) {
        // A view-parallel axis has no useful projection; keep a visible,
        // deterministic screen direction instead of dropping the handle.
        dir = axis == MoveAxis::Y ? glm::vec2(0.f, -1.f)
            : axis == MoveAxis::Z ? glm::vec2(0.7071f, 0.7071f)
                                  : glm::vec2(1.f, 0.f);
    }
    const float pixelLength = std::max(radius * 0.9f, 28.f);
    dir = glm::normalize(dir) * pixelLength;
    return { p0 - dir * 0.35f, p0 + dir };
}

float screenSegmentDistance(const glm::vec2& p, const glm::vec2& a,
                            const glm::vec2& b)
{
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-6f)
        return glm::distance(p, a);
    const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::distance(p, a + ab * t);
}

MoveAxis moveAxisHandleAt(const glm::vec2& p, const glm::vec3& worldCentre,
                          const vf::Camera& cam, float tanHalfFov,
                          const glm::ivec2& fb, float radius,
                          float slop)
{
    if (radius < 1e-3f)
        return MoveAxis::None;
    float best = slop;
    MoveAxis hit = MoveAxis::None;
    for (MoveAxis axis : { MoveAxis::X, MoveAxis::Y, MoveAxis::Z }) {
        const MoveAxisLine line = moveAxisScreenLine(
            worldCentre, axis, cam, tanHalfFov, fb, radius);
        const float d = screenSegmentDistance(p, line.a, line.b);
        if (d < best) {
            best = d;
            hit = axis;
        }
    }
    return hit;
}

// The visible trackball is centered on the placed object's bounds and sized
// from those bounds. It is an interaction surface only; object rotation still
// uses transformRecords' exact bottom-center pivot.
bool trackballScreenForBox(const vf::voxel::WorldAABB& b,
                           const vf::Camera& cam, float tanHalfFov,
                           const glm::ivec2& fb, glm::vec2& centre,
                           float& radius)
{
    if (!b.valid())
        return false;
    const glm::vec3 worldCentre = 0.5f * (b.lo + b.hi);
    if (glm::dot(worldCentre - cam.pos, cam.forward()) <= 0.05f)
        return false;
    centre = projectScreen(worldCentre, cam, tanHalfFov, fb);
    float maxR = 0.f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(
            b.lo.x + (i & 1 ? b.hi.x - b.lo.x : 0.f),
            b.lo.y + (i & 2 ? b.hi.y - b.lo.y : 0.f),
            b.lo.z + (i & 4 ? b.hi.z - b.lo.z : 0.f));
        if (glm::dot(corner - cam.pos, cam.forward()) <= 0.05f)
            continue;
        maxR = std::max(maxR, glm::distance(
            centre, projectScreen(corner, cam, tanHalfFov, fb)));
    }
    // Small objects still need a usable hit target; very large bounds stay
    // inside the viewport so all three rings remain reachable.
    radius = std::clamp(maxR, 24.f, 0.46f * float(std::min(fb.x, fb.y)));
    return true;
}

} // namespace app
} // namespace vf
