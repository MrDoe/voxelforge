#pragma once
// micro_mesh.hpp - a MESH + SKIN deformation source for a micro object.
//
// WHY THIS EXISTS. The micro object has always been a DENSE REST-POSE FIELD
// sampled through the INVERSE of a rigid bone transform (micro_overlay.comp).
// That map is discontinuous wherever bone territory flips: a point just inside
// the posed neck can un-bone to a rest point that belongs to a different bone,
// so the field jumps from positive to several voxels negative with no zero
// crossing in between. Measured consequences (see the wiki page
// concepts/micro-bone-claim-rule): a shell where NO bone claims the point, the
// tracer strides 6 voxels across it (hole), or lands deep inside a solid
// terminal that has no brick (black pixel).
//
// A MESH has no such map. The deformation is ordinary linear-blend skinning on
// vertices, and the field is VOXELIZED FROM THE POSED TRIANGLES. The surface
// the tracer reads is the surface the mesh has, so there is no world->rest
// transform to be discontinuous, and the bug class is removed by construction
// rather than mitigated.
//
// The stages here are deliberately app-free (src/voxel, no Vulkan): load the
// mesh + its skin, pose it at a given set of bone transforms, voxelize the
// posed triangles on a given grid, and build a MicroField from the result with
// appearance transferred through the REST field. That is everything P1 needs
// to prove the pipeline offline, and everything P2 needs to make it a delta.
//
// TWO DETAILS THAT ARE LOAD-BEARING:
//
//  1. The shell pass classifies a cell by the SIGN OF THE PSEUDO-NORMAL at the
//     closest triangle point, not by "the triangle touches this cube"
//     (mesh_voxel.hpp's triCubeOverlap). A face between a solid and an air
//     cell lies exactly ON the boundary plane, so a box/triangle overlap test
//     marks BOTH cells - and then the flood fill cannot escape, and the object
//     comes out one cell dilated on every axis. The sign test puts the cell
//     centre on the correct side and leaves the rest of the grid to the fill.
//
//  2. Only cells within half a cell DIAGONAL of a triangle are classified;
//     everything further away is decided by the flood fill (unreachable from
//     the border = interior = solid). That is what makes a thin wall (one cell
//     thick) solid instead of a shell with an air core.
//
// Appearance is NOT taken from the mesh: the STL carries one material and no
// palette art. A posed cell's centre is un-boned through its OWN bone's
// inverse transform into the rest field, and that cell's material/colour/
// response is copied. So the fleece stays fleece and the eyes stay dark.
#ifndef VOXELFORGE_MICRO_MESH_HPP
#define VOXELFORGE_MICRO_MESH_HPP

#include "voxel/mesh_voxel.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_rig.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace vf::voxel {

// A micro object's mesh and its rigid skin. The STL is NON-INDEXED: every
// triangle owns three vertices, so `rest` has exactly 3 * tris.size() entries
// and vertex i belongs to triangle i/3, corner i%3. That is why the skin
// side-car indexes VERTICES, not mesh vertices in the indexed sense: there is
// nothing to share.
struct MicroMesh {
    std::vector<glm::vec3> rest;  // rest-pose vertices, object frame (metres)
    std::vector<int32_t> bone;    // per-vertex bone index (-1 = unskinned)
    std::vector<MeshTri> tris;    // rest triangles (their v[] mirror `rest`)
    std::vector<std::string> boneNames; // from the skin header, for diagnostics
    // The translation that was REMOVED from the vertices on load, so the mesh's
    // min corner sits at the object's min corner. Non-zero means the file kept
    // raw lattice indices instead of the layer's frame; the shift is applied
    // (not tolerated), because a side-car mesh with a residual offset would
    // voxelize into the wrong cells and read as "the mesh does not match the
    // voxels".
    glm::vec3 restOffset { 0.f };
    bool loaded = false;

    size_t vertCount() const { return rest.size(); }
};

// Load a mesh and its skin side-car.
//
// The skin format is text:
//   # comments
//   <vertexCount> <boneCount>
//   <vertexIndex> <boneIndex>        (one line per vertex)
// A missing side-car is not an error: every vertex is then unskinned (-1) and
// the posed mesh equals the rest mesh, which is exactly what a rigid object
// wants.
bool loadMicroMesh(const std::string& meshPath, const std::string& skinPath,
                   MicroMesh& out, std::string* err);

// Linear-blend skinning with RIGID weights (one bone per vertex):
//   posed = R[bone] * rest + t[bone]
// This is the same composed transform the shader applies (composeBones), so a
// mesh posed here and a field sampled by the overlay agree bone for bone.
// Vertices with an out-of-range or unset bone stay in the rest pose.
void poseMicroMesh(const MicroMesh& mesh, const std::vector<glm::mat3>& boneRot,
                   const std::vector<glm::vec3>& boneXform,
                   std::vector<MeshTri>& posed);

// A voxelization result on a caller-chosen grid. `origin` is the object-frame
// position of cell (0,0,0)'s CORNER, the same convention MicroField uses.
struct MicroMeshGrid {
    glm::ivec3 dim { 0, 0, 0 };
    float voxel = 0.008f;
    glm::vec3 origin { 0.f };
    std::vector<uint8_t> solid;
    // The bone that owns each SOLID cell (filled by assignMicroMeshBones).
    // Needed twice: the appearance transfer un-bones the cell through it, and
    // the delta voxelizer (P2) decides which cells a moving bone can touch.
    std::vector<int32_t> bone;
    size_t shellCells = 0;   // cells a triangle passes within half a diagonal
    size_t interiorCells = 0; // cells decided by the fill (never touched)
    bool leak = false;        // no interior at all -> the fill escaped

    size_t size3() const
    {
        return size_t(dim.x) * size_t(dim.y) * size_t(dim.z);
    }
    size_t idx(int x, int y, int z) const
    {
        return (size_t(z) * size_t(dim.y) + size_t(y)) * size_t(dim.x) +
               size_t(x);
    }
    bool inside(int x, int y, int z) const
    {
        return x >= 0 && y >= 0 && z >= 0 && x < dim.x && y < dim.y && z < dim.z;
    }
    size_t solidCount() const
    {
        size_t n = 0;
        for (uint8_t s : solid)
            n += s ? 1 : 0;
        return n;
    }
};

// Voxelize the POSED triangles onto the grid `dim`/`voxel`/`origin`.
//
// Returns false only for a malformed request (empty mesh, empty grid, a
// non-positive voxel). A mesh that leaves the grid is not an error: the cells
// that fall outside simply do not exist, and `out.leak` reports the case where
// the surface does not enclose anything on this grid.
bool voxelizeMicroMesh(const std::vector<MeshTri>& posed, glm::ivec3 dim,
                       float voxel, glm::vec3 origin, MicroMeshGrid& out,
                       std::string* err);

// Attribute every solid cell to a bone, using the SAME rule the shader's fixed
// microBoneAt uses: un-bone the POSED cell centre with each candidate bone's
// own transform and measure the result against THAT bone's own rest segment;
// the nearest wins. Asking which rest segment is nearest the POSED point (the
// old rule) reclassifies whatever the pose moved, which is why the fleece used
// to slide over the body.
void assignMicroMeshBones(MicroMeshGrid& grid, const MicroRig& rig,
                          const std::vector<glm::mat3>& boneRot,
                          const std::vector<glm::vec3>& boneXform);

struct MeshFieldStats {
    size_t solidCells = 0;
    size_t transferred = 0; // appearance copied from the rest field
    size_t fallbacks = 0;   // no rest solid within the search radius
};

// Build a dense MicroField from a voxelized (posed) mesh: occupancy from
// `grid`, appearance transferred through `rest` (the rest-pose field) using
// each cell's bone transform, and the signed-distance band from the shared
// rule (buildMicroSdf). `grid.bone` must be filled (assignMicroMeshBones).
bool buildMicroFieldFromMesh(const MicroMeshGrid& grid,
                             const std::vector<glm::mat3>& boneRot,
                             const std::vector<glm::vec3>& boneXform,
                             const MicroField& rest, MicroField& out,
                             MeshFieldStats* stats, std::string* err);

} // namespace vf::voxel

#endif // VOXELFORGE_MICRO_MESH_HPP
