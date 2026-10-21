// collision.cpp - the one solidity oracle for the camera and the walkers.
//
// Two representations answer a world-space point query:
//   * the WORLD is the live ChunkStore, not the load-time VoxelField: an edit
//     has to be collidable the moment it lands (a carved tunnel must be
//     walkable, an added wall must block), and the store already carries the
//     terrain, the objects and every runtime edit;
//   * a MICRO object is its own fine-lattice field, sampled in its BAKED
//     frame after undoing the transforms the renderer applies - the staged
//     Move offset, the behaviour's walk, the staged rotation - so a collision
//     agrees with the pixels rather than with the bake.
//
// The sweep itself lives in voxel/collision.cpp and knows none of this; that
// keeps the response unit-testable against an analytic wall.
#include "app/app.hpp"

#include "voxel/collision.hpp"
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

namespace {

// The lattice cell containing a world point. (-1,-1,-1) = outside the world.
glm::ivec3 worldCell(glm::vec3 p)
{
    constexpr float kInv = 1.0f / vf::voxel::VOXEL;
    const int n = int(vf::voxel::WORLD / vf::voxel::VOXEL);
    const glm::ivec3 c(
        int(std::floor((p.x + 0.5f * vf::voxel::WORLD) * kInv)),
        int(std::floor((p.y + 0.5f * vf::voxel::WORLD) * kInv)),
        int(std::floor((p.z + 0.5f * vf::voxel::WORLD) * kInv)));
    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= n || c.y >= n || c.z >= n)
        return glm::ivec3(-1);
    return c;
}

} // namespace

float App::terrainTopAt(float x, float z)
{
    if (!m_layers.loaded())
        return -1e9f;
    const glm::ivec3 c = worldCell(glm::vec3(x, 0.0f, z));
    if (c.x < 0)
        return -1e9f;
    // The CPU mirror of the height texture the terrain is SHADED from: the
    // bake's column tops, patched synchronously after every live stamp
    // (patchHeightTexture). A store scan would snap to the voxel staircase,
    // up to a cell below the shaded top, and the animal would stand in the
    // ground on every slope. Nearest is safe here because the clamp takes the
    // MAX over the footprint's corners, which is >= the bilinear surface
    // between those texels.
    const int latN = m_layers.store().latN();
    if (latN > 0 && m_heightCpu.size() == size_t(latN) * size_t(latN)) {
        const float top =
            m_heightCpu[size_t(c.z) * size_t(latN) + size_t(c.x)].x;
        return top > -1e29f ? top : -1e9f;
    }
    return -1e9f;
}

bool App::worldSolidAt(glm::vec3 p)
{
    if (!m_layers.loaded())
        return false;
    const glm::ivec3 c = worldCell(p);
    if (c.x < 0)
        return false;
    return m_layers.store().cellAt(c.x, c.y, c.z).solid;
}

bool App::microFieldSolidAt(size_t index, glm::vec3 p)
{
    if (index >= m_microFields.size())
        return false;
    const vf::voxel::MicroField& f = m_microFields[index];
    const vf::voxel::MicroOccluder& o = m_microOccluders[index];
    if (f.size3() == 0)
        return false;
    glm::vec3 q = p;
    // The live Move preview shifts the RENDERED object; undo it first.
    if ((m_moveStaged || m_moving) && m_moveLayer == o.layerFile)
        q -= m_moveDelta;
    // The behaviour is the OUTERMOST transform (see the shader's
    // microToBaked): undo it, then the rotation preview.
    const bool behaviourHeld = m_moveStaged || m_rotationStaged;
    if (!behaviourHeld && index < m_microBehaviours.size() &&
        m_microBehaviours[index].active)
        q = m_microBehaviours[index].toLocal(q);
    if ((m_rotating || m_rotationStaged || m_rotationPreviewPending) &&
        m_rotateLayer == o.layerFile) {
        const auto it = std::find_if(
            m_worldLayers.begin(), m_worldLayers.end(),
            [&](const vf::voxel::worldfile::WorldLayer& l) {
                return l.file == o.layerFile;
            });
        glm::vec3 pivot;
        if (it != m_worldLayers.end() && layerPivotWorld(o.layerFile, pivot)) {
            // The preview maps the object's CURRENT absolute pose to the
            // staged one about the same pivot - the same helper the trackball
            // uses, so the collided pose cannot drift from the rendered one.
            const glm::mat3 R =
                vf::voxel::worldfile::relativePlacementRotation(
                    it->rotDeg, it->rotX, it->rotZ, it->rotDeg + m_rotateDy,
                    it->rotX + m_rotateDx, it->rotZ + m_rotateDz);
            q = pivot + glm::transpose(R) * (q - pivot);
        }
    }
    return f.sampleSdf(q - o.worldOffset) < 0.0f;
}

bool App::microSolidAt(glm::vec3 p)
{
    if (m_microFields.empty())
        return false;
    for (size_t i = 0; i < m_microFields.size(); ++i)
        if (microFieldSolidAt(i, p))
            return true;
    return false;
}

bool App::solidAt(glm::vec3 p)
{
    return worldSolidAt(p) || microSolidAt(p);
}

glm::vec3 App::resolveCameraMove(glm::vec3 from, glm::vec3 delta)
{
    if (!m_collisionEnabled)
        return from + delta;
    vf::voxel::SolidTest solid = [this](const glm::vec3& p) { return solidAt(p); };
    return vf::voxel::moveSphere(solid, from, delta, vf::voxel::kCameraRadius).pos;
}

bool App::microFootprintBlocked(int index, glm::vec3 offset)
{
    if (index < 0 || index >= int(m_microFields.size()))
        return false;
    const vf::voxel::MicroField& f = m_microFields[size_t(index)];
    if (f.solidMax.x < f.solidMin.x || f.size3() == 0)
        return false; // a rejected/empty field has no body to collide
    const vf::voxel::MicroOccluder& o = m_microOccluders[size_t(index)];

    // The object's body at rest, in world metres. solidMin/Max EXCLUDE the
    // grid's air margin, so this is the animal, not its box.
    const glm::vec3 grid = o.worldOffset + f.origin;
    const glm::vec3 lo = grid + glm::vec3(f.solidMin) * f.voxel;
    const glm::vec3 hi = grid + glm::vec3(f.solidMax + 1) * f.voxel;
    const float wx = hi.x - lo.x, wz = hi.z - lo.z;
    const float cx = 0.5f * (lo.x + hi.x) + offset.x;
    const float cz = 0.5f * (lo.z + hi.z) + offset.z;
    // The CIRCUMSCRIBED radius, not half the longer side: the behaviour turns
    // the object about its base, so the footprint sweeps the diagonal.
    const float radius = 0.5f * std::sqrt(wx * wx + wz * wz) + 0.02f;

    const float ground = terrainTopAt(cx, cz);
    // A walker has no buoyancy: do not stroll into the lake. The water plane
    // is world-wide, so this is cheaper and more honest than a swim state.
    if (ground > -1e8f && ground < vf::voxel::WATER_LEVEL + 0.01f)
        return true;

    // Test just above the object's own base: low enough to catch a fence or
    // the foot of a wall, high enough that the heightfield it stands on is
    // never the thing that refuses the step.
    const float base = std::max(lo.y + offset.y, ground > -1e8f ? ground : lo.y);
    const float y0 = base + 0.02f;
    const float y1 = y0 + std::max(0.10f, std::min(0.45f, (hi.y - lo.y) * 0.6f));

    vf::voxel::SolidTest blocked = [&](const glm::vec3& p) {
        // World objects only. Terrain is excluded BY DESIGN: the walker is
        // clamped to the heightfield, so a raised terrain cell is ground it
        // climbs, not an obstacle. A cabin wall is an object cell and does
        // block.
        const glm::ivec3 c = worldCell(p);
        if (c.x >= 0) {
            const vf::voxel::StoreCell cell = m_layers.store().cellAt(c.x, c.y, c.z);
            if (cell.solid && cell.obj)
                return true;
        }
        // Every OTHER micro object, through the same transform stack the
        // camera uses (behaviour + a staged Move/Rotate preview), so a preview
        // cannot collide with a pose that is not on screen.
        for (size_t j = 0; j < m_microFields.size(); ++j) {
            if (int(j) == index || m_microFields[j].size3() == 0)
                continue;
            if (microFieldSolidAt(j, p))
                return true;
        }
        return false;
    };
    return vf::voxel::cylinderBlocked(blocked, glm::vec3(cx, 0.0f, cz), radius,
                                      y0, y1);
}

} // namespace app
} // namespace vf
