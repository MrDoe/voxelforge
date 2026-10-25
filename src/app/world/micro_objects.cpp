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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
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
        m_furObjects.clear();
        m_furIndexOfMicro.clear();
        m_furSourceField.clear();
        m_furPass.setObjects({});
        return;
    }
    const std::string dir = std::filesystem::path(m_manifestPath).parent_path().string();

    std::vector<glm::vec3> occluderOffsets;
    std::vector<std::string> occluderFiles;
    std::vector<std::string> behaviourFiles;
    std::vector<std::string> commandFiles;
    std::vector<vf::voxel::MicroRig> microRigs;
    std::vector<vf::voxel::MicroWorld> worlds;
    struct FurStat { std::string file; size_t cards; double ms; int bones; };
    std::vector<FurStat> furStats;
    // VF_FUR=0 turns the fur off. It is the escape hatch: with the cards
    // gone the pass never dispatches and the frame is byte-identical to the
    // frame without the feature.
    const bool furEnabled = getenv("VF_FUR") == nullptr || atoi(getenv("VF_FUR")) != 0;
    int microWorldCount = 0;
    m_microFields.clear();
    m_microOccluders.clear();
    m_furObjects.clear();
    m_furIndexOfMicro.clear();
    m_furSourceField.clear();
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
        commandFiles.push_back(l.command);
// The FUR CARDS. A micro object whose material grows fur (21,
        // `fleece`) gets a coat of hair cards; the vase and the urn do not,
        // and the fur pass stays empty for them.
        //
        // The cards are generated in the field's LOCAL frame, so the layer's
        // world placement and the live rotation preview are applied by the
        // shader exactly as the micro overlay applies them to the field -
        // the fur and the body can never drift apart.
        int furIndex = -1;
        size_t nCards = 0;
        double furMs = 0.0;
        int nBones = 0;
        if (furEnabled && !world.chunkGrid.empty()) {
            const auto t0 = std::chrono::steady_clock::now();
            furIndex = growFurCards(field, offset, microRigs.back());
            furMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
            if (furIndex >= 0) {
                nCards = m_furObjects[size_t(furIndex)].cards.size();
                nBones = m_furObjects[size_t(furIndex)].boneCount;
                spdlog::info("fur: '{}' -> {} cards, {} bones ({:.1f} ms)", l.file, nCards, nBones, furMs);
            }
        }
        furStats.push_back({l.file, nCards, furMs, nBones});
        spdlog::info("micro: '{}' -> {} voxels on a {:.3f} m lattice ({}x{}x{} cells), "
                     "gridN {}, {} bricks, {:.1f} KB",
                     l.file, data.voxels.size(), field.voxel, field.dim.x, field.dim.y,
                     field.dim.z, world.gridN, world.brickCount(),
                     double(world.bytes()) / 1024.0);
        // The micro pass indexes the worlds it accepted (a world with an
        // empty chunk grid is skipped), so the fur index mapping has to use
        // the SAME count or the preview would land on the wrong object.
        if (!world.chunkGrid.empty()) {
            m_furIndexOfMicro.push_back(furIndex);
            m_furSourceField.push_back(furIndex >= 0 ? int(m_microFields.size()) - 1 : -1);
            ++microWorldCount;
        }
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
        // A manifest-issued POSE (rigged movement) overrides the gait: the
        // bones hold the authored rotations.
        auto axisRot = [](glm::vec3 axis, float angle) {
            axis = glm::normalize(axis);
            const float c = std::cos(angle), s = std::sin(angle), t = 1.f - c;
            const glm::vec3 k = axis;
            return glm::mat3(
                t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y,
                t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x,
                t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c);
        };
        if (i < layers.size() && !layers[i].pose.empty()) {
            const auto& p = layers[i].pose;
            if (i < microRigs.size())
                b.rig = microRigs[i];
            b.poseRot.assign(b.rig.bones.size(), glm::mat3(1.f));
            for (size_t k = 0; k + 3 < p.size(); k += 4) {
                const int bi = int(p[k]);
                if (bi < 0 || bi >= (int)b.poseRot.size())
                    continue;
                const float rx = glm::radians(p[k + 1]);
                const float ry = glm::radians(p[k + 2]);
                const float rz = glm::radians(p[k + 3]);
                b.poseRot[size_t(bi)] =
                    axisRot(glm::vec3(1, 0, 0), rx) *
                    axisRot(glm::vec3(0, 1, 0), ry) *
                    axisRot(glm::vec3(0, 0, 1), rz);
            }
            b.poseActive = true;
            b.active = true;
            b.command = vf::voxel::MicroBehaviour::Command::Stop; // hold the pose
            if (!wants) {
                b.reset(oc.aabbMin, float(i) * 7.31f + 1.7f);
                b.pivot = oc.aabbMin;
                if (i < microRigs.size())
                    b.rig = microRigs[i];
            }
        }

        // A manifest-issued command (from MCP/chat) overrides the default.
        const std::string& cmd = (i < commandFiles.size()) ? commandFiles[i] : std::string();
        if (!cmd.empty()) {
            if (cmd == "walk") b.command = vf::voxel::MicroBehaviour::Command::Walk;
            else if (cmd == "run") b.command = vf::voxel::MicroBehaviour::Command::Walk;
            else if (cmd == "graze") b.command = vf::voxel::MicroBehaviour::Command::Graze;
            else if (cmd == "stop") b.command = vf::voxel::MicroBehaviour::Command::Stop;
            else if (cmd == "auto") b.command = vf::voxel::MicroBehaviour::Command::Auto;
            b.active = true;
            if (b.rig.bones.empty() && i < microRigs.size())
                b.rig = microRigs[i];
            if (b.rig.bones.empty() && wants == false) {
                // ensure reset happened for a command on a non-grazer object
                b.reset(oc.aabbMin, float(i) * 7.31f + 1.7f);
                b.pivot = oc.aabbMin;
            }
        }
        m_microBehaviours.push_back(b);
    }
    m_microSvoPass.setMicroWorlds(worlds);
    // The fur cards go up last: the pass needs the objects, and the preview
    // mirroring needs the index mapping that was just built.
    m_furPass.setObjects(m_furObjects);

    // Persist a small runtime stats file the MCP (and humans) can read on
    // demand: per-object card counts + build time, plus the total. Runtime
    // state, not tracked content, so it lives under .opencode/.
    {
        const std::string statsPath = ".opencode/fur_stats.json";
        std::ofstream f(statsPath);
        if (f) {
            size_t total = 0;
            f << "{\n  \"objects\": [\n";
            for (size_t i = 0; i < furStats.size(); ++i) {
                const auto& s = furStats[i];
                total += s.cards;
                f << "    {\"file\": \"" << s.file << "\", \"cards\": " << s.cards
                  << ", \"bones\": " << s.bones
                  << ", \"build_ms\": " << s.ms << "}";
                if (i + 1 < furStats.size())
                    f << ",";
                f << "\n";
            }
            f << "  ],\n  \"total_cards\": " << total << ",\n  \"object_count\": "
              << furStats.size() << "\n}\n";
        }
    }
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

// Grow the fur cards for ONE micro field and append them to m_furObjects.
// Returns the new object's fur index, or -1 when it grows no coat.
//
// The cards are a function of the field and the parameters, so this is the
// only place that knows how to make one: the load and the rebuild (the
// sidebar's sliders changed) share it.
int App::growFurCards(const vf::voxel::MicroField& field, glm::vec3 worldOffset,
                      const vf::voxel::MicroRig& rig)
{
    if (!m_furEnabled)
        return -1;
    vf::voxel::FurParams fp;
    fp.sunDir = glm::vec3(m_sunDir);
    fp.perCell = m_furPerCell;
    fp.length = m_furLength;
    fp.width = m_furWidth;
    fp.curlAmp = m_furCurl;
    fp.curlCycles = m_furCycles;
    fp.tiltMax = m_furTilt;
    fp.tiltMin = std::min(12.0f, m_furTilt);
    fp.materials = m_furMaterials;
    vf::voxel::FurObject fo;
    std::string ferr;
    if (!vf::voxel::buildFurCards(field, fp, fo, &ferr, &rig, worldOffset))
        return -1;
    fo.worldMin = field.origin + worldOffset;
    m_furObjects.push_back(std::move(fo));
    return int(m_furObjects.size()) - 1;
}

// Re-grow every coat with the current parameters and upload it.
//
// The cards are a CPU product (a few hundred milliseconds for the alpaca),
// so this is called when a slider is RELEASED, not per tick.
void App::rebuildFurCards()
{
    if (!m_microSvoPass.empty() || !m_furObjects.empty() || !m_microFields.empty()) {
        // The preview state lives on the fur objects, so it is carried over
        // rather than lost: a coat rebuilt mid-drag must not drop the drag.
        //
        // It is carried over BY SOURCE, not by the coat's own index: the
        // index SHIFTS when a material starts or stops growing fur (the
        // Textures panel's per-material switch), and a shifted index would
        // hand one object's preview to another.
        const size_t nSrc = m_microFields.size();
        std::vector<glm::vec3> prevOffset(nSrc);
        std::vector<glm::mat3> prevRot(nSrc);
        std::vector<glm::vec3> prevPivot(nSrc);
        std::vector<char> prevOffsetOn(nSrc, 0), prevRotOn(nSrc, 0);
        for (size_t i = 0; i < m_furObjects.size(); ++i) {
            const int src = (i < m_furSourceField.size()) ? m_furSourceField[i] : -1;
            if (src < 0 || src >= int(nSrc))
                continue;
            prevOffset[size_t(src)] = m_furObjects[i].preview;
            prevOffsetOn[size_t(src)] = m_furObjects[i].previewActive ? 1 : 0;
            prevRot[size_t(src)] = m_furObjects[i].rot;
            prevPivot[size_t(src)] = m_furObjects[i].pivot;
            prevRotOn[size_t(src)] = m_furObjects[i].rotActive ? 1 : 0;
        }
        m_furObjects.clear();
        for (size_t i = 0; i < m_furSourceField.size(); ++i) {
            const int fi = m_furSourceField[i];
            if (fi < 0 || fi >= int(m_microFields.size()))
                continue;
            const int idx = growFurCards(m_microFields[size_t(fi)],
                                         m_microOccluders[size_t(fi)].worldOffset,
                                         (fi >= 0 && fi < int(m_microBehaviours.size()))
                                             ? m_microBehaviours[size_t(fi)].rig
                                             : vf::voxel::MicroRig{});
            if (idx >= 0) {
                m_furObjects[size_t(idx)].preview = prevOffset[size_t(fi)];
                m_furObjects[size_t(idx)].previewActive = prevOffsetOn[size_t(fi)] != 0;
                m_furObjects[size_t(idx)].rot = prevRot[size_t(fi)];
                m_furObjects[size_t(idx)].pivot = prevPivot[size_t(fi)];
                m_furObjects[size_t(idx)].rotActive = prevRotOn[size_t(fi)] != 0;
            }
        }
        m_furPass.setObjects(m_furObjects);
    }
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
        for (size_t i = 0; i < m_furObjects.size(); ++i)
            m_furPass.setWorldPreview(i, glm::vec3(0.f), false);
        return;
    }
    m_microSvoPass.setWorldPreview(size_t(idx), m_moveDelta, true);
    // The fur follows the body it belongs to - and the OBJECT keeps the
    // offset, so a coat rebuilt mid-drag does not drop the drag.
    if (idx < int(m_furIndexOfMicro.size())) {
        const int fi = m_furIndexOfMicro[size_t(idx)];
        if (fi >= 0) {
            m_furPass.setWorldPreview(size_t(fi), m_moveDelta, true);
            m_furObjects[size_t(fi)].preview = m_moveDelta;
            m_furObjects[size_t(fi)].previewActive = true;
        }
    }
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
    for (size_t i = 0; i < m_furObjects.size(); ++i) {
        m_furPass.setWorldRotation(i, glm::mat3(1.0f), glm::vec3(0.0f), false);
        m_furObjects[i].rotActive = false;
    }
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

namespace {

struct FreezePose {
    glm::vec3 offset { 0.f };
    float headingDeg = 0.f;
    float gait = 0.f;
    float graze = 0.f;
};

// VF_TEST_BEHAVIOUR_FREEZE="name:dx,dy,dz:headingDeg:gait[:graze][;...]"
//
// A headless `--shot` of an ANIMATED micro object is NOT deterministic: the
// behaviour advances with the frame clock (the step is clamped to 0.1 s) for
// every frame before the shot, so two runs of the identical binary differ by
// tens of thousands of pixels. That makes a shader A/B on the alpaca pose
// noise. Pinning the transform, the gait phase and the graze blend makes the
// render exact.
const std::map<std::string, FreezePose>& freezePoses()
{
    static std::map<std::string, FreezePose> poses;
    static const bool once = [] {
        const char* spec = std::getenv("VF_TEST_BEHAVIOUR_FREEZE");
        if (!spec || !*spec)
            return true;
        const std::string all(spec);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(';', pos);
            if (end == std::string::npos)
                end = all.size();
            const std::string item = all.substr(pos, end - pos);
            pos = end + 1;
            char name[64] = {};
            float x = 0.f, y = 0.f, z = 0.f, hd = 0.f, gait = 0.f, graze = 0.f;
            const int got = std::sscanf(item.c_str(), "%63[^:]:%f,%f,%f:%f:%f:%f",
                                        name, &x, &y, &z, &hd, &gait, &graze);
            if (got >= 5) {
                FreezePose p;
                p.offset = glm::vec3(x, y, z);
                p.headingDeg = hd;
                p.gait = gait;
                p.graze = got >= 6 ? graze : 0.f;
                poses[name] = p;
            }
        }
        return true;
    }();
    (void)once;
    return poses;
}

} // namespace

void App::updateMicroBehaviours(float dt)
{
    // VF_TRACE_BEHAVIOUR cost accounting: cumulative milliseconds per frame
    // bucket, printed every 600 frames. The animation's own maths is a
    // fraction of a percent; the collision probing around it is not, so
    // measure it rather than guess.
    static double cScan = 0, cAnim = 0, cUpload = 0, cProbe = 0;
    static long cFrames = 0, cProbeCalls = 0, cScanRuns = 0, cBlocked = 0;
    // Which staleness test asked for the rescan (a rescan can trip several).
    static long cRNever = 0, cRMove = 0, cRSwing = 0, cRArrive = 0, cRAge = 0;
    // getenv() walks environ; measured 1.01 % of CPU when these ran per
    // frame (and two of them ran before the `m_frameIdx == 0` gate that
    // makes them once-per-run diagnostics). Read each once per process.
    static const bool costTrace = getenv("VF_TRACE_BEHAVIOUR") != nullptr;
    static const bool noFurAnim = getenv("VF_FUR_NOANIM") != nullptr;
    static const bool furStat = getenv("VF_TEST_FUR_STAT") != nullptr;
    static const bool traceRig = getenv("VF_TRACE_RIG") != nullptr;
    auto nowMs = []() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

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
        // A pinned pose wins over both the state machine and the user's
        // manipulation: it exists so a render is exactly reproducible. It
        // does NOT skip the ground clamp below - a pinned pose still stands
        // on the terrain, and the clamp is deterministic for a fixed offset.
        bool frozen = false;
        {
            const auto& poses = freezePoses();
            if (!poses.empty() && i < m_microOccluders.size()) {
                std::string stem = m_microOccluders[i].layerFile;
                const size_t slash = stem.find_last_of('/');
                if (slash != std::string::npos)
                    stem = stem.substr(slash + 1);
                if (stem.size() > 4)
                    stem.resize(stem.size() - 4);
                const auto fit = poses.find(stem);
                if (fit != poses.end()) {
                    b.pinPose(fit->second.offset, fit->second.headingDeg,
                              fit->second.gait, fit->second.graze);
                    frozen = true;
                }
            }
        }

        // The fur pass has its OWN table, so the coat's index is the micro
        // world's index mapped through the fur mapping (-1 when that object
        // grows no coat).
        const int fi = (i < m_furIndexOfMicro.size()) ? m_furIndexOfMicro[i] : -1;
        const bool held = !frozen && (m_moveStaged || m_rotationStaged);
        if (held) {
            if (fi >= 0)
                m_furPass.setWorldAnimation(size_t(fi), glm::vec3(0.f),
                                            glm::mat3(1.f), glm::vec3(0.f), false);
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
        if (!frozen) {
            b.update(dt);
            // How long the cached escape answer has stood unanswered.
            b.scanAge += dt;
        }

        // OBSTACLE RESPONSE. The behaviour itself has no notion of geometry
        // (it is a pure transform), so the collision lives here: a candidate
        // step that overlaps a world object or another micro object is
        // withdrawn, and the heading is turned toward the first free retry.
        // If every retry is blocked the animal stops to graze, which is also
        // the only state that cannot walk into the obstacle next frame.
        const double tObst0 = costTrace ? nowMs() : 0.0;
        double probeMs = 0.0;
        bool blockedNow = false;
        int whyNow = 0; // 1 water, 2 world object cell, 3 another micro object
        if (!frozen && i < m_microFields.size()) {
            blockedNow = microFootprintBlocked(int(i), b.offset, &whyNow);
            if (costTrace) {
                probeMs = nowMs() - tObst0;
                cProbe += probeMs;
                ++cProbeCalls;
            }
        }
        if (blockedNow) {
            if (costTrace)
                ++cBlocked;
            // The animal is touching something. It must TURN AWAY, and the
            // turn must be a TARGET, not a jump: a jump is applied on every
            // frame while the obstacle is still there, so the heading
            // advanced +1.2 rad EVERY FRAME and the animal span in place -
            // which is what the flickering and the "moving too fast" turned
            // out to be. With a target the animal swings round at a limited
            // rate and walks off once it faces clear ground.
            //
            // The scan's LATER rungs are the animal's own FOOTPRINT, not the
            // frame's: with a frame step of a fraction of a millimetre in a
            // headless run, rungs that close all give the same verdict and the
            // scan could not tell a clear corridor from a blocked one. The
            // FIRST rung is deliberately the frame's step - see below.
            const float step = std::max(b.speed * std::min(dt, 0.1f), 0.55f);
            // Withdraw the refused step FIRST: everything below starts from
            // where the animal actually is, not from where it wished to be.
            b.offset = prevOffset;
            // Keep the gait moving slowly so the legs don't freeze mid-turn.
            b.gait = prevGait + step * 0.5f;

            // RE-RUN THE CLEARANCE SCAN ONLY WHEN ITS ANSWER CAN BE WRONG.
            // The scan is 64 footprint probes and cost ~0.19 ms - by far the
            // most expensive thing this function does - while it only ever
            // picks a DIRECTION. Its answer is stale when the pose moved
            // (the footprint is a circumscribed cylinder around the offset,
            // so the offset is the only thing that moves it), when the body
            // has swung far enough that the cached absolute angle no longer
            // means "where I point now", when the cached direction was
            // reached (then ask again from here), or when it has timed out
            // (the world can change under a still animal).
            const float moved =
                std::hypot(b.offset.x - b.scanOffset.x,
                           b.offset.z - b.scanOffset.z);
            float swung = b.heading - b.scanHeading;
            while (swung > 3.14159265f) swung -= 6.28318531f;
            while (swung < -3.14159265f) swung += 6.28318531f;
            // "We are AT the direction we picked and still stuck": the cached
            // answer has been used up, so ask again. Measured against the
            // heading rather than against hasHeadingTarget, because the
            // unblocked branch below clears that flag on every free frame -
            // keying the rescan off it collapsed the throttle back into a
            // rescan per blocked frame.
            float arrivedAt = b.scanAngle - b.heading;
            while (arrivedAt > 3.14159265f) arrivedAt -= 6.28318531f;
            while (arrivedAt < -3.14159265f) arrivedAt += 6.28318531f;
            const bool arrived =
                b.scanClear > 0.f && std::abs(arrivedAt) <= 0.20f;
            const bool needNever = !b.scanValid;
            const bool needMove = moved > 0.25f;
            const bool needSwing = std::abs(swung) > 0.6f;
            const bool needArrive = arrived;
            const bool needAge = b.scanAge > 0.4f;
            if (needNever || needMove || needSwing || needArrive || needAge) {
                if (costTrace) {
                    ++cScanRuns;
                    cRNever += needNever ? 1 : 0;
                    cRMove += needMove ? 1 : 0;
                    cRSwing += needSwing ? 1 : 0;
                    cRArrive += needArrive ? 1 : 0;
                    cRAge += needAge ? 1 : 0;
                }
                constexpr int N = 16;
                float clears[N] = {};
                float bestClear = -1.f;
                float bestAngle = prevHeading;
                int bestK = 0;
                // The first rung is the FRAME'S OWN STEP, and that is
                // load-bearing. Measured 2026-10-25 (VF_TRACE_BEHAVIOUR): the
                // animal stood in free space (`hereBlocked=false`), the step
                // 3 mm ahead was inside a world object (`stepWhy=2`), and the
                // scan - whose first rung was at 0.5*step = 0.275 m - still
                // called that direction clear for the whole 1.1 m. So k=0
                // won, the target equalled the current heading, the heading
                // never turned, the refused step was withdrawn every frame,
                // and the animal sat in a limit cycle until the state machine
                // happened to choose Graze. Testing the position the animal
                // will ACTUALLY occupy next frame makes k=0's verdict equal
                // the probe's verdict, so a refused step always loses and the
                // animal turns toward a direction whose near field is free.
                const float fstep = b.speed * std::min(dt, 0.1f);
                for (int k = 0; k < N; ++k) {
                    const float a =
                        prevHeading + (float(k) / float(N)) * 6.28318531f;
                    // How far can we step in this direction before hitting?
                    float clear = 0.f;
                    for (int s = 0; s <= 4; ++s) {
                        const float d = (s == 0) ? fstep
                                                 : step * float(s) * 0.5f;
                        glm::vec3 cand = prevOffset;
                        cand.x += std::sin(a) * d;
                        cand.z += std::cos(a) * d;
                        if (microFootprintBlocked(int(i), cand))
                            break;
                        clear = float(s + 1);
                    }
                    clears[k] = clear;
                    if (clear > bestClear) {
                        bestClear = clear;
                        bestAngle = a;
                        bestK = k;
                    }
                }
                if (costTrace && cScanRuns <= 60) {
                    // Trace only: is the spot we are STANDING on blocked too,
                    // or only the few millimetres we tried to advance? That is
                    // the difference between "already inside the obstacle" and
                    // "a hair's breadth from it" - and if we are standing free
                    // while stepping 3 mm forward is blocked, no amount of
                    // turning can ever clear the step.
                    int whyHere = 0;
                    const bool hereBlocked =
                        microFootprintBlocked(int(i), prevOffset, &whyHere);
                    spdlog::info(
                        "rescan#{:d} k={} clear={} clears={:.0f}{:.0f}{:.0f}"
                        "{:.0f}{:.0f}{:.0f}{:.0f}{:.0f}{:.0f}{:.0f}{:.0f}"
                        "{:.0f}{:.0f}{:.0f}{:.0f}{:.0f} heading={:+.3f} "
                        "stepWhy={} hereBlocked={} hereWhy={} "
                        "at=({:+.3f},{:+.3f},{:+.3f})",
                        int(cScanRuns), bestK, bestClear, clears[0], clears[1],
                        clears[2], clears[3], clears[4], clears[5], clears[6],
                        clears[7], clears[8], clears[9], clears[10],
                        clears[11], clears[12], clears[13], clears[14],
                        clears[15], double(prevHeading), whyNow, hereBlocked,
                        whyHere, double(b.offset.x), double(b.offset.y),
                        double(b.offset.z));
                }
                b.scanValid = true;
                b.scanAngle = bestAngle;
                b.scanClear = bestClear;
                b.scanOffset = prevOffset;
                b.scanHeading = prevHeading;
                b.scanAge = 0.f;
            }

            // APPLY the decision, cached or fresh - the same writes a fresh
            // scan would make, so a throttled animal behaves like a scanned
            // one, only cheaper. Re-asserting it on every blocked frame also
            // keeps stateT pinned in Graze exactly as before, so a cached
            // graze cannot expire halfway round the obstacle.
            if (b.scanClear > 0.f) {
                // Convert to a relative turn, wrapped.
                float d = b.scanAngle - b.heading;
                while (d > 3.14159265f) d -= 6.28318531f;
                while (d < -3.14159265f) d += 6.28318531f;
                b.headingTarget = b.heading + d;
                b.hasHeadingTarget = true;
                // The head leads the turn.
                b.headTurn = glm::clamp(d, -0.8f, 0.8f);
            } else {
                b.hasHeadingTarget = false;
                b.state = vf::voxel::MicroBehaviour::State::Graze;
                b.stateT = 0.0f;
            }
        } else {
            b.hasHeadingTarget = false;
            // The cache is NOT dropped here. It is the whole point of the
            // throttle: a turning animal alternates free/blocked frames (the
            // probe position moves every frame), so invalidating on every
            // free frame collapsed the throttle back to a rescan per blocked
            // frame - measured: 775 rescans over 2400 frames, i.e. no better
            // than the 832 it replaced. The three staleness tests above are
            // what expire it: a still world expires with scanAge (0.4 s), a
            // moving pose with `moved`, a turning body with `swung`.
        }
        const double tObst1 = costTrace ? nowMs() : 0.0;
        if (costTrace)
            cScan += tObst1 - tObst0 - probeMs;

        // Pose-aware ground clamp: animate the bones FIRST so the clamp can
        // account for a swinging leg that dips below the rest bottom (the
        // animal's feet must not sink into the terrain). The transforms are
        // in the local frame and independent of the lift, so re-running
        // animateBones after the clamp would be identical.
        b.animateBones();
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
                    // Pose-aware bottom: transform the 8 corners of the rest
                    // solid AABB by their nearest bone + the behaviour
                    // rotation, so a swinging leg that dips below the rest
                    // bottom is caught (feet must not sink or slide).
                    float posedLowest = lo.y;
                    if (!b.rig.bones.empty()) {
                        float minY = 1e9f;
                        for (int cx = 0; cx < 2; ++cx)
                            for (int cy = 0; cy < 2; ++cy)
                                for (int cz = 0; cz < 2; ++cz) {
                                    glm::vec3 c(
                                        lo.x + (cx ? (hi.x - lo.x) : 0.f),
                                        lo.y + (cy ? (hi.y - lo.y) : 0.f),
                                        lo.z + (cz ? (hi.z - lo.z) : 0.f));
                                    // rest frame (local): subtract grid origin
                                    c -= (o.worldOffset + f.origin);
                                    int bn = 0;
                                    float bd = 1e9f;
                                    for (int k = 0; k < (int)b.rig.bones.size(); ++k) {
                                        const glm::vec3 ab =
                                            b.rig.bones[size_t(k)].b -
                                            b.rig.bones[size_t(k)].a;
                                        const float t = glm::clamp(
                                            glm::dot(c - b.rig.bones[size_t(k)].a, ab) /
                                                glm::max(glm::dot(ab, ab), 1e-12f),
                                            0.0f, 1.0f);
                                        const float d = glm::length(
                                            c - (b.rig.bones[size_t(k)].a + ab * t));
                                        if (d < bd) {
                                            bd = d;
                                            bn = k;
                                        }
                                    }
                                    glm::vec3 posed =
                                        b.boneRot[size_t(bn)] * c +
                                        b.boneXform[size_t(bn)];
                                    glm::vec3 world =
                                        b.rot * (posed - b.pivot) + b.pivot;
                                    minY = std::min(minY, world.y);
                                }
                        if (minY < 1e8f)
                            posedLowest = minY;
                    }
                    const float bottom = b.offset.y + posedLowest;
                    if (ground > bottom) {
                        // Rate-limit the lift so a sudden step in the terrain
                        // (a slope under the footprint) eases the body up
                        // instead of snapping it - snapping reads as a jump.
                        // Lift-only: a dip is never a fall, and an authored
                        // platform height survives.
                        const float need = ground - bottom + 0.002f;
                        const float cap = 0.03f; // m per frame
                        b.offset.y += std::min(need, cap);
                    }
                }
            }
        }
        if (costTrace)
            cAnim += nowMs() - tObst1;
        if (costTrace)
            spdlog::info("behaviour {} frame {} offset=({:+.4f},{:+.4f},{:+.4f}) "
                         "heading={:+.2f} gait={:+.2f} state {}",
                         i, m_frameIdx, double(b.offset.x), double(b.offset.y),
                         double(b.offset.z), double(b.heading), double(b.gait),
                         int(b.state));
        const double tUp0 = costTrace ? nowMs() : 0.0;
        m_microSvoPass.setWorldAnimation(i, b.offset, b.rot, b.pivot, true);
        // The SKELETON's bones: the gait drives them, and the pass rewrites
        // their rotations (the segments and the joints stay as they were
        // uploaded).
        m_microSvoPass.setBoneRotations(i, b.boneRot, b.boneXform);
        // The COAT follows the walk: the behaviour moves the animal, and a
        // coat left in the rest pose would hang in the air where the animal
        // used to be.
        if (fi >= 0 && !noFurAnim) {
            m_furPass.setWorldAnimation(size_t(fi), b.offset, b.rot, b.pivot, true);
            // Per-bone: the bones' transforms are in the WORLD frame (the rig
            // was shifted by the layer's offset); the fur cards live in the
            // object's LOCAL frame. The translation must be converted with
            // the rotation applied: t_local = R*frame + t_world - frame.
            //
            // NOTE the two indices: `fi` is the FUR row (only objects that
            // grow a coat get one) and `i` is the MICRO world. They differ as
            // soon as a furless object precedes the furry one - the vase and
            // urn are micro worlds 0/1, the alpaca is 2 but fur row 0 - so
            // indexing the field with `fi` reads ANOTHER OBJECT's frame and
            // throws the whole coat metres away from the body.
            const glm::vec3 frame =
                m_microFields[size_t(i)].origin +
                m_microOccluders[size_t(i)].worldOffset;
            std::vector<glm::vec3> xformLocal = b.boneXform;
            for (size_t k = 0; k < xformLocal.size() && k < b.boneRot.size(); ++k)
                xformLocal[k] = b.boneRot[k] * frame + b.boneXform[k] - frame;
            m_furPass.setBoneRotations(size_t(fi), b.boneRot, xformLocal);
        }
        // DIAGNOSTIC (VF_TEST_FUR_STAT=1, frame 0 only): measures the rule
        // the SHADER uses on the POSED surface, and how far that rule moves
        // the fur's texture coordinate when the animal MOVES.
        //
        // The texture is sampled at the point UN-BONED into the rest pose
        // (microBoneAt -> microUnbone), so the fur stays glued to the skin
        // only if the bone rule is stable. The shader's rule un-bones the
        // POSED point with EACH bone's own transform and measures the result
        // against THAT bone's rest segment, then takes the nearest.
        //
        // This probe poses each exposed rest cell by its REST-POSE owner (the
        // bone whose segment is nearest the cell at rest - the truth), then
        // reports three comparisons:
        //   shader    - the shader rule's bone flips and texture drift against
        //               the rest assignment;
        //   old rule  - the same for the retired "nearest REST segment to the
        //               POSED point" rule (it reclassifies what the pose moved);
        //   walk A->B - the shader rule's bone flips and texture drift between
        //               the pinned pose and the same pose a quarter stride
        //               later - FUR SLIDE WHILE THE ANIMAL MOVES, as a number.
        // Feeding the REST position straight to a non-rest pose (the old
        // probe) measures a configuration the renderer never visits.
        if (furStat && m_frameIdx == 0 &&
            i < m_microFields.size() && i < m_microOccluders.size()) {
            const vf::voxel::MicroField& f = m_microFields[i];
            const glm::vec3 frame = m_microOccluders[i].worldOffset + f.origin;
            const int nb = int(b.rig.bones.size());
            if (nb > 0 && f.size3() > 0) {
                auto segDist = [](const glm::vec3& p, const glm::vec3& a,
                                  const glm::vec3& c) {
                    const glm::vec3 ab = c - a;
                    const float t = glm::clamp(
                        glm::dot(p - a, ab) / glm::max(glm::dot(ab, ab), 1e-12f),
                        0.0f, 1.0f);
                    return glm::length(p - (a + ab * t));
                };
                // A second pose a quarter stride later. animateBones() reads
                // only gait/graze/state, so a copy is enough.
                vf::voxel::MicroBehaviour poseB = b;
                poseB.gait = b.gait + 1.5707963f;
                poseB.animateBones();
                auto shaderRule = [&](const glm::vec3& p,
                                      const std::vector<glm::mat3>& rot,
                                      const std::vector<glm::vec3>& xf,
                                      int& boneOut, glm::vec3& restOut) {
                    boneOut = -1;
                    float bestD = 1e30f;
                    for (int k = 0; k < nb; ++k) {
                        const glm::vec3 q =
                            glm::transpose(rot[size_t(k)]) * (p - xf[size_t(k)]);
                        const float d = segDist(q, b.rig.bones[size_t(k)].a,
                                                b.rig.bones[size_t(k)].b);
                        if (d < bestD) {
                            bestD = d;
                            boneOut = k;
                            restOut = q;
                        }
                    }
                };
                auto oldRule = [&](const glm::vec3& p, int& boneOut,
                                   glm::vec3& restOut) {
                    boneOut = -1;
                    float bestD = 1e30f;
                    for (int k = 0; k < nb; ++k) {
                        const float d = segDist(p, b.rig.bones[size_t(k)].a,
                                                b.rig.bones[size_t(k)].b);
                        if (d < bestD) {
                            bestD = d;
                            boneOut = k;
                            restOut = glm::transpose(b.boneRot[size_t(k)]) *
                                      (p - b.boneXform[size_t(k)]);
                        }
                    }
                };
                size_t surf = 0, newFlip = 0, oldFlip = 0, driftFlip = 0;
                size_t neckSurf = 0, neckNewFlip = 0, neckDriftFlip = 0;
                double newSum = 0.0, newMax = 0.0;
                double oldSum = 0.0, oldMax = 0.0;
                double driftSum = 0.0, driftMax = 0.0;
                // Where the drift lives, by the owner's role.
                size_t roleSurf[5] = { 0, 0, 0, 0, 0 };
                size_t roleFlip[5] = { 0, 0, 0, 0, 0 };
                double roleDrift[5] = { 0, 0, 0, 0, 0 };
                size_t pairFlip[5][5] = {};
                glm::vec3 maxDriftRest(0.0f);
                for (int z = 1; z + 1 < f.dim.z; ++z)
                    for (int y = 1; y + 1 < f.dim.y; ++y)
                        for (int x = 1; x + 1 < f.dim.x; ++x) {
                            const size_t ci = f.idx(x, y, z);
                            if (!f.solid[ci])
                                continue;
                            const bool exposed =
                                !f.solid[f.idx(x + 1, y, z)] ||
                                !f.solid[f.idx(x - 1, y, z)] ||
                                !f.solid[f.idx(x, y + 1, z)] ||
                                !f.solid[f.idx(x, y - 1, z)] ||
                                !f.solid[f.idx(x, y, z + 1)] ||
                                !f.solid[f.idx(x, y, z - 1)];
                            if (!exposed)
                                continue;
                            const glm::vec3 rest =
                                frame + (glm::vec3(x, y, z) + 0.5f) * f.voxel;
                            // The truth: the bone nearest the cell AT REST.
                            int owner = 0;
                            float ownerD = 1e30f;
                            for (int k = 0; k < nb; ++k) {
                                const float d =
                                    segDist(rest, b.rig.bones[size_t(k)].a,
                                            b.rig.bones[size_t(k)].b);
                                if (d < ownerD) {
                                    ownerD = d;
                                    owner = k;
                                }
                            }
                            const glm::vec3 pA =
                                b.boneRot[size_t(owner)] * rest +
                                b.boneXform[size_t(owner)];
                            const glm::vec3 pB =
                                poseB.boneRot[size_t(owner)] * rest +
                                poseB.boneXform[size_t(owner)];
                            int newA = -1, newB = -1, oldA = -1;
                            glm::vec3 qA(0.0f), qB(0.0f), qOld(0.0f);
                            shaderRule(pA, b.boneRot, b.boneXform, newA, qA);
                            shaderRule(pB, poseB.boneRot, poseB.boneXform, newB, qB);
                            oldRule(pA, oldA, qOld);
                            const double eNew = glm::length(qA - rest);
                            const double eOld = glm::length(qOld - rest);
                            const double eDrift = glm::length(qA - qB);
                            ++surf;
                            newSum += eNew;
                            newMax = std::max(newMax, eNew);
                            oldSum += eOld;
                            oldMax = std::max(oldMax, eOld);
                            driftSum += eDrift;
                            driftMax = std::max(driftMax, eDrift);
                            if (newA != owner) ++newFlip;
                            if (oldA != owner) ++oldFlip;
                            if (newA != newB) ++driftFlip;
                            const auto role = b.rig.bones[size_t(owner)].role;
                            const int roleIdx = int(role);
                            if (roleIdx >= 0 && roleIdx < 5) {
                                ++roleSurf[roleIdx];
                                if (newA != newB) {
                                    ++roleFlip[roleIdx];
                                    roleDrift[roleIdx] += eDrift;
                                    const int ra = int(b.rig.bones[size_t(newA)].role);
                                    const int rb = int(b.rig.bones[size_t(newB)].role);
                                    if (ra >= 0 && ra < 5 && rb >= 0 && rb < 5)
                                        ++pairFlip[ra][rb];
                                }
                            }
                            if (eDrift > double(driftMax) - 1e-9)
                                maxDriftRest = rest - frame;
                            const bool neck =
                                role == vf::voxel::BoneRole::Neck ||
                                role == vf::voxel::BoneRole::Head;
                            if (neck) {
                                ++neckSurf;
                                if (newA != owner) ++neckNewFlip;
                                if (newA != newB) ++neckDriftFlip;
                            }
                        }
                const double inv = 1.0 / double(std::max<size_t>(surf, 1));
                {
                    static const char* rn[5] = { "spine", "leg", "head",
                                                "tail", "neck" };
                    std::string pairs;
                    for (int ra = 0; ra < 5; ++ra)
                        for (int rb = 0; rb < 5; ++rb)
                            if (pairFlip[ra][rb]) {
                                pairs += rn[ra];
                                pairs += "->";
                                pairs += rn[rb];
                                pairs += ":";
                                pairs += std::to_string(pairFlip[ra][rb]);
                                pairs += " ";
                            }
                    spdlog::info("furstat walk pairs: {}", pairs);
                }
                spdlog::info(
                    "furstat walk by owner: spine {}/{} ({:.1f}%) {:.1f} mm | "
                    "leg {}/{} ({:.1f}%) {:.1f} mm | head {}/{} tail {}/{} "
                    "at rest ({:+.3f},{:+.3f},{:+.3f})",
                    roleFlip[0], roleSurf[0],
                    100.0 * double(roleFlip[0]) /
                        double(std::max<size_t>(roleSurf[0], 1)),
                    roleDrift[0] * 1000.0 /
                        double(std::max<size_t>(roleFlip[0], 1)),
                    roleFlip[1], roleSurf[1],
                    100.0 * double(roleFlip[1]) /
                        double(std::max<size_t>(roleSurf[1], 1)),
                    roleDrift[1] * 1000.0 /
                        double(std::max<size_t>(roleFlip[1], 1)),
                    roleFlip[2], roleSurf[2], roleFlip[3], roleSurf[3],
                    double(maxDriftRest.x), double(maxDriftRest.y),
                    double(maxDriftRest.z));
                spdlog::info(
                    "furstat origin=({:+.3f},{:+.3f},{:+.3f}) surface {} | "
                    "shader flips {} ({:.1f}%) drift {:.1f}/{:.1f} mm | "
                    "old-rule flips {} ({:.1f}%) drift {:.1f}/{:.1f} mm | "
                    "walk A->B flips {} ({:.1f}%) drift {:.1f}/{:.1f} mm | "
                    "neck/head surface {} shader flips {} A->B flips {}",
                    double(frame.x), double(frame.y), double(frame.z), surf,
                    newFlip, 100.0 * double(newFlip) * inv,
                    newSum * inv * 1000.0, newMax * 1000.0,
                    oldFlip, 100.0 * double(oldFlip) * inv,
                    oldSum * inv * 1000.0, oldMax * 1000.0,
                    driftFlip, 100.0 * double(driftFlip) * inv,
                    driftSum * inv * 1000.0, driftMax * 1000.0,
                    neckSurf, neckNewFlip, neckDriftFlip);
            }
        }
        if (costTrace) {
            cUpload += nowMs() - tUp0;
            if (++cFrames % 600 == 0)
                spdlog::info(
                    "behaviour cost over {} frames: probe {:.4f} ms/f "
                    "({} calls), escape-scan {:.4f} ms/f ({} runs of {} "
                    "blocked frames), animate+clamp {:.4f} ms/f, "
                    "upload {:.4f} ms/f, TOTAL {:.4f} ms/f "
                    "[rescan reasons: never {} move {} swing {} arrive {} "
                    "age {}]",
                    cFrames, cProbe / double(cFrames), cProbeCalls,
                    cScan / double(cFrames), cScanRuns, cBlocked,
                    cAnim / double(cFrames), cUpload / double(cFrames),
                    (cProbe + cScan + cAnim + cUpload) / double(cFrames),
                    cRNever, cRMove, cRSwing, cRArrive, cRAge);
        }
        if (traceRig && m_frameIdx == 0) {
            spdlog::info("rig{} fwd=({:+.2f},{:+.2f},{:+.2f}) up=({:+.2f},{:+.2f},{:+.2f}) "
                         "center=({:+.2f},{:+.2f},{:+.2f})",
                         i, double(b.rig.forward.x), double(b.rig.forward.y),
                         double(b.rig.forward.z), double(b.rig.up.x),
                         double(b.rig.up.y), double(b.rig.up.z),
                         double(b.rig.center.x), double(b.rig.center.y),
                         double(b.rig.center.z));
            for (size_t k = 0; k < b.rig.bones.size(); ++k) {
                const auto& mb = b.rig.bones[k];
                const glm::vec3 tip = b.boneRot[k] * mb.b + b.boneXform[k];
                spdlog::info("  bone {} role {} parent {} a=({:+.3f},{:+.3f},{:+.3f}) "
                             "b=({:+.3f},{:+.3f},{:+.3f}) posedTip=({:+.3f},{:+.3f},{:+.3f})",
                             k, int(mb.role), mb.parent,
                             double(mb.a.x), double(mb.a.y), double(mb.a.z),
                             double(mb.b.x), double(mb.b.y), double(mb.b.z),
                             double(tip.x), double(tip.y), double(tip.z));
            }
        }
    }
}

} // namespace app
} // namespace vf