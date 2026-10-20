#pragma once
// Micro-voxel field: a SMALL object living on its OWN lattice, independent of
// the world lattice.
//
// The world is a fixed 0.1 m lattice (WORLD/VOXEL/GRID_N/BRICK_N are
// load-bearing, see docs/index.md). A 5 mm vase cannot ride that lattice
// without being quantised to 10 cm, and subdividing the world lattice is
// forbidden. So a micro object gets its own dense grid here, is baked into its
// OWN GpuWorld (micro_svo.hpp), and is rendered by its OWN overlay pass
// (micro_svo_pass.hpp). The two never share buffers, never share a chunk grid,
// and neither is ever asked about the other's cells.
//
// Everything is per-object and small: a 30 cm vase at 5 mm is ~60x15x15 = 13.5 k
// cells, so a dense grid plus a brute-force-quality distance field is cheap
// (no EDT/kPad machinery like the world's VoxelField needs).
#ifndef VOXELFORGE_MICRO_FIELD_HPP
#define VOXELFORGE_MICRO_FIELD_HPP

#include "voxel/worldfile.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace vf::voxel {

// Hard cap on a micro grid's total cells. A dense field costs ~12 B/cell, so
// 4 M cells is ~48 MB and a 20 m cube at 5 mm - far more than any object that
// belongs in a scene. Exceeding it is a load-time rejection with a reason, not
// an OOM: a mis-sized export must fail loudly rather than wedge the app.
inline constexpr size_t kMicroMaxCells = size_t(4) * 1024 * 1024;

// Distance quantisation. The world bake stores the SDF as an int8 count of
// WHOLE voxels, because the world raymarcher is a DDA: it steps cell to cell
// and tests the byte's SIGN, so the hit IS a cell and the cell index is
// always right. The micro overlay instead SPHERE-TRACES a continuous field,
// and whole-voxel truncation destroys exactly what a sphere tracer needs: the
// BFS places the surface at +-0.5 voxel from a cell centre, so truncating
// sends BOTH the outermost solid cell (-0.5 -> 0) and the adjacent air cell
// (+0.5 -> 0) to byte 0. The byte field is then flat zero across a two-cell
// band straddling the surface, the zero crossing lands at the band's edge
// instead of at the surface, and the material lookup reads an AIR cell.
//
// So the micro field counts EIGHTHS of a voxel: the same int8 byte, but
// 8x the resolution, which makes the stored field a faithful signed distance
// with a real zero crossing exactly at the surface. The range is +-127
// eighths = +-15.875 voxels, far beyond anything the tracer needs (and the
// clamp UNDER-estimates, which is the safe direction for a sphere tracer).
inline constexpr int kMicroSdfQuantum = 8;
inline constexpr int kMicroSdfMaxEighths = 127;
// BFS depth ceiling, in steps. (steps - 0.5) * quantum must fit the byte:
// 16 steps -> 15.5 voxels -> 124 eighths.
inline constexpr int kMicroSdfMaxSteps = 16;
inline constexpr float kMicroSdfMaxVoxels = float(kMicroSdfMaxSteps) - 0.5f;

// Air margin, in CELLS, kept around the object inside the micro grid. The
// world AABB is the grid, so the margin is what keeps the object's real
// surface away from the box face the tracer converges onto; see
// buildMicroField for the full reasoning.
inline constexpr int kMicroMarginCells = 4;

struct MicroField {
    // Cell size in METRES. This is the object's own voxel size (0.005 for the
    // 5 mm vase) and has nothing to do with the world VOXEL constant.
    float voxel = 0.005f;
    glm::ivec3 dim { 0, 0, 0 };
    // World position of cell (0,0,0)'s CORNER. Micro worlds are placed by an
    // explicit world position (unlike the world lattice, which is centred on
    // the origin at -WORLD/2), so the origin is carried per object.
    glm::vec3 origin { 0.0f };

    // Dense per-cell arrays, index = (z * dim.y + y) * dim.x + x.
    // `solid` is 1 inside the object. Appearance is only meaningful where solid
    // is 1, but is kept dense so brick filling is a flat read.
    std::vector<uint8_t> solid;
    std::vector<uint8_t> mat;
    std::vector<uint8_t> r, g, b;
    std::vector<uint8_t> refl, rough, tex;
    // Signed distance in METRES, negative inside. Built by buildMicroField.
    std::vector<float> sdf;

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
    glm::vec3 extent() const
    {
        return glm::vec3(dim) * voxel;
    }
    glm::vec3 aabbMin() const { return origin; }
    glm::vec3 aabbMax() const { return origin + extent(); }

    // World point -> cell index (floored). Out of range is clamped, so callers
    // that ask about a point just outside the object get the edge cell.
    glm::ivec3 cellOf(glm::vec3 p) const;
    bool solidAtWorld(glm::vec3 p) const;
    float sdfAtWorld(glm::vec3 p) const;
    // Trilinear sample of `sdf` in world space, clamped at the grid border.
    float sampleSdf(glm::vec3 p) const;
};

// Build a dense micro field from a layer's records, which are expressed in the
// layer's OWN coordinate system (the record's meta.voxelSize is the cell
// size). Fills appearance from each record and derives a signed distance
// field.
//
// Returns false with `err` explaining why (empty records, non-finite cell
// size, or a grid over kMicroMaxCells).
bool buildMicroField(const std::vector<VoxelRecord>& records,
                     const WorldFileMeta& meta, MicroField& out,
                     std::string* err);

// Pack one BRICK_N^3 brick of `f` starting at cell (bx0,by0,bz0) into `data`
// using the world's brick word layout, so micro objects use the identical
// packing the SVO raymarcher already decodes:
//   word0 { r | g<<8 | b<<16 | sdfByte<<24 }
//   word1 { a | refl<<8 | rough<<16 | matId<<24 }
// The object flag (bit 7 of the material byte) is set on every micro cell:
// these ARE objects, so they take the SVO gradient normal and the brick
// material rather than the analytic heightfield path.
void fillMicroBrick(const MicroField& f, uint32_t* data, int bx0, int by0,
                    int bz0);

// Rotate a micro layer's records about the object's BOTTOM-CENTRE pivot, the
// same pivot the world uses for its own layers (so the two agree on what
// "yaw about the base" means).
//
// The map is DESTINATION-driven: every cell of the rotated footprint queries
// its pre-image, so the rotated shape keeps the source's solidity - a forward
// map would drop cells wherever two source cells round to the same
// destination, punching holes in the surface.
//
// Unlike the world's transformRecords, the destination is NOT bounded by the
// layer's own grid: a micro object rotated 45 degrees needs a wider footprint
// than the grid it was authored in, and clipping it would cut the animal in
// half. `outMinShift` is the difference (in CELLS) between the rotated
// footprint's min corner and the source's, so the caller can keep the pivot
// fixed in world space.
//
// Returns false if there is nothing to rotate.
bool rotateMicroRecords(const std::vector<VoxelRecord>& src,
                         const WorldFileMeta& meta, float yawDeg, float pitchDeg,
                         float rollDeg, std::vector<VoxelRecord>& out,
                         glm::ivec3& outMinShift);

// gridN for a micro world holding an object of `extent` metres at `voxel`
// metres. A brick is BRICK_N^3 voxels, and a chunk holds one level of eight
// bricks, so one chunk spans 16 cells: gridN = ceil(maxExtent / (16*voxel)).
// Chosen so the target voxel size comes out EXACTLY (the SVO's brick voxel is
// chunkSize/16); the world grid is padded to a whole number of chunks and the
// object's cells sit inside it, the same way an object straddles a world chunk.
int microGridN(glm::vec3 extent, float voxel);

} // namespace vf::voxel

#endif // VOXELFORGE_MICRO_FIELD_HPP