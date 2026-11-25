// The live-edit brush: stamp an oriented volume into the runtime ChunkStore,
// patch the GPU buffers of the chunks it touched, and own the stroke's undo
// record plus the "Clear live edits" revert.
#include "app/app.hpp"

#include "app/ui/ui_types.hpp"
#include "app/world/store_overlay.hpp"
#include "render/texture_atlas.hpp"
#include "voxel/chunk_index.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// Fit the plane a Smooth stamp flattens toward from the surfels actually
// rendered around the pick. The mean normal and the surface centroid include
// a read-only ring beyond the brush, so the patch aligns to the surrounding
// voxels instead of cutting a shelf. Surfel normals are the bake's smoothed
// ones (terrain tops include the heightfield blend), which makes the fit more
// precise than voxel occupancy; water and synthetic edge bridges never vote.
vf::voxel::SmoothPlane App::collectSmoothPlane(const glm::ivec3& pickCell,
                                               float radiusM, bool objectPick)
{
    vf::voxel::SmoothPlane plane;
    const glm::vec3 centre = vf::voxel::voxelCenter(pickCell);
    const float ringM = std::min(radiusM, 1.0f);
    const float reach = radiusM + ringM;
    const auto cellOf = [](float w) {
        return int(std::floor((w + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL));
    };
    const glm::ivec3 lo(cellOf(centre.x - reach), cellOf(centre.y - reach),
                        cellOf(centre.z - reach));
    const glm::ivec3 hi(cellOf(centre.x + reach), cellOf(centre.y + reach),
                        cellOf(centre.z + reach));
    const auto chunkOf = [](int cell) {
        return std::clamp(cell / vf::voxel::CHUNK_N, 0, vf::voxel::GRID_N - 1);
    };

    // A short grounded object run is a bump: let the surrounding GROUND into
    // the fit so the bump collapses to the level around it. A tall run is a
    // feature (a post or a wall): only object surfels vote, so it aligns to
    // its own kind. Terrain picks always exclude object surfels entirely.
    bool bumpLike = false;
    if (objectPick) {
        const auto& store = m_layers.store();
        const vf::voxel::StoreCell picked =
            store.cellAt(pickCell.x, pickCell.y, pickCell.z);
        if (picked.solid && picked.obj) {
            int run = 0;
            for (int y = pickCell.y - 1; y >= 0; ++y) {
                const vf::voxel::StoreCell c =
                    store.cellAt(pickCell.x, y, pickCell.z);
                if (c.solid && c.obj)
                    ++run;
                else
                    break;
            }
            bumpLike = run > 0 &&
                       run <= vf::voxel::ChunkStore::kSmoothBumpRunCells;
        }
    }

    float sw = 0.0f;
    glm::vec3 sumN(0.f), sumP(0.f);
    int seenChunks = 0, seenSurfels = 0, rejectedWater = 0,
        rejectedOwner = 0, rejectedDist = 0, nearVotes = 0;
    auto vote = [&](const std::vector<vf::voxel::Surfel>& run) {
        for (const vf::voxel::Surfel& s : run) {
            ++seenSurfels;
            // mat_ao.w: AO (<1.5) terrain, AO+2 (~2) water, AO+8+16*id object.
            const float packed = s.mat_ao.w;
            if (packed > 1.5f && packed < 5.5f) {
                ++rejectedWater;
                continue; // the fixed water plane is never a flatten sample
            }
            const bool isObj = vf::voxel::surfelIsObject(packed);
            if (objectPick) {
                if (!isObj && !bumpLike) {
                    ++rejectedOwner;
                    continue;
                }
            } else if (isObj) {
                // Object-owned surfels can BE the surface around a terrain
                // pick (planks, stonework, a path). They may inform the local
                // plane, but only near the picked height: a wall towering
                // above must not drag the ground up with it. The planner's
                // objectInSpan guard still keeps terrain edits off their cells.
                if (std::abs(glm::vec3(s.pos_rU).y - centre.y) > 0.5f) {
                    ++rejectedOwner;
                    continue;
                }
            }
            const float d = glm::distance(glm::vec3(s.pos_rU), centre);
            if (d < 0.5f)
                ++nearVotes; // a buried/stale pick has no surfel to fit from
            if (d > reach) {
                ++rejectedDist;
                continue;
            }
            const float radial =
                d <= radiusM ? 1.0f
                             : 1.0f - (d - radiusM) / std::max(ringM, 1e-3f);
            const float area = std::max(1e-4f, s.pos_rU.w * s.normal_rV.w);
            float w = radial * area;
            if (objectPick && !isObj)
                w *= 0.5f; // ground only helps a short grounded bump
            else if (!objectPick && isObj)
                w *= 0.3f; // terrain votes dominate a terrain pick's fit
            const glm::vec3 n = glm::vec3(s.normal_rV);
            const float len = glm::length(n);
            if (len < 1e-4f)
                continue;
            sumN += (n / len) * w;
            sumP += glm::vec3(s.pos_rU) * w;
            sw += w;
        }
    };

    // Cached live-editor runs are the current post-stroke state; uncached
    // chunks splice from the GPU's current run, the same readback a stamp's
    // first touch would pay. Edge bridges and the LOD/water tail are excluded.
    vf::voxel::SurfelParams sp;
    for (int cy = chunkOf(lo.y); cy <= chunkOf(hi.y); ++cy)
        for (int cz = chunkOf(lo.z); cz <= chunkOf(hi.z); ++cz)
            for (int cx = chunkOf(lo.x); cx <= chunkOf(hi.x); ++cx) {
                const int ci = vf::voxel::chunkIndexOf(cx, cy, cz);
                ++seenChunks;
                if (m_liveEditor.hasChunk(ci)) {
                    vote(m_liveEditor.chunkSurfels(ci, sp));
                    continue;
                }
                std::vector<uint8_t> bytes;
                uint32_t edgeCount = 0;
                const uint32_t n = m_splatPass.readChunkSurfels(
                    uint32_t(ci), bytes, &edgeCount);
                const uint32_t parents = n - std::min(n, edgeCount);
                if (!parents)
                    continue;
                std::vector<vf::voxel::Surfel> gpu(parents);
                std::memcpy(gpu.data(), bytes.data(),
                            size_t(parents) * sizeof(vf::voxel::Surfel));
                vote(gpu);
            }

    // Without a vote ON the picked surface, the fit would describe geometry
    // metres away (a stale test cell or a buried pick) and could raise the
    // column to that foreign plane. Fall back to the store's voxel fit.
    if (sw > 1e-6f && nearVotes > 0 && glm::length(sumN) > 1e-3f) {
        plane.normal = glm::normalize(sumN);
        plane.point = sumP / sw;
        plane.valid = true;
    }
    if (getenv("VF_SMOOTH_PLANE") || getenv("VF_TRACE")) {
        spdlog::info("smooth plane: pick=({},{},{}) {} valid={} "
                     "chunks={} surfels={} (water={} owner={} far={} near={}) "
                     "weight={:.4f} "
                     "normal=({:.3f},{:.3f},{:.3f}) point=({:.2f},{:.2f},{:.2f})",
                     pickCell.x, pickCell.y, pickCell.z,
                     objectPick ? "object" : "terrain", int(plane.valid),
                     seenChunks, seenSurfels, rejectedWater, rejectedOwner,
                     rejectedDist, nearVotes, sw,
                     plane.normal.x, plane.normal.y, plane.normal.z,
                     plane.point.x, plane.point.y, plane.point.z);
    }
    return plane;
}

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
        // Smooth flattens the selection to the plane fitted from its mean
        // normal and the surrounding surfels. The store helper keeps the two
        // ownership classes separate: terrain never becomes an object, and
        // object surface edits never overwrite terrain.
        const vf::voxel::SmoothPlane plane =
            collectSmoothPlane(m_hoverHit.voxel, radius, m_hoverHit.object);
        vf::voxel::SmoothTerrainEdits smooth = store.makeSmoothEdits(
            m_hoverHit.voxel, radius, m_smoothStrength,
            m_smoothPreserveVolume, &plane);
        // Publish what the planner decided. The tint in the preview can only
        // mark geometry that already exists, so a relaxation that moved every
        // column by less than a voxel looks identical to one that did nothing;
        // these three numbers are what makes the difference legible in the
        // footer without a debug overlay.
        {
            // "N columns, widest run D cells" is what a relaxation actually
            // did, and a sub-voxel relaxation is otherwise invisible.
            // SINGLE PASS, no grouping container: the planner emits each
            // column's run contiguously (its loops are for-z { for-x { emit
            // this column's span } }), so a run ends exactly when the (x,z)
            // key changes. The first version grouped with a linear scan per
            // edit - O(n^2), i.e. tens of millions of comparisons on a 6 m
            // stamp's ~11k edits, added to every stamp for a readout. If the
            // planner ever interleaves columns this over-counts columns rather
            // than crashing, which is the right failure for a diagnostic.
            int columns = 0, widest = 0, lo = 0, hi = 0;
            // Lattice coords are >= 0, so -1 is a sentinel that cannot collide
            // (and needs no <climits>).
            int px = -1, pz = -1;
            for (const vf::voxel::StoreEdit& e : smooth.edits) {
                if (e.x != px || e.z != pz) {
                    widest = std::max(widest, hi - lo + 1);
                    ++columns;
                    lo = hi = e.y;
                    px = e.x;
                    pz = e.z;
                } else {
                    lo = std::min(lo, e.y);
                    hi = std::max(hi, e.y);
                }
            }
            widest = columns > 0 ? std::max(widest, hi - lo + 1) : 0;
            m_smoothLastColumns = columns;
            m_smoothLastMaxDelta = widest;
            m_smoothLastRise = smooth.riseCells;
        }
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
                    /*carve=*/true, m_editFalloffCurve);
                break;
            case EditBrush::Add:
                recs = m_add.makeDome(m_hoverHit.voxel, n, radius, length,
                                      m_editMat, m_editFalloffCurve);
                break;
            case EditBrush::Rotate:
            case EditBrush::Move:
                return; // object transforms are handled by their gizmos
            case EditBrush::Delete:
            case EditBrush::Paint:
                // brush ball centred on the hit cell: delete clears it, paint
                // recolours
                recs = m_carve.makeSphere(m_hoverHit.voxel, radius, m_editMat,
                                          m_editFalloffCurve);
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

    // Bounding-box selection: scope this stamp to the cells inside the box.
    // The hover tint clips to the same set (BrushUBO bBoxLo/bBoxHi), so what
    // the preview shows is exactly what survives this filter. A stamp that is
    // entirely outside the box does nothing at all: no store patch, no stroke,
    // no undo step - the click reads as inert instead of half-applied.
    if (m_boxSelect.valid()) {
        const size_t before = edits.size();
        const size_t kept = applyBoxSelectionScope(edits);
        m_lastStampOutside = int(before - kept);
        if (kept == 0) {
            spdlog::info("box select: stamp fully outside the selection box "
                         "({} cells dropped)", before);
            return;
        }
        if (m_lastStampOutside > 0)
            spdlog::info("box select: {} of {} stamp cells kept inside the box",
                         kept, before);
    } else {
        m_lastStampOutside = 0;
    }

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
            // The older steps are invalid now: each one restores pre-stroke
            // cell states, and replaying them over this unrecorded stroke
            // would resurrect or erase cells it changed. Drop the history.
            const size_t dropped = m_undo.size();
            m_undo.clear();
            spdlog::warn("undo: stroke too large to record, this stroke is not undoable "
                         "({} older undo steps dropped)", dropped);
        }
    }

    m_strokeRiseCells = std::max(m_strokeRiseCells, riseCells);
    // A paint dab can add or remove a foliage-bearing ground material. Mark
    // the cards dirty; finishStroke() rebuilds them from the live height
    // texture once the drag ends (one rebuild per stroke, not per dab).
    if (m_editBrush == EditBrush::Paint)
        m_foliagePaintDirty = true;
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

    const vf::voxel::SurfelParams sp = surfelParams(false);
    m_strokeMargin = margin; // undo must replay the same region
    std::vector<int> changed = m_liveEditor.stamp(edits, sp, margin);
    // The light trigger consumes these at stroke end (finishStroke/undo):
    // accumulate across the dabs of one drag.
    m_lastChangedChunks.insert(m_lastChangedChunks.end(), changed.begin(),
                               changed.end());
    const auto t1 = std::chrono::steady_clock::now();

    size_t nRunSurfels = 0, nRunParents = 0, nRunEdges = 0;
    for (int ci : changed) {
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        nRunSurfels += surfels.size();
        {
            // split the run by segment: a live edit that inflates a chunk shows
            // up here as parents/edges drifting apart from the baked counts
            const uint32_t ec = m_liveEditor.edgeCountOf(ci);
            const uint32_t n = uint32_t(surfels.size());
            nRunParents += n >= ec ? n - ec : 0u;
            nRunEdges += std::min(n, ec);
        }
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
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
    int hx0 = edits[0].x, hx1 = hx0, hz0 = edits[0].z, hz1 = hz0;
    for (const vf::voxel::StoreEdit& e : edits) {
        hx0 = std::min(hx0, e.x); hx1 = std::max(hx1, e.x);
        hz0 = std::min(hz0, e.z); hz1 = std::max(hz1, e.z);
    }
    patchHeightTexture(hx0 - 3, hz0 - 3, hx1 + 3, hz1 + 3, riseCells);
    // Grow the painted region by one brush cell of margin so blades whose
    // ROOT sits just outside the edited columns still get re-evaluated.
    if (m_editBrush == EditBrush::Paint) {
        m_foliageRegion.x = std::min(m_foliageRegion.x, float(hx0 - 1) * vf::voxel::VOXEL - vf::voxel::WORLD * 0.5f);
        m_foliageRegion.y = std::min(m_foliageRegion.y, float(hz0 - 1) * vf::voxel::VOXEL - vf::voxel::WORLD * 0.5f);
        m_foliageRegion.z = std::max(m_foliageRegion.z, float(hx1 + 2) * vf::voxel::VOXEL - vf::voxel::WORLD * 0.5f);
        m_foliageRegion.w = std::max(m_foliageRegion.w, float(hz1 + 2) * vf::voxel::VOXEL - vf::voxel::WORLD * 0.5f);
    }
    m_taaFirstFrame = true; // no history across a geometry change
    const auto t2 = std::chrono::steady_clock::now();    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    m_lastEditMs = float(ms(t0, t2));
    m_lastEditSurfels = nRunSurfels;
    m_lastEditCells = edits.size();
    m_lastEditChunks = changed.size();
    if (!m_dragging || getenv("VF_TRACE"))
        spdlog::info("live edit{}: {} cells, {} chunks, {} run surfels "
                     "({} parents + {} edges), pick {}, "
                     "{:.1f} ms (stamp {:.1f}, gpu {:.1f})",
                     what, edits.size(), changed.size(), nRunSurfels,
                     nRunParents, nRunEdges,
                     m_lastPickObject < 0 ? "none"
                                          : (m_lastPickObject ? "object" : "terrain"),
                     m_lastEditMs, ms(t0, t1), ms(t1, t2));
}

void App::refreshLiveLights()
{
    if (m_lastChangedChunks.empty() || !m_layers.loaded())
        return;
    auto& store = m_layers.store();
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<glm::vec3> emission =
        buildEmissionTable(m_texBindings, m_texAtlas);
    const int budget =
        std::clamp(m_lightBudget, 1, vf::voxel::kMaxLights);

    // First trigger ever: seed the splice base (shared helper: exactly the
    // field-enumerated set the bake upload derives), so the first stroke
    // refreshes instead of replacing the binding contents.
    ensureDerivedBase();

    std::vector<int> region = m_lastChangedChunks;
    m_lastChangedChunks.clear();
    std::sort(region.begin(), region.end());
    region.erase(std::unique(region.begin(), region.end()), region.end());

    // region clusters through the full shared tail (thin+lift inside the
    // region, deterministic like the bake)
    std::vector<vf::voxel::VoxelField::EmissiveCluster> fresh;
    store.collectEmissiveRegion(emission, fresh,
                                std::numeric_limits<int>::max(), region);

    // splice: drop cached clusters whose centroid falls in the touched
    // region (they are recomputed below); survivors keep slots verbatim,
    // so unrelated strokes can never flicker the light set.
    const int latN = store.latN();
    auto centroidChunk = [&](const glm::vec3& p) {
        const int gx = int(std::floor((p.x + 0.5f * vf::voxel::WORLD) /
                                      vf::voxel::VOXEL));
        const int gy = int(std::floor((p.y + 0.5f * vf::voxel::WORLD) /
                                      vf::voxel::VOXEL));
        const int gz = int(std::floor((p.z + 0.5f * vf::voxel::WORLD) /
                                      vf::voxel::VOXEL));
        if (gx < 0 || gy < 0 || gz < 0 || gx >= latN || gy >= latN || gz >= latN)
            return -1;
        return vf::voxel::chunkIndexOfCell(gx, gy, gz);
    };
    using Cluster = vf::voxel::VoxelField::EmissiveCluster;
    std::vector<Cluster> merged;
    merged.reserve(m_derivedClusters.size() + fresh.size());
    for (const Cluster& c : m_derivedClusters) {
        const int cc = centroidChunk(c.pos);
        if (cc >= 0 && std::binary_search(region.begin(), region.end(), cc))
            continue; // recomputed from the live store below
        merged.push_back(c);
    }
    // thin fresh against survivors (drop, never displace: survivors stable)
    const float thin2 = vf::voxel::kEmissiveThinDist * vf::voxel::kEmissiveThinDist;
    for (const Cluster& f : fresh) {
        bool tooClose = false;
        for (const Cluster& c : merged) {
            const glm::vec3 d = c.pos - f.pos;
            if (glm::dot(d, d) < thin2) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose)
            merged.push_back(f);
    }
    m_derivedClusters = merged;

    // Fill binding 25 from visible emissive clusters only. Authored point
    // lights are deliberately excluded; truncation is silent here because
    // the bake path owns the loud warning.
    vf::voxel::LightUBO ubo{};
    const std::vector<vf::voxel::LightSource> noAuthored;
    const int n = vf::voxel::fillLightUBO(noAuthored, merged, budget, ubo, false);
    ubo.perPixelK =
        std::clamp(m_lightK, 1, vf::voxel::worldfile::kLightKMax);
    m_svoPass.setLights(ubo);
    m_splatPass.setLights(ubo);
    const auto t1 = std::chrono::steady_clock::now();
    spdlog::info("live lights: {} derived from visible emissive materials in {} "
                 "chunks, {}/{} slots ({:.1f} ms)",
                 n, region.size(), n, budget,
                 std::chrono::duration<double, std::milli>(t1 - t0).count());
    // A staged move/rotate preview survives strokes: the both-pass upload
    // above just restored base to splat, so re-apply the overlay (no-op
    // when no preview is staged).
    refreshPreviewLights();
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
    refreshLiveLights();
    // One foliage rebuild per paint stroke: the live height texture now has
    // the painted materials, so cards grow on the new grass and vanish where
    // it was painted away. Done at stroke end to keep a drag cheap.
    if (m_foliagePaintDirty) {
        m_foliagePaintDirty = false;
        rebuildGrassCardsRegion(m_foliageRegion.x, m_foliageRegion.y,
                                m_foliageRegion.z, m_foliageRegion.w);
        m_foliageRegion = glm::vec4(1e30f, 1e30f, -1e30f, -1e30f);
    }
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
    queueOverlaySave();
    refreshLiveLights();
    // Undo restores the pre-stroke materials, so the card set follows it.
    rebuildGrassCards();
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
    // Move the file aside instead of deleting it: Clear used to be the one
    // irreversible action in the editor (no backup anywhere). The backup is
    // a plain overlay; copy it back over runtime_edits.vxw to restore.
    const std::string path = overlayPath();
    if (const std::string bak = backupOverlayFile(path, "cleared", true); !bak.empty())
        spdlog::info("live edits: previous overlay kept as '{}'", bak);
    m_overlayDiskChunks = 0;

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

    const vf::voxel::SurfelParams sp = surfelParams(false);
    size_t n = 0;
    for (int ci : chunks) {
        m_liveEditor.seedFromStore(ci, sp);
        const std::vector<vf::voxel::Surfel>& surfels = m_liveEditor.chunkRun(ci, sp);
        n += surfels.size();
        if (!getenv("VF_LIVE_NOSPLAT"))
            m_splatPass.patchChunkSurfels(uint32_t(ci), surfels.data(),
                                          surfels.size() * sizeof(vf::voxel::Surfel),
                                          surfels.size(),
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
    // The store is back to the baked materials: drop any paint-grown cards.
    rebuildGrassCards();
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
