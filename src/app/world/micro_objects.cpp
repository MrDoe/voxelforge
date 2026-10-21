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

namespace {

// The rig's cache path: the layer's file with its extension replaced
// ("micro/alpaca.vxw" -> "micro/alpaca.rig").
std::string rigCachePath(const std::string& dir, const std::string& file)
{
    std::string stem = file;
    const size_t slash = stem.find_last_of('/');
    const std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        stem = stem.substr(0, stem.size() - (name.size() - dot));
    return dir + "/" + stem + ".rig";
}

// A file's modification time as a number, so the rig cache can tell whether
// the layer it was derived from has changed.
uint64_t fileStamp(const std::string& path)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec)
        return 0;
    return uint64_t(t.time_since_epoch().count());
}

} // namespace


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
    std::vector<std::string> behaviourFiles;
    std::vector<vf::voxel::MicroRig> microRigs;
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
        // The SKELETON. It is DERIVED from the object's own voxels, so a
        // newly imported model animates too - nothing here knows what an
        // alpaca is. Deriving it is a pass over the voxels, so the result is
        // CACHED next to the layer and read back on every later load; the
        // cache carries the layer's signature, so an edited or re-imported
        // layer invalidates it.
        {
            vf::voxel::MicroRig rig;
            const std::string rigPath = rigCachePath(dir, l.file);
            std::error_code ec;
            const uint64_t lsize = std::filesystem::file_size(path, ec);
            const uint64_t lmtime = fileStamp(path);
            if (!vf::voxel::readMicroRigCache(rigPath, rig, lsize, lmtime)) {
                if (vf::voxel::deriveMicroRig(field, rig)) {
                    vf::voxel::writeMicroRigCache(rigPath, rig, lsize, lmtime);
                    spdlog::info("micro: derived a rig for '{}' ({} bones, "
                                 "{} legs) and cached it at {}",
                                 l.file, rig.bones.size(), rig.legCount,
                                 rigPath);
                }
            }
            // The rig's points come from the FIELD's frame (the cells the
            // object is made of), but the shader looks the bones up in the
            // object's ANCHORED frame - the field's frame plus the
            // placement. Shifting them here keeps the two in step; the cache
            // keeps the field-frame rig, so the shift is applied after it is
            // read either way.
            for (auto& b : rig.bones) {
                b.a += offset;
                b.b += offset;
            }
            world.rig = rig;
            microRigs.push_back(rig);
        }
        // Keep the field so the world's BAKED surfel shadow can cast onto the
        // world from it (see MicroOccluder). The AABB is the field's own
        // extent, which already includes the air margin - a ray that misses it
        // cannot touch the object.
        m_microFields.push_back(field);
        occluderOffsets.push_back(offset);
        occluderFiles.push_back(l.file);
        behaviourFiles.push_back(l.behaviour);
        spdlog::info("micro: '{}' -> {} voxels on a {:.3f} m lattice ({}x{}x{} cells), "
                     "gridN {}, {} bricks, {:.1f} KB",
                     l.file, data.voxels.size(), field.voxel, field.dim.x, field.dim.y,
                     field.dim.z, world.gridN, world.brickCount(),
                     double(world.bytes()) / 1024.0);
        worlds.push_back(std::move(world));
    }
    // Second pass: NOW that no more fields can move, point the occluders at
    // them. See the reserve above.
    m_microBehaviours.clear();
    for (size_t i = 0; i < m_microFields.size(); ++i) {
        vf::voxel::MicroOccluder oc;
        oc.field = &m_microFields[i];
        oc.worldOffset = occluderOffsets[i];
        oc.layerFile = occluderFiles[i];
        oc.aabbMin = m_microFields[i].origin + occluderOffsets[i];
        oc.aabbMax = oc.aabbMin + m_microFields[i].extent();
        m_microOccluders.push_back(oc);
        // A layer can ask for an AUTONOMOUS BEHAVIOUR: it walks on its own
        // and stops to graze. The behaviour is a transform, not a re-bake, so
        // the object keeps its exact geometry while it moves.
        vf::voxel::MicroBehaviour b;
        const bool wants = behaviourFiles[i] == "grazer";
        b.active = wants;
        if (wants) {
            b.reset(oc.aabbMin, float(i) * 7.31f + 1.7f);
            b.pivot = layerPivotWorld(oc.layerFile, b.pivot) ? b.pivot
                                                             : oc.aabbMin;
            // The rig the gait drives: the world that was just built carries
            // it (derived from the object's own voxels).
            if (i < microRigs.size())
                b.rig = microRigs[i];
        }
        m_microBehaviours.push_back(b);
    }
    m_microSvoPass.setMicroWorlds(worlds);
}

App::MicroObjectPick
App::pickMicroObjects(const std::vector<vf::voxel::MicroOccluder>& occ, glm::vec3 ro,
                      glm::vec3 rd) const
{
    MicroObjectPick best;
    for (size_t i = 0; i < occ.size(); ++i) {
        const vf::voxel::MicroOccluder& o = occ[i];
        if (!o.field)
            continue;
        // A MOVING object is picked where it IS: the ray has to be
        // transformed into the object's own frame first, or the pick hits
        // where the object was baked (and, once it has walked, nowhere at
        // all).
        glm::vec3 lro = ro, lrd = rd;
        if (i < m_microBehaviours.size() && m_microBehaviours[i].active) {
            lro = m_microBehaviours[i].toLocal(ro);
            lrd = m_microBehaviours[i].dirToLocal(rd);
        }
        // The AABB test first: a ray that misses the box never touches the
        // field, so the cost is bounded by the objects near the pointer.
        // The box is the object's BAKED box, so the test is done on the
        // transformed ray.
        const glm::vec3 inv = glm::vec3(1.0f) / lrd;
        const glm::vec3 t0 = (o.aabbMin - lro) * inv;
        const glm::vec3 t1 = (o.aabbMax - lro) * inv;
        const glm::vec3 tlo = glm::min(t0, t1), thi = glm::max(t0, t1);
        const float tEnter = glm::max(glm::max(tlo.x, tlo.y), tlo.z);
        const float tExit = glm::min(glm::min(thi.x, thi.y), thi.z);
        if (tExit <= glm::max(tEnter, 0.f))
            continue;
        const vf::voxel::MicroPick p =
            vf::voxel::pickMicroField(*o.field, o.worldOffset, lro, lrd);
        if (!p.hit)
            continue;
        if (best.hit && p.dist >= best.dist)
            continue;
        best.hit = true;
        best.dist = p.dist;
        // The hit is in the object's own frame; a moving object is at its
        // ANIMATED place in the world.
        best.pos = (i < m_microBehaviours.size() && m_microBehaviours[i].active)
                       ? m_microBehaviours[i].toWorld(p.pos)
                       : p.pos;
        best.normal = (i < m_microBehaviours.size() && m_microBehaviours[i].active)
                          ? m_microBehaviours[i].rot * p.normal
                          : p.normal;
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

bool App::setMicroBehaviour(const std::string& file, const std::string& command,
                             float speed)
{
    // Tell a micro object what to do: "run", "walk", "graze", "stop". It is
    // how the AI drives the animal ("let the alpaca run") and how a user
    // would do it from a panel.
    //
    // The file may be a manifest file ("micro/alpaca.vxw") or a layer NAME
    // ("alpaca") - the assistant knows the name, the app knows the file.
    for (size_t i = 0; i < m_microBehaviours.size(); ++i) {
        const std::string& layerFile = m_microOccluders[i].layerFile;
        const std::string stem = layerFile.substr(layerFile.find_last_of('/') + 1);
        const std::string name = stem.substr(0, stem.find_last_of('.'));
        if (layerFile != file && name != file)
            continue;
        vf::voxel::MicroBehaviour& b = m_microBehaviours[i];
        if (command == "run") {
            b.command = vf::voxel::MicroBehaviour::Command::Walk;
            b.speed = std::max(0.05f, speed > 0.f ? speed : b.speed * 2.5f);
        } else if (command == "walk") {
            b.command = vf::voxel::MicroBehaviour::Command::Walk;
            if (speed > 0.f)
                b.speed = speed;
        } else if (command == "graze" || command == "eat" || command == "grass") {
            b.command = vf::voxel::MicroBehaviour::Command::Graze;
        } else if (command == "stop" || command == "stand" || command == "halt") {
            b.command = vf::voxel::MicroBehaviour::Command::Stop;
        } else if (command == "auto" || command.empty()) {
            b.command = vf::voxel::MicroBehaviour::Command::Auto;
        } else {
            return false;
        }
        spdlog::info("micro: '{}' -> {} (speed {:.2f} m/s)", layerFile, command,
                     double(b.speed));
        return true;
    }
    return false;
}

void App::updateMicroBehaviours(float dt)
{
    // The autonomous behaviours, once per frame. Each one produces a
    // transform (an offset and a rotation about the pivot) and the micro
    // pass applies it when the field is sampled - so a walk costs no
    // re-bake and the object keeps its exact geometry.
    //
    // A behaviour is SUSPENDED while the user manipulates that object (a
    // staged move or a staged rotation): then the transform is the user's,
    // not the animal's.
    for (size_t i = 0; i < m_microBehaviours.size(); ++i) {
        vf::voxel::MicroBehaviour& b = m_microBehaviours[i];
        if (!b.active)
            continue;
        const bool held = m_moveStaged || m_rotationStaged;
        if (held) {
            m_microSvoPass.setWorldAnimation(i, glm::vec3(0.f), glm::mat3(1.f),
                                             b.pivot, false);
            continue;
        }

        // Snapshot the pose before update(): the state machine advances
        // regardless, but a REFUSED step must not move the animal (and must
        // not advance the gait, or the feet skate in place against a wall).
        const glm::vec3 prevOffset = b.offset;
        const float prevHeading = b.heading;
        const float prevGait = b.gait;
        b.update(dt);

        // OBSTACLE RESPONSE. The behaviour itself has no notion of geometry
        // (it is a pure transform), so the collision lives here: a candidate
        // step that overlaps a world object or another micro object is
        // withdrawn, and the heading is turned toward the first free retry.
        // If every retry is blocked the animal stops to graze, which is also
        // the only state that cannot walk into the obstacle next frame.
        if (i < m_microFields.size() &&
            microFootprintBlocked(int(i), b.offset)) {
            const float step = b.speed * std::min(dt, 0.1f);
            static const float kTurns[] = { 1.2f, -1.2f, 2.2f, -2.2f,
                                            3.0f, -3.0f };
            bool routed = false;
            for (float turn : kTurns) {
                glm::vec3 cand = prevOffset;
                cand.x += std::sin(prevHeading + turn) * step;
                cand.z += std::cos(prevHeading + turn) * step;
                if (!microFootprintBlocked(int(i), cand)) {
                    b.heading = prevHeading + turn;
                    routed = true;
                    break;
                }
            }
            b.offset = prevOffset;
            b.gait = prevGait;
            if (!routed) {
                b.state = vf::voxel::MicroBehaviour::State::Graze;
                b.stateT = 0.0f;
            }
        }

        // GROUND CLAMP. A walking animal must not sink into the terrain: lift
        // the body until its lowest SOLID cell clears the highest ground under
        // the footprint (the max at the four corners is what stops a slope
        // swallowing the downhill edge). Lift-only on purpose: a dip is not a
        // hole to fall into, and an object authored on a platform keeps the
        // height the author gave it.
        if (i < m_microFields.size() && i < m_microOccluders.size()) {
            const vf::voxel::MicroField& f = m_microFields[i];
            const vf::voxel::MicroOccluder& o = m_microOccluders[i];
            if (f.size3() > 0 && f.solidMax.x >= f.solidMin.x) {
                // solidMin/Max EXCLUDE the grid's air margin; adding origin +
                // worldOffset turns them into metres in the world.
                const glm::vec3 grid = o.worldOffset + f.origin;
                const glm::vec3 lo = grid + glm::vec3(f.solidMin) * f.voxel;
                const glm::vec3 hi =
                    grid + glm::vec3(f.solidMax + 1) * f.voxel;
                float ground =
                    terrainTopAt(0.5f * (lo.x + hi.x) + b.offset.x,
                                    0.5f * (lo.z + hi.z) + b.offset.z);
                const float xs[2] = { lo.x + b.offset.x, hi.x + b.offset.x };
                const float zs[2] = { lo.z + b.offset.z, hi.z + b.offset.z };
                for (float x : xs)
                    for (float z : zs)
                        ground = std::max(ground, terrainTopAt(x, z));
                if (ground > -1e8f) {
                    const float bottom = lo.y + b.offset.y;
                    if (ground > bottom)
                        b.offset.y += ground - bottom + 0.002f;
                }
            }
        }
        m_microSvoPass.setWorldAnimation(i, b.offset, b.rot, b.pivot, true);
        // The SKELETON's bones: the gait drives them, and the pass rewrites
        // their rotations (the segments and the joints stay as they were
        // uploaded).
        b.animateBones();
        m_microSvoPass.setBoneRotations(i, b.boneRot);
    }
}

} // namespace app
} // namespace vf