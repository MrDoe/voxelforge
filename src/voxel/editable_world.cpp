#include "voxel/editable_world.hpp"
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/picking.hpp"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <cmath>
#include <algorithm>

namespace vf::voxel {

EditableWorld::EditableWorld(std::string assetDir) : m_assetDir(std::move(assetDir)) {}

std::string EditableWorld::filePath() const { return m_assetDir + "/" + kFileName; }
std::string EditableWorld::manifestPath() const { return m_assetDir + "/world.json"; }
WorldFileMeta EditableWorld::meta() const { return {WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8}; }

bool EditableWorld::load() {
    WorldFileData d;
    if (worldfile::read(filePath(), d)) {
        if (d.meta.worldSize != WORLD || d.meta.voxelSize != VOXEL || d.meta.gridN != uint32_t(GRID_N)) {
            spdlog::warn("editable_world: meta mismatch in {}, discarding", filePath());
            m_records.clear();
            return false;
        }
        m_records = std::move(d.voxels);
            spdlog::info("editable_world: loaded {} records from {}", m_records.size(), filePath());
    } else {
        m_records.clear(); // no file yet -> empty layer
    }
    // manifest presence does not affect load, but ensure is called on save
    return true;
}

bool EditableWorld::ensureManifest() {
    std::vector<worldfile::WorldLayer> layers;
    bool hasManifest = worldfile::loadManifest(manifestPath(), layers);
    if (!hasManifest) {
        // no manifest at all -> create minimal with ai_edits + landscape + packed
        // but normally manifest exists; just add ai_edits
        layers.clear();
    }
    for (auto &l : layers) if (l.file == kFileName) return true;
    worldfile::WorldLayer nl;
    nl.file = kFileName;
    nl.role = "object";
    nl.name = kLayerName;
    nl.pos[0]=0.f; nl.pos[1]=0.f; nl.pos[2]=0.f;
    nl.rotDeg = 0.f;
    // presence only: starts DISABLED so a pristine valley stays pristine;
    // enableInManifest() flips it when AI content actually lands
    nl.enabled = false;
    nl.listed = true;
    // insert as first object layer (highest priority) but before landscape/packed
    // Find insertion point: before landscape
    size_t insertAt = 0;
    for (size_t i=0;i<layers.size();++i) if (layers[i].role=="landscape") { insertAt=i; break; }
    layers.insert(layers.begin()+insertAt, nl);
    if (!worldfile::writeManifest(manifestPath(), layers)) {
        spdlog::error("editable_world: failed to write manifest {}", manifestPath());
        return false;
    }
    spdlog::info("editable_world: inserted {} into manifest", kFileName);
    return true;
}

bool EditableWorld::save() const {
    WorldFileData d;
    d.meta = meta();
    d.voxels = m_records;
    // record-only file: SVO buffers stay empty (legal)
    if (!worldfile::write(filePath(), d)) {
        spdlog::error("editable_world: write failed {}", filePath());
        return false;
    }
    // ensure manifest lists it
    const_cast<EditableWorld*>(this)->ensureManifest();
    return true;
}

std::vector<VoxelRecord> EditableWorld::rasterize(glm::ivec3 anchor, glm::ivec3 halfExtent,
                                                  uint8_t mat,
                                                  const std::function<float(glm::vec3)>& sdf) const {
    std::vector<VoxelRecord> out;
    // brute force AABB sweep around anchor, keep shell |d| <= kBand
    // anchor is bottom-center voxel; local dy=0 => anchor.y
    for (int dz=-halfExtent.z; dz<=halfExtent.z; ++dz)
        for (int dy=0; dy<=halfExtent.y*2; ++dy) // height is upward from anchor (bottom)
            for (int dx=-halfExtent.x; dx<=halfExtent.x; ++dx) {
                // For height we handle halfExtent.y as half height? Use full height param instead.
                // This generic raster assumes anchor is center, not bottom. So shift:
                // We'll compute world pos of candidate cell center
                glm::ivec3 c(anchor.x + dx, anchor.y + dy, anchor.z + dz);
                if (c.x<0||c.y<0||c.z<0) continue;
                if (c.x>=1024||c.y>=1024||c.z>=1024) continue;
                glm::vec3 p = voxelCenter(c);
                // evaluate SDF relative to object center (anchor + halfExtent.y up)
                // Caller provides sdf that is already centered at appropriate place.
                float d = sdf(p);
                if (std::fabs(d) > kBand) continue;
                VoxelRecord v;
                v.x = uint16_t(c.x); v.y = uint16_t(c.y); v.z = uint16_t(c.z);
                const glm::vec3& col = kPalette[std::min(int(mat),8)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat),8)];
                v.r = uint8_t(col.r*255.f); v.g = uint8_t(col.g*255.f); v.b = uint8_t(col.b*255.f);
                v.a = 255; v.reflectivity = uint8_t(rr.x); v.roughness = uint8_t(rr.y);
                v.materialId = mat;
                out.push_back(v);
            }
    return out;
}

std::vector<VoxelRecord> EditableWorld::makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const {
    if (sizeVox.x<=0||sizeVox.y<=0||sizeVox.z<=0) return {};
    // sizeVox includes bottom-center cell: e.g., 3x3x3 => dx in [-1,1], dy 0..2, dz [-1,1]
    glm::ivec3 he(sizeVox.x/2, sizeVox.y-1, sizeVox.z/2);
    // object center for SDF: anchor + (0, he.y*VOXEL/2? Actually box half extents)
    // Box SDF centered at (0, height/2, 0) from anchor bottom
    glm::vec3 center = voxelCenter(anchor) + glm::vec3(0.f, (sizeVox.y * VOXEL)*0.5f - VOXEL*0.5f, 0.f);
    glm::vec3 half(sizeVox.x*VOXEL*0.5f, sizeVox.y*VOXEL*0.5f, sizeVox.z*VOXEL*0.5f);
    auto sdf = [&](glm::vec3 p){ return sdBoxF(p, center, half); };
    std::vector<VoxelRecord> res;
    for (int dz=-he.z; dz<=he.z + (sizeVox.z%2==0?1:0); ++dz)
        for (int dy=0; dy<sizeVox.y; ++dy)
            for (int dx=-he.x; dx<=he.x + (sizeVox.x%2==0?1:0); ++dx) {
                glm::ivec3 c(anchor.x + dx, anchor.y + dy, anchor.z + dz);
                if (c.x<0||c.y<0||c.z<0||c.x>=1024||c.y>=1024||c.z>=1024) continue;
                glm::vec3 p = voxelCenter(c);
                float d = sdf(p);
                if (std::fabs(d) > kBand) continue;
                VoxelRecord v; v.x=uint16_t(c.x); v.y=uint16_t(c.y); v.z=uint16_t(c.z);
                const glm::vec3& col = kPalette[std::min(int(mat),8)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat),8)];
                v.r=uint8_t(col.r*255.f); v.g=uint8_t(col.g*255.f); v.b=uint8_t(col.b*255.f);
                v.a=255; v.reflectivity=uint8_t(rr.x); v.roughness=uint8_t(rr.y); v.materialId=mat;
                res.push_back(v);
            }
    return res;
}

std::vector<VoxelRecord> EditableWorld::makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const {
    glm::ivec3 sizeVox(int(std::round(sizeM.x/VOXEL)), int(std::round(sizeM.y/VOXEL)), int(std::round(sizeM.z/VOXEL)));
    sizeVox = glm::max(sizeVox, glm::ivec3(1));
    return makeBox(anchor, sizeVox, mat);
}

std::vector<VoxelRecord> EditableWorld::makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const {
    radiusM = glm::max(radiusM, glm::vec3(VOXEL));
    glm::ivec3 he(int(std::ceil(radiusM.x/VOXEL)), int(std::ceil(radiusM.y/VOXEL)), int(std::ceil(radiusM.z/VOXEL)));
    glm::vec3 center = voxelCenter(anchor) + glm::vec3(0.f, radiusM.y, 0.f); // bottom at anchor top
    auto sdf = [&](glm::vec3 p){ return sdEllipsoid(p, center, radiusM); };
    std::vector<VoxelRecord> res;
    for (int dz=-he.z; dz<=he.z; ++dz)
        for (int dy=-he.y; dy<=he.y*2; ++dy) // allow full ellipsoid height
            for (int dx=-he.x; dx<=he.x; ++dx) {
                glm::ivec3 c(anchor.x + dx, anchor.y + dy, anchor.z + dz);
                if (c.x<0||c.y<0||c.z<0||c.x>=1024||c.y>=1024||c.z>=1024) continue;
                glm::vec3 p = voxelCenter(c);
                float d = sdf(p);
                if (std::fabs(d) > kBand) continue;
                VoxelRecord v; v.x=uint16_t(c.x); v.y=uint16_t(c.y); v.z=uint16_t(c.z);
                const glm::vec3& col = kPalette[std::min(int(mat),8)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat),8)];
                v.r=uint8_t(col.r*255.f); v.g=uint8_t(col.g*255.f); v.b=uint8_t(col.b*255.f);
                v.a=255; v.reflectivity=uint8_t(rr.x); v.roughness=uint8_t(rr.y); v.materialId=mat;
                res.push_back(v);
            }
    return res;
}

std::vector<VoxelRecord> EditableWorld::makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const {
    radiusM = std::max(radiusM, VOXEL*0.5f);
    heightM = std::max(heightM, VOXEL);
    int rCells = int(std::ceil(radiusM/VOXEL));
    int hCells = int(std::ceil(heightM/VOXEL));
    glm::vec3 center = voxelCenter(anchor);
    float y0 = center.y;
    float y1 = y0 + heightM;
    glm::vec2 cylCenter(center.x, center.z);
    std::vector<VoxelRecord> res;
    for (int dz=-rCells; dz<=rCells; ++dz)
        for (int dy=0; dy<hCells+2; ++dy)
            for (int dx=-rCells; dx<=rCells; ++dx) {
                glm::ivec3 c(anchor.x + dx, anchor.y + dy, anchor.z + dz);
                if (c.x<0||c.y<0||c.z<0||c.x>=1024||c.y>=1024||c.z>=1024) continue;
                glm::vec3 p = voxelCenter(c);
                float d = sdCylY(p, cylCenter, y0, y1, radiusM);
                if (std::fabs(d) > kBand) continue;
                VoxelRecord v; v.x=uint16_t(c.x); v.y=uint16_t(c.y); v.z=uint16_t(c.z);
                const glm::vec3& col = kPalette[std::min(int(mat),8)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat),8)];
                v.r=uint8_t(col.r*255.f); v.g=uint8_t(col.g*255.f); v.b=uint8_t(col.b*255.f);
                v.a=255; v.reflectivity=uint8_t(rr.x); v.roughness=uint8_t(rr.y); v.materialId=mat;
                res.push_back(v);
            }
    return res;
}

std::vector<VoxelRecord> EditableWorld::makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const {
    std::vector<VoxelRecord> res;
    res.reserve(cells.size());
    for (auto &c : cells) {
        glm::ivec3 p(anchor.x + c.dx, anchor.y + c.dy, anchor.z + c.dz);
        if (p.x<0||p.y<0||p.z<0||p.x>=1024||p.y>=1024||p.z>=1024) continue;
        const glm::vec3& col = kPalette[std::min(int(c.mat),8)];
        const glm::vec2& rr = kMaterialReflection[std::min(int(c.mat),8)];
        VoxelRecord v; v.x=uint16_t(p.x); v.y=uint16_t(p.y); v.z=uint16_t(p.z);
        v.r=uint8_t(col.r*255.f); v.g=uint8_t(col.g*255.f); v.b=uint8_t(col.b*255.f);
        v.a=255; v.reflectivity=uint8_t(rr.x); v.roughness=uint8_t(rr.y); v.materialId=c.mat;
        res.push_back(v);
    }
    // dedup exact shells not needed for stamps (assume pre-validated)
    return res;
}

// flip the ai_edits manifest entry to enabled so appended content becomes
// visible immediately (the default world starts with landscape only)
void EditableWorld::enableInManifest() const {
    std::vector<worldfile::WorldLayer> layers;
    if (!worldfile::loadManifest(manifestPath(), layers))
        return;
    bool changed = false;
    bool found = false;
    for (auto& l : layers) {
        if (l.file != kFileName)
            continue;
        found = true;
        if (!l.enabled) {
            l.enabled = true;
            changed = true;
        }
        break;
    }
    if (!found) {
        worldfile::WorldLayer nl;
        nl.file = kFileName;
        nl.name = kLayerName;
        nl.role = "object";
        nl.pos[0] = nl.pos[1] = nl.pos[2] = 0.f;
        nl.rotDeg = 0.f;
        nl.enabled = true;
        nl.listed = true;
        layers.insert(layers.begin(), nl); // highest priority
        changed = true;
    }
    if (changed)
        worldfile::writeManifest(manifestPath(), layers);
}

size_t EditableWorld::append(const std::vector<VoxelRecord>& newRecs) {
    if (newRecs.empty()) return 0;
    std::unordered_set<uint32_t> claimed;
    claimed.reserve(m_records.size()*2 + newRecs.size()*2);
    for (auto &v : m_records) claimed.insert((uint32_t(v.x)<<20)|(uint32_t(v.y)<<10)|uint32_t(v.z));
    size_t added=0;
    for (auto &v : newRecs) {
        uint32_t key=(uint32_t(v.x)<<20)|(uint32_t(v.y)<<10)|uint32_t(v.z);
        if (claimed.insert(key).second) { m_records.push_back(v); ++added; }
    }
    if (added) {
        save();
        enableInManifest();
    }
    spdlog::info("editable_world: append {} new ({} total)", added, m_records.size());
    return added;
}
size_t EditableWorld::appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) {
    auto recs = makeBox(anchor, sizeVox, mat);
    return append(recs);
}
void EditableWorld::clear() {
    m_records.clear();
    save();
    spdlog::info("editable_world: cleared");
}

}
