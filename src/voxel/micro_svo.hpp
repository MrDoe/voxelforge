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
        return (childBase.size() + payload.size() + handles.size() +
                bricks.size() + chunkGrid.size() + chunkInfo.size() * 4) * 4;
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