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
#include <limits>
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
    // The EVEN-ODD occupancy BEFORE the enclosure fill. The fill is not a
    // local operation (a pocket is sealed only if the grid border cannot
    // reach it), so the delta deformer cannot undo it: it keeps the raw
    // parity and re-derives the seal from it every update. For a full
    // voxelization the two differ only inside enclosed air pockets.
    std::vector<uint8_t> raw;
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

// The bone that owns a POSED point, using the one rule the shader and the fur
// share: un-bone the point with EACH candidate bone's own transform and measure
// the result against THAT bone's own rest segment; the nearest wins. Exposed
// because assignMicroMeshBones() and the delta deformer must not grow their own
// variants - a divergent rule reclassifies whatever the pose moved, which is
// how the fleece used to slide over the body.
int microBoneAtPosed(const MicroRig& rig, glm::vec3 posedPoint,
                     const std::vector<glm::mat3>& boneRot,
                     const std::vector<glm::vec3>& boneXform);

// A lattice that contains every pose of an animation, so the deformer's grid
// never has to grow per frame. `margin` cells of air are added on every side
// (the sphere tracer needs a real air band, and the enclosure fill needs an
// air border to start from). Origin is snapped to the mesh's own lattice
// (origin = gridMin * voxel - margin * voxel), so a cell index means the same
// thing in the rest field, the deformer and a full re-voxelization.
struct MicroUnionGrid {
    glm::ivec3 dim { 0, 0, 0 };
    float voxel = 0.f;
    glm::vec3 origin { 0.f };
};

// The bounds of every pose an object can reach. A union grid cannot be built
// from a list of poses in the app (the clips are sampled lazily and a frame's
// posed triangles are 2 MB), so the caller ACCUMULATES and the grid is made at
// the end. `add` takes any triangle set in the layer's frame: the rest mesh,
// then each clip pose at whatever step the caller can afford.
struct MicroUnionBounds {
    glm::vec3 lo { std::numeric_limits<float>::max() };
    glm::vec3 hi { -std::numeric_limits<float>::max() };
    void add(const std::vector<MeshTri>& tris)
    {
        for (const MeshTri& t : tris)
            for (int k = 0; k < 3; ++k) {
                lo = glm::min(lo, t.v[k]);
                hi = glm::max(hi, t.v[k]);
            }
    }
    bool empty() const { return hi.x < lo.x; }
};

MicroUnionGrid microUnionGrid(const MicroUnionBounds& bounds, float voxel,
                              int margin = kMicroMarginCells);

// What one pose update changed, and where the time went. The deltas are the
// input of the GPU patch (P3): only `dirtyBricks` need a brick re-pack, and
// only cells named by the occupancy/sdf changes need a dense-field upload.
struct MicroDeltaStats {
    size_t movedTris = 0;      // triangles re-posed (bones that moved)
    size_t parityFlips = 0;    // cells whose even-odd occupancy flipped
    size_t added = 0;          // solid 0 -> 1
    size_t removed = 0;        // solid 1 -> 0
    size_t rowEvents = 0;      // scanline crossings the delta had to sort
    size_t sdfCells = 0;       // cells whose signed distance was recomputed
    size_t sealAdded = 0;      // of `added`, cells only the enclosure seal closed
    size_t dirtyBricks = 0;    // 8^3 bricks touched (brick re-pack set)
    size_t appearanceTransferred = 0; // added cells that found a rest solid
    size_t appearanceFallbacks = 0;   // added cells that did not
    double poseMs = 0.0;
    double deltaMs = 0.0;
    double sealMs = 0.0;
    double sweepMs = 0.0;
    double sdfMs = 0.0;
    double regionMs = 0.0;     // affected-region + seed-ring construction
    double bfsMs = 0.0;        // the two distance transforms
    double writeMs = 0.0;      // band write + dirty-brick derivation
    double totalMs = 0.0;
};

// The DELTA DEFORMER: one micro object's posed voxels, kept up to date by
// applying ONLY the cells a bone motion changes.
//
// init() voxelizes the rest mesh once on the union grid. updatePose() then:
//   1. re-poses only the triangles whose bone transform changed (a grazing
//      neck does not re-skin the legs, and the skinning is rigid per vertex);
//   2. toggles the even-odd parity between each moved triangle's OLD and NEW
//      crossing, PER SCANLINE ROW - interval XOR, so the one-sided toggles a
//      triangle entering/leaving a row produces cancel against the neighbour
//      that enters/leaves with it (the net change is the surface's swept band,
//      never the whole +x tail of a row);
//   3. re-derives the enclosure seal from the border (exact; a pocket is a
//      global property, so this is the one global stage);
//   4. gives newly solid cells their appearance through the REST field and
//      their bone through the same rule the shader uses;
//   5. refreshes the signed-distance band over the K-DILATION of the changed
//      cells only, seeded at the ring around it from the stored |sdf| (exact:
//      BfsDist is a pure Chebyshev transform, and a ring cell K+1 out has an
//      exact stored value because nothing within K of it changed class).
//      Deliberately NOT a bounding box: a tilting body's flips span the whole
//      animal, and the box around them is ~6x the cells that can actually
//      differ.
//
// The object's cell size and grid are fixed for the object's whole life, which
// is why the grid is a union over every clip: a grazing head leaves the rest
// grid, and per-frame resizing would churn the GPU row/dims mid-frame.
struct MicroDeformer {
    MicroMesh mesh;
    MicroRig rig;
    MicroField rest;   // appearance source, never modified

    // LIVE state. `field` is what a renderer uploads: occupancy, appearance
    // and the signed-distance band.
    MicroField field;
    std::vector<uint8_t> parity;   // even-odd, before the seal
    std::vector<int32_t> bone;     // per-cell bone owner (-1 = none)
    std::vector<uint8_t> ext;      // scratch: air reachable from the border
    std::vector<MeshTri> posed;    // current pose, all triangles

    // internal
    std::vector<std::vector<uint32_t>> boneTris; // triangles touching a bone
    std::vector<uint32_t> triStamp;              // dedupe across moved bones
    uint32_t stamp = 0;
    std::vector<glm::mat3> lastRot;
    std::vector<glm::vec3> lastXform;
    std::vector<uint32_t> movedList;
    struct OldTri { uint32_t t = 0; MeshTri tri; };
    std::vector<OldTri> movedOld;
    struct RowEvent { uint32_t row = 0; int32_t x = 0; };
    std::vector<RowEvent> events;
    std::vector<uint32_t> queue;
    std::vector<uint8_t> dSolid, dAir;   // distance+1, 0 = out of range
    std::vector<std::vector<glm::ivec3>> sdfBuckets;
    std::vector<RowEvent> rowOrder;      // row-counted event order
    std::vector<uint32_t> rowStart;      // counting-sort prefix, dim.y*dim.z+1
    std::vector<uint32_t> brickStamp;
    uint32_t brickEpoch = 0;
    std::vector<uint32_t> dirtyBricks; // scratch, exposed for P3
    bool initialized = false;

    // The cell indices a renderer must re-read: every cell whose SOLID state
    // changed. The SDF is a pure function of the solid set, so this list is
    // both the SDF's input and the P3 upload set.
    std::vector<glm::ivec3> changedCells;

    bool init(const MicroMesh& m, const MicroRig& r, const MicroField& restField,
              MicroUnionGrid grid, MicroDeltaStats* stats, std::string* err);

    // Apply a pose. `boneRot`/`boneXform` are the composed chain transforms
    // (composeBoneChain). Returns false only for a malformed request.
    bool updatePose(const std::vector<glm::mat3>& boneRot,
                    const std::vector<glm::vec3>& boneXform,
                    MicroDeltaStats* stats, std::string* err);

    size_t solidCount() const;

private:
    void refreshSdf(MicroDeltaStats& st);

    // The affected region: every cell within kMicroSdfMaxSteps of a changed
    // cell. A cell further out has the same capped distance, so this is the
    // exact set of cells whose stored SDF can differ - which is why the delta
    // is bit-identical to a full rebuild without touching the whole grid.
    std::vector<glm::ivec3> region;
    std::vector<glm::ivec3> regionFrontier, regionNext;
    std::vector<uint32_t> regionStamp;
    uint32_t regionEpoch = 0;
};

} // namespace vf::voxel

#endif // VOXELFORGE_MICRO_MESH_HPP
