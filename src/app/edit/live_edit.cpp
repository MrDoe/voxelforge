// The live-edit brush: stamp an oriented volume into the runtime ChunkStore,
// patch the GPU buffers of the chunks it touched, and own the stroke's undo
// record plus the "Clear live edits" revert.
#include "app/app.hpp"

#include "app/ui/ui_types.hpp"
#include "app/world/store_overlay.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::applyEditLive()
{
    if (!m_hoverHit.hit)
        return;
    m_lastPickObject = m_hoverHit.object ? 1 : 0;
    glm::vec3 n = m_hoverHit.normal;
    if (glm::length(n) < 1e-3f)
        n = glm::vec3(0.f, 1.f, 0.f);
    n = glm::normalize(n);
    // Keep malformed headless values out of float-to-int conversions and the
    // analytic rasterizers. Interactive values already arrive clamped. The
    // floor is half a voxel, not 0.1 m, so a 1-voxel brush stays 1 voxel wide
    // (a radius clamp at 0.1 m silently made it 3 cells across).
    const float diameter = std::isfinite(m_editDiameter) ? m_editDiameter : 2.0f;
    const float radius = std::clamp(diameter * 0.5f, vf::voxel::VOXEL * 0.5f, 16.0f);
    const float length = std::clamp(
        std::isfinite(m_editDepth) ? m_editDepth : 1.5f, vf::voxel::VOXEL, 12.0f);
    // Per-voxel mode: a 1-voxel Add/Carve touches exactly one cell, so it skips
    // the volume rasterizers entirely (a radius-0.05 dome would still pick up
    // the cell above along the normal).
    const bool perVoxel =
        brushIsPerVoxel() && (m_editBrush == EditBrush::Add ||
                              m_editBrush == EditBrush::Carve);

    auto& store = m_layers.store();
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);

    std::vector<vf::voxel::StoreEdit> edits;
    int riseCells = 2;
    // Smooth refreshes its splats over the full store band (the object
    // relaxation changes neighbouring normals/AO beyond the brush), every
    // other brush keeps the cheap +-3 margin.
    int refreshMargin = vf::voxel::LiveEditor::kStampMargin;
    if (m_editBrush == EditBrush::Smooth) {
        // Smooth uses a terrain height relaxation for terrain picks and a
        // generalised surface-axis relaxation for object picks. The store
        // helper keeps the two ownership classes separate: terrain never
        // becomes an object, and object surface edits never overwrite terrain.
        vf::voxel::SmoothTerrainEdits smooth =
            store.makeSmoothEdits(m_hoverHit.voxel, radius, m_smoothStrength);
        edits = std::move(smooth.edits);
        riseCells = std::max(2, smooth.riseCells + 2);
        if (smooth.objectSurface)
            refreshMargin = vf::voxel::LiveEditor::kExactStampMargin;
    } else {
        riseCells = int(length / vf::voxel::VOXEL) + 2;
        std::vector<vf::voxel::VoxelRecord> recs;
        if (perVoxel) {
            // exactly one cell: Carve removes the voxel under the cursor, Add
            // places the one just outside the surface along the normal.
            recs = m_add.makeSingleVoxel(m_hoverHit.voxel, n, m_editMat,
                                         m_editBrush == EditBrush::Add);
            // remember the cell this stamp WROTE: a held click then re-picks it
            // (the new voxel's own top face) and the stamp trigger suppresses
            // that by identity instead of guessing at a distance threshold.
            if (recs.size() == 1)
                m_lastStampWroteCell = glm::ivec3(recs[0].x, recs[0].y, recs[0].z);
        } else {
            switch (m_editBrush) {
            case EditBrush::Carve:
                recs = m_carve.makeOrientedCylinder(
                    m_hoverHit.voxel, -n, radius, length, m_editMat,
                    /*carve=*/true);
                break;
            case EditBrush::Add:
                recs = m_add.makeDome(m_hoverHit.voxel, n, radius, length,
                                      m_editMat);
                break;
            case EditBrush::Rotate:
            case EditBrush::Move:
                return; // object transforms are handled by their gizmos
            case EditBrush::Delete:
            case EditBrush::Paint:
                // brush ball centred on the hit cell: delete clears it, paint
                // recolours
                recs = m_carve.makeSphere(m_hoverHit.voxel, radius, m_editMat);
                break;
            case EditBrush::Smooth:
                return; // handled above; keeps the switch exhaustive
            }
        }
        if (recs.empty())
            return;

        edits.reserve(recs.size());
        for (const vf::voxel::VoxelRecord& r : recs) {
            vf::voxel::StoreEdit e;
            e.x = r.x;
            e.y = r.y;
            e.z = r.z;
            e.mode = (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Delete)
                         ? vf::voxel::StoreEdit::Mode::Clear
                     : (m_editBrush == EditBrush::Paint)
                         ? vf::voxel::StoreEdit::Mode::Paint
                         : vf::voxel::StoreEdit::Mode::Set;
            e.mat = r.materialId;
            e.hasColor = true;
            e.r = r.r; e.g = r.g; e.b = r.b;
            e.reflectivity = r.reflectivity;
            e.roughness = r.roughness;
            edits.push_back(e);
        }
    }
    if (edits.empty())
        return;

    // Paint must not create geometry: the brush ball covers air cells too, so
    // keep only cells that are already solid in the store (Clear is a no-op on
    // air and needs no filter).
    if (m_editBrush == EditBrush::Paint) {
        std::vector<vf::voxel::StoreEdit> solid;
        solid.reserve(edits.size());
        for (vf::voxel::StoreEdit& e : edits)
            if (store.cellAt(e.x, e.y, e.z).solid)
                solid.push_back(e);
        edits.swap(solid);
        if (edits.empty())
            return; // nothing solid in the ball
    }

    // Undo bookkeeping: record the pre-stamp state of every cell this stamp
    // touches (first occurrence wins, so one undo returns the whole stroke to
    // the state before it started). Keep the original terrain/object bit when
    // restoring a cell that was already solid.
    if (!m_undoOverflow) {
        for (const vf::voxel::StoreEdit& e : edits) {
            const uint64_t key = (uint64_t(uint32_t(e.x)) << 42) |
                                 (uint64_t(uint32_t(e.y)) << 21) | uint64_t(uint32_t(e.z));
            const vf::voxel::StoreCell c = store.cellAt(e.x, e.y, e.z);
            vf::voxel::StoreEdit inv;
            inv.x = e.x; inv.y = e.y; inv.z = e.z;
            if (!c.solid) {
                inv.mode = vf::voxel::StoreEdit::Mode::Clear;
            } else {
                inv.mode = vf::voxel::StoreEdit::Mode::Set;
                inv.terrain = !c.obj;
                inv.mat = c.mat;
                inv.tags = c.tags;
                inv.hasColor = true;
                inv.r = c.r; inv.g = c.g; inv.b = c.b;
                inv.reflectivity = c.reflectivity;
                inv.roughness = c.roughness;
            }
            m_undoPending.emplace(key, inv);
        }
        if (m_undoPending.size() > kUndoMaxCells) {
            m_undoOverflow = true;
            m_undoPending.clear();
            spdlog::warn("undo: stroke too large to record, this stroke is not undoable");
        }
    }

    m_strokeRiseCells = std::max(m_strokeRiseCells, riseCells);
    commitStoreEdits(edits, riseCells, "", refreshMargin);
    if (!m_dragging)
        finishStroke(); // a lone stamp (incl. the headless VF_TEST_EDIT) is a stroke
}

void App::commitStoreEdits(std::vector<vf::voxel::StoreEdit>& edits, int riseCells,
                           const char* what, int margin)
{
    if (edits.empty())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    auto& store = m_layers.store();
    if (!m_liveEditor.attached())
        m_liveEditor.attach(&store);

    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // Live patches keep micro detail: LiveEditor regenerates the chunk's micro
    // tail with the same deterministic hash the bake uses (VF_MICRO=0 off).
    sp.microDetail = m_microDetail;
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    m_strokeMargin = margin; // undo must replay the same region
    std::vector<int> changed = m_liveEditor.stamp(edits, sp, margin);
    const auto t1 = std::chrono::steady_clock::now();

    size_t nRunSurfels = 0, nRunParents = 0, nRunEdges = 0, nRunMicros = 0;
    for (int ci : changed) {
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        nRunSurfels += surfels.size();
        {
            // split the run by segment: a live edit that inflates a chunk shows
            // up here as parents/edges drifting apart from the baked counts
            const uint32_t ms = m_liveEditor.microStartOf(ci);
            const uint32_t ec = m_liveEditor.edgeCountOf(ci);
            const uint32_t n = uint32_t(surfels.size());
            const uint32_t parents = std::min(n, ms >= ec ? ms - ec : 0u);
            nRunParents += parents;
            nRunEdges += std::min(n, ec);
            nRunMicros += n - std::min(n, ms);
        }
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.microStartOf(ci),
                                          m_liveEditor.edgeCountOf(ci));
        // SVO reference backend follows the same edit (chunk-local pool patch)
        const auto& pool = store.pool(ci);
        if (pool && !getenv("VF_LIVE_NOSVO"))
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    // keep the height texture current for the edited columns: the water plane
    // is already there (one fixed level, world-wide), and the foam /
    // absorption / reflected bed all read this texture, so a stale patch would
    // make the new water shade differently from the river.
    {
        int hx0 = edits[0].x, hx1 = hx0, hz0 = edits[0].z, hz1 = hz0;
        for (const vf::voxel::StoreEdit& e : edits) {
            hx0 = std::min(hx0, e.x); hx1 = std::max(hx1, e.x);
            hz0 = std::min(hz0, e.z); hz1 = std::max(hz1, e.z);
        }
        patchHeightTexture(hx0 - 3, hz0 - 3, hx1 + 3, hz1 + 3, riseCells);
    }
    m_taaFirstFrame = true; // no history across a geometry change
    const auto t2 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    m_lastEditMs = float(ms(t0, t2));
    m_lastEditSurfels = nRunSurfels;
    if (!m_dragging || getenv("VF_TRACE"))
        spdlog::info("live edit{}: {} cells, {} chunks, {} run surfels "
                     "({} parents + {} edges + {} micros), pick {}, "
                     "{:.1f} ms (stamp {:.1f}, gpu {:.1f})",
                     what, edits.size(), changed.size(), nRunSurfels,
                     nRunParents, nRunEdges, nRunMicros,
                     m_lastPickObject < 0 ? "none"
                                          : (m_lastPickObject ? "object" : "terrain"),
                     m_lastEditMs, ms(t0, t1), ms(t1, t2));
}

void App::finishStroke()
{
    if (!m_undoPending.empty()) {
        std::vector<vf::voxel::StoreEdit> inv;
        inv.reserve(m_undoPending.size());
        size_t nClear = 0;
        for (auto& kv : m_undoPending) {
            if (kv.second.mode == vf::voxel::StoreEdit::Mode::Clear)
                ++nClear;
            inv.push_back(kv.second);
        }

        // The forward edit can only raise a column by `m_strokeRiseCells`.
        // Undo is different: a Smooth/Delete stamp may have lowered a tall
        // spike, and replaying its inverse Set starts that column above the
        // current height texture. Derive the undo scan bound from the recorded
        // pre-stroke cells and the post-stamp tops, rather than assuming that
        // a lowering needs no upward headroom. This also covers a multi-stamp
        // stroke whose cumulative drop is larger than any one stamp.
        int undoRise = m_strokeRiseCells;
        const int latN = m_layers.loaded() ? m_layers.store().latN() : 0;
        if (latN > 0 && m_heightCpu.size() == size_t(latN) * size_t(latN)) {
            const float half = 0.5f * vf::voxel::WORLD;
            for (const vf::voxel::StoreEdit& e : inv) {
                if (e.mode != vf::voxel::StoreEdit::Mode::Set)
                    continue;
                const size_t h = size_t(e.z) * size_t(latN) + size_t(e.x);
                if (h >= m_heightCpu.size())
                    continue;
                const float top = m_heightCpu[h].x;
                if (top <= -1e29f) {
                    // A restored cell in a column with no current top must be
                    // found by a full-column scan.
                    undoRise = std::max(undoRise, latN + 2);
                    continue;
                }
                const int topCell = std::clamp(
                    int(std::floor((top + half) / vf::voxel::VOXEL)) - 1,
                    0, latN - 1);
                undoRise = std::max(undoRise, e.y - topCell + 2);
            }
        } else {
            undoRise = std::max(undoRise, latN + 2);
        }
        m_undo.push_back({std::move(inv), undoRise, m_strokeMargin});
        if (m_undo.size() > kUndoDepth)
            m_undo.erase(m_undo.begin());
        spdlog::info("undo: stroke recorded ({} cells: {} clear, {} restore, "
                     "{} steps available, height rise {})",
                     m_undo.back().edits.size(), nClear,
                     m_undo.back().edits.size() - nClear, m_undo.size(),
                     m_undo.back().rise);
    }
    m_undoPending.clear();
    m_strokeRiseCells = 2;
    m_strokeMargin = vf::voxel::LiveEditor::kStampMargin;
    m_lastStampWroteCell = glm::ivec3(-1, -1, -1);
    m_undoOverflow = false;
}

void App::undoEdit()
{
    if (m_undo.empty()) {
        spdlog::info("undo: nothing to undo");
        return;
    }
    std::vector<vf::voxel::StoreEdit> inv = std::move(m_undo.back().edits);
    const int rise = m_undo.back().rise;
    const int margin = m_undo.back().margin;
    m_undo.pop_back();
    // Replay the recorded pre-stroke cells through the same store/GPU path a
    // stamp uses, then persist the reverted state like a stroke end does.
    // Replay over the SAME margin the stroke stamped with, not the widest one:
    // a Smooth stroke needs the exact band (it changed normals/AO further than
    // the cheap margin covers), but forcing it on every undo re-derived a 25^3
    // box of geometry for a one-voxel edit, and on an object chunk the
    // store-derived surface does not match the bake (599 edge bridges in that
    // box against 52 in the whole chunk) - which hollowed geometry the user
    // never touched. Undo is now exactly as surgical as the edit it reverts.
    commitStoreEdits(inv, rise, " undo", margin);
    // Persist the reverted state (no full rebuild here: undo must stay
    // instant; the touched chunks keep the live path's store-derived shading
    // until the next reload, exactly like a painted stroke).
    m_overlayWriter.queue(m_layers.store(), overlayPath());
}

void App::clearLiveEdits()
{
    auto& store = m_layers.store();
    if (!store.hasEditedChunks()) {
        spdlog::info("live edits: nothing to clear");
        return;
    }
    // Stop a pending overlay write first: a snapshot queued before the click
    // could otherwise land after the delete and resurrect the edits.
    m_overlayWriter.flush();
    const std::string path = overlayPath();
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) && ec)
        spdlog::warn("live edits: cannot remove '{}': {}", path, ec.message());

    const std::vector<int> chunks = store.editedChunks();
    // the per-chunk edit AABBs drive the height-texture refresh; collect them
    // before the revert (the store forgets the edits)
    std::vector<std::pair<glm::ivec3, glm::ivec3>> bounds;
    bounds.reserve(chunks.size());
    for (int ci : chunks) {
        int lo[3], hi[3];
        if (store.chunkEditBounds(ci, lo, hi))
            bounds.emplace_back(glm::ivec3(lo[0], lo[1], lo[2]),
                                glm::ivec3(hi[0], hi[1], hi[2]));
    }
    // re-adopt the resident (baked) pools: every runtime edit is dropped
    m_layers.invalidateStore();
    auto& fresh = m_layers.store();
    m_liveEditor.attach(&fresh);

    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    sp.microDetail = m_microDetail;
    sp.lodRings = false;
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    size_t n = 0;
    for (int ci : chunks) {
        m_liveEditor.seedFromStore(ci, sp);
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        n += surfels.size();
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
                                          m_liveEditor.microStartOf(ci),
                                          m_liveEditor.edgeCountOf(ci));
        const auto& pool = fresh.pool(ci);
        if (pool && !getenv("VF_LIVE_NOSVO"))
            m_svoPass.patchChunk(uint32_t(ci), *pool);
    }
    // A cleared edit can restore a live Smooth column far above the cached
    // post-edit top. Use a full-column scan rather than a magic headroom.
    for (const auto& b : bounds)
        patchHeightTexture(b.first.x - 3, b.first.z - 3, b.second.x + 3,
                           b.second.z + 3, fresh.latN());
    m_undo.clear();
    m_undoPending.clear();
    m_strokeRiseCells = 2;
    m_undoOverflow = false;
    m_taaFirstFrame = true;
    spdlog::info("live edits: cleared {} chunks ({} surfels re-derived), overlay removed",
                 chunks.size(), n);
    // The in-place revert above lands this frame; a full rebuild then restores
    // what a live patch cannot (baked LOD rings, exact bake normals), so the
    // cleared world converges to the pristine bake.
    requestWorldReload();
}

glm::vec3 App::storeNormalAt(const glm::ivec3& v)
{
    auto& store = m_layers.store();
    const glm::vec3 p = vf::voxel::voxelCenter(v);
    const float e = 0.15f;
    glm::vec3 n(
        store.sampleWorld(p + glm::vec3(e, 0, 0)).d -
            store.sampleWorld(p - glm::vec3(e, 0, 0)).d,
        store.sampleWorld(p + glm::vec3(0, e, 0)).d -
            store.sampleWorld(p - glm::vec3(0, e, 0)).d,
        store.sampleWorld(p + glm::vec3(0, 0, e)).d -
            store.sampleWorld(p - glm::vec3(0, 0, e)).d);
    return glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.f, 1.f, 0.f);
}

void App::adoptPickOwnership(glm::ivec3 v)
{
    if (!m_layers.loaded())
        return;
    const auto smp = m_layers.field().sampleWorld(vf::voxel::voxelCenter(v));
    m_hoverHit.object = smp.obj;
    m_hoverHit.layer = smp.layer;
}

} // namespace app
} // namespace vf
