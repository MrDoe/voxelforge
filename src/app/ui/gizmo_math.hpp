#pragma once

// Screen-space geometry of the rotate trackball and the move axis handles.
//
// One contract between the code that DRAWS a gizmo and the frame loop that
// READS one, so the ring the user sees and the ring the pointer is tested
// against cannot disagree. Pure geometry over a Camera and an AABB: no App,
// no GPU, so it is directly unit-testable.

#include "app/ui/ui_types.hpp"

#include "core/camera.hpp"
#include "voxel/layered_world.hpp"

#include <glm/glm.hpp>

namespace vf {
namespace app {

// The screen-space line an axis handle is drawn along.
struct MoveAxisLine {
    glm::vec2 a;
    glm::vec2 b;
};
// Project a world point to viewport pixels (perspective, same rule the splat
// VS uses). Callers only feed objects the camera is looking at and skip
// behind-camera AABB corners.
glm::vec2 projectScreen(const glm::vec3& w, const vf::Camera& cam,
                        float tanHalfFov, const glm::ivec2& fb);
// Pixel distance from an ellipse (the ring is a flattened circle in screen
// space), and which of the three rings a pointer is over.
float trackballRingDistance(const glm::vec2& p, const glm::vec2& centre,
                            float radiusX, float radiusY);
TrackballHandle trackballHandleAt(const glm::vec2& p,
                                  const glm::vec2& centre, float radius,
                                  float slop = 10.f);
// The Move gizmo's world axis and its label.
glm::vec3 moveAxisVector(MoveAxis axis);
const char* moveAxisName(MoveAxis axis);
MoveAxisLine moveAxisScreenLine(const glm::vec3& worldCentre, MoveAxis axis,
                                const vf::Camera& cam, float tanHalfFov,
                                const glm::ivec2& fb, float radius);
// Point-to-segment distance, and which axis handle a pointer is over.
float screenSegmentDistance(const glm::vec2& p, const glm::vec2& a,
                            const glm::vec2& b);
MoveAxis moveAxisHandleAt(const glm::vec2& p, const glm::vec3& worldCentre,
                          const vf::Camera& cam, float tanHalfFov,
                          const glm::ivec2& fb, float radius,
                          float slop = 12.f);
// The visible trackball is centered on the placed object's bounds and sized
// from those bounds. It is an interaction surface only; object rotation still
// uses transformRecords' exact bottom-center pivot.
bool trackballScreenForBox(const vf::voxel::WorldAABB& b,
                           const vf::Camera& cam, float tanHalfFov,
                           const glm::ivec2& fb, glm::vec2& centre,
                           float& radius);

} // namespace app
} // namespace vf
