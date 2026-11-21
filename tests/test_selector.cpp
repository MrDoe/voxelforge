// Unit tests for the shared vf_mcp selector grammar. Pure geometry over
// VoxelRecords: no scenery, no GPU, no MCP transport.
#include "voxel/selector.hpp"
#include <doctest/doctest.h>

using namespace vf::voxel;
using namespace vf::voxel::selector;

static VoxelRecord rec(int x, int y, int z, uint8_t mat)
{
    VoxelRecord v{};
    v.x = uint16_t(x); v.y = uint16_t(y); v.z = uint16_t(z);
    v.materialId = mat;
    return v;
}

TEST_CASE("selector: box is inclusive and honours the material filter")
{
    std::vector<VoxelRecord> recs = {
        rec(10, 20, 30, 8), rec(11, 20, 30, 8), rec(12, 20, 30, 4)
    };
    Spec s;
    s.kind = Spec::Kind::Box;
    s.box = { 10, 20, 30, 11, 20, 30 };
    s.mat = 8;
    Selection sel;
    std::string err;
    REQUIRE(selectOne(recs, s, sel, err));
    CHECK(sel.size() == 2);
    CHECK(sel.contains(10, 20, 30));
    CHECK(sel.contains(11, 20, 30));
    CHECK_FALSE(sel.contains(12, 20, 30));
    CHECK(sel.lo[0] == 10);
    CHECK(sel.hi[0] == 11);
}

TEST_CASE("selector: a masked component stops at a different-material bridge")
{
    // A tree (mat 8) sitting on terrain (mat 2). The two trees touch through
    // the ground: an UNMASKED flood fill swallows both (the 13k-cell bug).
    std::vector<VoxelRecord> recs;
    for (int x = 0; x < 5; ++x)
        recs.push_back(rec(x, 20, 30, 2));           // ground bridge, mat 2
    recs.push_back(rec(1, 21, 30, 8));
    recs.push_back(rec(1, 22, 30, 8));
    recs.push_back(rec(3, 21, 30, 8));
    recs.push_back(rec(3, 22, 30, 8));

    Spec s;
    s.kind = Spec::Kind::Component;
    s.anchor[0] = 1; s.anchor[1] = 21; s.anchor[2] = 30;
    s.mat = 8;
    Selection sel;
    std::string err;
    REQUIRE(selectOne(recs, s, sel, err));
    CHECK(sel.size() == 2);                           // ONE tree, not both
    CHECK(sel.contains(1, 22, 30));
    CHECK_FALSE(sel.contains(3, 21, 30));
    CHECK_FALSE(sel.contains(2, 20, 30));

    // The unmasked fill is deliberately different — that is the failure mode
    // the mask exists to prevent, and the test documents it.
    Spec unmasked = s;
    unmasked.mat = -1;
    Selection all;
    REQUIRE(selectOne(recs, unmasked, all, err));
    CHECK(all.size() == recs.size());
}

TEST_CASE("selector: sphere is centred on the world origin, not the lattice origin")
{
    std::vector<VoxelRecord> recs = {
        rec(512, 508, 512, 4),  // world ~(0.05,-1.45,0.05)
        rec(512, 528, 512, 4),  // ~2 m higher
    };
    Spec s;
    s.kind = Spec::Kind::Sphere;
    s.anchor[0] = 512; s.anchor[1] = 508; s.anchor[2] = 512;
    s.radiusM = 0.5f;
    Selection sel;
    std::string err;
    REQUIRE(selectOne(recs, s, sel, err));
    CHECK(sel.size() == 1);
    CHECK(sel.contains(512, 508, 512));
}

TEST_CASE("selector: union of specs, invalid input, and without()")
{
    std::vector<VoxelRecord> recs = {
        rec(0, 0, 0, 1), rec(1, 0, 0, 2), rec(2, 0, 0, 3)
    };
    Spec a;
    a.kind = Spec::Kind::Box;
    a.box = { 0, 0, 0, 0, 0, 0 };
    Spec b;
    b.kind = Spec::Kind::Mat;
    b.mat = 3;
    Selection sel;
    std::string err;
    REQUIRE(selectCells(recs, { a, b }, sel, err));
    CHECK(sel.size() == 2);

    size_t removed = 0;
    auto keep = without(recs, sel, removed);
    CHECK(removed == 2);
    CHECK(keep.size() == 1);
    CHECK(keep[0].materialId == 2);

    Spec bad = a;
    bad.box[0] = 9;
    Selection tmp;
    CHECK_FALSE(selectOne(recs, bad, tmp, err));
    CHECK(err.find("min > max") != std::string::npos);

    Spec badConn = b;
    badConn.kind = Spec::Kind::Component;
    badConn.anchor[0] = 0;
    badConn.connectivity = 7;
    CHECK_FALSE(selectOne(recs, badConn, tmp, err));
    CHECK(err.find("connectivity") != std::string::npos);
}
