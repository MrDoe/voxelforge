// micro_objects.cpp - load "role":"micro" layers into SEPARATE micro SVO worlds.
//
// A micro layer is a high-detail object authored on its OWN fine lattice (a 30
// cm vase at 5 mm voxels). It is deliberately NOT merged into the world record
// set: the world lattice is fixed at 0.1 m and merging would quantise the object
// away. Instead each micro layer becomes its own SVO world (micro_svo.hpp) that
// MicroSvoPass owns and renders as a GPU overlay.
//
// What is loaded, and where it lives:
//   manifest "role":"micro" -> MicroField -> MicroWorld -> MicroSvoPass
// The world path never sees any of it: no VoxelField, no ChunkStore, no SVO
// pool, no live-edit overlay. A carve through the base world cannot disturb it,
// and it cannot be resampled onto the world lattice by accident (guarded in
// layered_world.cpp).
#include "app/app.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Load every enabled micro layer and hand the resulting worlds to the GPU pass.
// Called at startup and on every world reload (a manifest edit can add, remove
// or reposition one). Cheap: a micro layer is a few thousand cells, so this is
// milliseconds, not the world bake's seconds.
void App::loadMicroObjects()
{
    std::vector<vf::voxel::worldfile::WorldLayer> layers;
    if (!vf::voxel::worldfile::loadManifest(m_manifestPath, layers)) {
        spdlog::warn("micro: cannot read manifest {} - no micro objects",
                     m_manifestPath);
        m_microSvoPass.setMicroWorlds({});
        m_microFields.clear();
        m_microOccluders.clear();
        return;
    }
    const std::string dir = std::filesystem::path(m_manifestPath).parent_path().string();

    std::vector<glm::vec3> occluderOffsets;
    std::vector<std::string> occluderFiles;
    std::vector<vf::voxel::MicroWorld> worlds;
    m_microFields.clear();
    m_microOccluders.clear();
    // RESERVE before anything points into this vector. A push_back that
    // reallocates moves the fields, and every MicroOccluder::field pointer
    // would dangle - measured: the surfel bake's micro march read a garbage
    // `voxel` of 0 and the step became 0, so the march never advanced and a
    // --shot spun at 810 % CPU for minutes.
    m_microFields.reserve(layers.size());
    for (const auto& l : layers) {
        if (l.role != "micro" || !l.enabled)
            continue;
        const std::string path = dir + "/" + l.file;
        vf::voxel::WorldFileData data;
        if (!vf::voxel::worldfile::read(path, data)) {
            spdlog::error("micro: cannot read layer '{}'", l.file);
            continue;
        }
        // A micro layer MUST be finer than the world lattice. If it is not, it
        // is almost certainly a world object mis-tagged, and baking it here
        // would silently render it as a separate duplicate of world geometry.
        if (data.meta.voxelSize >= vf::voxel::VOXEL) {
            spdlog::error("micro: layer '{}' has voxelSize {} >= world voxel {} - "
                          "refusing (a micro layer must be finer than the world "
                          "lattice; did you mean role \"object\"?)",
                          l.file, data.meta.voxelSize, vf::voxel::VOXEL);
            continue;
        }
        // PLACEMENT. A micro layer's `pos` is the world position of the
        // object's MIN CORNER (unlike a world layer, where it is an offset of
        // the layer's centred lattice), and the rotation is about the
        // object's bottom-centre pivot - the same pivot the world uses, so
        // "yaw about the base" means the same thing in both.
        //
        // The rotation is applied to the RECORDS on the fine lattice, with a
        // destination-driven map so the rotated shape keeps its solidity.
        //
        // The pivot must stay put in world space while the shape spins about
        // it, and the rotation moves the shape's min corner - so the world
        // offset is corrected by exactly that shift. With no rotation the
        // shift is zero and the offset is `pos`, unchanged.
        std::vector<vf::voxel::VoxelRecord> placed = data.voxels;
        glm::ivec3 minShift(0);
        if (l.rotDeg != 0.0f || l.rotX != 0.0f || l.rotZ != 0.0f) {
            if (!vf::voxel::rotateMicroRecords(data.voxels, data.meta, l.rotDeg,
                                                l.rotX, l.rotZ, placed, minShift))
                placed = data.voxels; // nothing to rotate (empty layer)
        }
        vf::voxel::MicroField field;
        std::string err;
        if (!vf::voxel::buildMicroField(placed, data.meta, field, &err)) {
            spdlog::error("micro: layer '{}': {}", l.file, err);
            continue;
        }
        // Placement. A micro layer's records live in its OWN coordinate system,
        // so `pos` is the world position of the object's local min corner
        // (unlike an object layer, where it offsets already-placed lattice
        // records). Default placement sits the base on the terrain so a dropped
        // -in vase is not floating.
        glm::vec3 offset { l.pos[0], l.pos[1], l.pos[2] };
        // Keep the pivot fixed: the rotation moved the shape's min corner by
        // minShift cells, so the world offset moves with it.
        offset += glm::vec3(minShift) * data.meta.voxelSize;
        if (l.pos[0] == 0.0f && l.pos[1] == 0.0f && l.pos[2] == 0.0f) {
            const float h =
                vf::voxel::sharedHeightmap().sample(offset.x + 0.5f * field.extent().x,
                                                     offset.z + 0.5f * field.extent().z);
            offset.y = h;
        }
        vf::voxel::MicroWorld world;
        vf::voxel::synthesizeMicroSvo(field, offset, world);
        // Keep the field so the world's BAKED surfel shadow can cast onto the
        // world from it (see MicroOccluder). The AABB is the field's own
        // extent, which already includes the air margin - a ray that misses it
        // cannot touch the object.
        m_microFields.push_back(field);
        occluderOffsets.push_back(offset);
        occluderFiles.push_back(l.file);
        spdlog::info("micro: '{}' -> {} voxels on a {:.3f} m lattice ({}x{}x{} cells), "
                     "gridN {}, {} bricks, {:.1f} KB",
                     l.file, data.voxels.size(), field.voxel, field.dim.x, field.dim.y,
                     field.dim.z, world.gridN, world.brickCount(),
                     double(world.bytes()) / 1024.0);
        worlds.push_back(std::move(world));
    }
    // Second pass: NOW that no more fields can move, point the occluders at
    // them. See the reserve above.
    for (size_t i = 0; i < m_microFields.size(); ++i) {
        vf::voxel::MicroOccluder oc;
        oc.field = &m_microFields[i];
        oc.worldOffset = occluderOffsets[i];
        oc.layerFile = occluderFiles[i];
        oc.aabbMin = m_microFields[i].origin + occluderOffsets[i];
        oc.aabbMax = oc.aabbMin + m_microFields[i].extent();
        m_microOccluders.push_back(oc);
    }
    m_microSvoPass.setMicroWorlds(worlds);
}

App::MicroObjectPick
App::pickMicroObjects(const std::vector<vf::voxel::MicroOccluder>& occ, glm::vec3 ro,
                      glm::vec3 rd) const
{
    MicroObjectPick best;
    for (const vf::voxel::MicroOccluder& o : occ) {
        if (!o.field)
            continue;
        // The AABB test first: a ray that misses the box never touches the
        // field, so the cost is bounded by the objects near the pointer.
        const glm::vec3 inv = glm::vec3(1.0f) / rd;
        const glm::vec3 t0 = (o.aabbMin - ro) * inv;
        const glm::vec3 t1 = (o.aabbMax - ro) * inv;
        const glm::vec3 tlo = glm::min(t0, t1), thi = glm::max(t0, t1);
        const float tEnter = glm::max(glm::max(tlo.x, tlo.y), tlo.z);
        const float tExit = glm::min(glm::min(thi.x, thi.y), thi.z);
        if (tExit <= glm::max(tEnter, 0.f))
            continue;
        const vf::voxel::MicroPick p =
            vf::voxel::pickMicroField(*o.field, o.worldOffset, ro, rd);
        if (!p.hit)
            continue;
        if (best.hit && p.dist >= best.dist)
            continue;
        best.hit = true;
        best.dist = p.dist;
        best.pos = p.pos;
        best.normal = p.normal;
        best.cell = p.cell;
        best.file = o.layerFile;
    }
    return best;
}

vf::voxel::WorldAABB App::layerBounds(const std::string& file) const
{
    // A MICRO layer is not in the merged world, so its box comes from its own
    // field: the object's cells on its fine lattice, placed in the world.
    if (isMicroLayer(file)) {
        for (const vf::voxel::MicroOccluder& o : m_microOccluders) {
            if (o.layerFile != file || !o.field)
                continue;
            const glm::vec3 lo = o.field->origin + o.worldOffset;
            const glm::vec3 hi = lo + o.field->extent();
            return vf::voxel::WorldAABB { lo, hi };
        }
        return {};
    }
    return m_layers.layerBox(file);
}

int App::microWorldIndex(const std::string& file) const
{
    for (size_t i = 0; i < m_microOccluders.size(); ++i)
        if (m_microOccluders[i].layerFile == file)
            return int(i);
    return -1;
}

bool App::layerPivotWorld(const std::string& file, glm::vec3& pivot) const
{
    // A MICRO layer has no entry in the merged world, so the world pivot
    // lookup cannot know it. Its pivot is the BOTTOM-CENTRE of its own
    // records - the same convention the manifest's rotation uses (the
    // centre in x/z, the base in y), so "yaw about the base" means the same
    // thing for a micro object as for a world object.
    if (!isMicroLayer(file))
        return m_layers.layerPivot(file, pivot);
    for (const vf::voxel::MicroOccluder& o : m_microOccluders) {
        if (o.layerFile != file || !o.field)
            continue;
        // The field's grid starts kMicroMarginCells BEFORE the object's first
        // cell, so the records' min corner is the grid's origin plus the
        // margin, and the records' extent is the grid's extent minus it.
        const float M = float(vf::voxel::kMicroMarginCells);
        const glm::vec3 minCorner =
            o.field->origin + o.worldOffset + glm::vec3(M) * o.field->voxel;
        const glm::vec3 objDim = glm::vec3(o.field->dim) - 2.0f * M;
        pivot = minCorner + glm::vec3(0.5f * objDim.x * o.field->voxel, 0.0f,
                                      0.5f * objDim.z * o.field->voxel);
        return true;
    }
    return false;
}

void App::updateMicroMovePreview()
{
    // The preview offset belongs to the SELECTED micro object while it is
    // being dragged. Anything else clears it.
    const int idx = (m_moveLayer.empty() || !isMicroLayer(m_moveLayer))
                         ? -1
                         : microWorldIndex(m_moveLayer);
    if (idx < 0) {
        for (size_t i = 0; i < m_microOccluders.size(); ++i)
            m_microSvoPass.setWorldPreview(i, glm::vec3(0.f), false);
        return;
    }
    m_microSvoPass.setWorldPreview(size_t(idx), m_moveDelta, true);
}

void App::clearMicroRotatePreview()
{
    // The live rotation preview lives in the micro pass, because a micro
    // object has no world owner id and the splat pass's rotate preview is
    // keyed by exactly that. Clearing it is a no-op when no micro world is
    // loaded.
    for (size_t i = 0; i < m_microOccluders.size(); ++i)
        m_microSvoPass.setWorldRotation(i, glm::mat3(1.0f), glm::vec3(0.0f),
                                         false);
}

} // namespace app
} // namespace vf