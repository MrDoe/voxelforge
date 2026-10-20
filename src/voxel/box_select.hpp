#pragma once
// Bounding-box selection: a lattice-space AABB that acts as the selector for
// the live-edit brush. While a box is active, every stamp (Carve, Add,
// Delete, Paint, Smooth) only touches the cells inside it, the hover tint is
// clipped to the same set, and the post pass draws the box as a wireframe so
// the scope is visible before the click.
//
// This header is the pure math of that selector and nothing else: it takes no
// App, no store and no GPU state, so the containment/clamping rule can be
// pinned by a unit test (tests/test_box_select.cpp) instead of only by a
// render diff. The App wiring (drag gesture, report, feeds) lives in
// src/app/edit/box_select.cpp.
//
// The box is stored in INCLUSIVE lattice cell indices [lo, hi] - the same
// space the pick, the brush rasterizers and ChunkStore use - so a "contains"
// test is an integer compare and cannot drift by a half-voxel against the
// cell set a stamp actually writes.
#include "voxel/chunk_store.hpp" // StoreEdit (the clamped edit type)

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace vf::voxel {

struct CellBox {
    glm::ivec3 lo { 0, 0, 0 };
    glm::ivec3 hi { 0, 0, 0 };
    bool active = false;

    // Anchor the two opposite corners (any order) and activate.
    void set(glm::ivec3 a, glm::ivec3 b)
    {
        lo = glm::min(a, b);
        hi = glm::max(a, b);
        active = true;
    }
    void clear() { active = false; }

    bool valid() const
    {
        return active && hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z;
    }
    bool contains(const glm::ivec3& v) const
    {
        return v.x >= lo.x && v.x <= hi.x && v.y >= lo.y && v.y <= hi.y &&
               v.z >= lo.z && v.z <= hi.z;
    }
    // Inclusive cell count per axis (0 when inactive).
    glm::ivec3 sizeCells() const
    {
        return valid() ? hi - lo + glm::ivec3(1) : glm::ivec3(0);
    }
    long long volumeCells() const
    {
        const glm::ivec3 s = sizeCells();
        return valid() ? (long long)s.x * s.y * s.z : 0;
    }
    // World-space bounds of the same cells (cell i spans
    // [-WORLD/2 + i*VOXEL, -WORLD/2 + (i+1)*VOXEL]).
    glm::vec3 worldMin() const
    {
        return -0.5f * (float)WORLD + glm::vec3(lo) * (float)VOXEL;
    }
    glm::vec3 worldMax() const
    {
        return -0.5f * (float)WORLD + (glm::vec3(hi) + 1.0f) * (float)VOXEL;
    }
    // Lattice cell one point falls in, for a cell-vs-centre test in a shader.
    // The feed the shaders get carries cell indices, so their test and this
    // one are the same integer compare.
    static glm::ivec3 cellOf(const glm::vec3& p)
    {
        return glm::ivec3(glm::floor((p + 0.5f * (float)WORLD) / (float)VOXEL));
    }

    // Drop every edit outside the box, in place. Returns how many remain.
    // An inactive box keeps everything (the classic, unscoped brush).
    static size_t clampEdits(std::vector<StoreEdit>& edits, const CellBox& box)
    {
        if (!box.valid())
            return edits.size();
        size_t kept = 0;
        for (size_t i = 0; i < edits.size(); ++i) {
            StoreEdit& e = edits[i];
            if (box.contains(glm::ivec3(e.x, e.y, e.z)))
                edits[kept++] = e;
        }
        edits.resize(kept);
        return kept;
    }
};

} // namespace vf::voxel
