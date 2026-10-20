#pragma once
// Micro SVO synthesis: bake a MicroField into its OWN GpuWorld on the object's
// own fine lattice. Independent of the world GpuWorld in every way - separate
// chunk grid, separate chunk bases, separate pools. MicroOverlayPass concatenates
// several of these and the micro_overlay.comp shader resolves them from one
// per-world table, so N micro objects are N independent SVO worlds that never
// touch the 0.1 m world's data.
#ifndef VOXELFORGE_MICRO_SVO_HPP
#define VOXELFORGE_MICRO_SVO_HPP

#include "voxel/micro_field.hpp"
#include "voxel/world.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace vf::voxel {

// One micro SVO world. Everything is per-object; nothing is shared with the
// main world GpuWorld.
struct MicroWorld {
    // The same layout as the main GpuWorld (see world.hpp), so the shader's
    // brick/handle decode is identical; only the SCALES differ.
    std::vector<uint32_t> childBase, payload, handles, bricks;
    std::vector<int32_t> chunkGrid;   // gridN^3 root handles (-1 = empty)
    std::vector<GpuChunkInfo> chunkInfo; // gridN^3 bases

    // The DENSE signed-distance field, one signed byte per cell, in the same
    // order as MicroField (z * dimY + y) * dimX + x, and in EIGHTH-VOXEL
    // units (kMicroSdfQuantum).
    //
    // It is a SEPARATE array from the bricks on purpose. The bricks store the
    // field per cell too, but the shader's trilinear stencil reaches one cell
    // PAST the sample point, and a brick can only be indexed inside itself -
    // so the stencil has to be clamped to the brick, and a clamped stencil
    // makes the field JUMP at every brick boundary (a +0.5 voxel step to a
    // -1.0 voxel one, with no zero crossing in between). The sphere tracer
    // then has nothing to bisect and the object's surface is found at a
    // discontinuity instead of at the surface.
    //
    // A dense field has no such boundary: the stencil simply reads the
    // neighbouring cells of the same array, so the field is continuous
    // everywhere the object lives. It is affordable precisely because a micro
    // object is small - a 30 cm vase at 5 mm is 25x68x25 = 42 k cells = 42 KB.
    std::vector<uint8_t> field;

    float voxel = 0.005f;   // this world's cell size (m)
    int gridN = 1;          // chunks per axis
    // The AUTHORED grid's cell count per axis. Chunks span 16 cells, so a small
    // object's outermost chunks run past the grid; the shader clamps its brick
    // cell index to these or it reads filler cells (grass material).
    glm::ivec3 fieldDim { 0 };
    float chunkM = 0.0f;    // chunk edge in metres (= voxel * 16)
    glm::vec3 worldMin { 0.f }; // world position of cell (0,0,0)'s corner
    glm::vec3 worldMax { 0.f }; // exclusive far corner

    size_t brickCount() const { return bricks.size() / BRICK_WORDS; }
    size_t bytes() const
    {
        // The uint32 arrays are 4 bytes each; `field` is already ONE BYTE per
        // cell, so it must not be multiplied again (the log line overstated
        // the alpaca's GPU footprint 4x while it was added to the sum).
        return (childBase.size() + payload.size() + handles.size() +
                bricks.size() + chunkGrid.size() + chunkInfo.size() * 4) *
                   4 +
               field.size();
    }
};

// Synthesize a micro SVO world from `f`. The object's cells sit at the grid's
// origin; `worldOffset` translates the WHOLE micro world in the main world (the
// placement the layer asks for). Fully-inside chunks become solid terminals,
// empty ones stay empty, and only mixed chunks descend - the same
// classify/subdivide/brick recursion the world bake uses, at micro scale.
void synthesizeMicroSvo(const MicroField& f, glm::vec3 worldOffset,
                        MicroWorld& out);

} // namespace vf::voxel

#endif // VOXELFORGE_MICRO_SVO_HPP