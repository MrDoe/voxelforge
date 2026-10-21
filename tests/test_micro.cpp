// Micro-voxel field + SVO tests.
//
// The micro subsystem (a high-detail object on its own fine lattice, rendered
// as a SEPARATE SVO world) had NO automated coverage, and that is how a whole
// class of bugs shipped: the field's byte encoding was a whole-voxel
// truncation, which the world bake can afford (its raymarcher is a DDA that
// tests the byte's SIGN) but a sphere tracer cannot (it needs a real
// distance), so the field was flat zero across the surface and the material
// lookup read air cells - the vase rendered black.
//
// These tests pin the parts that actually bit:
//   * the SDF's sign convention and its half-cell offset,
//   * the EIGHTH-VOXEL quantisation (the exact distances, not a truncation),
//   * the margin that keeps the object off the grid's boundary,
//   * the DENSE field matching the CPU field cell for cell,
//   * the brick packing carrying the same distances,
//   * a CPU replica of the shader's trace finding the object's SURFACE (and
//     not the world's bounding box),
//   * the material lookup landing on the SOLID cell, not on air filler.
#include "voxel/micro_field.hpp"
#include "voxel/micro_svo.hpp"
#include "voxel/surfelize.hpp"
#include "voxel/worldfile.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vf::voxel;
using namespace glm;

namespace {

// A solid cube occupying cells [0, 2*half] on each axis, on a 5 mm lattice.
// It is anchored at the layer's ORIGIN so the object's first cell lands on
// field index kMicroMarginCells and the object's near surface sits exactly on
// the world offset - which makes every expectation below a plain number.
WorldFileData cubeLayer(int half, float voxel)
{
    WorldFileData d;
    const int side = 2 * half + 1;
    d.meta.worldSize = float(side) * voxel;
    d.meta.voxelSize = voxel;
    d.meta.waterLevel = -0.9f;
    d.meta.gridN = 1;
    d.meta.brickN = 8;
    for (int z = 0; z < side; ++z)
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) {
                VoxelRecord v;
                v.x = uint16_t(x);
                v.y = uint16_t(y);
                v.z = uint16_t(z);
                v.r = 188;
                v.g = 179;
                v.b = 164;
                v.a = 255;
                v.materialId = 20;
                d.voxels.push_back(v);
            }
    return d;
}

// The signed distance the shader reads back out of a byte: eighth-voxels.
float decodeByte(uint8_t raw, float voxel)
{
    int r = int(raw);
    if (r >= 128)
        r -= 256;
    return float(r) * voxel / float(kMicroSdfQuantum);
}

// --- a CPU replica of the shader's field sampling and trace -----------------
// This mirrors micro_overlay.comp. It is the same code the GPU runs, in a form
// a unit test can call: the bug it exists to catch (the trace converging on
// the world's bounding box instead of the object) is only observable end to
// end.

struct FieldWorld {
    const MicroWorld* w = nullptr;
    ivec3 dim { 0 };
    uint off = 0;

    float cell(ivec3 c) const
    {
        c = glm::clamp(c, ivec3(0), dim - ivec3(1));
        const size_t i = off + size_t((c.z * dim.y + c.y) * dim.x + c.x);
        return decodeByte(w->field[i], w->voxel);
    }

    // The same sample, but with the stencil CLAMPED TO ITS OWN 8-CELL BRICK -
    // which is what reading the field out of the bricks forces, because a
    // brick can only be indexed inside itself. Used as a negative control.
    float mapBrickClamped(vec3 p) const
    {
        const vec3 wmin = w->worldMin;
        const vec3 wsize = w->worldMax - wmin;
        const vec3 rp = p - wmin;
        if (rp.x < 0 || rp.y < 0 || rp.z < 0 || rp.x >= wsize.x || rp.y >= wsize.y ||
            rp.z >= wsize.z) {
            const vec3 q = glm::max(wmin - p, p - (wmin + wsize));
            return std::max(glm::length(glm::max(q, vec3(0.0f))) +
                                std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f),
                            1e-4f);
        }
        const vec3 g = rp / w->voxel - 0.5f;
        const ivec3 i0 = ivec3(glm::floor(g));
        const vec3 f = g - vec3(i0);
        // The brick the sample lands in, and the clamp to it.
        const ivec3 b0 = (i0 / 8) * 8;
        auto clamped = [&](ivec3 c) {
            return glm::clamp(c, b0, b0 + ivec3(7));
        };
        const float s000 = cell(clamped(i0 + ivec3(0, 0, 0)));
        const float s100 = cell(clamped(i0 + ivec3(1, 0, 0)));
        const float s010 = cell(clamped(i0 + ivec3(0, 1, 0)));
        const float s110 = cell(clamped(i0 + ivec3(1, 1, 0)));
        const float s001 = cell(clamped(i0 + ivec3(0, 0, 1)));
        const float s101 = cell(clamped(i0 + ivec3(1, 0, 1)));
        const float s011 = cell(clamped(i0 + ivec3(0, 1, 1)));
        const float s111 = cell(clamped(i0 + ivec3(1, 1, 1)));
        const float c00 = s000 + (s100 - s000) * f.x;
        const float c10 = s010 + (s110 - s010) * f.x;
        const float c01 = s001 + (s101 - s001) * f.x;
        const float c11 = s011 + (s111 - s011) * f.x;
        const float c0 = c00 + (c10 - c00) * f.y;
        const float c1 = c01 + (c11 - c01) * f.y;
        return c0 + (c1 - c0) * f.z;
    }

    // The shader's trilinear sample of the dense field.
    float map(vec3 p) const
    {
        const vec3 wmin = w->worldMin;
        const vec3 wsize = w->worldMax - wmin;
        const vec3 rp = p - wmin;
        if (rp.x < 0 || rp.y < 0 || rp.z < 0 || rp.x >= wsize.x || rp.y >= wsize.y ||
            rp.z >= wsize.z) {
            const vec3 q = glm::max(wmin - p, p - (wmin + wsize));
            return std::max(glm::length(glm::max(q, vec3(0.0f))) +
                                std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f),
                            1e-4f);
        }
        const vec3 g = rp / w->voxel - 0.5f;
        const ivec3 i0 = ivec3(glm::floor(g));
        const vec3 f = g - vec3(i0);
        const float s000 = cell(i0 + ivec3(0, 0, 0));
        const float s100 = cell(i0 + ivec3(1, 0, 0));
        const float s010 = cell(i0 + ivec3(0, 1, 0));
        const float s110 = cell(i0 + ivec3(1, 1, 0));
        const float s001 = cell(i0 + ivec3(0, 0, 1));
        const float s101 = cell(i0 + ivec3(1, 0, 1));
        const float s011 = cell(i0 + ivec3(0, 1, 1));
        const float s111 = cell(i0 + ivec3(1, 1, 1));
        const float c00 = s000 + (s100 - s000) * f.x;
        const float c10 = s010 + (s110 - s010) * f.x;
        const float c01 = s001 + (s101 - s001) * f.x;
        const float c11 = s011 + (s111 - s011) * f.x;
        const float c0 = c00 + (c10 - c00) * f.y;
        const float c1 = c01 + (c11 - c01) * f.y;
        return c0 + (c1 - c0) * f.z;
    }

    // The shader's microTrace: sphere-trace, then bisect to the crossing.
    bool trace(vec3 ro, vec3 rd, float tMax, float& tHit) const
    {
        tHit = 0.0f;
        if (map(ro) < 0.0f)
            return false;
        const float kMaxStep = 0.25f;
        const float minStep = 0.25f * w->voxel;
        float t = 1e-5f, prevT = t;
        for (int i = 0; i < 512; ++i) {
            if (t > tMax)
                return false;
            const float d = map(ro + rd * t);
            if (d < 0.0f) {
                float lo = prevT, hi = t;
                for (int b = 0; b < 16; ++b) {
                    const float mid = 0.5f * (lo + hi);
                    if (map(ro + rd * mid) > 0.0f)
                        lo = mid;
                    else
                        hi = mid;
                }
                tHit = hi;
                return true;
            }
            prevT = t;
            t += std::clamp(d * 0.85f, minStep, kMaxStep);
        }
        return false;
    }
};

} // namespace

TEST_CASE("micro field: the surface sits half a voxel out, with the right sign")
{
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
    REQUIRE(err.empty());

    const int m = kMicroMarginCells;
    // The outermost solid cell is half a voxel inside the surface.
    const size_t corner = f.idx(m, m, m);
    REQUIRE(f.solid[corner] == 1);
    CHECK(f.sdf[corner] == doctest::Approx(-0.5f * vox));

    // The cell just outside it is half a voxel outside the surface.
    const size_t outside = f.idx(m - 1, m, m);
    REQUIRE(f.solid[outside] == 0);
    CHECK(f.sdf[outside] == doctest::Approx(+0.5f * vox));

    // Two cells out is one and a half voxels.
    CHECK(f.sdf[f.idx(m - 2, m, m)] == doctest::Approx(+1.5f * vox));

    // One cell in, but still on the object's EDGE (y and z are the object's
    // minimum), so the nearest air is one step away: the depth is still half a
    // voxel.
    REQUIRE(f.solid[f.idx(m + 1, m, m)] == 1);
    CHECK(f.sdf[f.idx(m + 1, m, m)] == doctest::Approx(-0.5f * vox));

    // The cell diagonally in from the corner is the INTERIOR's first layer:
    // two steps from the nearest air, so a voxel and a half inside.
    REQUIRE(f.solid[f.idx(m + 1, m + 1, m + 1)] == 1);
    CHECK(f.sdf[f.idx(m + 1, m + 1, m + 1)] == doctest::Approx(-1.5f * vox));
}

TEST_CASE("micro field: the byte quantisation keeps the half-voxel offsets")
{
    // The bug this pins: the field used to be stored as int(sdf/voxel), so
    // -0.5 and +0.5 both truncated to 0 and the field was flat zero across the
    // whole surface. At eighth-voxel resolution they are -4 and +4.
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    const int m = kMicroMarginCells;
    const int rawSolid =
        int(std::lround(f.sdf[f.idx(m, m, m)] * kMicroSdfQuantum / vox));
    const int rawAir =
        int(std::lround(f.sdf[f.idx(m - 1, m, m)] * kMicroSdfQuantum / vox));
    CHECK(rawSolid == -4);
    CHECK(rawAir == 4);
    CHECK(rawSolid != 0);
    CHECK(rawAir != 0);
}

TEST_CASE("micro field: the margin keeps the object off the grid's boundary")
{
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    // The grid must be larger than the object, and the object's cells must sit
    // INSIDE it - the shader's field stencil reaches one cell past the sample
    // point, so a surface on the grid's edge would interpolate against the
    // filler and the field would explode there.
    const int m = kMicroMarginCells;
    CHECK(f.dim.x == 7 + 2 * m);
    CHECK(f.solid[f.idx(m, m, m)] == 1);
    CHECK(f.solid[f.idx(0, m, m)] == 0);
    CHECK(f.solid[f.idx(f.dim.x - 1, m, m)] == 0);
    // The origin moves BACK by the margin so the object's world position is
    // unchanged by the margin.
    CHECK(f.origin.x == doctest::Approx(-float(m) * vox));
}

TEST_CASE("micro world: the dense field matches the CPU field cell for cell")
{
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    MicroWorld w;
    synthesizeMicroSvo(f, glm::vec3(3.0f, 1.0f, 5.0f), w);
    REQUIRE(w.field.size() == f.size3());
    for (size_t i = 0; i < f.size3(); ++i) {
        const int raw = int(std::lround(f.sdf[i] * kMicroSdfQuantum / vox));
        CHECK(decodeByte(w.field[i], vox) ==
              doctest::Approx(float(raw) * vox / float(kMicroSdfQuantum)));
    }
    // The world is placed where it was asked to be, margin included.
    CHECK(w.worldMin.x == doctest::Approx(3.0f - float(kMicroMarginCells) * vox));
}

TEST_CASE("micro world: the bricks carry the same distances as the dense field")
{
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    MicroWorld w;
    synthesizeMicroSvo(f, glm::vec3(0.0f), w);
    REQUIRE(w.brickCount() > 0);

    // The bricks and the dense field hold the same field, so the multiset of
    // decoded distances has to be identical.
    std::vector<int> fromBricks, fromDense;
    for (size_t b = 0; b < w.brickCount(); ++b)
        for (int c = 0; c < 8 * 8 * 8; ++c) {
            const uint32_t w0 = w.bricks[b * BRICK_WORDS + size_t(c) * 2];
            int raw = int((w0 >> 24) & 255u);
            if (raw >= 128)
                raw -= 256;
            fromBricks.push_back(raw);
        }
    for (uint8_t raw : w.field) {
        int r = int(raw);
        if (r >= 128)
            r -= 256;
        fromDense.push_back(r);
    }
    std::sort(fromBricks.begin(), fromBricks.end());
    std::sort(fromDense.begin(), fromDense.end());
    // The bricks cover the chunk grid (a superset of the object's cells), so
    // compare the object's own values: every dense value must appear in the
    // brick set, and the brick set may hold extra filler cells.
    for (int raw : fromDense)
        CHECK(std::binary_search(fromBricks.begin(), fromBricks.end(), raw));
    CHECK(fromBricks.size() >= fromDense.size());
}

TEST_CASE("micro world: a solid cell in a brick carries the record's colour")
{
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    MicroWorld w;
    synthesizeMicroSvo(f, glm::vec3(0.0f), w);
    bool sawSolid = false;
    for (size_t b = 0; b < w.brickCount() && !sawSolid; ++b) {
        for (int c = 0; c < 8 * 8 * 8 && !sawSolid; ++c) {
            const uint32_t w0 = w.bricks[b * BRICK_WORDS + size_t(c) * 2];
            const uint32_t w1 = w.bricks[b * BRICK_WORDS + size_t(c) * 2 + 1];
            int raw = int((w0 >> 24) & 255u);
            if (raw >= 128)
                raw -= 256;
            if (raw >= 0)
                continue; // air cell
            // Solid: the albedo is the record colour and the material byte has
            // the object flag.
            CHECK((w0 & 255u) == 188u);
            CHECK(((w0 >> 8) & 255u) == 179u);
            CHECK(((w0 >> 16) & 255u) == 164u);
            CHECK((((w1 >> 24) & 128u) != 0u));
            CHECK(((w1 >> 24) & 127u) == 20u);
            sawSolid = true;
        }
    }
    CHECK(sawSolid);
}

TEST_CASE("micro world: an AIR cell is black, material 0 - and must never be read")
{
    // The air filler is what the material lookup used to find: black albedo,
    // grass material. It is deliberately distinguishable, so a lookup that
    // lands on air is a bug and not a palette accident.
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
    const int m = kMicroMarginCells;
    CHECK(f.solid[f.idx(m - 1, m, m)] == 0);
    CHECK(f.r[f.idx(m - 1, m, m)] == 0);
    CHECK(f.g[f.idx(m - 1, m, m)] == 0);
    CHECK(f.b[f.idx(m - 1, m, m)] == 0);
    CHECK(f.mat[f.idx(m - 1, m, m)] == 0);
}

TEST_CASE("micro world: the trace finds the OBJECT's surface, not the box's face")
{
    // The bug this pins: outside the world's AABB the shader falls back to the
    // distance to the box, which is ZERO on the box face. A trace that treats
    // "within eps of the surface" as a hit therefore stops on the BOX and the
    // object renders as its own bounding box - a flat slab, with the material
    // lookup landing on air. The hit must be the object's own surface.
    const float vox = 0.005f;
    const int half = 3;
    WorldFileData d = cubeLayer(half, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    MicroWorld w;
    synthesizeMicroSvo(f, glm::vec3(10.0f, 10.0f, 10.0f), w);
    FieldWorld fw;
    fw.w = &w;
    fw.dim = w.fieldDim;
    fw.off = 0;

    // The cube is anchored at the layer origin and is 2*half+1 cells wide, so
    // it spans [offset, offset + (2*half+1)*voxel] and the ray (coming from
    // +x) meets its FAR face first. The field's zero crossing is exactly on
    // that face: the SDF puts the surface half a voxel outside the outermost
    // solid cell's centre, i.e. on the cell's outer face.
    const float farFace = 10.0f + float(2 * half + 1) * vox;
    const float centre = 10.0f + float(half) * vox;

    // Look at the cube from +x, level with its centre.
    const vec3 ro(centre + 0.30f, centre, centre);
    const vec3 rd(-1.0f, 0.0f, 0.0f);
    float tHit = 0.0f;
    REQUIRE(fw.trace(ro, rd, 2.0f, tHit));
    const vec3 hit = ro + rd * tHit;
    // The hit is the cube's surface, NOT the AABB's face. The AABB's near face
    // is the margin (4 cells) further out, and a trace that stopped there
    // would be off by a whole margin.
    CHECK(std::abs(hit.x - farFace) < 0.25f * vox);
    CHECK(hit.x < w.worldMax.x - 2.0f * vox);
    // ... and the cell it lands in is solid.
    const ivec3 c = ivec3(glm::floor((hit - w.worldMin) / vox));
    CHECK(f.solid[f.idx(c.x, c.y, c.z)] == 1);
}

TEST_CASE("micro world: a micro layer can be ROTATED about its base, in place")
{
    // "Live movement" for a micro object: the manifest's rot/rotX/rotZ must
    // spin the object about its bottom-centre pivot WITHOUT moving it. The
    // pivot is the same convention the world uses, so "yaw about the base"
    // means the same thing in both.
    const float vox = 0.005f;
    const int half = 3;
    WorldFileData d = cubeLayer(half, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    // The object's bottom-centre pivot, in the field's cells. The field's
    // cells are the object's cells rebased by the margin, so the pivot is the
    // object's centre in x/z and its base in y.
    const glm::ivec3 objMin(kMicroMarginCells, kMicroMarginCells, kMicroMarginCells);
    const int objCells = 2 * half + 1;
    const glm::ivec3 pivotCell = objMin + glm::ivec3(objCells / 2, 0, objCells / 2);

    // The unrotated world, placed so the object's min corner is at POS.
    const glm::vec3 pos(10.0f, 2.0f, 10.0f);
    MicroWorld plain;
    synthesizeMicroSvo(f, pos, plain);
    const glm::vec3 plainPivot = plain.worldMin + (glm::vec3(pivotCell) + 0.5f) * vox;

    // Rotated 90 degrees about Y.
    std::vector<VoxelRecord> rotated;
    glm::ivec3 minShift;
    REQUIRE(rotateMicroRecords(d.voxels, d.meta, 90.0f, 0.0f, 0.0f, rotated, minShift));
    REQUIRE(!rotated.empty());
    // The records are uint16: a rotation that sweeps past zero must not wrap
    // a coordinate to 65535 (measured: a 90 degree yaw of an object longer in
    // z than in x reaches x = -35).
    for (const VoxelRecord& r : rotated) {
        CHECK(r.x < 60000u);
        CHECK(r.y < 60000u);
        CHECK(r.z < 60000u);
    }
    // The rotated shape keeps the source's solidity: a destination-driven map
    // cannot punch holes in the surface, so the volume survives.
    CHECK(rotated.size() > d.voxels.size() * 0.9f);

    MicroField rf;
    REQUIRE(buildMicroField(rotated, d.meta, rf, &err));
    MicroWorld turned;
    // The offset correction keeps the pivot fixed: the rotation moved the
    // shape's min corner by minShift cells, so the world offset moves with it.
    synthesizeMicroSvo(rf, pos + glm::vec3(minShift) * d.meta.voxelSize, turned);
    // The pivot's cell in the ROTATED field: the field rebases the rotated
    // records to their own min corner, and the pivot's offset from that min
    // shrank by minShift (the shape's min corner moved toward it).
    const glm::ivec3 turnedPivotCell = objMin +
                                       glm::ivec3(objCells / 2, 0, objCells / 2) -
                                       minShift;
    const glm::vec3 turnedPivot =
        turned.worldMin + (glm::vec3(turnedPivotCell) + 0.5f) * vox;
    CHECK(glm::length(turnedPivot - plainPivot) < 0.001f);
}

TEST_CASE("micro world: a micro object is PICKABLE")
{
    // The app's pick path marches the WORLD's store, which by design holds no
    // micro cells - so without a pick of its own a micro object is invisible to
    // the pointer: clicking it selects nothing and it cannot be moved.
    const float vox = 0.005f;
    const int half = 3;
    WorldFileData d = cubeLayer(half, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    const glm::vec3 offset(10.0f, 2.0f, 10.0f);
    // The object's centre in world space (the field's cells are the object's
    // cells, rebased by the margin).
    // The object's cells are 0..2*half, so its centre cell is (2*half)/2.
    const int objCells = 2 * half + 1;
    const float centreCell = float(objCells - 1) * 0.5f;
    const glm::vec3 centre = offset + glm::vec3(centreCell * vox);

    // A ray from outside straight at the object's centre.
    const glm::vec3 ro = centre + glm::vec3(0.30f, 0.0f, 0.0f);
    const glm::vec3 rd = glm::normalize(centre - ro);
    const MicroPick hit = pickMicroField(f, offset, ro, rd);
    REQUIRE(hit.hit);
    // The hit is on the object's near SURFACE (the ray comes from +x, so it
    // meets the LAST cell's outer face), and the distance matches.
    const float surfaceX = offset.x + (float(objCells) - 0.5f) * vox;
    // Within a voxel: the field is sampled trilinearly and the ray runs
    // along a cell BOUNDARY in y/z, so the crossing is a blend of four cells
    // rather than exactly the face midpoint.
    CHECK(std::abs(hit.pos.x - surfaceX) < vox);
    CHECK(std::abs(hit.pos.y - centre.y) < vox);
    CHECK(std::abs(hit.dist - (ro.x - surfaceX)) < vox);
    // The normal points back at the camera (out of the near face).
    CHECK(hit.normal.x < -0.5f);

    // A ray that misses the object entirely.
    const MicroPick miss = pickMicroField(f, offset, ro,
                                          glm::normalize(glm::vec3(1.f, 0.f, 0.5f)));
    CHECK(!miss.hit);
}

TEST_CASE("micro world: a micro object CASTS a shadow onto the world")
{
    // The world's sun shadow is BAKED per surfel, so the "cast" half of the
    // two-way shadow contract is a test the bake runs against the micro fields
    // - they are not in the VoxelField. microShadow is that test: it must be
    // occluded when the sun ray passes THROUGH the object and clear when it
    // passes beside it, and the world bake's 0.1 m DDA step is too coarse to
    // find a 5 mm object, which is why it marches at a quarter voxel.
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox); // 7 cells = 3.5 cm, on a 5 mm lattice
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    const glm::vec3 offset(10.0f, 2.0f, 10.0f);
    MicroOccluder oc;
    oc.field = &f;
    oc.worldOffset = offset;
    oc.aabbMin = f.origin + offset;
    oc.aabbMax = oc.aabbMin + f.extent();
    const std::vector<MicroOccluder> occ { oc };

    const glm::vec3 sun = glm::normalize(glm::vec3(-0.703f, 0.559f, -0.439f));
    // The object's own centre, NOT the grid's: the air margin is asymmetric
    // about the object (it sits at cell M..M+objDim), so f.extent()*0.5 is
    // 2.5 mm outside the object and the ray below would miss it.
    const int halfCells = 3; // cubeLayer(3, ...): cells 0..6
    const glm::vec3 centre =
        offset + glm::vec3(float(2 * halfCells + 1) * vox) * 0.5f;

    // A point on the ground beside the object, with the sun DIRECTLY behind
    // the object: the ray must pass through it.
    const glm::vec3 through = centre - sun * 0.30f;
    CHECK(microShadow(occ, through, sun) == 0.0f);

    // The same point with the ray offset sideways: clear.
    const glm::vec3 beside = through + glm::vec3(0.5f, 0.0f, 0.0f);
    CHECK(microShadow(occ, beside, sun) == 1.0f);

    // No occluders at all is a no-op (the bake must be bit-identical when the
    // feature is off).
    CHECK(microShadow({}, through, sun) == 1.0f);
}

TEST_CASE("micro world: the field is CONTINUOUS across a brick boundary")
{
    // The bug this pins: the field used to be read out of the BRICKS, whose
    // trilinear stencil can only reach inside its own brick, so the field
    // jumped at every brick boundary (+0.5 voxel to -1.0 voxel in one step)
    // and the tracer's bisection had no zero crossing to find. The dense
    // field has no such boundary, so the field must vary smoothly along a
    // line that crosses one.
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(6, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    MicroWorld w;
    synthesizeMicroSvo(f, glm::vec3(0.0f), w);
    FieldWorld fw;
    fw.w = &w;
    fw.dim = w.fieldDim;
    fw.off = 0;

    // A brick boundary is every 8 cells. Measure the field's DISCONTINUITY
    // across one: sample a hundredth of a voxel either side and compare.
    // A continuous field's value changes by the local slope over that
    // hundredth - a few hundredths of a voxel. A brick-clamped stencil jumps a
    // whole voxel, because the cell past the block's edge reads the edge cell
    // instead of its own.
    const int y = w.fieldDim.y / 2, z = w.fieldDim.z / 2;
    const float e = 0.01f * vox;
    // The jump of a brick-clamped stencil lands on the CENTRE of the brick's
    // first cell: below it the stencil is clamped to the previous brick's edge
    // cell, at it the stencil reaches the new brick's own cells.
    for (int bx = 8; bx + 1 < w.fieldDim.x; bx += 8) {
        const float at = (float(bx) + 0.5f) * vox;
        const vec3 before = w.worldMin + vec3(at - e, float(y) * vox, float(z) * vox);
        const vec3 after = w.worldMin + vec3(at + e, float(y) * vox, float(z) * vox);
        const float jump = std::abs(fw.map(after) - fw.map(before));
        CHECK(jump < 0.2f * vox);
    }

    // NEGATIVE CONTROL: the same measurement against a BRICK-CLAMPED stencil -
    // which is what reading the field out of the bricks forces - MUST be
    // discontinuous. If this ever passes, the measurement above is not
    // actually crossing a brick boundary and it tests nothing.
    float worstJump = 0.0f;
    for (int bx = 8; bx + 1 < w.fieldDim.x; bx += 8) {
        const float at = (float(bx) + 0.5f) * vox;
        const vec3 before = w.worldMin + vec3(at - e, float(y) * vox, float(z) * vox);
        const vec3 after = w.worldMin + vec3(at + e, float(y) * vox, float(z) * vox);
        worstJump = std::max(worstJump,
                             std::abs(fw.mapBrickClamped(after) -
                                      fw.mapBrickClamped(before)));
    }
    CHECK(worstJump > 0.5f * vox);
}
