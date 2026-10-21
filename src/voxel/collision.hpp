#pragma once
// Sphere / cylinder collision against the voxel world.
//
// The module deliberately knows NOTHING about VoxelField, ChunkStore or the
// micro SVO worlds: it asks a caller-supplied solidity predicate in WORLD
// space. App supplies that predicate from the live store and every micro field
// at its current animated transform (see src/app/world/collision.cpp). Keeping
// the geometry out of here is what lets the same sweep serve the fly camera (a
// sphere) and a walking micro object (a short vertical cylinder), and lets the
// unit tests pin the response against an analytic wall.
//
// The samples are POINT queries against a 0.1 m lattice, so "solid" means the
// cell containing the sample is solid. A sphere is therefore tested at 27
// points (centre, 6 faces, 12 edges, 8 corners): a wall is at least one cell
// thick and the samples span the whole sphere, so a wall cannot fit between
// two samples.
#include <functional>

#include <glm/glm.hpp>

namespace vf::voxel {

using SolidTest = std::function<bool(const glm::vec3&)>;

// The camera's collision radius (metres). The eye is a sphere, not a point: a
// point test lets the near plane cross a wall before anything refuses.
inline constexpr float kCameraRadius = 0.25f;

// True when any of the 27 samples of the sphere is solid.
bool sphereBlocked(const SolidTest& solid, glm::vec3 centre, float radius);

struct SphereMove {
    glm::vec3 pos { 0.f }; // the resolved centre
    bool hit = false;      // at least one axis was refused (slid/stopped)
    bool stopped = false;  // nothing could move at all (a corner)
};

// Move a sphere by `delta`, substepping at half a radius and sliding along the
// axes that remain free. If the START is already inside solid the delta is
// applied VERBATIM: that is the escape hatch that stops a camera which spawned
// inside geometry from being welded in place forever.
SphereMove moveSphere(const SolidTest& solid, glm::vec3 from, glm::vec3 delta,
                      float radius);

// A vertical cylinder (a walker's footprint): a ring of `sides` samples plus
// the centre, at `layers` heights from y0 to y1 inclusive. `centre`.xz is the
// footprint centre; y0/y1 are world heights (usually the object's solid
// bottom to a low point above it, NOT its full height - the barrel of a
// walker must not be blocked by a tabletop it is standing next to).
bool cylinderBlocked(const SolidTest& solid, glm::vec3 centre, float radius,
                     float y0, float y1, int sides = 10, int layers = 3);

} // namespace vf::voxel
