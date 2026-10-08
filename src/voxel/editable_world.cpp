#define GLM_ENABLE_EXPERIMENTAL
#include "voxel/editable_world.hpp"
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/picking.hpp"
#include <glm/gtx/quaternion.hpp>
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <cmath>
#include <algorithm>

namespace vf::voxel {

EditableWorld::EditableWorld(std::string assetDir, std::string fileName,
                               std::string layerName, std::string role)
    : m_assetDir(std::move(assetDir)), m_fileName(std::move(fileName)),
      m_layerName(std::move(layerName)), m_role(std::move(role)) {}

std::string EditableWorld::filePath() const { return m_assetDir + "/" + m_fileName; }
std::string EditableWorld::manifestPath() const { return m_assetDir + "/world.json"; }
WorldFileMeta EditableWorld::meta() const { return {WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8}; }

std::string EditableWorld::sanitizeLayerName(const std::string& name)
{
    std::string s;
    for (char c : name) {
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-')
            s += c;
    }
    if (s.empty() || s.size() > 40)
        return "";
    return s;
}

// Insert an object layer entry (or enable it if already present) before the
// landscape layer so AI-authored content wins cell collisions over terrain.
// When replacing an existing entry, leave pos/rot/rotX/rotZ untouched: those
// fields are the authored world placement/orientation, not mesh-import data.
static void upsertObjectLayer(std::vector<worldfile::WorldLayer>& layers,
                              const std::string& file, const std::string& name)
{
    for (auto& l : layers) {
        if (l.file == file) {
            l.enabled = true;
            l.role = "object";
            l.name = name;
            l.listed = true;
            return;
        }
    }
    worldfile::WorldLayer nl;
    nl.file = file;
    nl.name = name;
    nl.role = "object";
    nl.enabled = true;
    nl.listed = true;
    nl.pos[0] = nl.pos[1] = nl.pos[2] = 0.f;
    nl.rotDeg = 0.f;
    size_t insertAt = 0;
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].role == "landscape") {
            insertAt = i;
            break;
        }
    layers.insert(layers.begin() + insertAt, nl);
}

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
    for (auto &l : layers) if (l.file == m_fileName) return true;
    worldfile::WorldLayer nl;
    nl.file = m_fileName;
    nl.role = m_role;
    nl.name = m_layerName;
    nl.pos[0]=0.f; nl.pos[1]=0.f; nl.pos[2]=0.f;
    nl.rotDeg = 0.f;
    // presence only: starts DISABLED so a pristine valley stays pristine;
    // enableInManifest() flips it when content actually lands
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
    // The box SDF is centred on the anchor cell centre with half = size*VOXEL/2,
    // so the true d<=0 cell set is exactly -he..+he on X/Z: an ODD size covers
    // he+he+1 == size cells (3 -> -1..1), an EVEN size covers size+1 cells
    // (2 -> -1..1) because both end cells sit exactly on the surface (d==0).
    // The old bound ran to he+1 for even sizes: that extra column lies at
    // d=+VOXEL, still inside kBand, so it was emitted as a phantom shell slab
    // hanging off +X/+Z - every even-sized box (most thin posts and rails) was
    // one cell too big and lopsided. Shrinking to he-1 is equally wrong: it
    // drops a real d==0 boundary cell and skews the box the other way.
    for (int dz = -he.z; dz <= he.z; ++dz)
        for (int dy=0; dy<sizeVox.y; ++dy)
            for (int dx = -he.x; dx <= he.x; ++dx) {
                glm::ivec3 c(anchor.x + dx, anchor.y + dy, anchor.z + dz);
                if (c.x<0||c.y<0||c.z<0||c.x>=1024||c.y>=1024||c.z>=1024) continue;
                glm::vec3 p = voxelCenter(c);
                float d = sdf(p);
                if (std::fabs(d) > kBand) continue;
                VoxelRecord v; v.x=uint16_t(c.x); v.y=uint16_t(c.y); v.z=uint16_t(c.z);
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
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
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
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
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
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
        const glm::vec3& col = kPalette[std::min(int(c.mat), kPaletteN - 1)];
        const glm::vec2& rr = kMaterialReflection[std::min(int(c.mat), kPaletteN - 1)];
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
        if (l.file != m_fileName)
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
        nl.file = m_fileName;
        nl.name = m_layerName;
        nl.role = m_role;
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

size_t EditableWorld::importLayer(const std::string& vxwPath, glm::ivec3 anchor) {
    WorldFileData d;
    if (!worldfile::read(vxwPath, d)) {
        spdlog::warn("editable_world: importLayer cannot read {}", vxwPath);
        return 0;
    }
    const WorldFileMeta want = meta();
    if (d.voxels.empty()) {
        spdlog::warn("editable_world: importLayer {} empty", vxwPath);
        return 0;
    }
    if (d.meta.worldSize != want.worldSize || d.meta.voxelSize != want.voxelSize ||
        d.meta.gridN != want.gridN) {
        // fine/coarse-authored source: resample onto the world lattice
        std::vector<VoxelRecord> normalized;
        worldfile::resampleRecords(d.voxels, d.meta, want, 1.0f, normalized);
        if (normalized.empty()) {
            spdlog::warn("editable_world: importLayer {} resample produced nothing", vxwPath);
            return 0;
        }
        d.voxels = std::move(normalized);
    }
    // source AABB -> bottom-center origin in lattice coords (x/z centred,
    // y at the lowest record), matching the picking bottom-center contract
    glm::ivec3 mn(1 << 30), mx(-(1 << 30));
    for (const auto& v : d.voxels) {
        mn = glm::min(mn, glm::ivec3(v.x, v.y, v.z));
        mx = glm::max(mx, glm::ivec3(v.x, v.y, v.z));
    }
    glm::ivec3 delta = anchor - glm::ivec3((mn.x + mx.x) / 2, mn.y, (mn.z + mx.z) / 2);
    std::vector<VoxelRecord> moved;
    moved.reserve(d.voxels.size());
    for (const auto& v : d.voxels) {
        glm::ivec3 c(int(v.x) + delta.x, int(v.y) + delta.y, int(v.z) + delta.z);
        if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= 1024 || c.y >= 1024 || c.z >= 1024)
            continue;
        VoxelRecord t = v;
        t.x = uint16_t(c.x); t.y = uint16_t(c.y); t.z = uint16_t(c.z);
        moved.push_back(t);
    }
    return append(moved);
}
void EditableWorld::clear() {
    m_records.clear();
    save();
    spdlog::info("editable_world: cleared");
}

bool EditableWorld::writeObjectLayer(const std::string& name,
                                     const std::vector<VoxelRecord>& recs)
{
    std::string safe = sanitizeLayerName(name);
    if (safe.empty())
        return false;
    const std::string file = safe + ".vxw";
    WorldFileData d;
    d.meta = meta();
    d.voxels = recs;
    if (!worldfile::write(m_assetDir + "/" + file, d)) {
        spdlog::error("editable_world: failed to write object layer {}", file);
        return false;
    }
    std::vector<worldfile::WorldLayer> layers;
    worldfile::loadManifest(manifestPath(), layers);
    upsertObjectLayer(layers, file, safe);
    if (!worldfile::writeManifest(manifestPath(), layers)) {
        spdlog::error("editable_world: failed to update manifest for {}", file);
        return false;
    }
    spdlog::info("editable_world: wrote object layer {} ({} voxels)", file, recs.size());
    return true;
}

bool EditableWorld::deleteObjectLayer(const std::string& name)
{
    std::string safe = sanitizeLayerName(name);
    if (safe.empty())
        return false;
    const std::string file = safe + ".vxw";
    std::vector<worldfile::WorldLayer> layers;
    if (!worldfile::loadManifest(manifestPath(), layers))
        return false;
    bool found = false;
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        if (it->file != file)
            continue;
        if (it->role == "landscape" || it->role == "packed")
            return false; // protected: never delete core layers
        layers.erase(it);
        found = true;
        break;
    }
    if (!found)
        return false;
    worldfile::writeManifest(manifestPath(), layers);
    std::remove((m_assetDir + "/" + file).c_str());
    spdlog::info("editable_world: deleted object layer {}", file);
    return true;
}

std::vector<VoxelRecord> EditableWorld::makeOrientedCylinder(
    glm::ivec3 anchor, glm::vec3 axisDir, float radiusM, float lengthM, uint8_t mat, bool carve,
    FalloffCurve curve) const
{
    axisDir = glm::normalize(axisDir);
    radiusM = std::max(radiusM, VOXEL * 0.5f);
    lengthM = std::max(lengthM, VOXEL);

    // rotation mapping local +Y -> world axisDir; Rt brings a world offset into
    // the cylinder's local frame (axis = +Y) for the SDF test.
    const glm::mat3 R = glm::mat3(glm::rotation(glm::vec3(0.f, 1.f, 0.f), axisDir));
    const glm::mat3 Rt = glm::transpose(R);

    // Carve scoops open the ground they start at (kCarveTopMargin above the
    // base plane); see the note on the constant. Add keeps the exact
    // [0, length] extent: its shell cap must not poke above the surface.
    const float sdfY0 = carve ? -kCarveTopMargin : 0.f;

    const glm::vec3 base = voxelCenter(anchor);
    const glm::vec3 center = base + axisDir * (lengthM * 0.5f);
    const float r = radiusM;
    // world-space AABB of the oriented cylinder
    const float axial = lengthM * 0.5f + kCarveTopMargin;
    const glm::vec3 half(r + axial * std::abs(axisDir.x),
                         r + axial * std::abs(axisDir.y),
                         r + axial * std::abs(axisDir.z));
    const glm::vec3 lo = center - half, hi = center + half;

    const int N = int(WORLD / VOXEL);
    auto toCell = [&](float w) { return int(std::floor((w + 0.5f * WORLD) / VOXEL)); };
    const int x0 = std::max(0, toCell(lo.x)), x1 = std::min(N - 1, toCell(hi.x));
    const int y0 = std::max(0, toCell(lo.y)), y1 = std::min(N - 1, toCell(hi.y));
    const int z0 = std::max(0, toCell(lo.z)), z1 = std::min(N - 1, toCell(hi.z));

    std::vector<VoxelRecord> res;
    res.reserve(size_t((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1)) / 4);
    const glm::vec2 c(0.f, 0.f);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec3 p = voxelCenter(glm::ivec3(x, y, z));
                const glm::vec3 local = Rt * (p - base);
                const float d = sdCylY(local, c, sdfY0, lengthM, radiusM);
                // carve: emit the solid removed volume; add: emit a thin shell
                // band. The carve test keeps boundary cells (d == 0), e.g. the
                // anchor cell on the base plane: glm::rotation(up, axisDir) is
                // degenerate for an axis exactly opposite to up (it picks an
                // arbitrary perpendicular axis, and the resulting basis is
                // tilted by ~1e-7), so an exact `d > 0` test drops half of the
                // base-plane layer - the scoop would keep a one-cell lid above
                // half its disk and never flood. The epsilon is 0.001 voxel.
                if (carve ? (d > VOXEL * 1e-3f) : (std::fabs(d) > kBand))
                    continue;
                // Radial falloff feathers the scoop's FAR end per rim column, so
                // the dig is deepest under the cursor and tapers out at its edge
                // instead of leaving a vertical crater wall. TWO HAZARDS, both
                // load-bearing: (1) the near end is NOT tapered - sdfY0 above is
                // kCarveTopMargin, the margin that opens the ground the scoop
                // starts at, and fading it puts the one-cell roof back over the
                // dig (a covered void gains no water); a rim column still cuts
                // one cell, so the footprint boundary stays crisp. (2) Only the
                // carve is graded; the Add path is a shell band and takes its
                // taper from makeDome's height-per-column rule instead. Constant
                // is the legacy volume exactly: f == 1 maps far onto lengthM.
                if (carve && curve != FalloffCurve::Constant) {
                    const float perp = std::sqrt(local.x * local.x + local.z * local.z);
                    const float f = falloffCurveAt(curve, perp / std::max(radiusM, 1e-6f));
                    // A rim column (q = 1, f = 0 for every tapered curve) must
                    // still open the ground the scoop starts at, so the reach is
                    // floored at the top margin plus one cell - without that floor
                    // the taper reached exactly sdfY0, the rim cells at the margin
                    // boundary fell out on the strict comparison, and measured 2814
                    // of the 2821 cells the test expects. The epsilon matches the
                    // SDF test's own, so the last cell of a full-depth column is
                    // kept whichever side fp lands on.
                    const float reach =
                        std::max((lengthM - sdfY0) * f, kCarveTopMargin + VOXEL);
                    if (local.y > sdfY0 + reach + VOXEL * 1e-3f)
                        continue;
                }
                VoxelRecord v;
                v.x = uint16_t(x); v.y = uint16_t(y); v.z = uint16_t(z);
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
                v.r = uint8_t(col.r * 255.f); v.g = uint8_t(col.g * 255.f); v.b = uint8_t(col.b * 255.f);
                v.a = 255;
                v.reflectivity = uint8_t(rr.x);
                v.roughness = uint8_t(rr.y);
                v.materialId = mat;
                res.push_back(v);
            }
    return res;
}

std::vector<VoxelRecord> EditableWorld::makeDome(glm::ivec3 anchor, glm::vec3 axisDir,
                                                 float radiusM, float heightM, uint8_t mat,
                                                 FalloffCurve curve) const
{
    std::vector<VoxelRecord> res;
    if (radiusM <= 1e-3f || heightM <= 1e-3f)
        return res;
    axisDir = glm::normalize(axisDir);
    radiusM = std::max(radiusM, VOXEL * 0.5f);
    heightM = std::max(heightM, VOXEL * 0.5f);

    // rotation mapping local +Y -> world axisDir; Rt brings a world offset into
    // the dome's local frame (axis = +Y) for the profile test below.
    const glm::mat3 R = glm::mat3(glm::rotation(glm::vec3(0.f, 1.f, 0.f), axisDir));
    const glm::mat3 Rt = glm::transpose(R);

    // The volume grows OUT of the surface along +axisDir: the brush footprint
    // is a disk of radius `radiusM` in the plane across the axis, extruded
    // `heightM` along it and closed by a fillet of radius c = min(radiusM,
    // heightM) / 2, so the new face is flat over a radius of (radiusM - c).
    // Clicking a wall therefore thickens it by the full heightM over its whole
    // footprint and only rounds the outer edge - a dome that tapers to nothing
    // at the rim would bulge the middle instead.
    // The cross-section is a straight side for the first (heightM - c) and a
    // quarter round after that. The two branches of that rule AGREE at
    // t == lipY (both give radiusM), so a 1-ulp sign flip at the switch plane
    // cannot drop half the footprint - unlike a branch on the cap plane, which
    // is exactly the degenerate glm::rotation(up, -up) trap the carve cylinder
    // documents. c and lipY are now computed PER COLUMN in the loop (the curve
    // grades the height, so the fillet radius moves with it); for Constant they
    // come out as these two constants did, which is what keeps the legacy shape
    // exact.
    const float eps = VOXEL;

    // World AABB of that volume: +-radiusM across the axis, [0, heightM] along
    // it. The reach MUST be projected on the axis. The pre-2026-09 version of
    // this loop spanned the height on world Y (x/z got +-max(radius, height)),
    // which is only correct for an up-facing surface: on a wall it clipped the
    // footprint to y >= base.y, so the brush covered nothing below the picked
    // cell (measured: 0 of 4612 cells, vs 2148 now) and produced a bulge
    // instead of a thicker wall.
    const glm::vec3 base = voxelCenter(anchor);
    const glm::vec3 half(radiusM + heightM * std::abs(axisDir.x),
                         radiusM + heightM * std::abs(axisDir.y),
                         radiusM + heightM * std::abs(axisDir.z));
    const int N = int(WORLD / VOXEL);
    auto toCell = [&](float w) { return int(std::floor((w + 0.5f * WORLD) / VOXEL)); };
    // +1 cell of slack on every side: the box edge lands exactly ON the
    // footprint's boundary cell, and toCell() floors, so a sum that comes out
    // one ulp low silently drops that outer ring (the shape test rejects the
    // extra cells, the loop just has to visit them).
    const int x0 = std::max(0, toCell(base.x - half.x) - 1), x1 = std::min(N - 1, toCell(base.x + half.x) + 1);
    const int y0 = std::max(0, toCell(base.y - half.y) - 1), y1 = std::min(N - 1, toCell(base.y + half.y) + 1);
    const int z0 = std::max(0, toCell(base.z - half.z) - 1), z1 = std::min(N - 1, toCell(base.z + half.z) + 1);

    // boundary cells are kept (the footprint edge, the fillet): an exact test
    // would drop the rim of the disk wherever fp lands on the wrong side.
    const float keep = VOXEL * 1e-3f;
    const float r2 = radiusM * radiusM;
    // Radial falloff scales the growth HEIGHT per column and the fillet shrinks
    // with it, so the stamp is a mound - full heightM under the cursor, nothing
    // at the rim - instead of the legacy full-height rim. The hover preview
    // repeats exactly this profile (sharedInBrushVolume's dome branch), so the
    // tint marks the tapered growth and not the full-depth one; the two must
    // agree curve for curve or the preview lies. Constant is NOT a flat top
    // being "restored": it is the pre-curve shape, which is what every existing
    // stamp and test expects.
    const bool grade = curve != FalloffCurve::Constant;
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec3 p = voxelCenter(glm::ivec3(x, y, z));
                const glm::vec3 local = Rt * (p - base);
                const float t = local.y;
                if (t < -eps)
                    continue; // nothing behind the surface
                const float perp2 = local.x * local.x + local.z * local.z;
                // Per-column height and fillet radius for this curve.
                const float colHeight = grade
                                            ? heightM * falloffCurveAt(
                                                  curve, std::sqrt(perp2) / std::max(radiusM, 1e-6f))
                                            : heightM;
                // A column the taper grades to nothing emits nothing. Without
                // this the straight-disk branch below still tests the UNGRADED
                // perp2 <= r2, so the rim ring (q = 1, f = 0 for every tapered
                // curve) keeps its base cell and the mark never stops short of
                // the nominal width - measured as rim reach 1 where the test
                // pins "nothing at the rim". Gated on grade, so Constant keeps
                // the legacy boundary behaviour bit for bit.
                if (grade && colHeight < 0.5f * VOXEL)
                    continue;
                const float cf = 0.5f * std::min(radiusM, colHeight);
                const float lipYc = colHeight - cf;
                bool inside;
                if (t <= lipYc) {
                    inside = perp2 <= r2 + keep; // straight extruded disk
                } else {
                    const float k = t - lipYc;
                    if (k > cf + keep) {
                        inside = false; // past the top of the growth
                    } else {
                        const float rr = radiusM - cf +
                                         std::sqrt(std::max(0.f, cf * cf - k * k));
                        inside = perp2 <= rr * rr + keep; // rounded lip
                    }
                }
                if (!inside)
                    continue;
                VoxelRecord rec;
                rec.x = uint16_t(x); rec.y = uint16_t(y); rec.z = uint16_t(z);
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
                rec.r = uint8_t(col.r * 255.f); rec.g = uint8_t(col.g * 255.f); rec.b = uint8_t(col.b * 255.f);
                rec.a = 255;
                rec.reflectivity = uint8_t(rr.x);
                rec.roughness = uint8_t(rr.y);
                rec.materialId = mat;
                res.push_back(rec);
            }
    return res;
}

std::vector<VoxelRecord> EditableWorld::makeSingleVoxel(glm::ivec3 anchor,
                                                        glm::vec3 axisDir, uint8_t mat,
                                                        bool stepAlongNormal) const
{
    if (stepAlongNormal) {
        // "Add" a single voxel: the pick always lands on solid material, so
        // adding the picked cell would be a no-op. Step one cell along the
        // DOMINANT axis of the normal instead (round(normal / VOXEL) is wrong
        // for a smoothed corner normal like (0.7, 0.7, 0)), which stacks a
        // cube on a floor and hangs one off a wall, like the volume brush.
        if (glm::length(axisDir) < 1e-6f)
            axisDir = glm::vec3(0.f, 1.f, 0.f);
        const glm::vec3 a = glm::abs(glm::normalize(axisDir));
        const int ax = (a.y > a.x && a.y >= a.z) ? 1 : (a.x > a.z ? 0 : 2);
        glm::vec3 step(0.f);
        step[ax] = (glm::abs(axisDir[ax]) > 1e-6f &&
                    axisDir[ax] < 0.f) ? -1.f : 1.f;
        anchor += glm::ivec3(glm::round(step));
    }
    const int N = int(WORLD / VOXEL);
    if (anchor.x < 0 || anchor.y < 0 || anchor.z < 0 ||
        anchor.x >= N || anchor.y >= N || anchor.z >= N)
        return {}; // stepped out of the lattice
    return { makeVoxelRecord(anchor.x, anchor.y, anchor.z, mat) };
}

std::vector<VoxelRecord> EditableWorld::makeSphere(glm::ivec3 anchor, float radiusM,
                                                   uint8_t mat,
                                                   FalloffCurve curve) const
{
    radiusM = std::max(radiusM, VOXEL * 0.5f);
    const glm::vec3 c = voxelCenter(anchor);
    const int N = int(WORLD / VOXEL);
    auto toCell = [&](float w) { return int(std::floor((w + 0.5f * WORLD) / VOXEL)); };
    const int x0 = std::max(0, toCell(c.x - radiusM)), x1 = std::min(N - 1, toCell(c.x + radiusM));
    const int y0 = std::max(0, toCell(c.y - radiusM)), y1 = std::min(N - 1, toCell(c.y + radiusM));
    const int z0 = std::max(0, toCell(c.z - radiusM)), z1 = std::min(N - 1, toCell(c.z + radiusM));

    std::vector<VoxelRecord> res;
    const float r2 = radiusM * radiusM;
    // Radial falloff grades the ball's radius per cell, exactly as the hover
    // preview does (sharedInBrushVolume's ball branch, common_surfel.glsl):
    // a cell counts when dist <= radiusM * f(dist/radiusM), so Constant is the
    // original hard sphere (f == 1) and the tapered curves cut a graded crater
    // instead of a hard-edged ball. Branched so Constant keeps the legacy test
    // bit for bit rather than through an algebraically equal one.
    const bool grade = curve != FalloffCurve::Constant;
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec3 p = voxelCenter(glm::ivec3(x, y, z));
                const glm::vec3 d = p - c;
                if (grade) {
                    const float dist = glm::length(d);
                    const float q = dist / std::max(radiusM, 1e-6f);
                    if (dist > radiusM * falloffCurveAt(curve, q))
                        continue;
                } else if (glm::dot(d, d) > r2) {
                    continue;
                }
                VoxelRecord rec;
                rec.x = uint16_t(x); rec.y = uint16_t(y); rec.z = uint16_t(z);
                const glm::vec3& col = kPalette[std::min(int(mat), kPaletteN - 1)];
                const glm::vec2& rr = kMaterialReflection[std::min(int(mat), kPaletteN - 1)];
                rec.r = uint8_t(col.r * 255.f); rec.g = uint8_t(col.g * 255.f); rec.b = uint8_t(col.b * 255.f);
                rec.a = 255;
                rec.reflectivity = uint8_t(rr.x);
                rec.roughness = uint8_t(rr.y);
                rec.materialId = mat;
                res.push_back(rec);
            }
    return res;
}

}
