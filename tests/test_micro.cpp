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
#include "voxel/fur_cards.hpp"
#include "voxel/micro_clip.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_mesh.hpp"
#include "voxel/micro_svo.hpp"
#include "voxel/surfelize.hpp"
#include "voxel/worldfile.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace vf::voxel;
using namespace glm;

namespace {

// A solid cube occupying cells [0, 2*half] on each axis, on a 5 mm lattice.
// It is anchored at the layer's ORIGIN so the object's first cell lands on
// field index kMicroMarginCells and the object's near surface sits exactly on
// the world offset - which makes every expectation below a plain number.
WorldFileData cubeLayer(int half, float voxel, uint8_t material = 20)
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
                v.materialId = material;
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

TEST_CASE("micro world: the field is CONSERVATIVE outside its grid")
{
    // The regression that made a micro object unselectable in the app.
    //
    // Outside the field's grid there is no field at all, and a trilinear
    // sampler that finds no cell in range returns 0. A sphere tracer that
    // reads 0 steps 0, so the pick march crawled at its minimum step
    // (a quarter of the object's own voxel) and covered ~0.6 m in its whole
    // 512-step budget. The unit test above picks from 0.30 m, so it ARRIVED
    // and passed; the app's camera is 15 m away, so the ray never reached the
    // object and the alpaca could not be selected (measured: 0 of 81 rays
    // hit, while the AABB test passed on every one of them).
    //
    // The shader already handles this (microMapWorld returns the TRUE
    // distance to the world box outside it), so the CPU sampler must return
    // the same thing: a positive, conservative distance.
    const float vox = 0.005f;
    WorldFileData d = cubeLayer(3, vox);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    // Far outside the grid: a positive distance, and it GROWS with the
    // distance from the box (that is what makes it a usable step).
    const float far = 3.0f;
    const float d1 = f.sampleSdf(f.aabbMax() + glm::vec3(far, 0.f, 0.f));
    const float d2 = f.sampleSdf(f.aabbMax() + glm::vec3(2.f * far, 0.f, 0.f));
    CHECK(d1 > 0.0f);
    CHECK(d2 > d1);
    // The distance is at least the distance to the grid (the box contains the
    // object, so the box distance is a lower bound on the object distance and
    // 0.85 * it is a safe sphere-trace step).
    CHECK(d1 >= far - 1e-3f);

    // A pick from a REALISTIC distance - the app's camera is ~15 m from the
    // objects on the shore - must hit. This is the case the old sampler
    // failed: the march never arrived.
    const glm::vec3 offset(10.0f, 2.0f, 10.0f);
    const int objCells = 7;
    const glm::vec3 centre =
        offset + glm::vec3(float(objCells - 1) * 0.5f * vox);
    const glm::vec3 ro = centre + glm::vec3(15.0f, 0.0f, 0.0f);
    const glm::vec3 rd = glm::normalize(centre - ro);
    const MicroPick hit = pickMicroField(f, offset, ro, rd);
    REQUIRE(hit.hit);
    CHECK(std::abs(hit.dist - 15.0f) < 0.05f);
    // And a ray that passes by the object still misses.
    const MicroPick miss = pickMicroField(
        f, offset, ro, glm::normalize(glm::vec3(centre.x - ro.x, 0.5f, 0.4f)));
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

// ---------------------------------------------------------------------------
// The FUR CARDS.
//
// The fur is a translucent layer of hair cards over a micro object, and the
// three things that can go wrong are all silent: a coat that grows on an
// object that has no fur material, cards whose roots float off the surface
// (the coat then hangs in the air), and cards that differ between two builds
// of the same object (the same scene would render differently from one run
// to the next).
// ---------------------------------------------------------------------------

TEST_CASE("fur cards: only a fur material grows a coat")
{
    const float vox = 0.005f;
    FurParams p;
    p.materials = { 21 };  // fleece

    // A plaster cube (material 20) has no fur: the card list is empty and
    // the generation says so rather than failing silently.
    {
        WorldFileData d = cubeLayer(3, vox, 20);
        MicroField f;
        std::string err;
        REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
        FurObject o;
        std::string ferr;
        CHECK_FALSE(buildFurCards(f, p, o, &ferr));
        CHECK(o.cards.empty());
    }
    // The same cube in fleece grows one.
    {
        WorldFileData d = cubeLayer(3, vox, 21);
        MicroField f;
        std::string err;
        REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
        FurObject o;
        std::string ferr;
        REQUIRE(buildFurCards(f, p, o, &ferr));
        CHECK(o.cards.size() > 0);
    }
}

TEST_CASE("fur cards: the roots sit ON the surface, and the cards stand off it")
{
    const float vox = 0.005f;
    FurParams p;
    p.materials = { 21 };
    WorldFileData d = cubeLayer(3, vox, 21);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    FurObject o;
    std::string ferr;
    REQUIRE(buildFurCards(f, p, o, &ferr));
    REQUIRE(o.cards.size() > 0);

    int onSurface = 0;
    for (const FurCard& c : o.cards) {
        const vec3 root = vec3(c.rootLen);
        // The root is within a voxel of the surface: the field's value there
        // is (almost) zero. A root that floated off the surface would leave
        // the coat hanging in the air.
        const float d0 = f.sampleSdf(root + f.origin);
        CHECK(std::abs(d0) < 1.5f * vox);
        if (std::abs(d0) < 1.5f * vox)
            ++onSurface;
        // The strand points OUT of the surface, not along it: a card lying in
        // the tangent plane is invisible from the front - and, worse, a
        // DEGENERATE direction (the bug the spin used to have) points
        // straight out and reads as a spoke. The angle to the normal must
        // stay inside the requested range, which is what pins the bug.
        const float ndl = glm::dot(vec3(c.nrmAlpha), vec3(c.dirWidth));
        const float lo = std::sin(glm::radians(p.tiltMin));
        const float hi = std::sin(glm::radians(p.tiltMax));
        CHECK_MESSAGE(ndl >= lo - 1e-3f, "ndl=", ndl, " below ", lo);
        CHECK_MESSAGE(ndl <= hi + 1e-3f, "ndl=", ndl, " above ", hi);
        CHECK_MESSAGE(ndl > 0.15f, "ndl=", ndl, " len=", c.rootLen.w);
        // The card is a sliver, not a disk.
        CHECK(c.dirWidth.w < c.rootLen.w);
        // The strand is CURLY, not a straight needle: the centre line's
        // amplitude is a real fraction of the strand's length, and it makes
        // at least one cycle. The bounds are the PARAMETER's own times the
        // per-card jitter (0.55..1.45), so they follow a change of the
        // parameter instead of going stale.
        CHECK(c.curl.x > p.curlAmp * 0.45f * c.rootLen.w);
        CHECK(c.curl.x < p.curlAmp * 1.55f * c.rootLen.w);
        CHECK(c.curl.y >= 0.5f);
    }
    CHECK(onSurface == int(o.cards.size()));
}

TEST_CASE("fur cards: the bucket grid covers the CURL")
{
    // The centre line wanders off the straight root-to-tip line, so a card
    // filed in the cells of that line alone would be a card the DDA walks
    // past. The bucket range must cover the curl's amplitude.
    const float vox = 0.005f;
    FurParams p;
    p.materials = { 21 };
    WorldFileData d = cubeLayer(3, vox, 21);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
    FurObject o;
    std::string ferr;
    REQUIRE(buildFurCards(f, p, o, &ferr));
    REQUIRE(!o.cards.empty());

    // Every card appears in at least one cell, and every card's cells are
    // inside the grid.
    const int cells = o.grid.dim.x * o.grid.dim.y * o.grid.dim.z;
    CHECK(cells > 0);
    std::vector<int> seen(o.cards.size(), 0);
    for (uint32_t idx : o.cellItems) {
        REQUIRE(idx < o.cards.size());
        ++seen[idx];
    }
    for (size_t i = 0; i < seen.size(); ++i)
        CHECK(seen[i] >= 1);
    // The grid's extent covers every card's root and its tip.
    for (const FurCard& c : o.cards) {
        const vec3 root = vec3(c.rootLen);
        const vec3 tip = root + vec3(c.dirWidth) * c.rootLen.w;
        for (const vec3& p2 : { root, tip }) {
            const vec3 rel = p2 - o.grid.origin;
            CHECK(rel.x >= -o.grid.cell.x);
            CHECK(rel.y >= -o.grid.cell.y);
            CHECK(rel.z >= -o.grid.cell.z);
            CHECK(rel.x <= float(o.grid.dim.x) * o.grid.cell.x + o.grid.cell.x);
            CHECK(rel.y <= float(o.grid.dim.y) * o.grid.cell.y + o.grid.cell.y);
            CHECK(rel.z <= float(o.grid.dim.z) * o.grid.cell.z + o.grid.cell.z);
        }
    }
}

TEST_CASE("fur cards: the same field yields the same cards twice")
{
    // The determinism the whole renderer relies on: two builds of the same
    // object must produce the same coat, or the same scene renders
    // differently from one run to the next.
    const float vox = 0.005f;
    FurParams p;
    p.materials = { 21 };
    WorldFileData d = cubeLayer(3, vox, 21);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    FurObject a, b;
    std::string ferr;
    REQUIRE(buildFurCards(f, p, a, &ferr));
    REQUIRE(buildFurCards(f, p, b, &ferr));
    REQUIRE(a.cards.size() == b.cards.size());
    for (size_t i = 0; i < a.cards.size(); ++i) {
        CHECK(a.cards[i].rootLen == b.cards[i].rootLen);
        CHECK(a.cards[i].dirWidth == b.cards[i].dirWidth);
        CHECK(a.cards[i].nrmAlpha == b.cards[i].nrmAlpha);
    }
    CHECK(a.cellStart == b.cellStart);
    CHECK(a.cellItems == b.cellItems);
}

TEST_CASE("fur cards: every card is filed in a cell that holds it")
{
    // The bucket grid is the shader's DDA: a ray that clips a card must
    // find it. A card filed in the wrong cell is a card that is never drawn.
    const float vox = 0.005f;
    FurParams p;
    p.materials = { 21 };
    WorldFileData d = cubeLayer(3, vox, 21);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));
    FurObject o;
    std::string ferr;
    REQUIRE(buildFurCards(f, p, o, &ferr));
    REQUIRE(!o.cellItems.empty());

    // Every item index is a valid card, and every card appears at least once.
    std::vector<int> seen(o.cards.size(), 0);
    for (uint32_t idx : o.cellItems) {
        REQUIRE(idx < o.cards.size());
        ++seen[idx];
    }
    for (size_t i = 0; i < seen.size(); ++i)
        CHECK(seen[i] >= 1);
    // The cellStart array is a prefix sum over the cells.
    REQUIRE(o.cellStart.size() == size_t(o.grid.dim.x) * size_t(o.grid.dim.y) *
                                     size_t(o.grid.dim.z) + 1);
    for (size_t i = 1; i < o.cellStart.size(); ++i)
        CHECK(o.cellStart[i] >= o.cellStart[i - 1]);
    CHECK(o.cellStart.back() == o.cellItems.size());
}

// The MESH ACTOR's vertex lane. This is a cross-language contract - the CPU
// writer below and TWO shaders (micro_mesh.vert decodes it, micro_mesh.frag
// reads the material) - so a drift has to be caught here, not by looking at a
// grey alpaca. Byte order: bone+1, material (255 = "look it up"), AO, reserved.
TEST_CASE("micro mesh: the packed vertex lane")
{
    CHECK(meshVertexWord(-1, 255, 255) == 0x00FFFF00u);
    CHECK(meshVertexWord(0, 0, 0) == 0x00000001u);
    CHECK(meshVertexWord(8, 21, 128) == ((128u << 16) | (21u << 8) | 9u));
    // A bone index past 254 must saturate the byte, never wrap into "unskinned"
    // or into the material byte.
    CHECK((meshVertexWord(300, 0, 0) & 0xFFu) == 255u);

    // A mesh whose appearance pass has not run still uploads a VALID record:
    // unskinned and "look it up in the field" - never material 0, which would
    // shade the whole body as rock.
    MicroMesh mesh;
    mesh.rest.push_back(glm::vec3(0.1f, 0.2f, 0.3f));
    const glm::vec4 rec = meshVertexRecord(mesh, 0, glm::vec3(1.f, 2.f, 3.f));
    CHECK(rec.x == doctest::Approx(1.1f));
    CHECK(rec.y == doctest::Approx(2.2f));
    CHECK(rec.z == doctest::Approx(3.3f));
    const uint32_t bits = glm::floatBitsToUint(rec.w);
    CHECK((bits & 0xFFu) == 0u);
    CHECK(((bits >> 8) & 0xFFu) == 255u);
}

// The APPEARANCE pass: a vertex of a mesh sits a little OUTSIDE the field's
// surface (the two are different tessellations of the same animal), so the
// lookup search is two cells wide. Measured on the shipped alpaca: all 54,138
// vertices resolve. Here the triangle stands 2 mm off a material-21 cube's +X
// face - exactly the case the per-pixel lookup used to MISS and render grey.
TEST_CASE("micro mesh: per-vertex appearance resolves against the rest field")
{
    WorldFileData d = cubeLayer(6, 0.005f, 21);
    MicroField f;
    std::string err;
    REQUIRE(buildMicroField(d.voxels, d.meta, f, &err));

    // The solid cube occupies margin .. margin+12 cells; its +X surface is the
    // far corner of the last one. Read it off the field rather than assuming
    // the margin, so a change to kMicroMarginCells cannot silently move the
    // test's surface off the cube.
    const float face = f.origin.x + float(f.solidMax.x + 1) * f.voxel;
    MicroMesh mesh;
    MeshTri t;
    t.v[0] = vec3(face + 0.002f, 0.030f, 0.030f);
    t.v[1] = vec3(face + 0.002f, 0.040f, 0.030f);
    t.v[2] = vec3(face + 0.002f, 0.030f, 0.040f);
    t.mat = 21;
    mesh.tris.push_back(t);
    mesh.rest = { t.v[0], t.v[1], t.v[2] };
    mesh.bone.assign(3, -1);

    MeshAppearanceStats st;
    assignMicroMeshAppearance(f, mesh, &st);
    CHECK(st.vertices == 3);
    CHECK(st.missed == 0);
    CHECK(st.assigned == 3);
    for (size_t i = 0; i < mesh.mat.size(); ++i) {
        CHECK(mesh.mat[i] == 21);
        CHECK(((mesh.word[i] >> 8) & 0xFFu) == 21u);
        CHECK((mesh.word[i] & 0xFFu) == 0u);  // unskinned: no .skin bone
        CHECK(((mesh.word[i] >> 16) & 0xFFu) == mesh.ao[i]);
    }

    // A vertex far outside the surface has NO appearance - and it must say so
    // (byte 1 = 255), because that is what tells the shader to fall back
    // instead of shading material 0.
    MicroMesh far;
    MeshTri ft;
    ft.v[0] = vec3(10.f, 0.03f, 0.03f);
    ft.v[1] = vec3(10.f, 0.04f, 0.03f);
    ft.v[2] = vec3(10.f, 0.03f, 0.04f);
    far.tris.push_back(ft);
    far.rest = { ft.v[0], ft.v[1], ft.v[2] };
    far.bone.assign(3, -1);
    MeshAppearanceStats fst;
    assignMicroMeshAppearance(f, far, &fst);
    CHECK(fst.missed == 3);
    for (size_t i = 0; i < far.word.size(); ++i)
        CHECK(((far.word[i] >> 8) & 0xFFu) == 255u);
}

// ---- winding -----------------------------------------------------------
//
// The mesh raster pass writes its G-buffer from the FRAGMENT stage with
// `imageStore`, so a pixel covered TWICE (the near and the far wall of a closed
// shell) has two racing writers and the frame differs run to run. Culling
// removes the second writer, and culling is only correct if the winding is
// consistent and outward. The shipped alpaca arrived with 26.1 % of its shared
// edges traversed the same way by both neighbours, i.e. a quarter of the body
// faced inward - and the loader did not care, so this is the pin.
namespace {

// A unit cube at the origin, 12 triangles, CCW seen from OUTSIDE.
std::vector<MeshTri> unitCube()
{
    const glm::vec3 p[8] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
        { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 },
    };
    // corner index triples per face, in an order whose cross product is the
    // OUTWARD normal (checked by the assertions below, not by eye).
    const int f[12][3] = {
        { 0, 3, 2 }, { 0, 2, 1 }, // z = 0
        { 4, 5, 6 }, { 4, 6, 7 }, // z = 1
        { 0, 4, 7 }, { 0, 7, 3 }, // x = 0
        { 1, 2, 6 }, { 1, 6, 5 }, // x = 1
        { 0, 1, 5 }, { 0, 5, 4 }, // y = 0
        { 3, 7, 6 }, { 3, 6, 2 }, // y = 1
    };
    std::vector<MeshTri> tris(12);
    for (int t = 0; t < 12; ++t)
        for (int k = 0; k < 3; ++k)
            tris[size_t(t)].v[k] = p[f[t][k]];
    return tris;
}

double signedVolume(const std::vector<MeshTri>& tris)
{
    double v = 0.0;
    for (const MeshTri& t : tris)
        v += double(glm::dot(t.v[0], glm::cross(t.v[1], t.v[2]))) / 6.0;
    return v;
}

// Distance of each triangle's plane from the centre, measured along its own
// normal: positive for every face of an outward-wound convex solid.
float minOutwardDot(const std::vector<MeshTri>& tris)
{
    const glm::vec3 c(0.5f, 0.5f, 0.5f);
    float worst = std::numeric_limits<float>::max();
    for (const MeshTri& t : tris) {
        const glm::vec3 n = glm::cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
        const glm::vec3 mid = (t.v[0] + t.v[1] + t.v[2]) / 3.f;
        worst = std::min(worst, glm::dot(glm::normalize(n), mid - c));
    }
    return worst;
}

} // namespace

TEST_CASE("micro mesh: the winding is made consistent and OUTWARD")
{
    std::vector<MeshTri> tris = unitCube();
    // A clean cube is the control: the pass must not touch it.
    {
        MeshWindingStats s;
        CHECK(orientMeshWinding(tris, &s));
        CHECK(s.sameDirectionEdges == 0);
        CHECK(s.flipped == 0);
        CHECK(s.consistent);
        CHECK(s.outward);
        CHECK(s.closed);
        CHECK(s.components == 1);
        CHECK(signedVolume(tris) == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(minOutwardDot(tris) == doctest::Approx(0.5f).epsilon(1e-3));
    }

    // Now invert a third of the faces, the way the shipped STL is broken.
    std::vector<MeshTri> broken = unitCube();
    for (int t : { 1, 4, 7, 10 })
        std::swap(broken[size_t(t)].v[1], broken[size_t(t)].v[2]);
    CHECK(signedVolume(broken) != doctest::Approx(1.0).epsilon(1e-3));

    MeshWindingStats s;
    REQUIRE(orientMeshWinding(broken, &s));
    CHECK(s.sameDirectionEdges > 0);        // the defect WAS there
    CHECK(s.sameDirectionEdgesAfter == 0);  // and is gone
    CHECK(s.flipped > 0);
    CHECK(s.consistent);
    CHECK(s.closed);
    CHECK(s.nonManifoldEdges == 0);
    CHECK(signedVolume(broken) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(minOutwardDot(broken) == doctest::Approx(0.5f).epsilon(1e-3));

    // Idempotent: a second pass has nothing left to do. A pass that keeps
    // flipping would re-aim every face normal the AO pass samples along.
    MeshWindingStats again;
    CHECK(orientMeshWinding(broken, &again));
    CHECK(again.flipped == 0);
    CHECK(again.sameDirectionEdges == 0);
}

TEST_CASE("micro mesh: an entirely inside-out shell is turned outward")
{
    // Consistency alone does not say which side is OUT: a shell can arrive
    // uniformly inverted, and culling BACK would then delete the outside.
    std::vector<MeshTri> tris = unitCube();
    for (MeshTri& t : tris)
        std::swap(t.v[1], t.v[2]);
    REQUIRE(signedVolume(tris) == doctest::Approx(-1.0).epsilon(1e-4));

    MeshWindingStats s;
    REQUIRE(orientMeshWinding(tris, &s));
    CHECK(s.sameDirectionEdges == 0); // it was already consistent...
    CHECK(!s.outward);                // ...just inside-out
    CHECK(s.flipped == 12);           // every triangle reversed
    CHECK(signedVolume(tris) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(minOutwardDot(tris) == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("micro mesh: a soup too small to be a surface is left alone")
{
    std::vector<MeshTri> one(1);
    one[0].v[0] = { 0, 0, 0 };
    one[0].v[1] = { 1, 0, 0 };
    one[0].v[2] = { 0, 1, 0 };
    MeshWindingStats s;
    CHECK_FALSE(orientMeshWinding(one, &s));
    CHECK(s.triangles == 1);
    CHECK(one[0].v[1] == glm::vec3(1, 0, 0)); // untouched
}

// ---- the skinning weight kernel ----------------------------------------
//
// THE one skinning rule: `poseMicroMesh`, the fur-card build, the deformer's
// dirty set and the shader's `microBoneWeights` all read this kernel, so the
// body and the coat cannot be bound to different bones for the same surface
// point. Three properties have to hold or the animal tears again:
//
//   1. the weights SUM to 1 at every point, so the blend is an average and not
//      a shrink;
//   2. the weight is CONTINUOUS at the band's edge, so no crease forms where a
//      bone enters or leaves the blend;
//   3. past `kMicroWeightReach * radius` a bone contributes NOTHING and the
//      nearest bone carries the point rigidly - which is what keeps a hoof
//      moving 0.0 mm when the spine bends.
TEST_CASE("micro rig: the weight kernel sums to 1, is smooth, and is rigid past its reach")
{
    auto bone = [](glm::vec3 a, glm::vec3 b, float r, BoneRole role) {
        MicroBone B;
        B.a = a;
        B.b = b;
        B.radius = r;
        B.role = role;
        return B;
    };
    auto rig = [&](std::vector<MicroBone> bs) {
        MicroRig R;
        R.bones = std::move(bs);
        return R;
    };

    // A spine at the origin plus one leg hanging off its far end.
    const MicroRig R = rig({
        bone({ -0.30f, 0.f, 0.f }, { 0.30f, 0.f, 0.f }, 0.10f, BoneRole::Spine),
        bone({ 0.25f, -0.02f, 0.f }, { 0.25f, -0.45f, 0.f }, 0.06f, BoneRole::Leg),
    });

    // (1) normalized everywhere, including points no bone reaches.
    for (float x = -0.6f; x <= 0.6001f; x += 0.05f) {
        for (float y = -0.6f; y <= 0.3001f; y += 0.05f) {
            const BoneInfluences inf = R.boneInfluences({ x, y, 0.f });
            REQUIRE(inf.count >= 1);
            REQUIRE(inf.count <= kMicroMaxInfluences);
            float sum = 0.f;
            for (int i = 0; i < inf.count; ++i)
                sum += inf.weight[i];
            CHECK(sum == doctest::Approx(1.0f).epsilon(1e-4));
            // Descending, so bone[0] really is the dominant bone.
            for (int i = 1; i < inf.count; ++i)
                CHECK(inf.weight[i - 1] >= inf.weight[i]);
            for (int i = 0; i < inf.count; ++i) {
                CHECK(inf.bone[i] >= 0);
                CHECK(inf.bone[i] < int(R.bones.size()));
                CHECK(inf.weight[i] >= 0.f);
            }
            // `boneAt` is defined as the head of that list, never a re-derivation.
            CHECK(R.boneAt({ x, y, 0.f }) == inf.bone[0]);
        }
    }

    // (2) rigid on one bone where nothing else reaches.
    {
        // The hoof: on the leg's axis, 0.45 m below the spine, outside its band.
        const BoneInfluences hoof = R.boneInfluences({ 0.25f, -0.45f, 0.f });
        CHECK(hoof.count == 1);
        CHECK(hoof.bone[0] == 1);
        CHECK(hoof.weight[0] == doctest::Approx(1.0f));
        // Mid-spine: on the spine's axis, outside the leg's band.
        const BoneInfluences back = R.boneInfluences({ 0.f, 0.f, 0.f });
        CHECK(back.count == 1);
        CHECK(back.bone[0] == 0);
    }

    // (3) THE TEAR FIX: at the shoulder both bones must contribute. A single
    // dominant bone there is exactly the hard crease the kernel replaced.
    {
        const BoneInfluences sh = R.boneInfluences({ 0.25f, 0.10f, 0.f });
        CHECK(sh.count >= 2);
        CHECK(sh.weight[0] > 0.05f);
        CHECK(sh.weight[1] > 0.05f);
        CHECK(sh.weight[0] < 0.95f); // it is a BLEND, not a rigid attachment
    }

    // (4) the band edge is smooth. This has to be measured with TWO bones: the
    // weights are NORMALIZED, so a lone contributing bone always comes back
    // with weight 1 however small its raw kernel value is - which is exactly
    // what makes the weights a partition of unity and what an earlier version
    // of this check got wrong. The real property is that a bone's normalized
    // share goes to zero CONTINUOUSLY as it leaves its band, with the other
    // bone taking over.
    {
        const MicroRig two = rig({
            bone({ 0.f, 0.f, 0.f }, { 0.4f, 0.f, 0.f }, 0.05f, BoneRole::Spine),
            bone({ 0.f, 0.30f, 0.f }, { 0.4f, 0.30f, 0.f }, 0.05f, BoneRole::Neck),
        });
        const float reach = kMicroWeightReach * 0.05f;
        // On the spine's axis the neck is outside its band: A owns the point.
        const BoneInfluences onAxis = two.boneInfluences({ 0.2f, 0.f, 0.f });
        REQUIRE(onAxis.count == 1);
        CHECK(onAxis.bone[0] == 0);
        CHECK(onAxis.weight[0] == doctest::Approx(1.0f));

        // Walk out of the spine's band. ITS share must fall monotonically to
        // (near) nothing, never step. The list is sorted by weight, so once the
        // neck overtakes it the spine is no longer slot 0 - follow the bone id,
        // not the index. The range starts where the neck's band begins reaching,
        // so both contribute throughout and the rigid fallback cannot mask the
        // shape of the decay.
        float prev = 2.f;
        for (float y = 0.13f; y <= 0.174f; y += 0.004f) {
            const BoneInfluences inf = two.boneInfluences({ 0.2f, y, 0.f });
            REQUIRE(inf.count == 2); // both reach here, so no fallback can mask it
            int si = -1;
            for (int i = 0; i < inf.count; ++i)
                if (inf.bone[i] == 0)
                    si = i;
            REQUIRE(si >= 0);
            CHECK(inf.weight[si] <= prev);
            CHECK(inf.weight[0] + inf.weight[1]
                  == doctest::Approx(1.0f).epsilon(1e-4));
            prev = inf.weight[si];
        }
        const BoneInfluences atEdge =
            two.boneInfluences({ 0.2f, 0.999f * reach, 0.f });
        // The neck has taken over and the spine's share is smooth at the edge.
        CHECK(atEdge.bone[0] == 1);
        CHECK(atEdge.weight[0] > 0.99f);
        CHECK(atEdge.weight[1] < 1e-4f);
    }

    // (5) past every band the nearest bone carries the point RIGIDLY, which is
    // what keeps a hoof at 0.0 mm while the spine bends.
    {
        const MicroRig one = rig({ bone({ 0.f, 0.f, 0.f }, { 0.4f, 0.f, 0.f },
                                        0.05f, BoneRole::Spine) });
        const float reach = kMicroWeightReach * 0.05f;
        const BoneInfluences outside = one.boneInfluences({ 0.2f, 1.001f * reach, 0.f });
        CHECK(outside.count == 1);
        CHECK(outside.weight[0] == doctest::Approx(1.0f));
    }

    // (6) a rig with no bones is inert, not a crash.
    {
        const MicroRig empty;
        const BoneInfluences inf = empty.boneInfluences({ 0.f, 0.f, 0.f });
        CHECK(inf.count == 0);
        CHECK(empty.boneAt({ 0.f, 0.f, 0.f }) == -1);
    }
}

// ---- studio authoring: a bone key IS the pose ---------------------------
//
// The studio's bone gizmo writes `BoneKey { t, rotDeg }` at the playhead and
// nothing else. That is only an honest editor if the value written is the
// value the rig samples, so this pins the two rules the gizmo relies on:
//
//   1. at a key's own time the sampled rotation IS that key's rotDeg;
//   2. a track with ONE key holds it for the whole clip, which is what makes
//      "pose it once and the clip is posed" true (clamp at both ends).
//
// The rotation is composed as Rx * Ry * Rz (micro_clip.cpp), so the expected
// basis vectors below are stated as the geometric consequence, not as a copy
// of that expression: +90 about Y must send the bone's +X to -Z, and so on.
TEST_CASE("micro clip: a bone key at the playhead is the pose the rig samples")
{
    MicroRig rig;
    MicroBone bone;
    bone.id = 1;
    bone.name = "spine";
    bone.a = glm::vec3(0.f);
    bone.b = glm::vec3(1.f, 0.f, 0.f);
    bone.radius = 0.10f;
    bone.role = BoneRole::Spine;
    rig.bones.push_back(bone);

    auto sampleAt = [&](const MicroClip& clip, float t) {
        std::vector<glm::mat3> rot;
        glm::vec3 bodyOff;
        float bodyHdg = 0.f;
        clip.sample(t, rig, rot, bodyOff, bodyHdg);
        REQUIRE(rot.size() == 1);
        return rot[0] * glm::vec3(1.f, 0.f, 0.f);
    };

    // ---- (1) each axis, at the key's own time --------------------------
    // Absolute tolerance, not doctest::Approx's relative epsilon: several of
    // these expected values are 0, where a relative compare proves nothing.
    auto near = [](float a, float b, float eps = 1e-5f) {
        return std::abs(a - b) < eps;
    };
    {
        MicroClip clip;
        BoneTrack tr;
        tr.boneId = bone.id;
        tr.keys.push_back({ 0.f, glm::vec3(0.f, 90.f, 0.f), ClipEase::Smooth });
        clip.tracks.push_back(tr);
        const glm::vec3 x = sampleAt(clip, 0.f);   // bone axis after +90 yaw
        CHECK(near(x.x, 0.f));
        CHECK(near(x.z, -1.f));
        // ---- (2) and it holds everywhere else in the clip --------------
        CHECK(near(sampleAt(clip, 0.37f).z, -1.f));
        CHECK(near(sampleAt(clip, 5.f).z, -1.f));
    }
    {
        MicroClip clip;
        BoneTrack tr;
        tr.boneId = bone.id;
        tr.keys.push_back({ 0.f, glm::vec3(90.f, 0.f, 0.f), ClipEase::Smooth });
        clip.tracks.push_back(tr);
        // +90 about X sends the bone's +X nowhere (it is the axis) - use a
        // direction that MUST move: the bone's own +Y.
        std::vector<glm::mat3> rot;
        glm::vec3 bo;
        float bh = 0.f;
        clip.sample(0.f, rig, rot, bo, bh);
        const glm::vec3 y = rot[0] * glm::vec3(0.f, 1.f, 0.f);
        CHECK(near(y.y, 0.f));
        CHECK(near(y.z, 1.f));
    }

    // ---- (2b) TWO keys: the midpoint is the interpolated pose ---------
    // Smooth easing at u = 0.5 is exactly 0.5, so the midpoint of 0 -> 90
    // degrees of yaw is 45: the editor's "add a key, move the playhead, add
    // another" flow produces an even blend, not a step.
    {
        MicroClip clip;
        BoneTrack tr;
        tr.boneId = bone.id;
        tr.keys.push_back({ 0.f, glm::vec3(0.f), ClipEase::Smooth });
        tr.keys.push_back({ 1.f, glm::vec3(0.f, 90.f, 0.f), ClipEase::Smooth });
        clip.tracks.push_back(tr);
        const glm::vec3 x = sampleAt(clip, 0.5f);
        CHECK(near(x.x, std::cos(glm::radians(45.f))));
        CHECK(near(x.z, -std::sin(glm::radians(45.f)), 1e-6f));
        // ... and the ends are the keys themselves.
        CHECK(near(sampleAt(clip, 0.f).z, 0.f));
        CHECK(near(sampleAt(clip, 1.f).z, -1.f));
    }
}

TEST_CASE("micro clip: sampleTrackEuler is the ONE blend rule")
{
    // The studio's pose readout and `MicroClip::sample` must never be two
    // opinions about the curve: the rig is posed from this function, and the
    // panel displays this function. Pin it directly - clamp at both ends
    // (never extrapolate), and each ease mode.
    CHECK(sampleTrackEuler({}, 0.5f) == glm::vec3(0.f));

    const BoneKey a { 0.f, glm::vec3(0.f), ClipEase::Smooth };
    const BoneKey b { 1.f, glm::vec3(0.f, 90.f, 0.f), ClipEase::Smooth };
    CHECK(sampleTrackEuler({ a }, 0.7f) == glm::vec3(0.f)); // lone key holds
    CHECK(sampleTrackEuler({ a, b }, -1.f) == a.rotDeg);    // clamp low
    CHECK(sampleTrackEuler({ a, b }, 4.f) == b.rotDeg);     // clamp high

    // Smooth: u = 0.5 -> 0.5, so the midpoint is exactly half the travel.
    CHECK(sampleTrackEuler({ a, b }, 0.5f).y == doctest::Approx(45.f));
    // Linear: u = t.
    BoneKey lin0 = a; lin0.ease = ClipEase::Linear;
    CHECK(sampleTrackEuler({ lin0, b }, 0.25f).y == doctest::Approx(22.5f));
    // Step: the FIRST key's value holds for the whole span.
    BoneKey step0 = a; step0.ease = ClipEase::Step;
    CHECK(sampleTrackEuler({ step0, b }, 0.999f).y == doctest::Approx(0.f));
    CHECK(sampleTrackEuler({ step0, b }, 1.f).y == doctest::Approx(90.f));

    // Three keys: the bracketing pair decides, not the first two.
    const BoneKey c { 2.f, glm::vec3(0.f), ClipEase::Smooth };
    CHECK(sampleTrackEuler({ a, b, c }, 1.5f).y == doctest::Approx(45.f));
    CHECK(sampleTrackEuler({ a, b, c }, 0.5f).y == doctest::Approx(45.f));
    // ... and the ease belongs to the key the span STARTS from.
    BoneKey stepMid = b; stepMid.ease = ClipEase::Step;
    CHECK(sampleTrackEuler({ a, stepMid, c }, 1.99f).y == doctest::Approx(90.f));
}
