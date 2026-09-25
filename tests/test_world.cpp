// Tests for the layered world synthesis: chunked-SVO structure, VoxelField
// point queries against the analytic authoring truth, and determinism.
#include "voxel/layered_world.hpp"
#include "voxel/common.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <doctest/doctest.h>
#include <cmath>
#include <string>

using namespace vf::voxel;

std::string allLayersManifest();

namespace {
// Content-layer tests need every baked layer loaded, but the runtime default
// world.json starts landscape-only (layers are opt-in). Generate an
// all-enabled manifest next to the assets once per process.
LayeredWorld& allLayersWorld()
{
    static LayeredWorld lw;
    static std::string manifestPath;
    static bool ok = [] {
        namespace fs = std::filesystem;
        std::string dir = std::string(VOXELFORGE_ASSET_DIR);
        std::vector<std::pair<std::string, std::string>> files; // file, role
        for (fs::directory_iterator it(dir), end; it != end; ++it) {
            std::string f = it->path().filename().string();
            if (it->path().extension() != ".vxw" || f == "world.vxw")
                continue;
            files.push_back({ f, f == "landscape.vxw" ? "landscape"
                                : (f == "bushes.vxw" ? "scatter" : "object") });
        }
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) {
            if (a.second == "landscape" != (b.second == "landscape"))
                return a.second == "landscape"; // landscape last
            return a.first < b.first;
        });
        // ai_edits must claim cells first
        std::stable_sort(files.begin(), files.end(), [](auto& a, auto& b) {
            return a.first == "ai_edits.vxw";
        });
        std::string j = "{\"layers\":[";
        bool first = true;
        for (auto& [f, role] : files) {
            if (!first) j += ",";
            first = false;
            std::string name = f.substr(0, f.size() - 4);
            j += "{\"file\":\"" + f + "\",\"name\":\"" + name +
                 "\",\"role\":\"" + role +
                 "\",\"pos\":[0,0,0],\"rotDeg\":0,\"enabled\":true,\"listed\":true}";
        }
        j += "]}";
        manifestPath = dir + "/world_all.json";
        {
            std::ofstream out(manifestPath);
            out << j;
        }
        return lw.load(manifestPath);
    }();
    REQUIRE(ok);
    return lw;
}
} // namespace

// exposed for the determinism test
std::string allLayersWorldPath() { return allLayersManifest(); }

// Analytic reference composition. The default world is now runtime-authored
// (hamlet_*.vxw layers, no baker object sweeps), so the only analytic truth
// the field can be compared against is the terrain heightfield.
float analyticD(glm::vec3 p)
{
    const HeightMap& hm = sharedHeightmap();
    return p.y - hm.sample(p.x, p.z);
}
TEST_CASE("layered world synthesizes a sparse SVO")
{
    LayeredWorld& lw = allLayersWorld();
    auto st = lw.stats();
    MESSAGE("svo stats: nodes=", st.nodes, " bricks=", st.bricks,
            " activeChunks=", st.activeChunks);

    CHECK(st.records > 0);
    CHECK(st.activeChunks > 0);
    CHECK(st.activeChunks < size_t(GRID_N) * GRID_N * GRID_N); // sparsity
    CHECK(st.bricks > 100);
    // pruning must keep the tree well below full expansion (full = nodes ~ 4096*73)
    CHECK(st.nodes < 200000);
}

TEST_CASE("VoxelField sign matches analytic scene truth at probes")
{
    LayeredWorld& lw = allLayersWorld();
    const VoxelField& f = lw.field();
    REQUIRE(f.valid());

    // deep underground: solid
    CHECK(f.sampleWorld({ 0.f, -30.f, 0.f }).d < 0.0f);
    // high sky: empty
    CHECK(f.sampleWorld({ 0.f, 45.f, 0.f }).d > 1.0f);
    // deep in the river bed: solid
    glm::vec3 bc{ 0.f, -3.6f, 5.f };
    CHECK(f.sampleWorld(bc).d < -0.2f);

    // near-surface agreement with the analytic heightfield within a tolerance
    // band: walk a path across the valley and sample the field close to the
    // smooth terrain surface (objects are runtime-authored, so terrain is the
    // only analytic reference left)
    const HeightMap& hm = sharedHeightmap();
    int agree = 0, tested = 0;
    for (int i = 0; i < 400; ++i) {
        const float t = float(i) / 400.0f;
        const float x = glm::mix(-44.0f, 44.0f, t);
        const float z = glm::mix(-44.0f, 44.0f, cosf(t * 7.0f));
        glm::vec3 p(x, hm.sample(x, z) + sinf(t * 9.0f) * 1.2f, z);
        float dRef = analyticD(p);
        if (std::abs(dRef) > 1.5f)
            continue; // only compare near the surface
        float dField = f.sampleWorld(p).d;
        ++tested;
        bool ok = glm::sign(dRef) == glm::sign(dField) ||
                  std::abs(dField - dRef) < 0.35f;
        if (ok)
            ++agree;
    }
    MESSAGE("near-surface agreement: ", agree, "/", tested,
            " (quantised field vs analytic SDF)");
    CHECK(tested > 50);
    CHECK(agree > tested * 60 / 100);
}

TEST_CASE("synthesis is deterministic")
{
    LayeredWorld a, b;
    REQUIRE(a.load(allLayersWorldPath()));
    REQUIRE(b.load(allLayersWorldPath()));
    CHECK(a.gpu().handles == b.gpu().handles);
    CHECK(a.gpu().payload == b.gpu().payload);
    CHECK(a.gpu().childBase == b.gpu().childBase);
    CHECK(a.gpu().bricks == b.gpu().bricks);
    CHECK(a.gpu().chunkGrid == b.gpu().chunkGrid);
}
