#pragma once
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace vf::voxel {

// Persistent AI edits layer: assets/ai_edits.vxw + world.json entry.
// All AI-created objects are rasterized into VoxelRecords anchored at the
// user-picked voxel (center-bottom convention) and appended here.
// The app hot-reloads this layer via its disk poll within ~0.5 s.
struct EditableWorld {
    static constexpr const char* kFileName = "ai_edits.vxw";
    static constexpr const char* kLayerName = "ai_edits";
    static constexpr float kBand = 0.20f; // surface band like heightmap_gen sweep

    EditableWorld(std::string assetDir = std::string(VOXELFORGE_ASSET_DIR));

    bool load(); // reads existing ai_edits.vxw if present (empty if missing)
    bool save() const; // writes current records to ai_edits.vxw + ensures manifest
    void enableInManifest() const; // mark the ai_edits layer enabled in world.json
    bool ensureManifest(); // adds layer entry to world.json if absent

    // record generation helpers (anchor is lattice coord of selected voxel = bottom-center)
    std::vector<VoxelRecord> makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const;
    std::vector<VoxelRecord> makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const;
    std::vector<VoxelRecord> makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const;
    std::vector<VoxelRecord> makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const;
    std::vector<VoxelRecord> makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const;

    // Import a foreign .vxw layer file: translates its records so the object's
    // bottom-center lands on `anchor` and appends the copy to ai_edits.vxw.
    // This is the only runtime placement path — layer files store absolute
    // lattice coords, so enabling a layer shows it where it was baked, while
    // import stamps a moved copy here. Returns records added (0 on read/meta
    // failure or when nothing new survives dedupe/bounds).
    size_t importLayer(const std::string& vxwPath, glm::ivec3 anchor);

    // append new records, deduping within layer (first wins). Returns number added.
    size_t append(const std::vector<VoxelRecord>& newRecs);
    size_t appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat);
    void clear(); // removes all AI edits and persists

    // --- arbitrary-object authoring -----------------------------------------
    // Write a standalone named object layer (<name>.vxw, record-only) into the
    // asset dir and register it (enabled, role "object") in world.json so the
    // running app hot-reloads it. Overwrites an existing layer of the same name
    // (the modification path: read_object -> edit -> write_object). Returns
    // false for an empty/illegal name.
    bool writeObjectLayer(const std::string& name,
                          const std::vector<VoxelRecord>& recs);

    // Remove a named object layer file + its manifest entry (never landscape or
    // the packed cache). Returns false if absent, protected, or an illegal name.
    bool deleteObjectLayer(const std::string& name);

    size_t size() const { return m_records.size(); }
    const std::vector<VoxelRecord>& records() const { return m_records; }

    // Keep a layer name filesystem- and manifest-safe: [A-Za-z0-9_-], <=40 chars.
    static std::string sanitizeLayerName(const std::string& name);

private:
    std::string m_assetDir;
    std::string filePath() const;
    std::string manifestPath() const;
    WorldFileMeta meta() const;

    std::vector<VoxelRecord> m_records;
};

} // namespace vf::voxel
