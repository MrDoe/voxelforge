#pragma once
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>

namespace vf::voxel {

// Persistent AI edits layer: assets/ai_edits.vxw + world.json entry.
// All AI-created objects are rasterized into VoxelRecords anchored at the
// user-picked voxel (center-bottom convention) and appended here.
// Splats hot-reload from this file; SVO requires explicit bake.
struct EditableWorld {
    static constexpr const char* kFileName = "ai_edits.vxw";
    static constexpr const char* kLayerName = "ai_edits";
    static constexpr float kBand = 0.20f; // surface band like heightmap_gen sweep

    EditableWorld(std::string assetDir = std::string(VOXELFORGE_ASSET_DIR));

    bool load(); // reads existing ai_edits.vxw if present (empty if missing)
    bool save() const; // writes current records to ai_edits.vxw + ensures manifest
    bool ensureManifest(); // adds layer entry to world.json if absent

    // record generation helpers (anchor is lattice coord of selected voxel = bottom-center)
    std::vector<VoxelRecord> makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const;
    std::vector<VoxelRecord> makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const;
    std::vector<VoxelRecord> makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const;
    std::vector<VoxelRecord> makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const;
    std::vector<VoxelRecord> makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const;

    // append new records, deduping within layer (first wins). Returns number added.
    size_t append(const std::vector<VoxelRecord>& newRecs);
    size_t appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat);
    void clear(); // removes all AI edits and persists

    size_t size() const { return m_records.size(); }
    const std::vector<VoxelRecord>& records() const { return m_records; }
    bool staleSVO() const { return m_staleSVO; }
    void markSVOStale(bool s) { m_staleSVO = s; }

private:
    std::string m_assetDir;
    std::string filePath() const;
    std::string manifestPath() const;
    WorldFileMeta meta() const;

    // common raster: SDF lambda -> surface band records around anchor
    std::vector<VoxelRecord> rasterize(glm::ivec3 anchor, glm::ivec3 halfExtent,
                                       uint8_t mat,
                                       const std::function<float(glm::vec3)>& sdf) const;

    std::vector<VoxelRecord> m_records;
    bool m_staleSVO = false;
};

} // namespace vf::voxel
