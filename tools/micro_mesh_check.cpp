// micro_mesh_check.cpp - OFFLINE proof for the mesh-driven deformation path.
//
// The renderer still deforms the micro object by sampling its REST field
// through the inverse of a rigid bone transform (micro_overlay.comp), and that
// map is discontinuous wherever bone territory flips. This tool proves the
// replacement (micro_mesh.hpp) before any of it is wired into the app:
//
//   rest   - voxelize the REST mesh and compare it cell for cell with the
//            rest field baked from the .vxw records. The bound depends on the
//            mesh's PROVENANCE: a mesh extracted from those voxels must agree
//            at 99% (a disagreement means the voxelizer's sign or fill is
//            wrong), while an independently authored actor mesh sits at
//            95-99% because a triangle surface and a cell set differ by up to
//            one cell at the skin. Either way a wrong axis or scale collapses
//            the number to ~15%, which every later figure would inherit.
//   pose   - sample an animation clip, skin the mesh, voxelize the POSED
//            surface on a union grid, and report the delta (cells added and
//            removed), the bricks it touches, and how long each stage takes.
//            That is the whole P2 budget in one place: if the delta is small
//            and the pipeline is fast, only those bricks need re-uploading.
//
// Everything here is app-free (src/voxel only), so it runs headless:
//   ./build/vf_micro_mesh_check [layer.vxw] [mesh.stl] [skin] [clip.anim] [--up y|z]
#include "voxel/common.hpp"
#include "voxel/fur_cards.hpp"
#include "voxel/micro_clip.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_mesh.hpp"
#include "voxel/micro_rig.hpp"
#include "voxel/scenery.hpp"
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
    const std::string shared = VOXELFORGE_ASSET_DIR;
    // layer/micro/anim files are WORLD content: they live in the current scenery
    const std::string assets = vf::voxel::scenery::root();
    // `--up` names the axis the MESH FILE was authored in, exactly as the
    // app's `meshUp` does. It is load-bearing here: this tool voxelizes the
    // mesh and compares it with the layer's baked field, so a Z-up file read
    // as Y-up measures a rotated animal and every parity number below is
    // noise. With no flag, the canonical `alpaca_mesh.stl` (Z-up) decides.
    UpAxis up = UpAxis::Y;
    bool upSet = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--up" && i + 1 < argc) {
            const std::string v = argv[++i];
            upSet = true;
            if (v == "z" || v == "Z")
                up = UpAxis::Z;
            else if (v != "y" && v != "Y") {
                std::printf("FAIL --up expects y or z, got '%s'\n", v.c_str());
                return 1;
            }
        } else {
            pos.push_back(a);
        }
    }
    const std::string layerPath =
        pos.size() > 0 ? pos[0] : assets + "/micro/alpaca.vxw";
    const std::string meshPath =
        pos.size() > 1 ? pos[1] : shared + "/models/alpaca_mesh.stl";
    const std::string skinPath =
        pos.size() > 2 ? pos[2] : assets + "/micro/alpaca.skin";
    const std::string clipPath =
        pos.size() > 3 ? pos[3] : assets + "/micro/graze.anim";
    if (!upSet)
        up = meshPath.find("alpaca_mesh.stl") != std::string::npos ? UpAxis::Z
                                                                  : UpAxis::Y;
    std::printf("axis       %s%s\n", up == UpAxis::Z ? "z" : "y",
                upSet ? " (--up)" : " (inferred from the mesh path)");

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
    if (!loadMicroMesh(meshPath, skinPath, mesh, &err, up)) {
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
    // TWO BOUNDS, because the mesh has two possible provenances:
    //
    //   derived  - the mesh was extracted FROM these voxels (the historical
    //              `alpaca_from_voxels.stl`), so a cell-level disagreement is a
    //              VOXELIZER bug and the bound is 99%.
    //   authored - the canonical actor mesh is its own asset (a retopology of
    //              the same animal), and a triangle surface and an SDF-baked
    //              cell set legitimately differ by up to one cell all along the
    //              skin. The bound is 95%, which still catches the mistake this
    //              check exists for: a wrong AXIS or SCALE collapses agreement
    //              to ~15% (measured for a y/z mirror and a 180 deg yaw).
    const bool derivedMesh = meshPath.find("from_voxels") != std::string::npos;
    const double restBound = derivedMesh ? 99.0 : 95.0;
    if (!derivedMesh && agreePct < 99.0)
        std::printf("  note: an authored mesh sits in the 95-99%% band "
                    "(triangle vs cell, ~1 cell at the skin), not 99%%+\n");
    std::printf("REST_AGREEMENT=%.2f %s (bound %.0f, %s mesh)\n", agreePct,
                agreePct >= restBound ? "PASS" : "FAIL", restBound,
                derivedMesh ? "derived" : "authored");

    // ---- the appearance a mesh ACTOR would carry -------------------------
    // Phase B samples the material at every vertex ONCE
    // (assignMicroMeshAppearance), which is what removes the per-pixel field
    // lookup and with it the flat-grey fallback. Two numbers say how much of
    // the look that keeps: how many vertices resolve at all, and how much of a
    // cell's COLOUR is not explained by its material id (the records bake a
    // per-cell `shade`, so a material-only lane would flatten authored
    // variation - which is what still ties an actor to its field).
    {
        MeshAppearanceStats ast;
        assignMicroMeshAppearance(rest, mesh, &ast);
        std::printf("\n== per-vertex appearance ==\n");
        std::printf("  %zu vertices: %zu resolved, %zu missed (%.2f%%)\n",
                    ast.vertices, ast.assigned, ast.missed,
                    ast.vertices ? 100.0 * double(ast.missed) /
                                       double(ast.vertices)
                                 : 0.0);

        std::vector<float> ratio;
        ratio.reserve(size_t(fieldSolids));
        for (size_t k = 0; k < rest.size3(); ++k) {
            if (!rest.solid[k])
                continue;
            const int m = std::min<int>(rest.mat[k], kPaletteN - 1);
            const glm::vec3& pal = kPalette[m];
            const float pl =
                0.2126f * pal.r + 0.7152f * pal.g + 0.0722f * pal.b;
            if (!(pl > 0.01f))
                continue;
            const float cl = (0.2126f * rest.r[k] + 0.7152f * rest.g[k] +
                              0.0722f * rest.b[k]) / 255.f;
            ratio.push_back(cl / pl);
        }
        if (!ratio.empty()) {
            std::sort(ratio.begin(), ratio.end());
            auto pc = [&](double q) {
                return ratio[size_t(q * double(ratio.size() - 1))];
            };
            size_t flat = 0;
            for (float r : ratio)
                if (std::fabs(r - 1.f) < 0.02f)
                    ++flat;
            std::printf("  cell colour / palette: p1 %.3f p50 %.3f p99 %.3f, "
                        "%.1f%% within 2%% of the palette\n",
                        pc(0.01), pc(0.50), pc(0.99),
                        100.0 * double(flat) / double(ratio.size()));
        }
        // The COST of a flat per-corner material. A triangle whose three
        // vertices resolve to different materials renders as ONE of them (the
        // provoking vertex), so a material boundary turns triangle-shaped. The
        // class that actually shows is a coat triangle touching a bare one:
        // the alpaca's eyes, muzzle and hooves ride other materials INSIDE the
        // fleece. This is the number that says whether the packed lane is free
        // or whether the material has to travel per corner with a dominant
        // pick instead.
        const std::vector<uint8_t> furMats = FurParams {}.materials;
        auto growsFur = [&](uint8_t m) {
            return std::find(furMats.begin(), furMats.end(), m) != furMats.end();
        };
        size_t uni = 0, seam = 0, coatEdge = 0, unresolved = 0;
        for (size_t t = 0; t < mesh.tris.size(); ++t) {
            const uint8_t m[3] = { mesh.mat[t * 3], mesh.mat[t * 3 + 1],
                                   mesh.mat[t * 3 + 2] };
            if (m[0] == 255 || m[1] == 255 || m[2] == 255) {
                ++unresolved;
                continue;
            }
            if (m[0] == m[1] && m[1] == m[2]) {
                ++uni;
                continue;
            }
            ++seam;
            int fur = 0;
            for (uint8_t v : m)
                fur += growsFur(v) ? 1 : 0;
            if (fur > 0 && fur < 3)
                ++coatEdge;
        }
        std::printf("  triangles: %zu uniform, %zu material-seam (%.2f%%), "
                    "%zu coat-against-bare, %zu unresolved\n",
                    uni, seam,
                    mesh.tris.empty() ? 0.0
                                      : 100.0 * double(seam) / double(mesh.tris.size()),
                    coatEdge, unresolved);

        // The mesh's OPEN boundaries. An unpaired half-edge belongs to a
        // boundary LOOP, and this asset has them: nothing shows on screen
        // because the traced path seals the shell with the field's enclosure
        // fill, but a MESH-seeded fur root on such a triangle would grow off an
        // open edge into thin air. The edge COUNT mispredicts how visible that
        // is - a dozen edges in one loop on the flank is nothing, the same
        // dozen scattered over the muzzle is a fringe - so loops, total length
        // and the longest single loop are printed as well. Vertices are matched
        // at 1e-5 m (10 um) so a welded point agrees without being bit-identical.
        using P3 = std::array<int64_t, 3>;
        auto qd = [](const glm::vec3& p) {
            return P3{ std::llround(double(p.x) * 1e5),
                       std::llround(double(p.y) * 1e5),
                       std::llround(double(p.z) * 1e5) };
        };
        auto less3 = [](const P3& a, const P3& b) {
            return std::tie(a[0], a[1], a[2]) < std::tie(b[0], b[1], b[2]);
        };
        struct P3Hash {
            size_t operator()(const P3& p) const
            {
                size_t h = 1469598103934665603ull;
                for (int64_t v : p) {
                    h ^= size_t(v);
                    h *= 1099511628211ull;
                }
                return h;
            }
        };
        // std::hash has no std::array specialization, so the 6-int64 edge key
        // needs its own hasher too (the same FNV-1a fold over both endpoints).
        struct P6Hash {
            size_t operator()(const std::array<int64_t, 6>& p) const
            {
                size_t h = 1469598103934665603ull;
                for (int64_t v : p) {
                    h ^= size_t(v);
                    h *= 1099511628211ull;
                }
                return h;
            }
        };
        // Same fold, over nine int64s (a triangle's three quantised corners).
        struct P9Hash {
            size_t operator()(const std::array<int64_t, 9>& p) const
            {
                size_t h = 1469598103934665603ull;
                for (int64_t v : p) {
                    h ^= size_t(v);
                    h *= 1099511628211ull;
                }
                return h;
            }
        };
        std::unordered_map<std::array<int64_t, 6>, int, P6Hash> half;
        // A SECOND table, keyed DIRECTED. This is not decoration: an undirected
        // key pairs the two traversals of a shared edge, while a directed key
        // leaves them as two different entries - so on a perfectly closed
        // welded mesh a directed census reports ALMOST EVERY EDGE UNPAIRED. A
        // peer audit reading "39,849 singletons" has almost certainly keyed
        // directed; printing both here settles it instead of arguing about it.
        std::unordered_map<std::array<int64_t, 6>, int, P6Hash> halfDir;
        half.reserve(mesh.tris.size() * 3);
        for (const MeshTri& t : mesh.tris) {
            const P3 c[3] = { qd(t.v[0]), qd(t.v[1]), qd(t.v[2]) };
            for (int e = 0; e < 3; ++e) {
                const P3 a0 = c[e], b0 = c[(e + 1) % 3];
                halfDir[{ a0[0], a0[1], a0[2], b0[0], b0[1], b0[2] }]++;
                P3 a = a0, b = b0;
                if (less3(b, a))
                    std::swap(a, b);  // undirected: both windings share a key
                half[{ a[0], a[1], a[2], b[0], b[1], b[2] }]++;
            }
        }
        struct BEdge { P3 a, b; float len; };
        std::vector<BEdge> open;
        for (const auto& kv : half) {
            if (kv.second != 1)
                continue;  // paired (or over-shared): not a boundary
            const P3 a{ kv.first[0], kv.first[1], kv.first[2] };
            const P3 b{ kv.first[3], kv.first[4], kv.first[5] };
            const glm::vec3 pa(float(a[0]) * 1e-5f, float(a[1]) * 1e-5f,
                               float(a[2]) * 1e-5f);
            const glm::vec3 pb(float(b[0]) * 1e-5f, float(b[1]) * 1e-5f,
                               float(b[2]) * 1e-5f);
            open.push_back({ a, b, glm::length(pa - pb) });
        }
        // Connected components over the SHARED vertices: one component = one
        // loop. (A "loop" here is any edge-connected chain; a genuinely closed
        // surface has none of these at all.)
        auto findRoot = [](std::vector<int>& p, int x) {
            while (p[x] != x) {
                p[x] = p[p[x]];
                x = p[x];
            }
            return x;
        };
        std::unordered_map<P3, int, P3Hash> vid;
        std::vector<int> parent;
        auto vertexId = [&](const P3& p) {
            const auto it = vid.find(p);
            if (it != vid.end())
                return it->second;
            const int id = int(parent.size());
            parent.push_back(id);
            vid.emplace(p, id);
            return id;
        };
        for (const BEdge& e : open) {
            const int ra = findRoot(parent, vertexId(e.a));
            const int rb = findRoot(parent, vertexId(e.b));
            if (ra != rb)
                parent[ra] = rb;
        }
        std::map<int, std::pair<int, float>> comp;  // root -> (edges, metres)
        for (const BEdge& e : open) {
            auto& c = comp[findRoot(parent, vertexId(e.a))];
            c.first += 1;
            c.second += e.len;
        }
        // EDGE MULTIPLICITY, both conventions. Culling (and Phase C root
        // seeding) is decided by the UNDIRECTED table: multiplicity 1 means a
        // real boundary loop. The DIRECTED table is printed beside it because a
        // directed census reports ~everything unpaired on a CLOSED mesh (each
        // shared edge is traversed one way by each neighbour), and that is the
        // classic false positive for "this mesh is not manifold".
        auto histo = [](const std::unordered_map<std::array<int64_t, 6>, int,
                                                P6Hash>& m) {
            std::map<int, int> h;
            for (const auto& kv : m)
                h[std::min(kv.second, 5)]++;
            std::string s;
            for (const auto& kv : h)
                s += std::to_string(kv.first) + ":" + std::to_string(kv.second) + " ";
            return s;
        };
        std::printf("  edge multiplicity undirected {%s}  directed {%s}\n",
                    histo(half).c_str(), histo(halfDir).c_str());
        // Closed is not the same as CONSISTENTLY WOUND. An edge shared by two
        // triangles traversed in the SAME direction means the two faces
        // disagree about which side is outside, and back-face culling would then
        // delete the OUTER wall of one of them. Counting it is free once both
        // tables exist: an undirected edge with two traversals is proper iff the
        // directed table shows one of each orientation, and same-direction iff it
        // shows two of one and none of the other.
        size_t shared = 0, sameDir = 0;
        for (const auto& kv : half) {
            if (kv.second < 2)
                continue;
            ++shared;
            const auto& k = kv.first;
            const std::array<int64_t, 6> fwd{ k[0], k[1], k[2], k[3], k[4], k[5] };
            const std::array<int64_t, 6> rev{ k[3], k[4], k[5], k[0], k[1], k[2] };
            const size_t f = halfDir.count(fwd) ? halfDir.at(fwd) : 0;
            const size_t r = halfDir.count(rev) ? halfDir.at(rev) : 0;
            if ((f == kv.second && r == 0) || (r == kv.second && f == 0))
                sameDir += kv.second / 2;
        }
        std::printf("  orientation: %zu shared edges, %zu same-direction pair(s) "
                    "(%.1f%%) - culling is unsafe where this is non-zero\n",
                    shared, sameDir,
                    shared ? 100.0 * double(sameDir) / double(shared) : 0.0);
        // Duplicated triangles, counted DIRECTLY. Multiplicity 4 on an edge is
        // NOT evidence of a coincident face pair: a non-manifold edge with four
        // distinct incident faces looks the same, and inferring "~28 duplicated
        // triangles" from 86/3 is exactly the kind of arithmetic shortcut that
        // started the boundary-loop mistake (86 is not even divisible by 3).
        // Coincident faces are what would revive the depth-tie theory, so they
        // get measured: two triangles are duplicates when their three quantised
        // corners agree as a SET, whatever the winding.
        {
            std::unordered_map<std::array<int64_t, 9>, int, P9Hash> face;
            face.reserve(mesh.tris.size() * 2);
            for (const MeshTri& t : mesh.tris) {
                std::array<P3, 3> c = { qd(t.v[0]), qd(t.v[1]), qd(t.v[2]) };
                std::sort(c.begin(), c.end(),
                          [&less3](const P3& a, const P3& b) { return less3(a, b); });
                face[{ c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2],
                       c[2][0], c[2][1], c[2][2] }]++;
            }
            size_t dupFaces = 0, dupExtra = 0;
            for (const auto& kv : face)
                if (kv.second > 1) {
                    ++dupFaces;
                    dupExtra += size_t(kv.second - 1);
                }
            std::printf("  coincident faces: %zu distinct triangle(s) repeated, "
                        "%zu redundant triangle(s) total -> depth ties %s\n",
                        dupFaces, dupExtra, dupExtra ? "ARE possible" : "IMPOSSIBLE");
        }
        float totalLen = 0.f, longestLen = 0.f;
        int longestEdges = 0;
        for (const auto& kv : comp) {
            totalLen += kv.second.second;
            if (kv.second.second > longestLen) {
                longestLen = kv.second.second;
                longestEdges = kv.second.first;
            }
        }
        std::printf("  boundary: %zu open edges in %zu loops, total %.4f m, "
                    "longest %.4f m (%d edges), mean %.4f m/loop\n",
                    open.size(), comp.size(), double(totalLen),
                    double(longestLen), longestEdges,
                    comp.empty() ? 0.0 : double(totalLen) / double(comp.size()));
    }

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
        poseMicroMesh(mesh, rig, boneRot, boneXform, posed);
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

    // ---- DELTA DEFORMER (P2) ---------------------------------------------
    //
    // The design being proved: instead of deforming a REST field through the
    // inverse of a bone transform (discontinuous wherever bone territory
    // flips - the whole bug class), skin the MESH, voxelize the POSED surface,
    // and apply only the cells that changed.
    //
    // This section is the gate for "only the cells that changed", and it is
    // deliberately EXACT rather than statistical:
    //
    //   parity - the deformer's even-odd occupancy against a FULL voxelization
    //            of the same pose. It is maintained per scanline row as an XOR
    //            of swept intervals, so a mis-ordered event or a ray that is
    //            not bit-identical to the voxelizer's shows up as a seam.
    //   solid  - the same, after the enclosure seal (which is global, and so
    //            re-derived from the border every update).
    //   sdf    - bit for bit. The window refresh seeds its edge from the
    //            stored distance; that is exact only because BfsDist is a pure
    //            Chebyshev transform and the window is wider than its cap.
    //   extents- solidMin/solidMax drive the footprint and the ground clamp.
    //
    // Appearance is deliberately NOT compared: it is STICKY for a cell that
    // stays solid, so a walking animal's fleece does not crawl over the body.
    // Only a newly solid cell takes a material, through the rest field.
    std::printf("\n== DELTA DEFORMER (P2) ==\n");

    struct ClipCase {
        std::string label;
        std::string path;
        MicroClip clip;
    };
    std::vector<ClipCase> clips;
    clips.push_back(ClipCase { "graze", clipPath, clip });
    {
        const std::string walkPath =
            argc > 5 ? argv[5] : assets + "/micro/walk.anim";
        MicroClip walk;
        if (readMicroClip(walkPath, walk))
            clips.push_back(ClipCase { "walk", walkPath, walk });
        else
            std::printf("  (no walk clip at %s)\n", walkPath.c_str());
    }

    // The union grid: one lattice that contains EVERY pose of every clip, so
    // the deformer never has to grow or re-origin mid-animation (a resize
    // would invalidate every cell index the GPU holds).
    const float dt = 0.02f;
    MicroUnionBounds bounds;
    bounds.add(mesh.tris);
    {
        const auto tBounds = Clock::now();
        std::vector<MeshTri> posed;
        for (const ClipCase& c : clips)
            for (float t = 0.f; t <= c.clip.length + 1e-4f; t += dt) {
                std::vector<glm::mat3> localRot;
                glm::vec3 bodyOffset(0.f);
                float bodyHeading = 0.f;
                c.clip.sample(t, rig, localRot, bodyOffset, bodyHeading);
                std::vector<glm::mat3> br;
                std::vector<glm::vec3> bx;
                composeBoneChain(rig, localRot, br, bx);
                poseMicroMesh(mesh, rig, br, bx, posed);
                bounds.add(posed);
            }
        const MicroUnionGrid ug = microUnionGrid(bounds, rest.voxel);
        std::printf("  union grid %dx%dx%d (%.2f M cells) from %zu clip(s), "
                    "%.0f ms\n",
                    ug.dim.x, ug.dim.y, ug.dim.z,
                    double(ug.dim.x) * double(ug.dim.y) * double(ug.dim.z) /
                        1e6,
                    clips.size(), msSince(tBounds));
    }
    const MicroUnionGrid grid = microUnionGrid(bounds, rest.voxel);
    if (grid.dim.x <= 0) {
        std::printf("FAIL union grid\n");
        return 1;
    }

    MicroDeformer def;
    MicroDeltaStats initStats;
    const auto tInit = Clock::now();
    if (!def.init(mesh, rig, rest, grid, &initStats, &err)) {
        std::printf("FAIL deformer init: %s\n", err.c_str());
        return 1;
    }
    const double initMs = msSince(tInit);
    std::printf("  init %.0f ms, %zu solid cells (%.1f%% of the grid)\n",
                initMs, def.solidCount(),
                100.0 * double(def.solidCount()) / double(def.field.size3()));

    // The deformer's lattice must line up with the rest field's: both snap to
    // the object's min corner, so the difference is a whole number of cells.
    // If it is not (or the sign is wrong) the rest projection below reads
    // almost nothing, which is why it is printed.
    const glm::ivec3 cellShift = glm::ivec3(
        glm::round((rest.origin - grid.origin) / rest.voxel));
    auto restCellSolid = [&](int x, int y, int z) {
        const int rx = x - cellShift.x, ry = y - cellShift.y,
                  rz = z - cellShift.z;
        return rest.inside(rx, ry, rz) && rest.solid[rest.idx(rx, ry, rz)];
    };
    {
        size_t inBoth = 0, defOnly = 0, restOnly = 0;
        for (int z = 0; z < grid.dim.z; ++z)
            for (int y = 0; y < grid.dim.y; ++y)
                for (int x = 0; x < grid.dim.x; ++x) {
                    const size_t i = def.field.idx(x, y, z);
                    const bool a = def.field.solid[i] != 0;
                    const bool b = restCellSolid(x, y, z);
                    if (a && b)
                        ++inBoth;
                    else if (a)
                        ++defOnly;
                    else if (b)
                        ++restOnly;
                }
        std::printf("  rest projection: cell shift %d %d %d, agree %zu, "
                    "deformer-only %zu, rest-only %zu\n",
                    cellShift.x, cellShift.y, cellShift.z, inBoth, defOnly,
                    restOnly);
    }

    bool allExact = true;
    auto compareToFull = [&](const char* label,
                             const std::vector<glm::mat3>& br,
                             const std::vector<glm::vec3>& bx) {
        std::vector<MeshTri> posed;
        poseMicroMesh(mesh, rig, br, bx, posed);
        MicroMeshGrid rg;
        if (!voxelizeMicroMesh(posed, grid.dim, grid.voxel, grid.origin, rg,
                               &err)) {
            std::printf("  FAIL reference voxelization at %s: %s\n", label,
                        err.c_str());
            allExact = false;
            return;
        }
        MicroField refField;
        MeshFieldStats fst;
        if (!buildMicroFieldFromMesh(rg, br, bx, rest, refField, &fst, &err)) {
            std::printf("  FAIL reference field at %s: %s\n", label,
                        err.c_str());
            allExact = false;
            return;
        }
        size_t parityBad = 0, solidBad = 0, sdfBad = 0;
        size_t sdfSolidBad = 0, sdfAirBad = 0, borderBad = 0;
        float maxSdf = 0.f;
        int shown = 0;
        for (int z = 0; z < grid.dim.z; ++z)
            for (int y = 0; y < grid.dim.y; ++y)
                for (int x = 0; x < grid.dim.x; ++x) {
                    const size_t i = def.field.idx(x, y, z);
                    if (def.parity[i] != rg.raw[i])
                        ++parityBad;
                    if (def.field.solid[i] != rg.solid[i])
                        ++solidBad;
                    const float d =
                        std::fabs(def.field.sdf[i] - refField.sdf[i]);
                    if (d != 0.f) {
                        ++sdfBad;
                        if (rg.solid[i])
                            ++sdfSolidBad;
                        else
                            ++sdfAirBad;
                        const int tb = std::min(
                            std::min(x, grid.dim.x - 1 - x),
                            std::min(std::min(y, grid.dim.y - 1 - y),
                                     z));
                        const int tb2 = std::min(tb, grid.dim.z - 1 - z);
                        if (tb2 <= kMicroSdfMaxSteps)
                            ++borderBad;
                        if (shown < 6) {
                            ++shown;
                            // The ground truth, brute force: the Chebyshev
                            // distance from this cell to the nearest solid
                            // cell, over the deformer's OWN solid set. That
                            // separates "the window BFS is wrong" from "the
                            // reference disagrees with a plain distance".
                            int best = 1 << 20;
                            for (int zz = 0; zz < grid.dim.z; ++zz)
                                for (int yy = 0; yy < grid.dim.y; ++yy)
                                    for (int xx = 0; xx < grid.dim.x; ++xx) {
                                        if (!def.field.solid[def.field.idx(
                                                xx, yy, zz)])
                                            continue;
                                        const int dd = std::max(
                                            std::max(std::abs(xx - x),
                                                     std::abs(yy - y)),
                                            std::abs(zz - z));
                                        best = std::min(best, dd);
                                        if (best == 0)
                                            break;
                                    }
                            std::printf("    bad (%d %d %d) solid %d ref %d | "
                                        "def %.1f vox ref %.1f vox | %d to "
                                        "border | brute %.1f vox\n",
                                        x, y, z, int(def.field.solid[i]),
                                        int(rg.solid[i]),
                                        double(def.field.sdf[i] / grid.voxel),
                                        double(refField.sdf[i] / grid.voxel),
                                        tb2, double(best) - 0.5);
                        }
                        maxSdf = std::max(maxSdf, d);
                    }
                }
        const bool extentsOk = def.field.solidMin == refField.solidMin &&
                               def.field.solidMax == refField.solidMax;
        const bool ok = parityBad == 0 && solidBad == 0 && sdfBad == 0 &&
                        extentsOk;
        allExact = allExact && ok;
        std::printf("  %-10s parity %zu bad | solid %zu bad | sdf %zu bad "
                    "(%zu solid, %zu air, %zu near border; max %.3g m) | "
                    "extents %s -> %s\n",
                    label, parityBad, solidBad, sdfBad, sdfSolidBad, sdfAirBad,
                    borderBad, double(maxSdf),
                    extentsOk ? "match" : "DIFFER", ok ? "ok" : "MISMATCH");
    };

    double sumPose = 0, sumDelta = 0, sumSeal = 0, sumSweep = 0, sumSdf = 0,
           sumTotal = 0, maxTotal = 0, sumRegion = 0, sumBfs = 0, sumWrite = 0;
    size_t framesTotal = 0, addedTotal = 0, removedTotal = 0, trisTotal = 0,
           dirtyTotal = 0, sdfTotal = 0, flipsTotal = 0, eventsTotal = 0,
           sealedTotal = 0;

    for (const ClipCase& c : clips) {
        std::printf("\n-- %s: %.2f s at %.3f s steps --\n", c.label.c_str(),
                    c.clip.length, dt);
        const int frames = int(std::floor(c.clip.length / dt + 1e-4f)) + 1;
        std::vector<int> checkAt;
        for (int f : { 0, frames / 4, frames / 2, (3 * frames) / 4,
                       frames - 1 }) {
            bool dup = false;
            for (int g : checkAt)
                if (g == f)
                    dup = true;
            if (!dup)
                checkAt.push_back(f);
        }
        std::sort(checkAt.begin(), checkAt.end());
        size_t cp = 0;
        for (int f = 0; f < frames; ++f) {
            const float t = std::min(float(f) * dt, c.clip.length);
            std::vector<glm::mat3> localRot;
            glm::vec3 bodyOffset(0.f);
            float bodyHeading = 0.f;
            c.clip.sample(t, rig, localRot, bodyOffset, bodyHeading);
            std::vector<glm::mat3> br;
            std::vector<glm::vec3> bx;
            composeBoneChain(rig, localRot, br, bx);

            MicroDeltaStats st;
            if (!def.updatePose(br, bx, &st, &err)) {
                std::printf("FAIL updatePose at %s t=%.2f: %s\n",
                            c.label.c_str(), t, err.c_str());
                return 1;
            }
            ++framesTotal;
            sumPose += st.poseMs;
            sumDelta += st.deltaMs;
            sumSeal += st.sealMs;
            sumSweep += st.sweepMs;
            sumSdf += st.sdfMs;
            sumRegion += st.regionMs;
            sumBfs += st.bfsMs;
            sumWrite += st.writeMs;
            sumTotal += st.totalMs;
            maxTotal = std::max(maxTotal, st.totalMs);
            addedTotal += st.added;
            removedTotal += st.removed;
            trisTotal += st.movedTris;
            dirtyTotal += st.dirtyBricks;
            sdfTotal += st.sdfCells;
            flipsTotal += st.parityFlips;
            eventsTotal += st.rowEvents;
            sealedTotal += st.sealAdded;

            if (cp < checkAt.size() && checkAt[cp] == f) {
                ++cp;
                char label[64];
                std::snprintf(label, sizeof(label), "t=%.2f", double(t));
                compareToFull(label, br, bx);
            }
        }
    }

    const double nFrames = double(std::max<size_t>(framesTotal, 1));
    std::printf("\n  %zu frames total (%.1f per clip-second at %.3f s)\n",
                framesTotal, 1.0 / double(dt), double(dt));
    std::printf("  per frame: re-posed %zu tris, %zu row events, %zu parity "
                "flips, %zu added (%zu seal-only), %zu removed, %zu sdf cells, "
                "%zu dirty bricks\n",
                size_t(double(trisTotal) / nFrames),
                size_t(double(eventsTotal) / nFrames),
                size_t(double(flipsTotal) / nFrames),
                size_t(double(addedTotal) / nFrames),
                size_t(double(sealedTotal) / nFrames),
                size_t(double(removedTotal) / nFrames),
                size_t(double(sdfTotal) / nFrames),
                size_t(double(dirtyTotal) / nFrames));
    std::printf("  ms/frame: pose %.2f | delta %.2f | seal %.2f | sweep %.2f | "
                "sdf %.2f (region %.2f, bfs %.2f, write %.2f) | "
                "TOTAL %.2f (max %.2f)\n",
                sumPose / nFrames, sumDelta / nFrames, sumSeal / nFrames,
                sumSweep / nFrames, sumSdf / nFrames, sumRegion / nFrames,
                sumBfs / nFrames, sumWrite / nFrames, sumTotal / nFrames,
                maxTotal);
    std::printf("DELTA_EXACT=%s\n", allExact ? "PASS" : "FAIL");
    if (!allExact)
        return 1;

    return 0;
}
