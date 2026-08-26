// Tests for Ctrl+LMB picking: rayPick against the records-derived VoxelField.
// Regression guard: the field is cell-quantised (samples evaluated at cell
// centres), so rayPick must detect sign flips - a thin |d| window never fires.
#include "voxel/picking.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <doctest/doctest.h>
#include <cmath>
#include <string>

using namespace vf::voxel;

namespace {
glm::vec3 heroForward()
{
    glm::vec3 pos{-16.f, 6.5f, -14.f}, tgt{6.5f, 0.8f, 11.f};
    return glm::normalize(tgt - pos);
}

int ix0(float wx) { return int((wx + 0.5f * WORLD) / VOXEL); }
int iz0(float wz) { return int((wz + 0.5f * WORLD) / VOXEL); }


struct TempLayer {
    std::string dir = std::string(VOXELFORGE_ASSET_DIR);
    std::string vxw = dir + "/picktest_obj.vxw";
    std::string json = dir + "/picktest.json";
    ~TempLayer()
    {
        std::remove(vxw.c_str());
        std::remove(json.c_str());
    }
};
} // namespace

TEST_CASE("rayPick hits terrain from the hero camera")
{
    LayeredWorld lw;
    REQUIRE(lw.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json"));
    const VoxelField& f = lw.field();
    REQUIRE(f.valid());

    // centre-of-screen ray toward the cabin target crosses terrain
    PickHit h = rayPick(f, {-16.f, 6.5f, -14.f}, heroForward());
    CHECK(h.hit);
    if (h.hit) {
        CHECK(h.dist > 0.f);
        CHECK(h.dist < 90.f);
        CHECK(h.mat != 0);
        // the picked cell itself must be solid in the live field
        CHECK(f.sampleWorld(voxelCenter(h.voxel)).d <= 0.f);
        // normal points out of the surface (upward-ish for terrain)
        CHECK(h.normal.y > -0.1f);
    }

    // rays into the sky must not hit
    PickHit sky = rayPick(f, {-16.f, 6.5f, -14.f}, {0.f, 1.f, 0.f});
    CHECK_FALSE(sky.hit);
}

TEST_CASE("rayPick straight down matches the field's own top solid cell")
{
    LayeredWorld lw;
    REQUIRE(lw.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json"));
    const VoxelField& f = lw.field();
    REQUIRE(f.valid());

    const float wx = 6.5f, wz = 11.f;
    // oracle: scan the field itself for the first solid cell from above -
    // valid whatever layers the manifest currently enables
    int iyTop = int(std::lround(WORLD / VOXEL)) - 1;
    bool found = false;
    int oracleY = 0;
    for (int y = iyTop; y >= -iyTop; --y) {
        if (f.sampleWorld(voxelCenter({ix0(wx), y, iz0(wz)})).d <= 0.f) {
            oracleY = y;
            found = true;
            break;
        }
    }
    REQUIRE(found);

    PickHit h = rayPick(f, {wx, 20.f, wz}, {0.f, -1.f, 0.f});
    CHECK(h.hit);
    if (h.hit) {
        CHECK(std::abs(h.voxel.y - oracleY) <= 1);
        int dx = std::abs(h.voxel.x - ix0(wx));
        CHECK(dx <= 1);
        CHECK(f.sampleWorld(voxelCenter(h.voxel)).d <= 0.f);
    }
}

TEST_CASE("rayPick selects an object layer voxel by material")
{
    TempLayer tmp;
    namespace fs = std::filesystem;

    // find dry ground away from the hero view, then plant a 7x7x7 box there
    const float bx = -20.f, bz = -20.f;
    const HeightMap& hm = sharedHeightmap();
    float ground = hm.sample(bx, bz);
    int ix = int((bx + 0.5f * WORLD) / VOXEL);
    int iz = int((bz + 0.5f * WORLD) / VOXEL);
    int iy = int(std::floor((ground + 0.5f * WORLD) / VOXEL)) - 2;
    constexpr uint8_t kObjMat = 4;

    WorldFileData data;
    data.meta.worldSize = WORLD;
    data.meta.voxelSize = VOXEL;
    data.meta.waterLevel = WATER_LEVEL;
    data.meta.gridN = GRID_N;
    data.meta.brickN = BRICK_N;
    for (int x = ix - 3; x <= ix + 3; ++x)
        for (int y = iy; y <= iy + 6; ++y)
            for (int z = iz - 3; z <= iz + 3; ++z) {
                VoxelRecord r;
                r.x = uint16_t(x);
                r.y = uint16_t(y);
                r.z = uint16_t(z);
                r.r = 200;
                r.g = 160;
                r.b = 80;
                r.materialId = kObjMat;
                data.voxels.push_back(r);
            }
    REQUIRE(worldfile::write(tmp.vxw, data));

    {
        std::ofstream out(tmp.json);
        out << "{\"layers\":["
               "{\"file\":\"picktest_obj.vxw\",\"role\":\"object\",\"name\":"
               "\"picktest\",\"pos\":[0,0,0],\"rotDeg\":0,\"enabled\":true},"
               "{\"file\":\"landscape.vxw\",\"role\":\"landscape\",\"name\":"
               "\"landscape\",\"pos\":[0,0,0],\"rotDeg\":0,\"enabled\":true}]}";
    }

    LayeredWorld lw;
    REQUIRE(lw.load(tmp.json));
    const VoxelField& f = lw.field();
    REQUIRE(f.valid());

    PickHit h = rayPick(f, {bx, ground + 8.f, bz}, {0.f, -1.f, 0.f});
    CHECK(h.hit);
    if (h.hit) {
        CHECK(int(h.mat) == int(kObjMat));
        CHECK(f.sampleWorld(voxelCenter(h.voxel)).d <= 0.f);
        CHECK(h.voxel.x >= ix - 3);
        CHECK(h.voxel.x <= ix + 3);
        CHECK(h.voxel.z >= iz - 3);
        CHECK(h.voxel.z <= iz + 3);
    }
}

TEST_CASE("anchor helpers keep the bottom-center contract")
{
    glm::ivec3 v{100, 50, 200};
    glm::vec3 c = voxelCenter(v);
    CHECK(anchorBottomCenter(v) == c);
    CHECK(worldToVoxel(c) == v);
}
