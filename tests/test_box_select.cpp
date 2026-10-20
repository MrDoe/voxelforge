// Pure-logic pin for the bounding-box selection (src/voxel/box_select.hpp):
// corner normalisation, containment and the edit clamp that scopes the brush.
// The App wiring is verified end-to-end by live_edit_check.py's box check;
// these cases pin the math the shaders and the store agree on.
#include <voxel/box_select.hpp>

#include <doctest/doctest.h>

using vf::voxel::CellBox;
using vf::voxel::StoreEdit;

TEST_CASE("box select: corners normalise in any order")
{
    CellBox b;
    b.set(glm::ivec3(10, 5, 9), glm::ivec3(2, 8, 1));
    CHECK(b.valid());
    CHECK(b.lo == glm::ivec3(2, 5, 1));
    CHECK(b.hi == glm::ivec3(10, 8, 9));
    CHECK(b.sizeCells() == glm::ivec3(9, 4, 9));
    CHECK(b.volumeCells() == 9 * 4 * 9);
    // a single-cell box (the anchor corner) is valid and selects one cell
    b.set(glm::ivec3(3, 3, 3), glm::ivec3(3, 3, 3));
    CHECK(b.valid());
    CHECK(b.volumeCells() == 1);
    CHECK(b.contains(glm::ivec3(3, 3, 3)));
    CHECK(!b.contains(glm::ivec3(3, 3, 4)));
}

TEST_CASE("box select: containment is inclusive on both faces")
{
    CellBox b;
    b.set(glm::ivec3(4, 4, 4), glm::ivec3(6, 6, 6));
    for (int x = 3; x <= 7; ++x)
        for (int y = 3; y <= 7; ++y)
            for (int z = 3; z <= 7; ++z) {
                const bool want = x >= 4 && x <= 6 && y >= 4 && y <= 6 &&
                                  z >= 4 && z <= 6;
                CHECK(b.contains(glm::ivec3(x, y, z)) == want);
            }
}

TEST_CASE("box select: clampEdits keeps only in-box edits")
{
    CellBox b;
    b.set(glm::ivec3(0, 0, 0), glm::ivec3(2, 2, 2));
    std::vector<StoreEdit> edits;
    for (int x = -1; x <= 3; ++x)
        for (int y = -1; y <= 3; ++y)
            for (int z = -1; z <= 3; ++z) {
                StoreEdit e;
                e.x = x;
                e.y = y;
                e.z = z;
                edits.push_back(e);
            }
    const size_t kept = CellBox::clampEdits(edits, b);
    CHECK(kept == 27); // 3^3 inside
    CHECK(edits.size() == 27);
    for (const StoreEdit& e : edits)
        CHECK(b.contains(glm::ivec3(e.x, e.y, e.z)));
}

TEST_CASE("box select: an inactive box clamps nothing")
{
    CellBox b;
    std::vector<StoreEdit> edits;
    for (int i = 0; i < 5; ++i) {
        StoreEdit e;
        e.x = i;
        e.y = 0;
        e.z = 0;
        edits.push_back(e);
    }
    CHECK(CellBox::clampEdits(edits, b) == 5);
    CHECK(edits.size() == 5);
    b.clear();
    CHECK(!b.valid());
    CHECK(CellBox::clampEdits(edits, b) == 5);
}

TEST_CASE("box select: world bounds and cellOf agree with the lattice")
{
    CellBox b;
    b.set(glm::ivec3(512, 507, 512), glm::ivec3(514, 509, 514));
    const glm::vec3 wlo = b.worldMin();
    const glm::vec3 whi = b.worldMax();
    // cell 512 sits at world x = -WORLD/2 + 512*VOXEL
    CHECK(wlo.x == doctest::Approx(-vf::voxel::WORLD * 0.5f + 512 * vf::voxel::VOXEL));
    // inclusive top: hi + 1 cells out
    CHECK(whi.x ==
          doctest::Approx(-vf::voxel::WORLD * 0.5f + 515 * vf::voxel::VOXEL));
    // every world point inside the cell AABB maps back into the box
    for (int i = 0; i < 5; ++i) {
        const glm::vec3 p = 0.5f * (wlo + whi);
        CHECK(b.contains(CellBox::cellOf(p)));
    }
    // a point one cell below the floor leaves the box
    const glm::vec3 below(wlo.x - 2.0f * vf::voxel::VOXEL, wlo.y, wlo.z);
    CHECK(!b.contains(CellBox::cellOf(below)));
}
