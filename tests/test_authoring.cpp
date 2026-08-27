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
const VoxelField& testField()
{
    static LayeredWorld lw;
    static bool ok = lw.load(allLayersManifest());
    REQUIRE(ok);
    return lw.field();
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

TEST_CASE("baked field: cabin walls solid, door carved")
{
    const VoxelField& f = testField();
    // mid front log course: solid wood in the baked field too
    auto wall = f.sampleWorld({ kHousePos.x, kPadY + 0.42f + 0.145f, kHousePos.y - 2.0f });
    CHECK(wall.d < 0.0f);
    CHECK(wall.mat == 6);

    // door opening centre on the river side (-z): carved out of the wall
    auto door = f.sampleWorld({ kHousePos.x - 0.7f, kPadY + 1.30f, kHousePos.y - 2.0f });
    CHECK(door.d > 0.1f);
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

TEST_CASE("baked field: paddock and alpaca reachable")
{
    const VoxelField& field = testField();

    auto a = field.sampleWorld({ kAlpacaSpot.x,
                                 sharedHeightmap().sample(kAlpacaSpot.x, kAlpacaSpot.y) + 0.64f,
                                 kAlpacaSpot.y });
    CHECK(a.d < 0.0f);
    CHECK(a.mat == 5);

    glm::vec2 mid(0.5f * (kPaddockMin.x + kPaddockMax.x), kPaddockMax.y);
    auto f = field.sampleWorld({ mid.x, sharedHeightmap().sample(mid.x, mid.y) + 0.36f, mid.y });
    CHECK(f.d < 0.0f);
    CHECK(f.mat == 6);
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
    f.build(recs0, colTop, colMat, {}, {}, carveCells, carveMats);

    // the centred carve cell sits inside the subtracted volume -> reads as air
    auto s = f.sampleWorld(glm::vec3(0.f, 0.f, 0.f));
    CHECK(s.d > 0.0f);

    // outside the carve radius the terrain is intact
    auto t = f.sampleWorld(glm::vec3(3.f, -0.1f, 0.f));
    CHECK(t.d <= 0.0f);
}

