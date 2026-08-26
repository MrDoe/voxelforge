#pragma once
// Voxelforge world file (VXW v1) - canonical binary voxel world asset.
//
// Layout (little-endian):
//   Header      64 B  : magic "VXWF", version, meta, counts, CRC32 of payload
//   SVO buffers       : chunkGrid(i32) childBase(u32) payload(u32)
//                       handles(u32) bricks(2 x u32 per voxel:
//                       word0 = r|g<<8|b<<16|sdf<<24,
//                       word1 = a|refl<<8|rough<<16|mat<<24)
//   Voxel records     : 16 B each - explicit surface voxels
//                       pos u16 x3 (grid index on the VOXEL lattice),
//                       rgba u8 x4, reflectivity u8, roughness u8,
//                       materialId u8, reserved u8
//
// Layer files are written by tools/heightmap_gen (the offline bake, run
// explicitly via `ninja -C build world`) or by EditableWorld (ai_edits.vxw).
// Nothing is regenerated automatically; a missing bake makes app/tests exit
// with "run 'ninja -C build world'".
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace vf::voxel {


struct WorldFileMeta {
    float worldSize = 0.f;
    float voxelSize = 0.f;
    float waterLevel = 0.f;
    uint32_t gridN = 0;
    uint32_t brickN = 8;
};

struct VoxelRecord {
    uint16_t x = 0, y = 0, z = 0; // grid indices, p = -worldSize/2 + idx*voxelSize
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint8_t reflectivity = 0; // 0..255 -> 0..1
    uint8_t roughness = 0;    // 0..255 -> 0..1
    uint8_t materialId = 0;
    uint8_t reserved = 0;

    glm::vec3 position(const WorldFileMeta& m) const
    {
        return { -0.5f * m.worldSize + (float(x) + 0.5f) * m.voxelSize,
                 -0.5f * m.worldSize + (float(y) + 0.5f) * m.voxelSize,
                 -0.5f * m.worldSize + (float(z) + 0.5f) * m.voxelSize };
    }
};

struct WorldFileData {
    WorldFileMeta meta;
    std::vector<int32_t> chunkGrid;
    std::vector<uint32_t> childBase, payload, handles, bricks; // GPU layout
    std::vector<VoxelRecord> voxels;                           // surface records
};

namespace worldfile {
inline constexpr char kMagic[4] = { 'V', 'X', 'W', 'F' };
inline constexpr uint32_t kVersion = 1;

bool write(const std::string& path, const WorldFileData& data);
bool read(const std::string& path, WorldFileData& out); // validates magic+version+CRC

// --- layered worlds ---------------------------------------------------------
// The static world is described by a set of record-only .vxw layer files plus
// a JSON manifest (assets/world.json). Each layer holds voxels already in
// WORLD-lattice coordinates; the optional pos/rot fields document where the
// layer was placed by the packer (tools/heightmap_gen) and are applied there,
// not at runtime. Runtime re-placement happens by translating records on
// import (EditableWorld::importLayer -> ai_edits.vxw). A layer with role
// "packed" (combined.vxw) carries the merged SVO used for ray-marching; it is
// regenerated whenever any layer changes.
struct WorldLayer {
    std::string file;   // relative to the manifest directory
    std::string role;   // "landscape" | "object" | "scatter" | "packed"
    std::string name;   // human-readable id (e.g. "alpaca")
    float pos[3] = { 0.f, 0.f, 0.f }; // world-space placement (informational)
    float rotDeg = 0.f;               // Y rotation applied by the packer
    bool enabled = true;              // false = kept on disk, excluded from merges
    bool listed = true;               // runtime-only: false = discovered folder entry
};

bool loadManifest(const std::string& path, std::vector<WorldLayer>& out);
bool writeManifest(const std::string& path, const std::vector<WorldLayer>& layers);

// Read every layer listed in the manifest (skipping "packed" entries) into one
// record set. Earlier layers win on cell collisions (manifest order =
// priority). Returns false if the manifest is missing or any layer fails to
// load / validate against the expected meta.
bool readLayered(const std::string& manifestPath, const WorldFileMeta& expected,
                 std::vector<VoxelRecord>& out);
} // namespace worldfile

} // namespace vf::voxel
