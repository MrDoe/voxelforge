// Unit tests for the voxel-object authoring helpers in common.hpp:
// extra SDF primitives (capsule/ellipsoid/cone/smin) and voxel stamps,
// plus cross-checks that the baked VoxelField matches the analytic truth.
#include "voxel/common.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/picking.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <doctest/doctest.h>
#include <cmath>

using namespace vf::voxel;

std::string allLayersManifest()
{
    namespace fs = std::filesystem;
    static std::string path;
    if (path.empty()) {
        std::string dir = std::string(VOXELFORGE_ASSET_DIR);
        std::vector<std::pair<std::string, std::string>> files;
        for (fs::directory_iterator it(dir), end; it != end; ++it) {
            std::string f = it->path().filename().string();
            if (it->path().extension() != ".vxw" || f == "world.vxw")
                continue;
            files.push_back({ f, f == "landscape.vxw" ? "landscape"
                                : (f == "bushes.vxw" ? "scatter" : "object") });
        }
        std::sort(files.begin(), files.end());
        std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) {
            return a.second == "landscape"; // landscape claims last
        });
        std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) {
            return a.first == "ai_edits.vxw"; // ai_edits claims first
        });
        std::string j = "{\"layers\":[";
        bool first = true;
        for (auto& [f, role] : files) {
            if (!first) j += ",";
            first = false;
            j += "{\"file\":\"" + f + "\",\"name\":\"" + f.substr(0, f.size() - 4) +
                 "\",\"role\":\"" + role +
                 "\",\"pos\":[0,0,0],\"rotDeg\":0,\"enabled\":true,\"listed\":true}";
        }
        j += "]}";
        path = dir + "/world_all.json";
        std::ofstream(path) << j;
    }
    return path;
}
LayeredWorld& testLayeredWorld()
{
    static LayeredWorld lw;
    static bool ok = lw.load(allLayersManifest());
    REQUIRE(ok);
    return lw;
}
const VoxelField& testField()
{
    return testLayeredWorld().field();
}

TEST_CASE("authoring primitives: capsule")
{
    const glm::vec3 a(0.f, -1.f, 0.f), b(0.f, 1.f, 0.f);
    CHECK(sdCapsule({ 0.f, 0.f, 0.f }, a, b, 0.25f) == doctest::Approx(-0.25f));
    CHECK(sdCapsule({ 0.f, 1.5f, 0.f }, a, b, 0.25f) == doctest::Approx(0.25f));
    CHECK(sdCapsule({ 0.75f, 0.f, 0.f }, a, b, 0.25f) == doctest::Approx(0.50f));
    // past the tip: distance to the cap sphere centre minus radius
    CHECK(sdCapsule({ 0.f, 2.f, 0.f }, a, b, 0.25f) == doctest::Approx(0.75f));
}

TEST_CASE("authoring primitives: ellipsoid")
{
    const glm::vec3 c(2.f, 1.f, -3.f);
    // spheres must be exact
    float r = 0.8f;
    CHECK(sdEllipsoid({ 2.f, 1.f + r, -3.f }, c, { r, r, r }) ==
          doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(sdEllipsoid(c, c, { r, r, r }) == doctest::Approx(-r).epsilon(1e-5));
    // anisotropic: sign flips across the surface along each axis
    glm::vec3 rad { 1.0f, 0.5f, 0.25f };
    CHECK(sdEllipsoid({ 2.f, 1.6f, -3.f }, c, rad) > 0.0f); // above top (y extent .5)
    CHECK(sdEllipsoid({ 3.5f, 1.f, -3.f }, c, rad) > 0.0f); // beyond x extent 1.0
    CHECK(sdEllipsoid({ 2.9f, 1.f, -3.f }, c, rad) < 0.0f); // inside x extent
}

TEST_CASE("authoring primitives: coneY taper")
{
    const glm::vec2 c(10.f, -5.f);
    // wide at the base, narrow at the top
    CHECK(sdConeY({ 10.5f, 0.1f, -5.f }, c, 0.f, 2.f, 1.f, 0.2f) < 0.0f);
    CHECK(sdConeY({ 10.5f, 1.9f, -5.f }, c, 0.f, 2.f, 1.f, 0.2f) > 0.0f);
    // straight above/below the flat caps
    CHECK(sdConeY({ 10.f, 2.5f, -5.f }, c, 0.f, 2.f, 1.f, 0.2f) ==
          doctest::Approx(0.5f));
    CHECK(sdConeY({ 10.f, -0.5f, -5.f }, c, 0.f, 2.f, 1.f, 0.2f) ==
          doctest::Approx(0.5f));
}

TEST_CASE("authoring primitives: smin properties")
{
    // far apart: equals plain min
    CHECK(smin(0.f, 10.f, 1.f) == doctest::Approx(0.0f));
    CHECK(smin(10.f, 0.f, 1.f) == doctest::Approx(0.0f));
    // equal inputs: dips by k/4
    CHECK(smin(0.f, 0.f, 2.f) == doctest::Approx(-0.5f));
    // never above min
    for (int i = 0; i < 20; ++i) {
        float a = float(i) * 0.37f, b = 3.1f - float(i) * 0.21f;
        CHECK(smin(a, b, 0.8f) <= glm::min(a, b) + 1e-6f);
    }
}

TEST_CASE("stamp: hit, pocket, and conservative-distance semantics")
{
    static const StampCell cells[] = {
        { 0, 0, 0, 4 }, { 1, 0, 0, 4 }, { 2, 0, 0, 4 }, // base row of rock
        { 0, 1, 0, 6 },                                 // one wood cube on cell (0,0)
    };
    const glm::vec3 o(10.f, 0.f, -5.f);

    // repeat queries exercise both index-build and cached-index paths
    StampHit first = stampAt({ 10.f, 0.f, -5.f }, o, cells, 4);
    StampHit again = stampAt({ 10.f, 0.f, -5.f }, o, cells, 4);
    CHECK(first.d == again.d);

    // centre of a rock cell: fully inside
    CHECK(first.d == doctest::Approx(-0.05f));
    CHECK(first.mat == 4);

    // inside the wood cell above cell (0,0)
    StampHit wood = stampAt({ 10.f, 0.1f, -5.f }, o, cells, 4);
    CHECK(wood.d < 0.0f);
    CHECK(wood.mat == 6);

    // empty pocket inside the AABB: exact vertical clearance (+0.05 m) to the
    // unambiguous rock neighbour below cell (2,0)
    StampHit pocket = stampAt({ 10.2f, 0.1f, -5.f }, o, cells, 4);
    CHECK(pocket.d == doctest::Approx(0.05f).epsilon(1e-4));
    CHECK(pocket.mat == 4);

    // well outside the AABB: positive but a conservative UNDERESTIMATE of the
    // true distance (AABB top face sits 4.85 m away; we must return less)
    StampHit far_ = stampAt({ 10.f, 5.f, -5.f }, o, cells, 4);
    CHECK(far_.d > 4.0f);
    CHECK(far_.d < 4.85f);

    // empty stamp never claims geometry
    StampHit none = stampAt(o, o, cells, 0);
    CHECK(none.d > 1000.0f);
}

TEST_CASE("paddock fence: rails solid, gate open")
{
    const HeightMap& hm = sharedHeightmap();

    // north side lower-rail midpoint: solid wood
    glm::vec2 mid(0.5f * (kPaddockMin.x + kPaddockMax.x), kPaddockMax.y);
    float g = hm.sample(mid.x, mid.y);
    ObjHit rail = fenceAt({ mid.x, g + 0.36f, mid.y });
    CHECK(rail.d < 0.0f);
    CHECK(rail.mat == 6);

    // gate opening on the west side is clear at both rail heights
    float gw = hm.sample(kPaddockMin.x, kGateCenter);
    CHECK(fenceAt({ kPaddockMin.x, gw + 0.36f, kGateCenter }).d > 0.2f);
    CHECK(fenceAt({ kPaddockMin.x, gw + 0.70f, kGateCenter }).d > 0.2f);

    // west rails exist away from the gate
    glm::vec2 wz(kPaddockMin.x, kPaddockMin.y + 1.2f);
    float gww = hm.sample(wz.x, wz.y);
    CHECK(fenceAt({ wz.x, gww + 0.36f, wz.y }).d < 0.0f);
}

TEST_CASE("alpaca: wool body, dark legs and muzzle")
{
    const HeightMap& hm = sharedHeightmap();
    const glm::vec3 o(kAlpacaSpot.x, sharedHeightmap().sample(kAlpacaSpot.x, kAlpacaSpot.y),
                      kAlpacaSpot.y);

    ObjHit body = alpacaAt({ o.x, o.y + 0.64f, o.z });
    CHECK(body.d < -0.15f);
    CHECK(body.mat == 5);

    ObjHit leg = alpacaAt({ o.x - 0.30f, o.y + 0.25f, o.z + 0.15f });
    CHECK(leg.d < 0.0f);
    CHECK(leg.mat == 2);

    // muzzle sits ahead of the head, below ear level
    ObjHit muzzle = alpacaAt({ o.x - 0.86f, o.y + 1.19f, o.z });
    CHECK(muzzle.d < 0.0f);
    CHECK(muzzle.mat == 2);

    // ears are wool
    CHECK(alpacaAt({ o.x - 0.59f, o.y + 1.40f, o.z + 0.07f }).mat == 5);
}

TEST_CASE("carve: subtractive cylinder cuts a hole through terrain and objects")
{
    // generate carve cells with the same rasterizer the app uses
    EditableWorld carver(std::string(VOXELFORGE_ASSET_DIR),
                         std::string(EditableWorld::kCarveFileName),
                         std::string(EditableWorld::kCarveLayerName),
                         std::string("carve"));
    const glm::ivec3 anchor = worldToVoxel(glm::vec3(0.f, 0.f, 0.f));
    std::vector<VoxelRecord> crecs =
        carver.makeOrientedCylinder(anchor, glm::vec3(0.f, -1.f, 0.f), 1.0f, 1.5f, 6, /*carve=*/true);
    REQUIRE(!crecs.empty());
    std::vector<uint32_t> carveCells;
    std::vector<uint8_t> carveMats;
    for (auto& r : crecs) {
        carveCells.push_back(((uint32_t)r.x << 20) | ((uint32_t)r.y << 10) | (uint32_t)r.z);
        carveMats.push_back(r.materialId);
    }

    // build a flat terrain patch with its surface at the anchor column
    const int latN = 1024;
    std::vector<int16_t> colTop(latN * latN, -1);
    std::vector<uint8_t> colMat(latN * latN, 0);
    for (int dz = -60; dz <= 60; ++dz)
        for (int dx = -60; dx <= 60; ++dx) {
            int x = anchor.x + dx, z = anchor.z + dz;
            if (x < 0 || z < 0 || x >= latN || z >= latN)
                continue;
            colTop[size_t(z) * latN + x] = (int16_t)anchor.y;
            colMat[size_t(z) * latN + x] = 1;
        }

    VoxelField f;
    std::vector<VoxelRecord> recs0;
    f.build(recs0, colTop, colMat, {}, {}, {}, carveCells, carveMats);

    // the centred carve cell sits inside the subtracted volume -> reads as air
    auto s = f.sampleWorld(glm::vec3(0.f, 0.f, 0.f));
    CHECK(s.d > 0.0f);

    // outside the carve radius the terrain is intact
    auto t = f.sampleWorld(glm::vec3(3.f, -0.1f, 0.f));
    CHECK(t.d <= 0.0f);
}

TEST_CASE("raise: the dome lifts the whole footprint to the brush depth")
{
    EditableWorld raiser(std::string(VOXELFORGE_ASSET_DIR),
                         std::string(EditableWorld::kRaiseFileName),
                         std::string(EditableWorld::kRaiseLayerName),
                         std::string("raise"));
    const glm::ivec3 anchor = worldToVoxel(glm::vec3(0.f, 0.f, 0.f));
    std::vector<VoxelRecord> drecs =
        raiser.makeDome(anchor, glm::vec3(0.f, 1.f, 0.f), 1.0f, 1.5f, 6);
    REQUIRE(!drecs.empty());
    std::vector<uint32_t> raiseCells;
    std::vector<uint8_t> raiseMats;
    for (auto& r : drecs) {
        raiseCells.push_back(((uint32_t)r.x << 20) | ((uint32_t)r.y << 10) | (uint32_t)r.z);
        raiseMats.push_back(r.materialId);
    }

    const int latN = 1024;
    std::vector<int16_t> colTop(latN * latN, -1);
    std::vector<uint8_t> colMat(latN * latN, 0);
    for (int dz = -60; dz <= 60; ++dz)
        for (int dx = -60; dx <= 60; ++dx) {
            int x = anchor.x + dx, z = anchor.z + dz;
            if (x < 0 || z < 0 || x >= latN || z >= latN)
                continue;
            colTop[size_t(z) * latN + x] = (int16_t)anchor.y;
            colMat[size_t(z) * latN + x] = 1;
        }

    VoxelField f;
    std::vector<VoxelRecord> recs0;
    f.build(recs0, colTop, colMat, {}, {}, {}, {}, {}, raiseCells, raiseMats);

    const auto& ht = f.heightTexture();
    // flat ground well outside the footprint is the reference level (the
    // centre column is itself raised, so it cannot be its own baseline)
    const float ground = ht[size_t(anchor.z) * latN + anchor.x + 30].x;
    const float hCenter = ht[size_t(anchor.z) * latN + anchor.x].x;
    const int rim = anchor.x + int(1.0f / VOXEL); // one radius out from the centre
    const float hRim = ht[size_t(anchor.z) * latN + rim].x;
    // The Add volume is an extruded footprint closed by a fillet, NOT a dome
    // that tapers to nothing at the rim: the whole footprint rises to
    // (depth - c) and the centre to the full depth, which is what makes a
    // clicked wall read as thicker rather than as a bulge.
    CHECK(hCenter > ground + 1.3f); // full brush depth at the centre
    CHECK(hRim > ground + 0.9f);    // the rim is nearly as high
    CHECK(hCenter > hRim);          // the centre is still the peak
    CHECK(hCenter - hRim < 0.7f);   // by the fillet only, not a tapering spike
}

// --- mesh -> voxel conversion (tools/mesh_to_voxel.cpp + vf_mcp import_mesh)
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"

// 12 triangles forming an axis-aligned box [min, max]
static std::vector<MeshTri> boxTris(const glm::vec3& mn, const glm::vec3& mx)
{
    const glm::vec3 v[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mn.y, mx.z },
        { mn.x, mn.y, mx.z }, { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z },
        { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
    };
    const int idx[12][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 },
                             { 0, 1, 5 }, { 0, 5, 4 }, { 3, 6, 2 }, { 3, 7, 6 },
                             { 0, 4, 7 }, { 0, 7, 3 }, { 1, 2, 6 }, { 1, 6, 5 } };
    std::vector<MeshTri> out;
    for (const auto& f : idx) {
        MeshTri t;
        for (int k = 0; k < 3; ++k)
            t.v[k] = v[f[k]];
        out.push_back(t);
    }
    return out;
}

TEST_CASE("mesh voxelization: unit cube is fully solid")
{
    // A 1 m cube on a 0.1 m lattice. Conservative voxelization marks every
    // cell the surface passes through, so the solid block is up to one voxel
    // larger than the mesh per axis (that one-cell overlap is what keeps the
    // shell watertight): here 11x11x11 = 1331 cells, grid 13 wide (the flood
    // fill's air ring adds a cell each side).
    std::vector<MeshTri> tris = boxTris(glm::vec3(0.f), glm::vec3(1.f));
    VoxelizedMesh vm;
    std::string err;
    REQUIRE(voxelizeMesh(tris, vm, MeshVoxelOptions {}, &err));
    CHECK(vm.nx == 13);
    CHECK(vm.ny == 13);
    CHECK(vm.nz == 13);
    CHECK(vm.solidCount() == 1331);
    CHECK_FALSE(vm.leak);

    // the anti-hollow regression: a probe through the middle of the object
    // must be solid all the way, not solid/air/solid
    const int cx = 6, cz = 6;
    for (int y = 1; y < vm.ny - 1; ++y) // skip the flood-fill air ring
        CHECK(vm.cell[vm.idx(cx, y, cz)] != 0);
}

TEST_CASE("mesh voxelization: shell-only mode emits no interior")
{
    std::vector<MeshTri> tris = boxTris(glm::vec3(0.f), glm::vec3(1.f));
    MeshVoxelOptions o;
    o.solid = false;
    VoxelizedMesh vm;
    std::string err;
    REQUIRE(voxelizeMesh(tris, vm, o, &err));
    int shell = 0, interior = 0;
    for (uint8_t c : vm.cell)
        if (c == 1)
            ++shell;
        else if (c == 2)
            ++interior;
    CHECK(interior == 0);
    CHECK(shell < 1331); // strictly fewer than the solid fill
}

TEST_CASE("mesh voxelization: a non-watertight mesh reports a leak")
{
    // a box missing its top face: the interior is open to the outside, so the
    // exterior flood escapes and no interior can be classified
    std::vector<MeshTri> tris = boxTris(glm::vec3(0.f), glm::vec3(1.f));
    tris.erase(tris.begin() + 4, tris.begin() + 6); // drop the two top faces
    VoxelizedMesh vm;
    std::string err;
    REQUIRE(voxelizeMesh(tris, vm, MeshVoxelOptions {}, &err));
    CHECK(vm.leak);
}

TEST_CASE("meshToRecords: bottom-center placement and lattice bounds")
{
    std::vector<MeshTri> tris = boxTris(glm::vec3(0.f), glm::vec3(1.f));
    VoxelizedMesh vm;
    std::string err;
    REQUIRE(voxelizeMesh(tris, vm, MeshVoxelOptions {}, &err));

    std::vector<VoxelRecord> recs;
    int clamped = 0;
    // anchor = the object's bottom-center cell
    const glm::ivec3 anchor(500, 500, 500);
    REQUIRE(meshToRecords(vm, anchor, recs, &clamped) == 1331);
    CHECK(clamped == 0);

    int mn[3] = { 1 << 30, 1 << 30, 1 << 30 };
    int mx[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
    for (const auto& r : recs) {
        int c[3] = { r.x, r.y, r.z };
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], c[a]);
            mx[a] = std::max(mx[a], c[a]);
        }
    }
    // the 11-cell solid block is centred on the anchor and its base sits ON
    // the anchor cell (not a voxel above it)
    CHECK(mn[0] == anchor.x - 5);
    CHECK(mx[0] == anchor.x + 5);
    CHECK(mn[1] == anchor.y);
    CHECK(mx[1] == anchor.y + 10);
    CHECK(mn[2] == anchor.z - 5);
    CHECK(mx[2] == anchor.z + 5);

    // out-of-bounds cells are dropped, not clamped into the world
    recs.clear();
    clamped = 0;
    const glm::ivec3 edge(1019, 500, 500);
    meshToRecords(vm, edge, recs, &clamped);
    CHECK(clamped > 0);
    for (const auto& r : recs)
        CHECK(r.x <= 1023);
}

TEST_CASE("mesh import: OBJ file resolves and converts end to end")
{
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "voxelforge_mesh_import_test.obj";
    const fs::path mtl = fs::temp_directory_path() / "voxelforge_mesh_import_test.mtl";
    {
        std::ofstream material(mtl);
        REQUIRE(material.good());
        material << "newmtl cabin_red\nKd 1 0 0\n";
    }
    {
        std::ofstream out(file);
        REQUIRE(out.good());
        out << "mtllib " << mtl.filename().string() << "\n"
               "usemtl cabin_red\n"
               "v 0 0 0\n"
               "v 1 0 0\n"
               "v 1 1 0\n"
               "v 0 1 0\n"
               "v 0 0 1\n"
               "v 1 0 1\n"
               "v 1 1 1\n"
               "v 0 1 1\n"
               "f 1 3 2 1 4 3\n"
               "f 5 6 7 5 7 8\n"
               "f 1 2 6 1 6 5\n"
               "f 2 3 7 2 7 6\n"
               "f 3 4 8 3 8 7\n"
               "f 4 1 5 4 5 8\n";
    }

    MeshImportOptions options;
    options.hasFit = true;
    options.fitMeters = 1.0f;
    options.mat = 6;
    std::vector<VoxelRecord> records;
    MeshImportStats stats;
    std::string err;
    REQUIRE(convertMeshToRecords(file.string(), options, true,
                                glm::ivec3(500, 500, 500), records, stats, err));
    CHECK(err.empty());
    CHECK(stats.triangles == 24); // six quads fan-triangulate to two triangles each
    CHECK(stats.nx == 13);
    CHECK(stats.ny == 13);
    CHECK(stats.nz == 13);
    CHECK_FALSE(stats.leak);
    REQUIRE(records.size() == 1331);
    for (const auto& r : records) {
        CHECK(r.r > 200);
        CHECK(r.g < 50);
        CHECK(r.b < 50);
    }

    int minX = 1024, maxX = -1, minY = 1024, maxY = -1, minZ = 1024, maxZ = -1;
    for (const auto& r : records) {
        minX = std::min(minX, int(r.x)); maxX = std::max(maxX, int(r.x));
        minY = std::min(minY, int(r.y)); maxY = std::max(maxY, int(r.y));
        minZ = std::min(minZ, int(r.z)); maxZ = std::max(maxZ, int(r.z));
    }
    CHECK(minX == 495);
    CHECK(maxX == 505);
    CHECK(minY == 500);
    CHECK(maxY == 510);
    CHECK(minZ == 495);
    CHECK(maxZ == 505);

    std::error_code ec;
    fs::remove(file, ec);
    fs::remove(mtl, ec);
}
