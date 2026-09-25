#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <set>

using namespace vf::voxel;

namespace {
std::string tmpPath()
{
    return (std::filesystem::temp_directory_path() / "vf_test_world.vxw").string();
}

WorldFileData sampleData()
{
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    d.chunkGrid = { -1, 5, 9, -1 };
    d.childBase = { 0u, 8u };
    d.payload = { 0xFFu, 0x1Fu };
    d.handles = { 0xFFFFFFFFu, 4u, kSolidHandle, 6u, 8u, 10u, 12u, 14u,
                  16u, kEmptyHandle, 20u, 22u, 24u, 26u, 28u, 30u };
    // two voxels worth of brick words: rgb+sdf | a+refl+rough+mat
    d.bricks = { 0x40C02880u, 0x03E628FFu, 0x60C00020u, 0x018732FFu };
    for (int i = 0; i < 3; ++i) {
        VoxelRecord v;
        v.x = uint16_t(10 + i);
        v.y = uint16_t(500);
        v.z = uint16_t(1000 - i);
        v.r = uint8_t(30 + i);
        v.g = uint8_t(120);
        v.b = uint8_t(60);
        v.a = 255;
        v.reflectivity = uint8_t(35 * (i + 1));
        v.roughness = uint8_t(200);
        v.materialId = uint8_t(i);
        d.voxels.push_back(v);
    }
    return d;
}
} // namespace

TEST_CASE("worldfile roundtrip preserves all data")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));

    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    CHECK(out.meta.worldSize == src.meta.worldSize);
    CHECK(out.meta.voxelSize == src.meta.voxelSize);
    CHECK(out.meta.waterLevel == src.meta.waterLevel);
    CHECK(out.meta.gridN == src.meta.gridN);
    CHECK(out.chunkGrid == src.chunkGrid);
    CHECK(out.childBase == src.childBase);
    CHECK(out.payload == src.payload);
    CHECK(out.handles == src.handles);
    CHECK(out.bricks == src.bricks);
    REQUIRE(out.voxels.size() == src.voxels.size());
    for (size_t i = 0; i < src.voxels.size(); ++i) {
        const VoxelRecord& a = src.voxels[i];
        const VoxelRecord& b = out.voxels[i];
        CHECK(b.x == a.x);
        CHECK(b.y == a.y);
        CHECK(b.z == a.z);
        CHECK(b.r == a.r);
        CHECK(b.reflectivity == a.reflectivity);
        CHECK(b.roughness == a.roughness);
        CHECK(b.materialId == a.materialId);
    }
    glm::vec3 p = out.voxels[0].position(out.meta);
    CHECK(p.x > -0.5f * WORLD);
    CHECK(p.x < 0.0f);
    std::remove(path.c_str());
}

TEST_CASE("worldfile v2 sections: store overlay round trip + opaque preservation")
{
    WorldFileData src = sampleData();
    // one overlay section with arbitrary opaque bytes
    Section overlay;
    overlay.type = worldfile::kSectionStoreChunks;
    for (uint8_t i = 0; i < 64; ++i)
        overlay.data.push_back(uint8_t(i * 7 + 3));
    src.sections.push_back(overlay);

    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));

    // v2 header round trip: records + legacy arrays + the opaque section
    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    CHECK(out.chunkGrid == src.chunkGrid);
    CHECK(out.handles == src.handles);
    CHECK(out.bricks == src.bricks);
    REQUIRE(out.voxels.size() == src.voxels.size());
    CHECK(out.voxels[1].materialId == 1);
    REQUIRE(out.sections.size() == 1);
    CHECK(out.sections[0].type == worldfile::kSectionStoreChunks);
    CHECK(out.sections[0].data == overlay.data);

    // a read-modify-write preserves the opaque section (forward compat)
    WorldFileData again = out;
    REQUIRE(worldfile::write(path, again));
    WorldFileData out2;
    REQUIRE(worldfile::read(path, out2));
    REQUIRE(out2.sections.size() == 1);
    CHECK(out2.sections[0].data == overlay.data);

    // CRC still guards v2 payloads
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, n - 1, SEEK_SET);
        int c = std::fgetc(f);
        std::fseek(f, n - 1, SEEK_SET);
        std::fputc(c ^ 0x5A, f);
        std::fclose(f);
        WorldFileData bad;
        CHECK_FALSE(worldfile::read(path, bad));
    }
    std::remove(path.c_str());
}

TEST_CASE("worldfile rejects corrupted payloads")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));

    FILE* f = fopen(path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    fseek(f, -10, SEEK_END); // flip a byte inside the record region
    int c = fgetc(f);
    fseek(f, -1, SEEK_CUR);
    fputc(c ^ 0xFF, f);
    fclose(f);

    WorldFileData out;
    CHECK_FALSE(worldfile::read(path, out));
    std::remove(path.c_str());
}

TEST_CASE("worldfile rejects wrong magic")
{
    WorldFileData src = sampleData();
    std::string path = tmpPath();
    REQUIRE(worldfile::write(path, src));
    FILE* f = fopen(path.c_str(), "r+b");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_SET);
    fputc('X', f);
    fclose(f);
    WorldFileData out;
    CHECK_FALSE(worldfile::read(path, out));
    std::remove(path.c_str());
}

TEST_CASE("manifest roundtrip and layered dedupe")
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "vf_layers_test";
    std::filesystem::create_directories(dir);

    // two record-only layers sharing one cell: first layer must win
    auto mk = [&](uint16_t x, uint8_t mat) {
        VoxelRecord v;
        v.x = x;
        v.y = 512;
        v.z = 512;
        v.materialId = mat;
        return v;
    };
    worldfile::WorldLayer a{ "a.vxw", "object", "a", {}, 0.f };
    worldfile::WorldLayer b{ "b.vxw", "object", "b", {}, 0.f };
    b.enabled = false; // disabled layers stay on disk but leave the merge
    // placement roundtrips through the manifest; c stays disabled so its
    // pos/rot are parsed but never merged (a merge would move its records)
    worldfile::WorldLayer c{ "c.vxw", "object", "c", { 1.f, 2.f, 3.f }, 45.f };
    c.enabled = false;
    CHECK(worldfile::writeManifest((dir / "world.json").string(), { a, b, c }));

    std::vector<worldfile::WorldLayer> loaded;
    REQUIRE(worldfile::loadManifest((dir / "world.json").string(), loaded));
    REQUIRE(loaded.size() == 3);
    CHECK(loaded[0].file == "a.vxw");
    CHECK(loaded[0].role == "object");
    CHECK(loaded[0].name == "a");
    CHECK(loaded[1].enabled == false); // bool literal parsed back
    CHECK(loaded[2].pos[0] == doctest::Approx(1.f));
    CHECK(loaded[2].rotDeg == doctest::Approx(45.f));
    CHECK(loaded[2].enabled == false);

    // 3-axis placement roundtrips: pitch and roll are first-class manifest
    // fields now (the trackball writes all three)
    worldfile::WorldLayer d{ "d.vxw", "object", "d", { 2.f, -1.f, 4.f }, 30.f };
    d.rotX = 15.f;
    d.rotZ = -25.f;
    CHECK(worldfile::writeManifest((dir / "world2.json").string(), { d }));
    std::vector<worldfile::WorldLayer> loaded2;
    REQUIRE(worldfile::loadManifest((dir / "world2.json").string(), loaded2));
    REQUIRE(loaded2.size() == 1);
    CHECK(loaded2[0].pos[1] == doctest::Approx(-1.f));
    CHECK(loaded2[0].pos[2] == doctest::Approx(4.f));
    CHECK(loaded2[0].rotDeg == doctest::Approx(30.f));
    CHECK(loaded2[0].rotX == doctest::Approx(15.f));
    CHECK(loaded2[0].rotZ == doctest::Approx(-25.f));
    std::filesystem::remove(dir / "world2.json");

    WorldFileData da, db;
    da.meta = db.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    da.voxels = { mk(100, 6), mk(101, 6) };
    db.voxels = { mk(101, 2), mk(102, 2) }; // 101 overlaps -> b loses there
    REQUIRE(worldfile::write((dir / "a.vxw").string(), da));
    REQUIRE(worldfile::write((dir / "b.vxw").string(), db));

    std::vector<VoxelRecord> merged;
    WorldFileMeta expected{ WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), expected, merged));
    REQUIRE(merged.size() == 2); // only a - b is disabled
    CHECK(merged[0].materialId == 6);
    CHECK(merged[1].materialId == 6);

    // re-enable b: overlap cell must go to a (earlier layer wins)
    loaded[1].enabled = true;
    CHECK(worldfile::writeManifest((dir / "world.json").string(), loaded));
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), expected, merged));
    REQUIRE(merged.size() == 3);
    CHECK(merged[1].materialId == 6);
    CHECK(merged[2].materialId == 2);

    // meta mismatch must be rejected
    expected.voxelSize = 0.5f;
    CHECK_FALSE(worldfile::readLayered((dir / "world.json").string(), expected, merged));

    std::filesystem::remove_all(dir);
}

TEST_CASE("readLayered applies pitch and roll when yaw is zero")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vf_layer_axes_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const WorldFileMeta meta{ WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    std::vector<VoxelRecord> merged;

    auto writeLayer = [&](const char* file,
                          const std::vector<VoxelRecord>& records,
                          float pitch, float roll) {
        WorldFileData data;
        data.meta = meta;
        data.voxels = records;
        REQUIRE(worldfile::write((dir / file).string(), data));
        worldfile::WorldLayer layer;
        layer.file = file;
        layer.role = "object";
        layer.name = file;
        layer.rotX = pitch;
        layer.rotZ = roll;
        REQUIRE(worldfile::writeManifest((dir / "world.json").string(), { layer }));
    };

    // Roll alone used to skip transformRecords because the fast-path guard
    // checked only rot/yaw. A three-cell X bar must stand upright along Y.
    std::vector<VoxelRecord> xbar;
    for (uint16_t x = 511; x <= 513; ++x) {
        VoxelRecord v;
        v.x = x;
        v.y = 400;
        v.z = 512;
        v.materialId = 6;
        xbar.push_back(v);
    }
    writeLayer("roll.vxw", xbar, 0.0f, 90.0f);
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), meta, merged));
    REQUIRE(merged.size() == xbar.size());
    int minY = 1 << 30;
    int maxY = -(1 << 30);
    for (const auto& v : merged) {
        CHECK(v.x == 512);
        CHECK(v.z == 512);
        minY = std::min(minY, int(v.y));
        maxY = std::max(maxY, int(v.y));
    }
    CHECK(minY == 399);
    CHECK(maxY == 401);

    // Pitch alone follows the same rule: a Z bar rises through the source
    // plane instead of passing through unchanged.
    std::vector<VoxelRecord> zbar;
    for (uint16_t z = 508; z <= 516; ++z) {
        VoxelRecord v;
        v.x = 512;
        v.y = 400;
        v.z = z;
        v.materialId = 6;
        zbar.push_back(v);
    }
    writeLayer("pitch.vxw", zbar, 90.0f, 0.0f);
    REQUIRE(worldfile::readLayered((dir / "world.json").string(), meta, merged));
    REQUIRE(merged.size() == zbar.size());
    minY = 1 << 30;
    maxY = -(1 << 30);
    for (const auto& v : merged) {
        CHECK(v.x == 512);
        minY = std::min(minY, int(v.y));
        maxY = std::max(maxY, int(v.y));
    }
    CHECK(minY < 400);
    CHECK(maxY > 400);

    fs::remove_all(dir);
}

TEST_CASE("transformRecords places a layer (identity / translate / rotate)")
{
    const WorldFileMeta meta{ WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    const auto mk = [](uint16_t x, uint16_t y, uint16_t z, uint8_t mat) {
        VoxelRecord v;
        v.x = x;
        v.y = y;
        v.z = z;
        v.materialId = mat;
        v.reserved = 17;
        return v;
    };
    // a 4x1x4 plate centred on x=z=512
    std::vector<VoxelRecord> plate;
    for (uint16_t x = 510; x <= 513; ++x)
        for (uint16_t z = 510; z <= 513; ++z)
            plate.push_back(mk(x, 400, z, 6));
    const auto aabb = [](const std::vector<VoxelRecord>& r) {
        glm::ivec3 mn(1 << 30), mx(-(1 << 30));
        for (const auto& v : r) {
            mn = glm::min(mn, glm::ivec3(v.x, v.y, v.z));
            mx = glm::max(mx, glm::ivec3(v.x, v.y, v.z));
        }
        return std::make_pair(mn, mx);
    };

    // identity is an exact passthrough (same cells, same order)
    std::vector<VoxelRecord> out;
    worldfile::transformRecords(plate, meta, glm::vec3(0.f), 0.f, 0.f, 0.f, out);
    REQUIRE(out.size() == plate.size());
    for (size_t i = 0; i < plate.size(); ++i) {
        CHECK(out[i].x == plate[i].x);
        CHECK(out[i].y == plate[i].y);
        CHECK(out[i].z == plate[i].z);
        CHECK(out[i].materialId == plate[i].materialId);
        CHECK(out[i].reserved == plate[i].reserved);
    }

    // translation by whole cells is exact: every record shifts by 10 cells
    worldfile::transformRecords(plate, meta, glm::vec3(VOXEL * 10, 0.f, VOXEL * -5),
                                0.f, 0.f, 0.f, out);
    REQUIRE(out.size() == plate.size());
    auto [mn, mx] = aabb(out);
    CHECK(mn == glm::ivec3(520, 400, 505));
    CHECK(mx == glm::ivec3(523, 400, 508));

    // 90-degree rotation about the bottom-center is cell-exact: the plate is
    // square so the footprint maps onto itself, spun in place
    worldfile::transformRecords(plate, meta, glm::vec3(0.f), 90.f, 0.f, 0.f, out);
    REQUIRE(out.size() == plate.size());
    auto [mn90, mx90] = aabb(out);
    CHECK(mn90 == glm::ivec3(510, 400, 510));
    CHECK(mx90 == glm::ivec3(513, 400, 513));
    std::set<uint32_t> cells;
    for (const auto& v : out)
        cells.insert((uint32_t(v.x) << 20) | (uint32_t(v.y) << 10) | uint32_t(v.z));
    // no duplicates and the corner cell moved: (510,510) -> (510,513)
    CHECK(cells.size() == out.size());
    CHECK(cells.count((510u << 20) | (400u << 10) | 513u));

    // arbitrary rotation keeps a watertight footprint: the 4x4 plate at 30
    // degrees covers more cells than it started with (the rotated square's
    // axis-aligned hull), never fewer, and keeps the grid's cell count sane
    worldfile::transformRecords(plate, meta, glm::vec3(0.f), 30.f, 0.f, 0.f, out);
    CHECK(out.size() >= plate.size());
    auto [mn30, mx30] = aabb(out);
    const int w30 = mx30.x - mn30.x + 1;
    const int d30 = mx30.z - mn30.z + 1;
    CHECK(w30 == 6); // ceil(4*cos30 + 4*sin30) = ceil(3.46+2) = 6
    CHECK(d30 == 6);

    // pitch (rotX) lifts one end of the plate off the ground plane: a 1x1x9
    // bar lying along Z, pitched 90 degrees, ends up standing up along Y.
    // (Odd length on purpose: the bottom-center pivot must sit on a cell
    // CENTRE so a 90-degree step maps centres to centres - at an edge pivot
    // the pre-images land exactly on cell boundaries and the fp residual
    // splits them across both sides.)
    std::vector<VoxelRecord> bar;
    for (uint16_t z = 508; z <= 516; ++z)
        bar.push_back(mk(512, 400, z, 6));
    worldfile::transformRecords(bar, meta, glm::vec3(0.f), 0.f, 90.f, 0.f, out);
    REQUIRE(out.size() == bar.size());
    auto [mnp, mxp] = aabb(out);
    CHECK(mnp.y < 400);        // some cells rose above the original floor
    CHECK(mxp.y > 400);
    CHECK(mnp.x == 512);       // pitch about +X leaves X alone
    // a watertight footprint: the destination-driven map never drops a cell
    // the source covered (the bar's vertical span is the full 9 cells)
    CHECK(mxp.y - mnp.y + 1 >= 9);

    // roll (rotZ) about the view axis: a 3-cell bar lying along X, rolled 90
    // degrees, ends up standing along Y. The bar must have extent along X
    // (and the pivot on a cell centre: 3 wide puts it there) - a bar lying
    // ON the pivot axis would be invariant under roll.
    std::vector<VoxelRecord> xbar = { mk(511, 400, 512, 6), mk(512, 400, 512, 6),
                                      mk(513, 400, 512, 6) };
    worldfile::transformRecords(xbar, meta, glm::vec3(0.f), 0.f, 0.f, 90.f, out);
    REQUIRE(out.size() == xbar.size());
    auto [mnr, mxr] = aabb(out);
    CHECK(mnr.x == 512);       // the X extent folded into Y
    CHECK(mxr.x == 512);
    CHECK(mnr.y == 399);
    CHECK(mxr.y == 401);

    // combined yaw+pitch+roll: the composite is orthonormal, so the inverse
    // map stays exact and the shell stays watertight at any angle mix
    worldfile::transformRecords(bar, meta, glm::vec3(0.f), 35.f, 25.f, -15.f, out);
    CHECK(out.size() >= bar.size());

    // Live preview math must map the CURRENT absolute pose to the committed
    // absolute pose, including layers that already have non-zero pitch/roll.
    glm::vec3 pivot;
    REQUIRE(worldfile::recordBottomCenter(plate, meta, pivot));
    const glm::vec3 sourcePoint = plate.front().position(meta);
    const glm::mat3 oldR = worldfile::placementRotation(35.f, 25.f, -15.f);
    const glm::mat3 newR = worldfile::placementRotation(50.f, 10.f, 20.f);
    const glm::mat3 relative =
        worldfile::relativePlacementRotation(35.f, 25.f, -15.f, 50.f, 10.f, 20.f);
    const glm::vec3 current = pivot + oldR * (sourcePoint - pivot);
    const glm::vec3 expected = pivot + newR * (sourcePoint - pivot);
    const glm::vec3 preview = pivot + relative * (current - pivot);
    CHECK(glm::distance(preview, expected) < 1e-4f);

    // cells moved out of the world are dropped, never wrapped
    std::vector<VoxelRecord> edge = { mk(1020, 500, 500, 6), mk(5, 500, 500, 6) };
    worldfile::transformRecords(edge, meta, glm::vec3(VOXEL * 30, 0.f, 0.f),
                                0.f, 0.f, 0.f, out);
    CHECK(out.size() == 1);
    CHECK(out[0].x == 35);
}

TEST_CASE("placement Euler roundtrip and local-axis rotation")
{
    const glm::mat3 base = worldfile::placementRotation(35.f, 25.f, -15.f);
    const glm::vec3 recovered = worldfile::placementEuler(base);
    const glm::mat3 rebuilt = worldfile::placementRotation(
        recovered.x, recovered.y, recovered.z);
    float max_error = 0.0f;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            max_error = std::max(max_error, std::abs(base[c][r] - rebuilt[c][r]));
    CHECK(max_error < 1e-4f);

    for (auto axis : { worldfile::PlacementAxis::X,
                       worldfile::PlacementAxis::Y,
                       worldfile::PlacementAxis::Z }) {
        const glm::mat3 local = worldfile::rotatePlacementLocal(base, axis, 17.f);
        const glm::vec3 euler = worldfile::placementEuler(local);
        const glm::mat3 roundtrip = worldfile::placementRotation(
            euler.x, euler.y, euler.z);
        float error = 0.0f;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                error = std::max(error, std::abs(local[c][r] - roundtrip[c][r]));
        CHECK(error < 1e-4f);
    }
}

TEST_CASE("record-only layer files are valid VXW (empty SVO sections)")
{
    std::string path = tmpPath();
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    VoxelRecord only;
    only.x = 7;
    only.materialId = 4;
    d.voxels.push_back(only);
    REQUIRE(worldfile::write(path, d));
    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    CHECK(out.chunkGrid.empty());
    CHECK(out.bricks.empty());
    REQUIRE(out.voxels.size() == 1);
    CHECK(out.voxels[0].x == 7);
    std::remove(path.c_str());
}

TEST_CASE("the reserved byte (per-cell texture override) survives a roundtrip")
{
    // phase-2 per-object textures ride the record's reserved byte end to end:
    // mcp write_object -> .vxw -> LayeredWorld -> VoxelField -> surfel
    std::string path = tmpPath();
    WorldFileData d;
    d.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    for (int i = 0; i < 3; ++i) {
        VoxelRecord v;
        v.x = uint16_t(500 + i);
        v.y = 600;
        v.z = 512;
        v.materialId = 1;
        v.reserved = uint8_t(i + 1); // atlas layer 1..3
        d.voxels.push_back(v);
    }
    REQUIRE(worldfile::write(path, d));
    WorldFileData out;
    REQUIRE(worldfile::read(path, out));
    REQUIRE(out.voxels.size() == 3);
    for (size_t i = 0; i < out.voxels.size(); ++i)
        CHECK(out.voxels[i].reserved == uint8_t(i + 1));
    std::remove(path.c_str());
}

TEST_CASE("writeManifest preserves the textures table and unknown keys")
{
    // vf_mcp only knows about layers; a manifest rewrite must not silently
    // drop the top-level "textures" array (or any other key it does not own)
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vf_manifest_keep";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string p = (dir / "world.json").string();
    {
        const std::string src =
            "{\n  \"version\": 1,\n  \"layers\": [\n"
            "    { \"file\": \"a.vxw\", \"role\": \"object\", \"name\": \"a\",\n"
            "      \"pos\": [0.0, 0.0, 0.0], \"rot\": 0.0, \"enabled\": true }\n"
            "  ],\n"
            "  \"textures\": [\n"
            "    { \"file\": \"wood.png\", \"mat\": 6, \"scale\": 0.5 }\n"
            "  ],\n"
            "  \"notes\": \"hand-authored scene\"\n"
            "}\n";
        std::ofstream(p) << src;
    }
    std::vector<worldfile::WorldLayer> layers = {
        { "b.vxw", "object", "b", { 0.f, 0.f, 0.f }, 0.f, true, true },
    };
    REQUIRE(worldfile::writeManifest(p, layers));

    std::vector<worldfile::WorldLayer> back;
    REQUIRE(worldfile::loadManifest(p, back));
    REQUIRE(back.size() == 1);
    CHECK(back[0].file == "b.vxw");

    std::ifstream in(p);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("\"textures\"") != std::string::npos);
    CHECK(text.find("wood.png") != std::string::npos);
    CHECK(text.find("\"notes\"") != std::string::npos);
    CHECK(text.find("hand-authored scene") != std::string::npos);
    // the replaced layers array must not keep the stale entry
    CHECK(text.find("\"a.vxw\"") == std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("texture manifest parses the optional textures table")
{
    using worldfile::TextureBinding;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vf_tex_manifest";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string p = (dir / "world.json").string();
    {
        std::FILE* f = std::fopen(p.c_str(), "wb");
        REQUIRE(f);
        std::fprintf(f,
            "{\n  \"version\": 1,\n  \"layers\": [\n"
            "    { \"file\": \"landscape.vxw\", \"role\": \"landscape\", \"enabled\": true }\n"
            "  ],\n"
            "  \"textures\": [\n"
            "    { \"file\": \"wood.png\", \"mat\": 6, \"scale\": 0.35 },\n"
            "    { \"file\": \"thatch.png\", \"mat\": 7 }\n"
            "  ]\n}\n");
        std::fclose(f);
    }

    std::vector<TextureBinding> out;
    REQUIRE(worldfile::loadTextureManifest(p, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].file == "wood.png");
    CHECK(out[0].mat == 6);
    CHECK(out[0].scale == doctest::Approx(0.35f));
    CHECK(out[1].file == "thatch.png");
    CHECK(out[1].mat == 7);
    CHECK(out[1].scale == doctest::Approx(0.5f)); // default when omitted

    // a manifest without a textures table parses fine (empty result)
    {
        std::FILE* f = std::fopen(p.c_str(), "wb");
        REQUIRE(f);
        std::fprintf(f, "{ \"layers\": [ { \"file\": \"a.vxw\" } ] }\n");
        std::fclose(f);
    }
    std::vector<TextureBinding> out2;
    REQUIRE(worldfile::loadTextureManifest(p, out2));
    CHECK(out2.empty());

    // malformed entries are skipped, never fatal
    {
        std::FILE* f = std::fopen(p.c_str(), "wb");
        REQUIRE(f);
        std::fprintf(f,
            "{ \"layers\": [], \"textures\": [\n"
            "  { \"file\": \"ok.png\", \"mat\": 3 },\n"
            "  { \"file\": \"bad.png\" },\n"          // no mat -> skipped
            "  { \"mat\": 99 },\n"                    // no file -> skipped
            "  { \"file\": \"x.png\", \"mat\": 99 }\n" // mat out of range -> skipped
            "] }\n");
        std::fclose(f);
    }
    std::vector<TextureBinding> out3;
    REQUIRE(worldfile::loadTextureManifest(p, out3));
    REQUIRE(out3.size() == 1);
    CHECK(out3[0].file == "ok.png");
    CHECK(out3[0].mat == 3);

    // a missing manifest is not an error (palette fallback)
    CHECK_FALSE(worldfile::loadTextureManifest(
        (dir / "nope.json").string(), out3));

    fs::remove_all(dir);
}

TEST_CASE("PNG decode fails cleanly on missing/truncated files")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vf_tex_png";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string p = (dir / "t.png").string();
    {
        std::FILE* f = std::fopen(p.c_str(), "wb"); // empty file
        REQUIRE(f);
        std::fclose(f);
    }
    int w = 0, h = 0;
    std::vector<uint8_t> out;
    // the happy path (a real PNG round-trip) is covered end-to-end by
    // texture_check.py; here we only assert the failure paths never crash
    CHECK_FALSE(loadPngRGBA8(p, out, w, h));          // truncated/empty
    CHECK_FALSE(loadPngRGBA8((dir / "missing.png").string(), out, w, h));
    fs::remove_all(dir);
}

TEST_CASE("writeTextureManifest swaps only the textures table")
{
    // the in-app picker rewrites "textures" while every other top-level key
    // (layers included) must survive byte-for-byte, so swapping a material's
    // texture can never disturb the world
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "vf_tex_write";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string p = (dir / "world.json").string();
    {
        const std::string src =
            "{\n  \"version\": 1,\n  \"layers\": [\n"
            "    { \"file\": \"a.vxw\", \"role\": \"object\", \"name\": \"a\",\n"
            "      \"pos\": [0.0, 0.0, 0.0], \"rot\": 0.0, \"enabled\": true }\n"
            "  ],\n"
            "  \"textures\": [\n"
            "    { \"file\": \"old.png\", \"mat\": 6, \"scale\": 0.5 }\n"
            "  ],\n"
            "  \"notes\": \"keep me\"\n"
            "}\n";
        std::ofstream(p) << src;
    }
    std::vector<worldfile::TextureBinding> wanted = {
        { "textures/rock.png", 4, 1.25f },
        { "textures/grass.png", 0, 2.5f },
    };
    REQUIRE(worldfile::writeTextureManifest(p, wanted));

    // the new table parses back (order = as written)
    std::vector<worldfile::TextureBinding> back;
    REQUIRE(worldfile::loadTextureManifest(p, back));
    REQUIRE(back.size() == 2);
    CHECK(back[0].file == "textures/rock.png");
    CHECK(back[0].mat == 4);
    CHECK(back[0].scale == doctest::Approx(1.25f));
    CHECK(back[1].mat == 0);

    // layers + unknown keys survive; the stale entry is gone
    std::vector<worldfile::WorldLayer> layers;
    REQUIRE(worldfile::loadManifest(p, layers));
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].file == "a.vxw");
    std::ifstream in(p);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("\"notes\"") != std::string::npos);
    CHECK(text.find("keep me") != std::string::npos);
    CHECK(text.find("old.png") == std::string::npos);

    // an empty table writes an empty array (not a dropped key)
    REQUIRE(worldfile::writeTextureManifest(p, {}));
    std::vector<worldfile::TextureBinding> none;
    REQUIRE(worldfile::loadTextureManifest(p, none));
    CHECK(none.empty());
    fs::remove_all(dir);
}
