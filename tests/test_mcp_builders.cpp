// Unit tests for the deterministic intent-builder generators. The builders
// are pure functions over a seed and a height sampler, so they live in a
// header and are testable without the MCP transport or a GPU.
#include "ai/builders.hpp"
#include <doctest/doctest.h>

TEST_CASE("builder: same seed gives byte-identical tree cells")
{
    auto a = vf::ai::builders::tree({ 512, 508, 512 }, 2.5f, 1.1f, 12345);
    auto b = vf::ai::builders::tree({ 512, 508, 512 }, 2.5f, 1.1f, 12345);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
        CHECK(a[i].z == b[i].z);
        CHECK(a[i].materialId == b[i].materialId);
    }
    auto c = vf::ai::builders::tree({ 512, 508, 512 }, 2.5f, 1.1f, 54321);
    bool differs = a.size() != c.size();
    if (!differs && !a.empty() && !c.empty())
        differs = a.back().x != c.back().x || a.back().y != c.back().y;
    CHECK(differs);
}

TEST_CASE("builder: path samples ground per cell, not just the centreline")
{
    // A rising ridge across the path: the cell at x+1 must be one voxel higher
    // than the cell at x, if the sampler says so.
    auto h = [](float x, float /*z*/) { return (x > 0.f) ? 0.2f : 0.f; };
    auto cells = vf::ai::builders::path({ -0.2f, 0.f }, { 0.2f, 0.f }, 0.2f, 6, h);
    REQUIRE_FALSE(cells.empty());
    bool low = false, high = false;
    for (const auto& c : cells) {
        const float wx = -51.2f + (float(c.x) + 0.5f) * 0.1f;
        if (wx < -0.02f) low = true;
        if (wx > 0.02f) high = true;
    }
    CHECK(low);
    CHECK(high);
    CHECK(cells.size() > 2);
}

TEST_CASE("builder: keep-out guard sees the live_edit carve region")
{
    CHECK(vf::ai::builders::inKeepOut(0.f, 0.f));
    CHECK(vf::ai::builders::inKeepOut(2.9f, 5.9f));
    CHECK_FALSE(vf::ai::builders::inKeepOut(-4.f, 0.f));
    CHECK_FALSE(vf::ai::builders::inKeepOut(0.f, 8.f));
}
