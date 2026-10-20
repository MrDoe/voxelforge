// Bounding-box selection: the App half of the feature (the pure math lives in
// src/voxel/box_select.hpp, pinned by tests/test_box_select.cpp).
//
//   Shift+LMB drag   define the box (corner to corner, inclusive lattice AABB)
//   X                clear it (the whole world is editable again)
//   while active     every stamp (Carve/Add/Delete/Paint/Smooth) is scoped to
//                    the cells inside it, the hover tint is clipped to the
//                    same set, and the post pass draws the wireframe
//
// The box is stored once (m_boxSelect) and read by three consumers - the
// stamp clamp in edit/live_edit.cpp, the BrushUBO clip in both backends, and
// the post-pass wireframe - all fed through boxSelectionFeeds(), so the drawn
// box, the previewed cells and the written cells cannot drift apart.
#include "app/app.hpp"

#include "voxel/chunk_index.hpp"
#include "voxel/surfelize.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The pick comes from the store, so it is in range; an env-provided corner is
// not. Clamp onto the lattice so CellBox never holds cells cellAt() cannot see.
static glm::ivec3 clampCell(const vf::voxel::ChunkStore& store, const glm::ivec3& v)
{
    const int n = store.latN();
    return glm::clamp(v, glm::ivec3(0), glm::ivec3(n - 1));
}

void App::beginBoxSelection(const glm::ivec3& anchor)
{
    m_boxAnchor = m_layers.store().loaded() ? clampCell(m_layers.store(), anchor)
                                            : anchor;
    m_boxDragging = true;
    m_boxSelect.set(m_boxAnchor, m_boxAnchor);
    spdlog::info("box select: start at {} {} {}", m_boxAnchor.x, m_boxAnchor.y,
                 m_boxAnchor.z);
}

void App::updateBoxSelection(const glm::ivec3& hover)
{
    if (!m_boxDragging)
        return;
    const glm::ivec3 h = m_layers.store().loaded() ? clampCell(m_layers.store(), hover)
                                                   : hover;
    m_boxSelect.set(m_boxAnchor, h);
}

void App::endBoxSelection()
{
    m_boxDragging = false;
    if (!m_boxSelect.valid()) {
        clearBoxSelection();
        return;
    }
    const glm::ivec3 d = m_boxSelect.sizeCells();
    spdlog::info("box select: {}x{}x{} cells ({} total) ({} {} {}) -> ({} {} {})",
                 d.x, d.y, d.z, m_boxSelect.volumeCells(), m_boxSelect.lo.x,
                 m_boxSelect.lo.y, m_boxSelect.lo.z, m_boxSelect.hi.x,
                 m_boxSelect.hi.y, m_boxSelect.hi.z);
    reportBoxSelection();
}

void App::clearBoxSelection()
{
    if (m_boxSelect.active || m_boxDragging) {
        m_boxSelect.clear();
        m_boxDragging = false;
        m_boxStatCells = m_boxStatObjects = m_boxStatSplats = 0;
        m_lastStampOutside = 0;
        spdlog::info("box select: cleared (the whole world is editable again)");
        return;
    }
    m_lastStampOutside = 0;
}

size_t App::applyBoxSelectionScope(std::vector<vf::voxel::StoreEdit>& edits)
{
    if (!m_boxSelect.valid())
        return edits.size();
    return vf::voxel::CellBox::clampEdits(edits, m_boxSelect);
}

void App::boxSelectionFeeds(glm::vec4& lo, glm::vec4& hi) const
{
    if (m_boxSelect.valid()) {
        lo = glm::vec4(glm::vec3(m_boxSelect.lo), 1.f);
        hi = glm::vec4(glm::vec3(m_boxSelect.hi), 1.f);
    } else {
        lo = glm::vec4(0.f);
        hi = glm::vec4(0.f);
    }
}

void App::reportBoxSelection()
{
    if (!m_boxSelect.valid())
        return;
    auto& store = m_layers.store();
    if (!store.loaded()) {
        spdlog::warn("box select: report requested without a loaded store");
        return;
    }
    const glm::ivec3 lo = m_boxSelect.lo;
    const glm::ivec3 hi = m_boxSelect.hi;

    // Cell census straight from the runtime store: what IS in the box, so the
    // user can see whether the selection covers the structure they meant.
    int cells = 0, objectCells = 0;
    int mats[vf::voxel::kPaletteN] = {};
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) {
                const vf::voxel::StoreCell c = store.cellAt(x, y, z);
                if (!c.solid)
                    continue;
                ++cells;
                if (c.obj)
                    ++objectCells;
                const int m = std::clamp(int(c.mat), 0, int(vf::voxel::kPaletteN) - 1);
                ++mats[m];
            }

    // Surfel census: the disks the box holds, via the SAME range builder the
    // live-edit path uses (bake normals, not a local rule of its own), summed
    // over the chunks the box touches. Boundary disks of a chunk see the
    // range the way describeSurfelsAt does it - per chunk, clamped.
    int parents = 0, edges = 0;
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.edgeBridgeSize = m_edgeBridgeSize;
    sp.cornerFill = m_cornerFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.anisotropy = true;
    const int latN = store.latN();
    const glm::ivec3 blo = glm::max(lo, glm::ivec3(0));
    const glm::ivec3 bhi = glm::min(hi, glm::ivec3(latN - 1));
    for (int cz = blo.z / vf::voxel::CHUNK_N; cz <= bhi.z / vf::voxel::CHUNK_N; ++cz)
        for (int cy = blo.y / vf::voxel::CHUNK_N; cy <= bhi.y / vf::voxel::CHUNK_N; ++cy)
            for (int cx = blo.x / vf::voxel::CHUNK_N;
                 cx <= bhi.x / vf::voxel::CHUNK_N; ++cx) {
                const int chunk = vf::voxel::chunkIndexOf(cx, cy, cz);
                const glm::ivec3 clo = glm::max(blo, glm::ivec3(cx, cy, cz) *
                                                           vf::voxel::CHUNK_N);
                const glm::ivec3 chi = glm::min(bhi, glm::ivec3(cx, cy, cz) *
                                                           vf::voxel::CHUNK_N +
                                                       glm::ivec3(vf::voxel::CHUNK_N - 1));
                const vf::voxel::SurfelRange rg = vf::voxel::buildChunkSurfelsRange(
                    store, chunk, clo, chi + glm::ivec3(1), sp);
                parents += int(rg.surfels.size());
                edges += int(rg.edgeSurfels.size());
            }

    m_boxStatCells = cells;
    m_boxStatObjects = objectCells;
    m_boxStatSplats = parents;

    // Top material, so the log names the dominant substance too.
    int topMat = 0;
    for (int m = 1; m < int(vf::voxel::kPaletteN); ++m)
        if (mats[m] > mats[topMat])
            topMat = m;
    const glm::ivec3 d = m_boxSelect.sizeCells();
    spdlog::info(
        "box select: dims {}x{}x{}  cells {} solid ({} object-owned)  "
        "splats {} (+{} edge)  top mat {} ({} cells)",
        d.x, d.y, d.z, cells, objectCells, parents, edges, topMat, mats[topMat]);
}

} // namespace app
} // namespace vf
