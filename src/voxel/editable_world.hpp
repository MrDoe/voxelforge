#pragma once
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace vf::voxel {

// Persistent editable layer: assets/<file> + world.json entry.
// All created objects are rasterized into VoxelRecords anchored at the
// user-picked voxel (center-bottom convention) and appended here.
// The app hot-reloads this layer via its disk poll within ~0.5 s.
//
// The default instance is the AI "add" layer (ai_edits.vxw, role "object").
// A second instance with role "carve" holds subtractive carve voxels that
// depress the terrain; a third with role "raise" holds additive dome voxels
// that lift the terrain into a half-sphere bump.
struct EditableWorld {
    static constexpr const char* kFileName = "ai_edits.vxw";
    static constexpr const char* kLayerName = "ai_edits";
    static constexpr const char* kCarveFileName = "carve_edits.vxw";
    static constexpr const char* kCarveLayerName = "carve_edits";
    static constexpr const char* kRaiseFileName = "raise_edits.vxw";
    static constexpr const char* kRaiseLayerName = "raise_edits";
    static constexpr float kBand = 0.20f; // surface band like heightmap_gen sweep

    EditableWorld(std::string assetDir = std::string(VOXELFORGE_ASSET_DIR),
                  std::string fileName = std::string(kFileName),
                  std::string layerName = std::string(kLayerName),
                  std::string role = std::string("object"));

    bool load(); // reads existing file if present (empty if missing)
    bool save() const; // writes current records to the layer file + ensures manifest
    void enableInManifest() const; // mark this layer enabled in world.json
    bool ensureManifest(); // adds layer entry to world.json if absent

    // record generation helpers (anchor is lattice coord of selected voxel = bottom-center)
    std::vector<VoxelRecord> makeBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat) const;
    std::vector<VoxelRecord> makeBoxMeters(glm::ivec3 anchor, glm::vec3 sizeM, uint8_t mat) const;
    std::vector<VoxelRecord> makeEllipsoid(glm::ivec3 anchor, glm::vec3 radiusM, uint8_t mat) const;
    std::vector<VoxelRecord> makeCylinderY(glm::ivec3 anchor, float radiusM, float heightM, uint8_t mat) const;
    std::vector<VoxelRecord> makeStamp(glm::ivec3 anchor, const std::vector<StampCell>& cells) const;

    // A carve scoop reaches this far above the base plane along -axisDir so
    // the brush opens the ground it starts at: the terrain's top cell wobbles
    // by +-1 cell of relief around the pick, and a flat cap would leave a
    // one-cell roof over the dig (a covered void gains no water). The floor
    // stays exactly at the picked depth. The hover preview uses the same
    // extent (App::applyEditLive's brush volume).
    static constexpr float kCarveTopMargin = 2.0f * VOXEL;

    // Radial influence profile shared by Add, Carve, Delete and Paint.
    // q = perpendicular distance / radius, so the CENTRE of the brush is at
    // full influence and the rim at zero. Every curve satisfies f(0) = 1,
    // f(1) = 0 and is non-increasing.
    //
    // These are NAMED curves rather than one hidden exponent, because the old
    // scalar had a cliff and no usable middle: `falloff <= 0` returned exactly
    // 1.0, while `falloff = 0.001` was already ((1+cos)/2)^1.001 = 0.5 at half
    // radius - a 0.001 nudge jumped from "no taper at all" to "halved", and the
    // rest of the travel only steepened towards a spike (k = 3 -> 0.125 at half
    // radius). That is what made the control read as inert then confusing.
    // The half-radius column below is the practical way to choose one.
    //
    //  Constant  1              1.00  flat - the legacy footprint, flat-bottomed digs
    //  Sphere    sqrt(1-q^2)    0.87  round dome (hemisphere silhouette)
    //  Root      sqrt(1-q)      0.71  broad shoulder
    //  Smooth    1-3q^2+2q^3    0.50  default; the sculpt-standard S curve
    //  Linear    1-q            0.50  straight cone
    //  Sharp     (1-q)^2        0.25  crease / tight falloff
    enum class FalloffCurve : uint8_t {
        Constant = 0, Sphere, Root, Smooth, Linear, Sharp, Count
    };

    static const char* falloffCurveName(FalloffCurve c)
    {
        switch (c) {
        case FalloffCurve::Constant: return "Constant";
        case FalloffCurve::Sphere: return "Sphere";
        case FalloffCurve::Root: return "Root";
        case FalloffCurve::Smooth: return "Smooth";
        case FalloffCurve::Linear: return "Linear";
        case FalloffCurve::Sharp: return "Sharp";
        default: return "Smooth";
        }
    }

    // f(q) for one curve. Continuous everywhere including q -> 1, so the
    // "does this column emit a cell" decision is never on a floating-point
    // knife edge at the footprint boundary.
    static float falloffCurveAt(FalloffCurve c, float q)
    {
        q = std::clamp(q, 0.0f, 1.0f);
        switch (c) {
        case FalloffCurve::Constant: return 1.0f;
        case FalloffCurve::Sphere: return std::sqrt(std::max(0.0f, 1.0f - q * q));
        case FalloffCurve::Root: return std::sqrt(std::max(0.0f, 1.0f - q));
        case FalloffCurve::Smooth: return 1.0f - q * q * (3.0f - 2.0f * q);
        case FalloffCurve::Linear: return 1.0f - q;
        case FalloffCurve::Sharp: return (1.0f - q) * (1.0f - q);
        default: return 1.0f - q * q * (3.0f - 2.0f * q);
        }
    }

    // Kept for the existing call sites that only know a 0..1 slider: the old
    // scalar now selects the curve nearest to its intent, so a stored value of
    // 0 still means "flat" and mid values land on the dome-like curves instead
    // of a spike. New code should pass a FalloffCurve directly.
    static float radialFalloff(float q, float falloff)
    {
        if (falloff <= 0.0f)
            return 1.0f;
        const FalloffCurve c = falloff < 0.2f   ? FalloffCurve::Root
                               : falloff < 0.45f ? FalloffCurve::Sphere
                               : falloff < 0.8f  ? FalloffCurve::Smooth
                                                 : FalloffCurve::Sharp;
        return falloffCurveAt(c, q);
    }

    // Oriented cylinder stamped along `axisDir` (unit world vector) for `lengthM`,
    // starting at the anchor (base centre). When `carve` is true the FULL solid
    // volume is emitted (the removed material), reaching kCarveTopMargin above
    // the base; otherwise a thin shell band is emitted (flood-filled to solid
    // by VoxelField::build, like other objects) with the exact [0, length] extent.
    // `curve` tapers the FAR end of the volume towards the rim, so the scoop is
    // deepest under the cursor and feathers out at its edge instead of leaving a
    // vertical crater wall. Default Constant = the original hard-edged shape.
    // The near end is NOT tapered: kCarveTopMargin is what opens the ground the
    // scoop starts at, and fading it would put the one-cell roof back. A rim
    // column still cuts one cell, so the footprint boundary stays crisp.
    std::vector<VoxelRecord> makeOrientedCylinder(glm::ivec3 anchor, glm::vec3 axisDir,
                                                  float radiusM, float lengthM,
                                                  uint8_t mat, bool carve,
                                                  FalloffCurve curve = FalloffCurve::Constant) const;

    // "Add" brush volume: the surface GROWS OUT along `axisDir` (the picked
    // surface normal) from the anchor. The footprint is a disk of radius
    // `radiusM` in the plane ACROSS the axis, extruded `heightM` along it and
    // closed by a fillet of radius c = min(radiusM, heightM)/2, so the new
    // face is flat out to radius (radiusM - c): clicking a wall thickens it by
    // the full heightM over its whole footprint and only rounds the outer
    // edge, and the reach is always exactly the brush depth. Nothing is
    // emitted behind the surface (one voxel of base layer excepted, which
    // seals the growth against the surface), so clicking a wall never erodes
    // the far side.
    // `curve` scales that height PER COLUMN by falloffCurveAt(curve, q), and the
    // fillet shrinks with it, so the growth becomes a mound: full heightM under
    // the cursor, nothing at the rim. This REVERSES the flat-top decision above
    // on purpose - a full-height rim is right for "thicken this wall" and wrong
    // for "raise this spot" - so `FalloffCurve::Constant` keeps the old shape
    // reachable. Do not "restore the flat top": the shape was re-chosen so the
    // brush has proportional influence, per the falloff control.
    // The hover preview tints this set (BrushUBO with w < 0 on the axis; the
    // shader repeats the extruded-disk + fillet test, and the curve).
    std::vector<VoxelRecord> makeDome(glm::ivec3 anchor, glm::vec3 axisDir,
                                      float radiusM, float heightM, uint8_t mat,
                                      FalloffCurve curve = FalloffCurve::Constant) const;

    // Exactly ONE cell - the per-voxel Add/Carve target, used when the brush
    // width is 1 voxel (the pick is the cell under the cursor, which the hover
    // outline draws). `stepAlongNormal` moves the target one cell along the
    // dominant axis of `axisDir` first, which is what Add needs: the pick lands
    // on solid material, so adding the picked cell would do nothing. Carve
    // passes false and therefore removes exactly the voxel you hovered.
    // Empty when the target leaves the lattice.
    std::vector<VoxelRecord> makeSingleVoxel(glm::ivec3 anchor, glm::vec3 axisDir,
                                             uint8_t mat, bool stepAlongNormal) const;

    // Full solid ball centred on `anchor` (radiusM in meters): every cell whose
    // centre lies inside. The brush volume of the Delete (clear) and Paint
    // (recolour) edit modes, and what the hover preview tints.
    // `curve` scales the RADIUS per cell by falloffCurveAt(curve, q): a cell is
    // included when its distance is within radiusM * f(q), which turns a Delete
    // from a hard-edged ball into a graded crater and makes the Falloff control
    // mean the same thing in every brush. Default Constant = the original ball.
    std::vector<VoxelRecord> makeSphere(glm::ivec3 anchor, float radiusM, uint8_t mat,
                                        FalloffCurve curve = FalloffCurve::Constant) const;
    // Import a foreign .vxw layer file: translates its records so the object's
    // bottom-center lands on `anchor` and appends the copy to ai_edits.vxw.
    // This is the only runtime placement path — layer files store absolute
    // lattice coords, so enabling a layer shows it where it was baked, while
    // import stamps a moved copy here. Returns records added (0 on read/meta
    // failure or when nothing new survives dedupe/bounds).
    size_t importLayer(const std::string& vxwPath, glm::ivec3 anchor);

    // append new records, deduping within layer (first wins). Returns number added.
    size_t append(const std::vector<VoxelRecord>& newRecs);
    size_t appendBox(glm::ivec3 anchor, glm::ivec3 sizeVox, uint8_t mat);
    void clear(); // removes all AI edits and persists

    // --- arbitrary-object authoring -----------------------------------------
    // Write a standalone named object layer (<name>.vxw, record-only) into the
    // asset dir and register it (enabled, role "object") in world.json so the
    // running app hot-reloads it. Overwrites an existing layer of the same name
    // (the modification path: read_object -> edit -> write_object). Returns
    // false for an empty/illegal name.
    bool writeObjectLayer(const std::string& name,
                          const std::vector<VoxelRecord>& recs);

    // Remove a named object layer file + its manifest entry (never landscape or
    // the packed cache). Returns false if absent, protected, or an illegal name.
    bool deleteObjectLayer(const std::string& name);

    size_t size() const { return m_records.size(); }
    const std::vector<VoxelRecord>& records() const { return m_records; }

    // Keep a layer name filesystem- and manifest-safe: [A-Za-z0-9_-], <=40 chars.
    static std::string sanitizeLayerName(const std::string& name);

private:
    std::string m_assetDir;
    std::string m_fileName;   // e.g. "ai_edits.vxw" or "carve_edits.vxw"
    std::string m_layerName;  // manifest layer name
    std::string m_role;       // "object" or "carve"
    std::string filePath() const;
    std::string manifestPath() const;
    WorldFileMeta meta() const;

    std::vector<VoxelRecord> m_records;
};

} // namespace vf::voxel
