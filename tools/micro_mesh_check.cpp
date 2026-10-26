// micro_mesh_check.cpp - OFFLINE proof for the mesh-driven deformation path.
//
// The renderer still deforms the micro object by sampling its REST field
// through the inverse of a rigid bone transform (micro_overlay.comp), and that
// map is discontinuous wherever bone territory flips. This tool proves the
// replacement (micro_mesh.hpp) before any of it is wired into the app:
//
//   rest   - voxelize the REST mesh and compare it cell for cell with the
//            rest field baked from the .vxw records. The mesh was extracted
//            from those voxels, so the two must agree; a disagreement means
//            the voxelizer's sign or fill is wrong, and every later number
//            would be built on it.
//   pose   - sample an animation clip, skin the mesh, voxelize the POSED
//            surface on a union grid, and report the delta (cells added and
//            removed), the bricks it touches, and how long each stage takes.
//            That is the whole P2 budget in one place: if the delta is small
//            and the pipeline is fast, only those bricks need re-uploading.
//
// Everything here is app-free (src/voxel only), so it runs headless:
//   ./build/vf_micro_mesh_check [layer.vxw] [mesh.stl] [skin] [clip.anim]
#include "voxel/micro_clip.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_mesh.hpp"
#include "voxel/micro_rig.hpp"
#include "voxel/worldfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

using namespace vf::voxel;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// The rest field's occupancy projected into a LARGER grid that shares its
// lattice (same voxel size, origin shifted by whole cells). Delta counts are
// only meaningful when both occupancies live on one lattice.
struct UnionGrid {
    glm::ivec3 shift { 0, 0, 0 }; // union cell -> rest cell = union + shift
    glm::ivec3 dim { 0, 0, 0 };
    glm::vec3 origin { 0.f };
};

UnionGrid unionGridFor(const MicroField& rest, const std::vector<MeshTri>& posed)
{
    const float v = rest.voxel;
    glm::vec3 lo = rest.origin;
    glm::vec3 hi = rest.origin + glm::vec3(rest.dim) * v;
    for (const MeshTri& t : posed)
        for (int k = 0; k < 3; ++k) {
            lo = glm::min(lo, t.v[k]);
            hi = glm::max(hi, t.v[k]);
        }
    // One cell of slack on every side: the flood fill needs an air border to
    // start from, and a grid that exactly touches the mesh has none.
    const glm::vec3 rel0 = (lo - rest.origin) / v;
    const glm::vec3 rel1 = (hi - rest.origin) / v;
    UnionGrid g;
    g.shift = -glm::ivec3(glm::floor(rel0)) - glm::ivec3(2);
    g.dim = glm::ivec3(glm::ceil(rel1)) + glm::ivec3(2) - g.shift;
    g.origin = rest.origin + glm::vec3(g.shift) * v;
    return g;
}

bool restSolidAt(const MicroField& rest, int ux, int uy, int uz,
                 const UnionGrid& g)
{
    const int x = ux + g.shift.x, y = uy + g.shift.y, z = uz + g.shift.z;
    if (!rest.inside(x, y, z))
        return false;
    return rest.solid[rest.idx(x, y, z)] != 0;
}

size_t countSolid(const MicroField& f)
{
    size_t n = 0;
    for (uint8_t s : f.solid)
        n += s ? 1 : 0;
    return n;
}

// Edge census by POSITION. A closed polygon soup uses every shared edge
// exactly twice; a triangle count of 1 means the surface has a boundary (a
// crack), which the interior fill then escapes through.
struct EdgeCensus {
    size_t open = 0;     // edges used once
    size_t same = 0;     // used twice in the SAME direction (flipped neighbour)
    size_t manifold = 0; // used twice in opposite directions
};

EdgeCensus edgeCensus(const std::vector<MeshTri>& tris)
{
    auto q = [](const glm::vec3& p) {
        return glm::ivec3(glm::round(p * 1e5f));
    };
    struct Key {
        glm::ivec3 a, b;
        bool operator==(const Key& o) const { return a == o.a && b == o.b; }
    };
    struct Hash {
        size_t operator()(const Key& k) const
        {
            size_t h = 1469598103934665603ull;
            auto mix = [&](int v) {
                h ^= size_t(uint32_t(v));
                h *= 1099511628211ull;
            };
            mix(k.a.x); mix(k.a.y); mix(k.a.z);
            mix(k.b.x); mix(k.b.y); mix(k.b.z);
            return h;
        }
    };
    // Directed edges; lookup of the reversed edge tells us whether the
    // neighbour ran the other way.
    std::unordered_map<Key, int, Hash> directed;
    directed.reserve(tris.size() * 3);
    EdgeCensus out;
    for (const MeshTri& t : tris) {
        const glm::ivec3 p[3] = { q(t.v[0]), q(t.v[1]), q(t.v[2]) };
        for (int k = 0; k < 3; ++k) {
            const glm::ivec3 a = p[k], b = p[(k + 1) % 3];
            if (a == b)
                continue; // degenerate edge, not part of the surface
            ++directed[Key { a, b }];
        }
    }
    for (const auto& kv : directed) {
        if (kv.second != 1)
            continue;
        const int rev = [&] {
            auto it = directed.find(Key { kv.first.b, kv.first.a });
            return it == directed.end() ? 0 : it->second;
        }();
        if (rev == 0)
            ++out.open;
        else if (rev == 1)
            ++out.manifold;
        else
            ++out.same;
    }
    return out;
}

size_t countBricksTouched(const MicroMeshGrid& a, const MicroMeshGrid& b,
                          int brick)
{
    const int bx = (a.dim.x + brick - 1) / brick;
    const int by = (a.dim.y + brick - 1) / brick;
    const int bz = (a.dim.z + brick - 1) / brick;
    size_t touched = 0, total = 0;
    for (int z = 0; z < bz; ++z)
        for (int y = 0; y < by; ++y)
            for (int x = 0; x < bx; ++x) {
                bool any = false, occupied = false;
                for (int dz = 0; dz < brick && !any; ++dz)
                    for (int dy = 0; dy < brick && !any; ++dy)
                        for (int dx = 0; dx < brick && !any; ++dx) {
                            const int cx = x * brick + dx, cy = y * brick + dy,
                                      cz = z * brick + dz;
                            if (cx >= a.dim.x || cy >= a.dim.y || cz >= a.dim.z)
                                continue;
                            const size_t i = a.idx(cx, cy, cz);
                            if (a.solid[i])
                                occupied = true;
                            if (a.solid[i] != b.solid[i])
                                any = true;
                        }
                if (occupied)
                    ++total;
                if (any)
                    ++touched;
            }
    std::printf("      bricks: %zu of %zu occupied are touched by the delta\n",
                touched, total);
    return touched;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string assets = VOXELFORGE_ASSET_DIR;
    const std::string layerPath =
        argc > 1 ? argv[1] : assets + "/micro/alpaca.vxw";
    const std::string meshPath =
        argc > 2 ? argv[2] : assets + "/models/alpaca_from_voxels.stl";
    const std::string skinPath =
        argc > 3 ? argv[3] : assets + "/micro/alpaca.skin";
    const std::string clipPath =
        argc > 4 ? argv[4] : assets + "/micro/graze.anim";

    // ---- the layer's rest field (the appearance + reference occupancy) ----
    WorldFileData data;
    if (!worldfile::read(layerPath, data)) {
        std::printf("FAIL cannot read layer %s\n", layerPath.c_str());
        return 1;
    }
    MicroField rest;
    std::string err;
    if (!buildMicroField(data.voxels, data.meta, rest, &err)) {
        std::printf("FAIL rest field: %s\n", err.c_str());
        return 1;
    }
    std::printf("layer      %s\n", layerPath.c_str());
    std::printf("  records %zu, cell %.4f m, grid %dx%dx%d (%.1f M cells)\n",
                data.voxels.size(), rest.voxel, rest.dim.x, rest.dim.y,
                rest.dim.z, double(rest.size3()) / 1e6);
    std::printf("  solid   %zu cells\n", countSolid(rest));

    // ---- the mesh + skin --------------------------------------------------
    MicroMesh mesh;
    const auto tLoad = Clock::now();
    if (!loadMicroMesh(meshPath, skinPath, mesh, &err)) {
        std::printf("FAIL %s\n", err.c_str());
        return 1;
    }
    std::printf("mesh       %s\n", meshPath.c_str());
    std::printf("  %zu tris, %zu verts, loaded in %.1f ms\n", mesh.tris.size(),
                mesh.rest.size(), msSince(tLoad));
    std::printf("  frame offset applied %.4f %.4f %.4f m\n", mesh.restOffset.x,
                mesh.restOffset.y, mesh.restOffset.z);
    std::printf("  skin bones: %zu declared", mesh.boneNames.size());
    for (const auto& n : mesh.boneNames)
        std::printf(" %s", n.c_str());
    std::printf("\n");
    {
        std::vector<size_t> perBone(mesh.boneNames.size() + 1, 0);
        size_t unskinned = 0;
        for (int32_t b : mesh.bone) {
            if (b < 0)
                ++unskinned;
            else if (size_t(b) < perBone.size())
                ++perBone[size_t(b)];
            else
                ++unskinned;
        }
        std::printf("  per-bone verts:");
        for (size_t b = 0; b < perBone.size(); ++b)
            if (perBone[b])
                std::printf(" %zu:%zu", b, perBone[b]);
        std::printf(" | unskinned %zu\n", unskinned);
    }

    // ---- the rig ----------------------------------------------------------
    MicroRig rig;
    {
        std::error_code ec;
        const uint64_t lsize = std::filesystem::file_size(layerPath, ec);
        const auto mt = std::filesystem::last_write_time(layerPath, ec);
        const uint64_t lmtime = ec ? 0 : uint64_t(mt.time_since_epoch().count());
        std::string rigPath = layerPath;
        const size_t dot = rigPath.find_last_of('.');
        if (dot != std::string::npos)
            rigPath = rigPath.substr(0, dot) + ".rig";
        if (!readMicroRigCache(rigPath, rig, lsize, lmtime)) {
            std::printf("  rig cache %s: no match, deriving\n", rigPath.c_str());
            if (!deriveMicroRig(rest, rig)) {
                std::printf("FAIL could not derive a rig\n");
                return 1;
            }
        } else {
            std::printf("  rig cache %s hit\n", rigPath.c_str());
        }
    }
    std::printf("rig        %zu bones, %d legs\n", rig.bones.size(),
                rig.legCount);
    for (size_t b = 0; b < rig.bones.size(); ++b)
        std::printf("  %2zu %-8s parent %2d role %d seg (%.3f %.3f %.3f)->(%.3f "
                    "%.3f %.3f)\n",
                    b, rig.bones[b].name.c_str(), rig.bones[b].parent,
                    int(rig.bones[b].role), rig.bones[b].a.x, rig.bones[b].a.y,
                    rig.bones[b].a.z, rig.bones[b].b.x, rig.bones[b].b.y,
                    rig.bones[b].b.z);

    // ---- REST round trip: the voxelizer against the baked field ----------
    std::printf("\n== REST round trip ==\n");
    const glm::ivec3 restDim = rest.dim;
    MicroMeshGrid restGrid;
    const auto tVox0 = Clock::now();
    if (!voxelizeMicroMesh(mesh.tris, restDim, rest.voxel, rest.origin,
                           restGrid, &err)) {
        std::printf("FAIL rest voxelization: %s\n", err.c_str());
        return 1;
    }
    const double restVoxMs = msSince(tVox0);
    size_t agree = 0, meshOnly = 0, fieldOnly = 0;
    for (int z = 0; z < restDim.z; ++z)
        for (int y = 0; y < restDim.y; ++y)
            for (int x = 0; x < restDim.x; ++x) {
                const size_t i = rest.idx(x, y, z);
                const bool a = rest.solid[i] != 0, b = restGrid.solid[i] != 0;
                if (a && b)
                    ++agree;
                else if (b)
                    ++meshOnly;
                else if (a)
                    ++fieldOnly;
            }
    const size_t fieldSolids = agree + fieldOnly;
    const double agreePct =
        fieldSolids ? 100.0 * double(agree) / double(fieldSolids) : 0.0;
    std::printf("  mesh  %zu solid (shell %zu + interior %zu), leak=%s\n",
                restGrid.solidCount(), restGrid.shellCells,
                restGrid.interiorCells, restGrid.leak ? "YES" : "no");
    std::printf("  field %zu solid\n", fieldSolids);
    std::printf("  agree %zu (%.2f%%), mesh-only %zu, field-only %zu\n", agree,
                agreePct, meshOnly, fieldOnly);
    std::printf("  voxelize %.1f ms\n", restVoxMs);
    {
        const EdgeCensus e = edgeCensus(mesh.tris);
        std::printf("  rest edges: manifold %zu, open %zu, flipped-neighbour %zu\n",
                    e.manifold, e.open, e.same);
        // Skin consistency at a POSITION: the STL is a non-indexed soup, so the
        // same corner appears once per adjacent triangle. If copies disagree
        // on the bone, rigid skinning moves them apart and the posed surface
        // CRACKS along every bone boundary - which is exactly what a solid
        // voxelization cannot survive.
        std::map<std::tuple<int, int, int>, int> boneOf;
        size_t conflicts = 0, positions = 0;
        for (size_t i = 0; i < mesh.rest.size(); ++i) {
            const glm::vec3& v = mesh.rest[i];
            const auto key = std::make_tuple(int(glm::round(v.x / rest.voxel)),
                                             int(glm::round(v.y / rest.voxel)),
                                             int(glm::round(v.z / rest.voxel)));
            const int b = mesh.bone[i];
            auto it = boneOf.find(key);
            if (it == boneOf.end()) {
                boneOf[key] = b;
                ++positions;
            } else if (it->second != b) {
                ++conflicts;
            }
        }
        std::printf("  skin at a POSITION: %zu positions, %zu vertex copies "
                    "disagree with their own position's bone\n",
                    positions, conflicts);
    }
    std::printf("REST_AGREEMENT=%.2f %s\n", agreePct,
                agreePct >= 99.0 ? "PASS" : "FAIL");

    // ---- clip -------------------------------------------------------------
    MicroClip clip;
    if (!readMicroClip(clipPath, clip)) {
        std::printf("FAIL cannot read clip %s\n", clipPath.c_str());
        return 1;
    }
    std::printf("\nclip       %s (%.2f s, %zu tracks, %zu body keys)\n",
                clipPath.c_str(), clip.length, clip.tracks.size(),
                clip.body.size());


    // Rest-pose bone ownership is the reference the shader's fixed rule
    // reproduces; the posed ownership is what a delta voxelizer would use.
    MicroMeshGrid restBones = restGrid;
    assignMicroMeshBones(restBones, rig, {}, {});

    const float times[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (float t : times) {
        std::vector<glm::mat3> localRot;
        glm::vec3 bodyOffset(0.f);
        float bodyHeading = 0.f;
        clip.sample(t, rig, localRot, bodyOffset, bodyHeading);
        // The same chain rule the behaviour uses (micro_rig's composeBoneChain,
        // which MicroBehaviour::composeBones delegates to).
        std::vector<glm::mat3> boneRot;
        std::vector<glm::vec3> boneXform;
        composeBoneChain(rig, localRot, boneRot, boneXform);

        std::vector<MeshTri> posed;
        const auto tPose = Clock::now();
        poseMicroMesh(mesh, boneRot, boneXform, posed);
        const double poseMs = msSince(tPose);

        // The pose must MOVE something, or every number below is the rest
        // case wearing a costume.
        float maxMove = 0.f;
        for (size_t i = 0; i < posed.size(); ++i)
            for (int k = 0; k < 3; ++k)
                maxMove = std::max(maxMove,
                                   glm::length(posed[i].v[k] - mesh.tris[i].v[k]));

        const UnionGrid ug = unionGridFor(rest, posed);
        MicroMeshGrid posedGrid;
        const auto tVox = Clock::now();
        if (!voxelizeMicroMesh(posed, ug.dim, rest.voxel, ug.origin, posedGrid,
                               &err)) {
            std::printf("FAIL pose voxelization at t=%.2f: %s\n", t, err.c_str());
            return 1;
        }
        const double voxMs = msSince(tVox);

        const auto tBone = Clock::now();
        assignMicroMeshBones(posedGrid, rig, boneRot, boneXform);
        const double boneMs = msSince(tBone);

        // Ownership flips: the fixed rule (un-bone the posed point, measure
        // its own rest segment) against the old rule (nearest rest segment to
        // the posed point). A big percentage here is why the fur used to
        // slide; the fixed rule is the one implemented.
        size_t flips = 0, bonesSet = 0;
        for (int z = 0; z < ug.dim.z; ++z)
            for (int y = 0; y < ug.dim.y; ++y)
                for (int x = 0; x < ug.dim.x; ++x) {
                    const size_t i = posedGrid.idx(x, y, z);
                    if (!posedGrid.solid[i])
                        continue;
                    ++bonesSet;
                    const glm::vec3 p =
                        ug.origin +
                        (glm::vec3(float(x), float(y), float(z)) + 0.5f) *
                            rest.voxel;
                    if (rig.boneAt(p) != posedGrid.bone[i])
                        ++flips;
                }

        MicroField posedField;
        MeshFieldStats stats;
        const auto tBuild = Clock::now();
        if (!buildMicroFieldFromMesh(posedGrid, boneRot, boneXform, rest,
                                     posedField, &stats, &err)) {
            std::printf("FAIL posed field at t=%.2f: %s\n", t, err.c_str());
            return 1;
        }
        const double buildMs = msSince(tBuild);

        size_t added = 0, removed = 0, common = 0;
        MicroMeshGrid unionRest;
        unionRest.dim = ug.dim;
        unionRest.voxel = rest.voxel;
        unionRest.origin = ug.origin;
        unionRest.solid.assign(unionRest.size3(), 0);
        for (int z = 0; z < ug.dim.z; ++z)
            for (int y = 0; y < ug.dim.y; ++y)
                for (int x = 0; x < ug.dim.x; ++x) {
                    const size_t i = posedGrid.idx(x, y, z);
                    const bool a = posedGrid.solid[i] != 0;
                    const bool b = restSolidAt(rest, x, y, z, ug);
                    unionRest.solid[i] = b ? 1 : 0;
                    if (a && b)
                        ++common;
                    else if (a)
                        ++added;
                    else if (b)
                        ++removed;
                }

        std::printf("\n-- t=%.2f  (union grid %dx%dx%d, %.1f M cells) --\n", t,
                    ug.dim.x, ug.dim.y, ug.dim.z,
                    double(ug.dim.x) * double(ug.dim.y) * double(ug.dim.z) / 1e6);
        std::printf("  max vertex move %.4f m, pose %.1f ms\n", maxMove,
                    poseMs);
        std::printf("  voxel: %zu solid (shell %zu + interior %zu), leak=%s, "
                    "%.1f ms\n",
                    posedGrid.solidCount(), posedGrid.shellCells,
                    posedGrid.interiorCells, posedGrid.leak ? "YES" : "no",
                    voxMs);
        std::printf("  bones: %.1f ms, %zu/%zu cells flip vs the old rule "
                    "(%.1f%%)\n",
                    boneMs, flips, bonesSet,
                    bonesSet ? 100.0 * double(flips) / double(bonesSet) : 0.0);
        std::printf("  delta: added %zu, removed %zu, common %zu -> %.1f%% of "
                    "the solid set changes\n",
                    added, removed, common,
                    (added + removed + common)
                        ? 100.0 * double(added + removed) /
                              double(added + removed + common)
                        : 0.0);
        std::printf("  field: %.1f ms, appearance transferred %zu, fallback "
                    "%zu\n",
                    buildMs, stats.transferred, stats.fallbacks);
        {
            const EdgeCensus e = edgeCensus(posed);
            std::printf("  posed edges: manifold %zu, open %zu, "
                        "flipped-neighbour %zu\n",
                        e.manifold, e.open, e.same);
        }
        countBricksTouched(unionRest, posedGrid, 8);
    }

    return 0;
}
