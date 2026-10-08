#include "surfelize.hpp"
#include "voxel/chunk_index.hpp"
#include <numeric>
#include <core/log.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vf::voxel {

namespace {

constexpr int kSurfGridN = 16;
constexpr int kChunkSize = 64;

// NaN-proof normalize: glm::normalize of a zero vector yields NaN (0/0),
// which then poisons == comparisons, sorting keys derived from it, and the
// GPU. Any degenerate input falls back to +Y.
inline glm::vec3 safeNormalize(glm::vec3 v) {
    const float l2 = glm::dot(v, v);
    if (!(l2 > 1e-12f) || !std::isfinite(l2))
        return glm::vec3(0.0f, 1.0f, 0.0f);
    return v * (1.0f / std::sqrt(l2));
}

// A hard edge is the intersection of two exposed, non-opposite lattice faces.
// Keeping the face pair (rather than a normal-disagreement score) prevents
// smooth voxel curvature and thin stems from being tightened accidentally.
struct EdgeInfo {
    int pairCount = 0;
    glm::vec3 faces[3] {};
    glm::vec3 axes[3] {};
    int cornerCount = 0;
    glm::vec3 cornerDirs[8] {}; // f0+f1+f2 per corner (unnormalized)
};

EdgeInfo edgeInfoFromMask(unsigned exposedFaces)
{
    constexpr int dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                 { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    EdgeInfo out;
    glm::vec3 face[6] {};
    int faces = 0;
    for (int d = 0; d < 6; ++d) {
        if (!(exposedFaces & (1u << d)))
            continue;
        face[faces++] = glm::vec3(float(dirs[d][0]), float(dirs[d][1]),
                                  float(dirs[d][2]));
    }
    for (int i = 0; i < faces; ++i) {
        for (int j = i + 1; j < faces; ++j) {
            // Opposite faces of a thin plate share one axis and do not form
            // an edge. Distinct exposed axes are orthogonal and always make a
            // genuine voxel crease.
            if (glm::dot(face[i], face[j]) < -0.5f)
                continue;
            if (out.pairCount >= 3)
                break;
            glm::vec3 axis = safeNormalize(glm::cross(face[i], face[j]));
            // Tangent sign has no visual meaning; canonicalize its first
            // significant component so full/live bakes stay byte-identical.
            const float ax = std::fabs(axis.x);
            const float ay = std::fabs(axis.y);
            const float az = std::fabs(axis.z);
            const bool negative = ax > ay && ax > az ? axis.x < 0.0f
                              : ay > az ? axis.y < 0.0f : axis.z < 0.0f;
            if (negative)
                axis = -axis;
            out.faces[out.pairCount * 2] = face[i];
            out.faces[out.pairCount * 2 + 1] = face[j];
            out.axes[out.pairCount++] = axis;
        }
    }
    // Detect corners: 3 mutually orthogonal exposed faces meeting at a vertex.
    // A corner at sign (sx, sy, sz) is exposed when the 3 faces meeting
    // there are all air. The corner direction is the diagonal (sx, sy, sz).
    // Up to 8 corners (a lone voxel exposes all 8 vertices).
    for (int cx = 0; cx < 2; ++cx) {
        for (int cy = 0; cy < 2; ++cy) {
            for (int cz = 0; cz < 2; ++cz) {
                const int fx = cx ? 0 : 1;
                const int fy = cy ? 2 : 3;
                const int fz = cz ? 4 : 5;
                if ((exposedFaces & (1u << fx)) &&
                    (exposedFaces & (1u << fy)) &&
                    (exposedFaces & (1u << fz))) {
                    out.cornerDirs[out.cornerCount++] = glm::vec3(
                        cx ? 1.0f : -1.0f,
                        cy ? 1.0f : -1.0f,
                        cz ? 1.0f : -1.0f);
                }
            }
        }
    }
    return out;
}

// Emit a small, tangent-aligned coverage chain directly on the true face-plane
// intersection. It inherits all shading/ownership/material data from the
// parent, so edge quality costs geometry only: no extra shadow or AO march.
void appendEdgeBridges(glm::vec3 cellCentre, const Surfel& parent,
                        const EdgeInfo& edge, float baseRadius,
                        float bridgeSize,
                        std::vector<Surfel>& out)
{
    if (edge.pairCount <= 0)
        return;
    for (int p = 0; p < edge.pairCount; ++p) {
        const glm::vec3 f0 = edge.faces[p * 2];
        const glm::vec3 f1 = edge.faces[p * 2 + 1];
        const glm::vec3 creaseN = safeNormalize(f0 + f1);
        const glm::vec3 pos = cellCentre + 0.5f * VOXEL * (f0 + f1);
        float rV = std::min(0.40f * baseRadius,
                            std::max(0.25f * VOXEL,
                                     0.65f * parent.normal_rV.w));
        float rU = std::max(0.60f * VOXEL, rV);
        rV *= bridgeSize;
        rU *= bridgeSize;
        // Re-floor after scaling (size < 1 must not breach the pinhole floor).
        rV = std::max(0.25f * VOXEL, rV);
        rU = std::max(0.60f * VOXEL, rU);
        Surfel bridge = parent;
        bridge.pos_rU = glm::vec4(pos, rU);
        bridge.normal_rV = glm::vec4(creaseN, rV);
        bridge.bent_sh = glm::vec4(
            safeNormalize(glm::vec3(parent.bent_sh) + creaseN), parent.bent_sh.w);
        bridge.tan_aspect = glm::vec4(edge.axes[p], parent.tan_aspect.w);
        out.push_back(bridge);
    }
}

// Emit a small isotropic cap directly on a tri-face corner vertex. Like the
// edge bridges, it inherits all shading/ownership from the parent so corner
// coverage costs geometry only.
void appendCornerCaps(glm::vec3 cellCentre, const Surfel& parent,
                       const EdgeInfo& edge, float baseRadius,
                       float bridgeSize,
                       std::vector<Surfel>& out)
{
    if (edge.cornerCount <= 0)
        return;
    // Coverage guard. The parent is a flat Gaussian disk centred at
    // pc = centre + n*0.5*VOXEL with in-plane radius parentR. If a corner
    // vertex projects inside that footprint, the parent already covers it and
    // a cap would only add a spurious (often darker) speck — its diagonal
    // normal has worse ndl than the wall it lands on. Fire a cap only where
    // the parent genuinely does not reach: thin-shell / narrow disks and
    // concave corners. A normal 0.14 m parent covers the 0.037–0.13 m corner
    // offset in every direction, so convex/multi-exposed cells emit nothing.
    const glm::vec3 pn = safeNormalize(glm::vec3(parent.normal_rV));
    const glm::vec3 pc = cellCentre + pn * (0.5f * VOXEL);
    const float parentR = std::max(parent.pos_rU.w, parent.normal_rV.w);
    for (int c = 0; c < edge.cornerCount; ++c) {
        const glm::vec3 dir = edge.cornerDirs[c];
        const glm::vec3 n = safeNormalize(dir);
        const glm::vec3 cv = cellCentre + 0.5f * VOXEL * dir;
        const glm::vec3 delta = cv - pc;
        const glm::vec3 inPlane = delta - pn * glm::dot(delta, pn);
        if (glm::dot(inPlane, inPlane) < parentR * parentR)
            continue; // parent disk already reaches this vertex
        const glm::vec3 pos = cv;
        float rV = std::min(0.30f * baseRadius,
                            std::max(0.20f * VOXEL,
                                     0.50f * parent.normal_rV.w));
        float rU = std::max(0.50f * VOXEL, rV);
        rV *= bridgeSize;
        rU *= bridgeSize;
        // Re-floor after scaling so a size < 1 cannot push under the pinhole
        // threshold (grid corner is at 0.707*VOXEL from the centres).
        rV = std::max(0.20f * VOXEL, rV);
        rU = std::max(0.50f * VOXEL, rU);
        Surfel cap = parent;
        cap.pos_rU = glm::vec4(pos, rU);
        cap.normal_rV = glm::vec4(n, rV);
        cap.bent_sh = glm::vec4(
            safeNormalize(glm::vec3(parent.bent_sh) + n), parent.bent_sh.w);
        cap.tan_aspect = glm::vec4(0.0f, 0.0f, 0.0f, parent.tan_aspect.w);
        out.push_back(cap);
    }
}

inline uint64_t packKey(int x, int y, int z) {
    return (uint64_t(std::uint32_t(x)) << 20) | (uint32_t(y) << 10) | uint32_t(z);
}

// Deterministic 0..1 hash from lattice coords + slot (no RNG state, so two
// builds are bit-identical). Wraps sin-hash like the shader hashN.
inline float microHash(int x, int y, int z, int slot)
{
    float h = sinf(float(x) * 12.9898f + float(y) * 78.233f +
                   float(z) * 37.719f + float(slot) * 11.13f) * 43758.55f;
    return h - floorf(h);
}

inline void unpackKey(uint64_t k, int& x, int& y, int& z) {
    x = int((k >> 20) & 0x3FFu);
    y = int((k >> 10) & 0x3FFu);
    z = int(k & 0x3FFu);
}

inline int chunkIndex(int x, int y, int z) {
    return chunkIndexOf(x / kChunkSize, y / kChunkSize, z / kChunkSize);
}

bool terrainSurface(const VoxelField& field, int x, int y, int z) {
    const int latN = field.latN();
    if (y < 0 || y >= latN || x < 0 || x >= latN || z < 0 || z >= latN) return false;
    const int16_t colTop = field.colTops()[size_t(z) * latN + size_t(x)];
    if (colTop < 0 || y > colTop) return false;
    if (y == colTop) return true;
    if (x > 0) {
        int16_t n = field.colTops()[size_t(z) * latN + size_t(x - 1)];
        if (n < 0 || n < y) return true;
    }
    if (x < latN - 1) {
        int16_t n = field.colTops()[size_t(z) * latN + size_t(x + 1)];
        if (n < 0 || n < y) return true;
    }
    if (z > 0) {
        int16_t n = field.colTops()[size_t(z - 1) * latN + size_t(x)];
        if (n < 0 || n < y) return true;
    }
    if (z < latN - 1) {
        int16_t n = field.colTops()[size_t(z + 1) * latN + size_t(x)];
        if (n < 0 || n < y) return true;
    }
    return false;
}

float heightAt(const VoxelField& field, float wx, float wz) {
    const auto& ht = field.heightTexture();
    const int sz = field.latN();
    if (sz <= 0 || ht.size() < size_t(sz) * size_t(sz)) return -1e9f;
    const glm::vec2 tc = glm::clamp(glm::vec2(wx, wz) / 102.4f + 0.5f,
                                        glm::vec2(0.0f), glm::vec2(1.0f))
                          * glm::vec2(sz - 1);
    const glm::ivec2 i0(glm::floor(tc)), i1(glm::min(i0 + 1, sz - 1));
    const glm::vec2 f = tc - glm::vec2(i0);
    const float h00 = ht[size_t(i0.y) * sz + size_t(i0.x)].r;
    const float h10 = ht[size_t(i0.y) * sz + size_t(i1.x)].r;
    const float h01 = ht[size_t(i1.y) * sz + size_t(i0.x)].r;
    const float h11 = ht[size_t(i1.y) * sz + size_t(i1.x)].r;
    return glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.y), f.y);
}

glm::vec3 heightfieldNormal(const VoxelField& field, glm::vec3 p) {
    const float e = 0.35f, e2 = 0.10f;
    const glm::vec3 nWide = safeNormalize(glm::vec3(
        heightAt(field, p.x - e, p.z) - heightAt(field, p.x + e, p.z),
        2.0f * e,
        heightAt(field, p.x, p.z - e) - heightAt(field, p.x, p.z + e)));
    const glm::vec3 nFine = safeNormalize(glm::vec3(
        heightAt(field, p.x - e2, p.z) - heightAt(field, p.x + e2, p.z),
        2.0f * e2,
        heightAt(field, p.x, p.z - e2) - heightAt(field, p.x, p.z + e2)));
    return safeNormalize(glm::mix(nFine, nWide, 0.55f));
}

// Mean outward direction over the 6 face neighbours that are air (field
// sample d > 0). Returns a ZERO vector when no neighbour is air: the SDF
// marks enclosed air (building interiors, hollow roof/wall shells, sealed
// cavities) as solid, so such a cell is buried and invisible from outside.
// Callers must drop those cells: a fallback normal would (a) tilt the
// smoothed normals of the real surface cells around them and (b) add dark
// interior-fill disks that bleed through the depth-resolve band at grazing
// angles (the "holes" on walls and stepped roofs).
template <typename FieldT>
glm::vec3 meanNormal(const FieldT& field, int x, int y, int z) {
    const int latN = field.latN();
    glm::vec3 n(0.0f);
    const int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (auto &d : dirs) {
        const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < 0 || nx >= latN || nz < 0 || nz >= latN || ny < 0 || ny >= latN) {
            n += glm::vec3(float(d[0]), float(d[1]), float(d[2]));
            continue;
        }
        if (field.sample(nx, ny, nz).d > 0.0f)
            n += glm::vec3(float(d[0]), float(d[1]), float(d[2]));
    }
    const float len = glm::length(n);
    return len > 1e-6f ? (n / len) : glm::vec3(0.0f);
}

// Anisotropy from the local normal field: the surface bends toward the
// neighbours whose normals differ, so the disk stretches along the crease —
// perpendicular to the accumulated bend. `bendSum` is the sum of
// (neighbour normal - cell normal) over existing face neighbours; a flat
// neighbourhood sums to ~zero and stays isotropic. `aspect` scales radiusU
// (along `tangent`), radiusV (across) keeps the base radius.
inline void anisotropyFromBend(glm::vec3 bendSum, glm::vec3 n, float& aspect,
                               glm::vec3& tangent)
{
    const glm::vec3 g = bendSum - n * glm::dot(bendSum, n);
    const float gl = glm::length(g);
    aspect = 1.0f;
    tangent = glm::vec3(0.0f);
    if (gl < 1e-3f)
        return;
    tangent = safeNormalize(glm::cross(n, g / gl));
    aspect = glm::clamp(1.0f + 0.6f * glm::smoothstep(0.2f, 1.2f, gl), 1.0f, 1.6f);
}

// ---- thin-structure footprints --------------------------------------------
// A structure that is LONG along one lattice axis and only a few cells THICK
// across it does not read correctly as a round disk: the disk is far wider
// than the structure, so it smears a one-cell column into a blob. Such cells
// get an anisotropic footprint instead - narrowed to the structure's own width
// and stretched along its long axis - and cells whose surface normal is
// parallel to that axis (the caps) get a small round footprint of the
// structure's cross-section. Everything else keeps the round disk.
//
// The rule is purely geometric: material, colour and object identity play no
// part. The same code serves the bake and the runtime store path, so live
// edits reproduce baked geometry exactly.

// Footprint constants, in cells / baseR units. The across radius is half a
// cell because the structure is one cell thick, so the disk is exactly as wide
// as the voxel column it stands for; the along radius is a small multiple of a
// cell so stacked disks stay a continuous strip without spilling past the ends.
//
// SEALING: disk centres sit on a VOXEL grid, so the least-covered point of a
// face is a grid corner at VOXEL/sqrt(2) ~= 0.707 cells from the four nearest
// centres. A single-cell column has only one disk per row, so 0.55 cells is
// enough (and keeps grass blades / reed stems narrow); but a face TWO or more
// cells wide has a grid of disks whose corners must be covered too. Below the
// 0.707 floor those pinholes line up into continuous background slots between
// the disk columns - every vertical post 2..3 cells wide rendered as hollow
// stripes (invisible on horizontal rails, which sit on terrain and fail
// kThinMaxCross, so they keep the round disk).
constexpr float kThinAcrossCells = 0.55f;      // one-cell-thick structures
constexpr float kThinAcrossSeal = 0.75f;       // faces 2+ cells wide (>1/sqrt2)
constexpr float kThinTallLo = 1.6f;
constexpr float kThinTallHi = 2.4f;
constexpr int kThinProbeCells = 8;   // how far each axis is probed
constexpr int kThinMaxCross = 3;     // "a few cells" across (the cross-section)
constexpr int kThinMinRun = 4;       // "long" (cells along the axis, both ways)

// Per-cell variation of the along radius, so a run of cells does not produce a
// lattice of identical ellipses. Deterministic (survives rebuilds).
inline float thinAspect(int x, int y, int z, float lo, float hi)
{
    return lo + (hi - lo) * microHash(x, y, z, 31);
}

// How far the solid continues from the cell centre along `dir` (cells).
template <typename SolidAt>
inline int solidRunAlong(SolidAt solidAt, glm::vec3 cellCentre, glm::vec3 dir,
                         int maxCells)
{
    int n = 0;
    for (int i = 1; i <= maxCells; ++i) {
        if (!solidAt(cellCentre + dir * (VOXEL * float(i))))
            break;
        ++n;
    }
    return n;
}

// The footprint decision for one surface cell.
struct ThinFootprint {
    bool thin = false;     // true => use along/across below
    glm::vec3 axis { 0.0f }; // in-plane long axis; zero => cap (round)
    float along = 0.0f;    // radius along the long axis
    float across = 0.0f;   // radius across it
};

// Shared by the bake and the store path. `solidAt(p)` answers whether the
// world point p is inside solid geometry; `x,y,z` are the cell's lattice
// coordinates (used only for the deterministic per-cell variation).
// The thin test, shared by the footprint bake and the face-expansion pass:
// LONG (>= kThinMinRun cells along some lattice axis, both ways) and only a
// few cells across on the two others. `run` reports the six probes and `best`
// the long lattice axis.
const glm::vec3 kThinAxes[6] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                                 { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };

template <typename SolidAt>
bool thinRunsAt(SolidAt solidAt, glm::vec3 cellCentre, int run[6], int& best)
{
    for (int a = 0; a < 6; ++a)
        run[a] = solidRunAlong(solidAt, cellCentre, kThinAxes[a], kThinProbeCells);

    // The long axis is the lattice axis with the largest total run.
    int bestSum = 0;
    best = 0;
    for (int a = 0; a < 3; ++a) {
        const int sum = run[2 * a] + run[2 * a + 1];
        if (sum > bestSum) {
            bestSum = sum;
            best = a;
        }
    }
    if (bestSum < kThinMinRun)
        return false; // not long: a small blob, keep the round disk

    // THIN means the CROSS-SECTION is only a few cells: both axes
    // perpendicular to the long axis must be short. Measuring the solid depth
    // along the surface normal instead looks equivalent but is not - on the
    // CAP of a long column the inward direction runs the length of the column,
    // so the cap was classified as thick and kept the oversized round disk
    // (the "plate hovering above the stem" artifact).
    for (int a = 0; a < 3; ++a) {
        if (a == best)
            continue;
        if (run[2 * a] + run[2 * a + 1] > kThinMaxCross)
            return false; // thick across: keep the round disk
    }
    return true;
}

template <typename SolidAt>
bool thinCellAt(SolidAt solidAt, glm::vec3 cellCentre)
{
    int run[6];
    int best = 0;
    return thinRunsAt(solidAt, cellCentre, run, best);
}

template <typename SolidAt>
ThinFootprint thinFootprintAt(SolidAt solidAt, glm::vec3 cellCentre, glm::vec3 n,
                              int x, int y, int z)
{
    ThinFootprint fp;
    int run[6];
    int best = 0;
    if (!thinRunsAt(solidAt, cellCentre, run, best))
        return fp;
    fp.thin = true;
    // The across radius must cover the face. A disk alone cannot: centres sit
    // VOXEL apart, so between them the least-covered point is VOXEL/sqrt(2)
    // from the nearest centres and a 0.55-cell radius leaves a pinhole - on a
    // face of two or more cells those pinholes line up into continuous
    // background slots (a 2-cell post rendered as hollow stripes). Where the
    // structure extends sideways, widen the disk past that floor; a genuinely
    // one-cell stem (grass blade, reed) keeps the narrow radius and stays thin.
    //
    // "Extends sideways" is probed over the whole 3x3 neighbourhood in the
    // plane PERPENDICULAR to the long axis, not along the two lattice axes:
    // on a diagonal surface (a thin cylinder wall) both axis probes can miss
    // the neighbour, which left round posts and masts slotted.
    int acrossNb = 0;
    {
        const int au = (best + 1) % 3, av = (best + 2) % 3;
        for (int du = -1; du <= 1 && !acrossNb; ++du)
            for (int dv = -1; dv <= 1; ++dv) {
                if (!du && !dv)
                    continue;
                glm::vec3 q = cellCentre;
                q[au] += VOXEL * float(du);
                q[av] += VOXEL * float(dv);
                if (solidAt(q)) {
                    acrossNb = 1;
                    break;
                }
            }
    }
    fp.across = (acrossNb ? kThinAcrossSeal : kThinAcrossCells) * VOXEL;

    // Project the long axis into the disk plane. When it is (nearly) the
    // surface normal the cell is a cap: its footprint is the structure's own
    // cross-section, round and small - this is the disk that used to hover
    // over one-cell columns as an oversized plate.
    const glm::vec3 axis(kThinAxes[2 * best]);
    glm::vec3 t = axis - n * glm::dot(axis, n);
    const float l = glm::length(t);
    if (l <= 0.25f) {
        fp.along = fp.across;
        return fp;
    }
    fp.axis = t / l;

    // Clamp the along radius to the structure's own remaining length so the
    // footprint never reaches past its ends (a splat sticking out into air
    // reads as a detached blob); the floor keeps neighbouring cells' disks
    // overlapping, so the run stays continuous. The floor must also be at
    // least the across radius: the footprint is defined so `along` is the long
    // axis, and the unit test (and the ellipse invariant) require rU >= rV.
    const int shortRun = std::min(run[2 * best], run[2 * best + 1]);
    const float reach = (float(shortRun) + 0.5f) * VOXEL;
    const float want =
        1.4f * VOXEL * thinAspect(x, y, z, kThinTallLo, kThinTallHi);
    fp.along = std::max(fp.across, std::min(want, reach));
    return fp;
}

// Binary sun-occlusion march over field.sample: exact Amanatides DDA over
// lattice cells for the first 3 m (visits every cell like the SVO
// reference, so thin eaves/logs are never tunnelled), then sphere-tracing
// with tight clamps to 60 m. 0 = occluded, 1 = clear. Templated on the query
// so it serves both the base VoxelField and the runtime ChunkStore.
template <typename FieldT>
float shadowMarch(const FieldT& f, glm::vec3 ro, glm::vec3 rd)
{
    // Phase 1: cell-exact DDA to 3 m. sampleWorld floors to the cell, so
    // testing every visited cell matches the voxel truth (SVO parity).
    {
        glm::vec3 cell = glm::floor((ro + 51.2f) / 0.1f);
        const glm::vec3 stp = glm::vec3(rd.x >= 0.0f ? 1.0f : -1.0f,
                                        rd.y >= 0.0f ? 1.0f : -1.0f,
                                        rd.z >= 0.0f ? 1.0f : -1.0f);
        const glm::vec3 rdi = glm::vec3(1.0f) / rd;
        glm::vec3 tMax = ((cell + (stp * 0.5f + 0.5f)) * 0.1f - 51.2f - ro) * rdi;
        const glm::vec3 tDelta = glm::abs(rdi) * 0.1f;
        float t = 0.05f;
        for (int i = 0; i < 64; ++i) {
            const glm::vec3 sp = ro + rd * t;
            if (f.sampleWorld(sp).d < -0.02f)
                return 0.0f;
            // advance to next cell boundary (NaN-safe: rdi inf -> huge tMax)
            float tn = tMax.x;
            int ax = 0;
            if (tMax.y < tn) { tn = tMax.y; ax = 1; }
            if (tMax.z < tn) { tn = tMax.z; ax = 2; }
            if (!(tn > t))
                tn = t + 0.05f;
            t = tn + 1e-4f;
            if (ax == 0) { cell.x += stp.x; tMax.x += tDelta.x; }
            else if (ax == 1) { cell.y += stp.y; tMax.y += tDelta.y; }
            else { cell.z += stp.z; tMax.z += tDelta.z; }
            if (t > 3.0f)
                break;
        }
    }
    // Phase 2: sphere-tracing to 60 m for hills/treelines.
    {
        float t = 3.0f;
        for (int i = 0; i < 80; ++i) {
            const glm::vec3 sp = ro + rd * t;
            const float d = f.sampleWorld(sp).d;
            if (d < -0.02f)
                return 0.0f;
            t += glm::clamp(std::fabs(d) * 0.7f, 0.04f, 1.0f);
            if (t > 60.0f)
                break;
        }
    }
    return 1.0f;
}

// Few-tap bent-normal AO over the exact oracle (mirrors splatAO's rings).
template <typename FieldT>
void aoBake(const FieldT& f, glm::vec3 p, glm::vec3 n, float& ao, glm::vec3& bent)
{
    const float rad[2] = { 0.18f, 0.60f };
    glm::vec3 tv = glm::cross(n, glm::vec3(0.0001f, 1.0f, 0.0001f));
    glm::vec3 tang = safeNormalize(tv + glm::vec3(1e-5f, 0.0f, 0.0f));
    glm::vec3 bitan = safeNormalize(glm::cross(n, tang));
    float occ = 0.0f, wsum = 0.0f;
    glm::vec3 bd = n * 0.5f;
    for (int ring = 0; ring < 2; ++ring) {
        const float r = rad[ring];
        for (int a = 0; a < 4; ++a) {
            const float ang = float(a) * 1.5708f + float(ring) * 0.785f;
            glm::vec3 dir = safeNormalize(n * 0.85f +
                (std::cos(ang) * tang + std::sin(ang) * bitan) * 0.7f);
            const float s = f.sampleWorld(p + dir * r).d;
            float fall = glm::clamp(1.0f - s / r, 0.0f, 1.0f);
            fall = fall * fall * (3.0f - 2.0f * fall);
            const float w = 1.0f / (1.0f + fall * fall * 4.0f);
            occ += w * fall;
            wsum += w;
            bd += dir * w * (1.0f - fall);
        }
    }
    ao = glm::clamp(1.0f - 0.85f * occ / std::max(wsum, 1e-4f), 0.0f, 1.0f);
    bent = safeNormalize(bd + glm::vec3(1e-6f, 0.0f, 0.0f));
}

// Per-cell micro-detail emission: 0-3 deterministic child disks of one base
// surface cell (moss/soil grain, pebbles, bark relief, leaflets, roof-
// underside fillers). Hash-driven from the lattice cell + slot, so two builds
// are bit-identical; children inherit the base cell's material and baked
// shadow/AO/bent (no extra marches). Shared by the bake (buildSurfels) and
// the live store path (LiveEditor::chunkRun), so the same cell always yields
// the same micro geometry.
void emitMicroSurfelsForCell(int x, int y, int z, const Surfel& b,
                             std::vector<Surfel>& micros)
{
    const int mat = int(b.mat_ao.x + 0.5f);
    if (mat < 0 || mat >= kPaletteN)
        return;
    if (mat >= 9 && mat <= 15)
        return; // emissive: keep crisp, no fuzz
    glm::vec3 bn = safeNormalize(glm::vec3(b.normal_rV));
    const glm::vec3 bp(b.pos_rU);
    const glm::vec3 up = std::fabs(bn.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 t = safeNormalize(glm::cross(bn, up));
    const glm::vec3 bb = safeNormalize(glm::cross(bn, t));
    auto emit = [&](float o1, float o2, float lift, float tilt,
                    float rScale, float aoMul, int slot, float bladeAspect = 1.0f) {
        const float j1 = microHash(x, y, z, slot * 2 + 101) - 0.5f;
        const float j2 = microHash(x, y, z, slot * 2 + 102) - 0.5f;
        const glm::vec3 nn = safeNormalize(bn + (t * j1 + bb * j2) * tilt);
        const glm::vec3 pp = bp + (t * o1 + bb * o2) + bn * lift;
        Surfel m;
        // When the parent is an anisotropic blade/frond, the ACROSS radius is
        // the structure's true width; using the along radius would re-fatten
        // the stem the base pass just narrowed.
        const float pU = b.pos_rU.w, pV = b.normal_rV.w;
        const float rrBase =
            std::max((pU > pV * 1.3f ? pV : pU) * rScale, 1e-4f);
        // Vegetation micros stretch along their own lean, so a grass crumb
        // becomes a leaning blade instead of a dot (a near-vertical base
        // normal leaves no usable "up" axis in the disk plane, which is why
        // this uses the lean rather than the structure's long axis).
        // radiusU (along the blade) scales, radiusV keeps the base radius -
        // same convention as the base-surfel anisotropy.
        float rU = rrBase;
        glm::vec3 tanDir(0.0f);
        if (bladeAspect > 1.0f) {
            glm::vec3 lean = t * j1 + bb * j2;
            lean -= nn * glm::dot(lean, nn);
            const float ll = glm::length(lean);
            if (ll > 1e-4f) {
                tanDir = lean / ll;
                rU = rrBase * bladeAspect;
            }
        }
        m.pos_rU = glm::vec4(pp, rU);
        m.normal_rV = glm::vec4(nn, rrBase);
        m.bent_sh = glm::vec4(
            safeNormalize(glm::vec3(b.bent_sh) + (nn - bn) * 0.5f), b.bent_sh.w);
        // Decode AO before multiplying it, then re-pack the parent's layer
        // metadata. Clamping the packed word itself used to erase the object
        // tag, so micro disks stayed behind while their base surfels rotated.
        const uint8_t parentLayer = surfelLayerId(b.mat_ao.w);
        const float childAo = surfelBakedAo(b.mat_ao.w) * aoMul;
        m.mat_ao = glm::vec4(b.mat_ao.x, b.mat_ao.y, b.mat_ao.z,
                             packSurfelAo(childAo, parentLayer));
        // xyz = blade tangent (zero = isotropic disk); w INHERITS the parent's
        // per-cell texture override. Leaving it at 0 made every micro disk
        // sample the material's own atlas slot, so a bark-tagged trunk rendered
        // bark base splats with plank micro splats mixed in.
        m.tan_aspect = glm::vec4(tanDir, b.tan_aspect.w);
        micros.push_back(m);
    };
    const float h0 = microHash(x, y, z, 1);
    const float h1 = microHash(x, y, z, 2);
    const float h2 = microHash(x, y, z, 3);
    const float oA = (h1 - 0.5f) * 0.09f;
    const float oB = (h2 - 0.5f) * 0.09f;
    // Micros inherit the parent's footprint: a parent that is a long thin
    // ellipsoid (a stem) passes its blade stretch on, a round parent keeps
    // round micros. Purely geometric - no material test.
    const float bA = (b.pos_rU.w > b.normal_rV.w * 1.3f) ? 1.7f : 1.0f;
    // micro-grain: small dense children (2-6 cm apparent) so close-ups read
    // as moss grain, sand, bark fibre and leaflets instead of flat 10 cm
    // disks. Spawn rates are high on purpose: the renderer distance-culls
    // micros in far chunks.
    //
    // Facet contrast is deliberately LOW for the non-foliage materials: a
    // micro disk is only a few pixels wide at mid distance, so a strong
    // normal tilt made each child shade visibly darker/lighter than its base
    // cell and the surfaces read as speckled/"holed" (worst on large flat
    // planks and the stepped roof). The parallax relief comes from the
    // geometry (offset + lift) and survives; the tilt/AO now only modulate it.
    // Foliage (mat 8) keeps its high tilt: there the facet noise *is* the
    // canopy volume.
    if (mat <= 1) { // meadow blades / soil crumbs
        if (h0 < 0.80f)
            emit(oA, oB, 0.014f, 0.28f, 0.30f, 0.97f, 1, bA);
        if (h2 < 0.30f)
            emit(-oA, -oB, 0.020f, 0.36f, 0.24f, 0.94f, 11, bA);
    } else if (mat == 2 || mat == 3) { // pebbles / sand grain
        if (h0 < 0.70f)
            emit(oA, oB, 0.006f, 0.24f, 0.20f + 0.12f * h1, 0.97f, 2, bA);
        if (h2 < 0.30f)
            emit(-oA * 0.7f, -oB * 0.7f, 0.004f, 0.32f, 0.16f, 0.95f, 12, bA);
    } else if (mat == 4 || mat == 5 || mat == 16) { // rock strata chips
        if (h0 < 0.65f)
            emit(oA, oB, 0.008f, 0.30f, 0.34f, 0.94f, 3, bA);
        if (h2 < 0.25f)
            emit(-oA, -oB, 0.012f, 0.40f, 0.26f, 0.91f, 13, bA);
    } else if (mat == 6) { // bark relief along the tangent
        if (h0 < 0.80f)
            emit(oA * 1.6f, oB * 0.5f, 0.005f, 0.18f, 0.28f, 0.98f, 4, bA);
        if (h2 < 0.35f)
            emit(-oA * 1.2f, oB * 0.8f, 0.004f, 0.26f, 0.22f, 0.96f, 14, bA);
    } else if (mat == 7) { // roof: seal undersides, moss the tops
        if (bn.y < -0.2f) {
            emit(0.0f, 0.0f, -0.005f, 0.0f, 1.15f, 1.0f, 5, bA);
        } else {
            if (h0 < 0.85f)
                emit(oA, oB, 0.011f, 0.35f, 0.38f, 0.95f, 6, bA);
            if (h2 < 0.40f)
                emit(-oA, -oB, 0.015f, 0.45f, 0.30f, 0.92f, 16, bA);
        }
    } else if (mat == 8) { // canopy leaflets: real volume
        if (h0 < 0.90f)
            emit(oA * 1.3f, oB * 1.3f, 0.008f, 1.20f, 0.38f + 0.20f * h1, 0.90f, 7, bA);
        if (h2 < 0.55f)
            emit(-oA, -oB, 0.013f, 1.40f, 0.30f, 0.85f, 8, bA);
        if (microHash(x, y, z, 9) < 0.30f)
            emit(oB, -oA, 0.018f, 1.10f, 0.26f, 0.88f, 19, bA);
    }
}

} // namespace

SurfelSet buildSurfels(const VoxelField& field, const SurfelParams& params) {
    SurfelSet set;
    const auto t0 = std::chrono::steady_clock::now();
    set.chunkRange.assign(size_t(kSurfGridN * kSurfGridN * kSurfGridN) + 1, 0);

    std::vector<uint64_t> keys;
    keys.reserve(1200000);

    const int latN = field.latN();
    for (int z = 0; z < latN; ++z) {
        for (int x = 0; x < latN; ++x) {
            const int16_t colTop = field.colTops()[size_t(z) * latN + size_t(x)];
            if (colTop < 0)
                continue;
            // A cell is laterally exposed only where a neighbour column is
            // lower, so the surface range is [minNbrTop, colTop] (flat
            // ground: just the top cell; canyon walls: the full exposed
            // face). This skips ~550 buried cells per column on average.
            int16_t minNbr = colTop;
            if (x > 0)
                minNbr = std::min(minNbr, field.colTops()[size_t(z) * latN + size_t(x - 1)]);
            if (x < latN - 1)
                minNbr = std::min(minNbr, field.colTops()[size_t(z) * latN + size_t(x + 1)]);
            if (z > 0)
                minNbr = std::min(minNbr, field.colTops()[size_t(z - 1) * latN + size_t(x)]);
            if (z < latN - 1)
                minNbr = std::min(minNbr, field.colTops()[size_t(z + 1) * latN + size_t(x)]);
            if (minNbr < 0)
                minNbr = 0;
            for (int y = minNbr; y <= colTop; ++y) {
                if (terrainSurface(field, x, y, z))
                    keys.push_back(packKey(x, y, z));
            }
        }
    }

    const auto& blk = field.objectBlockMask();
    const int nBlocks = VoxelField::globalBlocks(); // 256
    const int blkSize = VoxelField::blockSize();    // 4
    if (blk.size() >= size_t(nBlocks) * size_t(nBlocks) * size_t(nBlocks)) {
    for (int bz = 0; bz < nBlocks; ++bz)
    for (int by = 0; by < nBlocks; ++by)
    for (int bx = 0; bx < nBlocks; ++bx) {
        if (blk[(size_t(bz) * nBlocks + size_t(by)) * nBlocks + size_t(bx)] == 0) continue;
        const int baseX = bx * blkSize, baseY = by * blkSize, baseZ = bz * blkSize;
        for (int dz = 0; dz < blkSize; ++dz)
        for (int dy = 0; dy < blkSize; ++dy)
        for (int dx = 0; dx < blkSize; ++dx) {
            const int cx = baseX + dx, cy = baseY + dy, cz = baseZ + dz;
            if (cx < 0 || cx >= latN || cy < 0 || cy >= latN || cz < 0 || cz >= latN) continue;
            const auto s = field.sample(cx, cy, cz);
            if (s.d <= 0.0f && s.obj)
                keys.push_back(packKey(cx, cy, cz));
        }
    }
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const auto tEnum = std::chrono::steady_clock::now();

    const float baseR = params.baseRadius;
    const float voxel = 0.1f;
    int n = int(keys.size());
    set.terrainCount = 0;
    set.objectCount = 0;

    std::vector<glm::vec3> rawNormals(n);
    std::vector<EdgeInfo> edgeInfos(n);
    std::vector<Surfel> surfels(n);
    std::vector<uint8_t> isObj(n);    // pass-2 snapshot: object-field winners
    std::vector<uint8_t> layerId(n);  // owning .vxw layer, 0 = terrain/unowned

    // Pass 1: raw mean normals (sharded). Smoothing lookups below use
    // binary search over the sorted keys (lock-free, no hash build).
    {
        // meanNormal() is 6 lock-free field samples per cell: shard it in
        // batches to avoid one atomic per surfel (1M+ fetch_adds).
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        constexpr int kBatch = 256;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                for (;;) {
                    const int begin = next.fetch_add(kBatch);
                    if (begin >= n)
                        return;
                    const int end = std::min(begin + kBatch, n);
                    for (int i = begin; i < end; ++i) {
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);
                    rawNormals[i] = meanNormal(field, x, y, z);
                    }
                }
            });
        for (auto& th : threads)
            th.join();
    }

    // Hard-edge classification is occupancy-based, not curvature-based.
    // Two exposed faces on different axes form a real crease; opposite faces
    // of a thin plate do not. Normal disagreement remains available below for
    // optional anisotropy, but it never tightens the coverage footprint.
    {
        constexpr int dirs[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                                     { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
        for (int i = 0; i < n; ++i) {
            int x, y, z;
            unpackKey(keys[i], x, y, z);
            if (!field.sample(x, y, z).obj)
                continue;
            unsigned exposedFaces = 0;
            for (int d = 0; d < 6; ++d) {
                const int nx = x + dirs[d][0];
                const int ny = y + dirs[d][1];
                const int nz = z + dirs[d][2];
                const bool air = nx < 0 || nx >= latN || ny < 0 || ny >= latN ||
                                 nz < 0 || nz >= latN ||
                                 field.sample(nx, ny, nz).d > 0.0f;
                if (air)
                    exposedFaces |= 1u << d;
            }
            edgeInfos[i] = edgeInfoFromMask(exposedFaces);
        }
    }

    // Expand degenerate cells instead of dropping them.
    //
    // meanNormal() sums the outward directions of the exposed faces, so a cell
    // whose exposure is symmetric cancels to exactly zero: the middle of a
    // one-cell-thick column (a reed stem, a thin post) has +/-X and +/-Z air
    // and +/-Y solid, i.e. sum == 0. The old filter dropped those cells, which
    // removed the entire BODY of every thin structure and left only its caps -
    // the "disc hovering above the stem" artifact. Cells with no air neighbour
    // at all (enclosed air / buried, the case the filter was written for) are
    // still dropped, because the expansion below emits nothing for them.
    //
    // A degenerate cell becomes one entry per exposed face, carrying that
    // face's axis-aligned normal, so a one-cell column renders as its actual
    // voxel faces from every direction. `faceEntry` marks those entries so
    // pass 2 can skip the neighbourhood smoothing (they must stay axis
    // aligned) and the micro pass can emit one micro set per cell, not per
    // face.
    //
    // The same expansion is needed for THIN structures whose cells have a
    // DIAGONAL mean normal (two perpendicular exposed faces: every cell of a
    // two-cell-wide post is such a corner). A 45-degree disk covers the corner
    // but not the flat face it belongs to - it reaches only rV/sqrt(2) per
    // axis - and on a two-cell face the two corner disks leave a strip down
    // the middle: the "hollow post" slots. The perpendicular face cannot cover
    // it either (it is backfaced when seen head-on), so thin structures must
    // render their voxel faces. Thick objects keep the mean normal: there the
    // 45-degree disk is the wanted corner smoothing.
    std::vector<uint64_t> keys2;
    std::vector<glm::vec3> normals2;
    std::vector<EdgeInfo> edgeInfos2;
    std::vector<uint8_t> faceEntry;
    keys2.reserve(keys.size() + keys.size() / 8);
    normals2.reserve(keys.size() + keys.size() / 8);
    edgeInfos2.reserve(keys.size() + keys.size() / 8);
    faceEntry.reserve(keys.size() + keys.size() / 8);
    {
        const int dirs[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                                 { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
        const auto solidW = [&](glm::vec3 q) {
            return field.sampleWorld(q).d <= 0.0f;
        };
        for (size_t i = 0; i < keys.size(); ++i) {
            int x, y, z;
            unpackKey(keys[i], x, y, z);
            const glm::vec3 cellCentre(
                -51.2f + (x + 0.5f) * voxel, -51.2f + (y + 0.5f) * voxel,
                -51.2f + (z + 0.5f) * voxel);
            const bool degenerate =
                glm::dot(rawNormals[i], rawNormals[i]) < 1e-6f;
            bool thinCorner = false;
            if (!degenerate && field.sample(x, y, z).obj) {
                const glm::vec3 an = glm::abs(rawNormals[i]);
                const int nonZero = (an.x > 1e-3f) + (an.y > 1e-3f) +
                                    (an.z > 1e-3f);
                thinCorner = nonZero >= 2 && thinCellAt(solidW, cellCentre);
            }
            if (!degenerate && !thinCorner) {
                keys2.push_back(keys[i]);
                normals2.push_back(rawNormals[i]);
                edgeInfos2.push_back(edgeInfos[i]);
                faceEntry.push_back(0);
                continue;
            }
            for (int d = 0; d < 6; ++d) {
                const int nx = x + dirs[d][0], ny = y + dirs[d][1],
                          nz = z + dirs[d][2];
                // out-of-lattice counts as air, matching meanNormal()
                const bool air =
                    nx < 0 || nx >= latN || ny < 0 || ny >= latN || nz < 0 ||
                    nz >= latN || field.sample(nx, ny, nz).d > 0.0f;
                if (!air)
                    continue;
                keys2.push_back(keys[i]);
                normals2.push_back(glm::vec3(float(dirs[d][0]), float(dirs[d][1]),
                                             float(dirs[d][2])));
                edgeInfos2.push_back(edgeInfos[i]);
                faceEntry.push_back(uint8_t(d + 1));
            }
        }
    }
    keys.swap(keys2);
    rawNormals.swap(normals2);
    edgeInfos.swap(edgeInfos2);
    n = int(keys.size());
    surfels.resize(size_t(n));
    isObj.resize(size_t(n));
    layerId.resize(size_t(n));

    // Pass 2: blend, smooth, bake shadow+AO, and emit (also sharded; every
    // index is independent and keys/rawNormals are read-only here).
    const glm::vec3 sunDir = glm::normalize(params.sunDir);
    std::atomic<size_t> terrainCount{ 0 }, objectCount{ 0 };
    {
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                size_t locTerrain = 0, locObject = 0;
                for (;;) {
                    const int i = next.fetch_add(1);
                    if (i >= n)
                        break;
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);

                    const auto s = field.sample(x, y, z);
                    glm::vec3 n = rawNormals[i];

                    if (params.terrainHeightfieldNormals) {
                        const int16_t colTop =
                            field.colTops()[size_t(z) * latN + size_t(x)];
                        if (colTop >= 0 && y == colTop) {
                            const glm::vec3 hn = heightfieldNormal(
                                field,
                                glm::vec3(-51.2f + (x + 0.5f) * voxel,
                                          -51.2f + (y + 0.5f) * voxel,
                                          -51.2f + (z + 0.5f) * voxel));
                            n = safeNormalize(glm::mix(n, hn, params.heightfieldBlend));
                        }
                    }

                    // rChaos grows disks where neighbour normals disagree
                    // (wedges that leak); 1.0 on agreed patches.
                    float rChaos = 1.0f;
                    float aspect = 1.0f;
                    glm::vec3 tanDir(0.0f);
                    // Face-expanded entries (one-cell-thick columns/plates)
                    // keep their axis-aligned face normal: smoothing them
                    // against neighbours from other faces would tilt them off
                    // the voxel face they stand for.
                    if (params.smoothNormals && faceEntry[i] == 0) {
                        glm::vec3 acc = n;
                        float wsum = 1.0f;
                        const int faceDirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 },
                                                     { 0, 1, 0 }, { 0, -1, 0 },
                                                     { 0, 0, 1 }, { 0, 0, -1 } };
                        glm::vec3 agr = rawNormals[i];
                        float agrN = 1.0f;
                        glm::vec3 bendSum(0.0f);
                        for (auto& d : faceDirs) {
                            const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                            auto it = std::lower_bound(keys.begin(), keys.end(), nk);
                            if (it != keys.end() && *it == nk) {
                                const glm::vec3 nn = rawNormals[size_t(it - keys.begin())];
                                acc += nn;
                                ++wsum;
                                agr += nn;
                                ++agrN;
                                bendSum += nn - rawNormals[i];
                            }
                        }
                        n = safeNormalize(acc / wsum);
                        // Curvature may guide the optional footprint tangent,
                        // but it is not a coverage test. Terrain and foliage
                        // retain their historical disagreement growth; opaque
                        // object parents keep their full radius unless the
                        // occupancy classifier below finds a genuine hard edge.
                        const float agreement =
                            glm::clamp(glm::length(agr) / agrN, 0.0f, 1.0f);
                        const float dis = 1.0f - agreement;
                        if (params.anisotropy)
                            anisotropyFromBend(bendSum, n, aspect, tanDir);
                        rChaos = s.obj ? 1.0f : 1.0f + 0.8f * dis;
                    }

                    const glm::vec3 cellCentre =
                        glm::vec3(-51.2f + (x + 0.5f) * voxel,
                                  -51.2f + (y + 0.5f) * voxel,
                                  -51.2f + (z + 0.5f) * voxel);
                    const glm::vec3 pos = cellCentre + n * (0.5f * voxel);

                    // baked sun shadow (binary march on the exact oracle) +
                    // bent-normal AO; backfaces skip the march (FS gates too).
                    // Origin rides 0.3 above the surface like the SVO backend
                    // (p + n*0.35): a tight offset buries canopy/thin-object
                    // origins inside neighbouring solid and self-shadows.
                    float shadow = 1.0f;
                    if (glm::dot(n, sunDir) > 0.02f)
                        shadow = shadowMarch(field, pos + n * 0.3f, sunDir);
                    float ao = 1.0f;
                    glm::vec3 bent = n;
                    aoBake(field, pos + n * 0.02f, n, ao, bent);

                    const uint8_t mat = s.mat;
                    const float refl = kMaterialReflection[std::min(int(mat), kPaletteN - 1)].x;
                    const float rough = kMaterialReflection[std::min(int(mat), kPaletteN - 1)].y;

                    // Vegetation/structure shape. The rule is purely GEOMETRIC
                    // - the material is irrelevant:
                    //
                    //  - LONG and only a few voxels thick (grass blades, reed
                    //    stems, thin posts and branches: <= 3 cells across,
                    //    >= 4 cells tall) become LONG VERTICAL ELLIPSOIDS. Both
                    //    radii matter: the across radius must SHRINK to roughly
                    //    the structure's own width, otherwise the chaos-grown
                    //    disk swamps a one-cell column and the splat reads as a
                    //    fat blob however far it is stretched.
                    //  - everything else - thick masses AND small blobs that
                    //    are thin but not long - keeps the round disk.
                    const ThinFootprint fp =
                        params.anisotropy
                            ? thinFootprintAt(
                                  [&](glm::vec3 q) { return field.sampleWorld(q).d <= 0.0f; },
                                  cellCentre, n, x, y, z)
                            : ThinFootprint {};

                    // Foliage growth (2x) exists to SEAL sparse terrain canopy:
                    // grass cards and bushes are represented by splats alone,
                    // so the disks must overlap. Object foliage is real
                    // geometry (leaf clusters, canopy blobs); growing those
                    // makes neighbouring clusters merge into one mass and
                    // swallow the branches between them, so object foliage
                    // keeps the base radius and the voxel silhouette.
                    const float foliageGrow = (mat == 8 && !s.obj) ? 2.0f : 1.0f;
                    // Only a genuine lattice hard edge tightens an opaque
                    // object parent. Explicit thin footprints already encode
                    // their true width and must not be reduced a second time.
                    const bool hardEdge =
                        s.obj && mat != 8 && !(mat >= 9 && mat <= 15) &&
                        edgeInfos[i].pairCount > 0 && !fp.thin;
                    const float edgeFactor =
                        hardEdge
                            ? 1.0f - glm::clamp(params.edgeShrink, 0.0f, 1.0f)
                            : 1.0f;
                    const float rr =
                        baseR * std::min(foliageGrow * rChaos * edgeFactor, 2.2f);
                    float rU = rr, rV = rr;
                    glm::vec3 tanOut(0.0f);
                    if (fp.thin) {
                        rU = fp.along;
                        rV = fp.across;
                        tanOut = fp.axis;
                    } else if (hardEdge) {
                        // The bridge carries the edge-aligned elongation. Keep
                        // the tightened parent isotropic so its full diameter,
                        // rather than just one axis, is reduced.
                        aspect = 1.0f;
                        tanDir = glm::vec3(0.0f);
                    } else if (params.anisotropy && aspect > 1.0f) {
                        rU = rr * aspect;
                        tanOut = tanDir;
                    }
                    Surfel sl;
                    sl.pos_rU = glm::vec4(pos, rU);
                    sl.normal_rV = glm::vec4(n, rV);
                    sl.bent_sh = glm::vec4(bent, shadow);
                    sl.mat_ao = glm::vec4(float(mat), refl, rough, ao);
                    // w = per-cell texture override (0 = use the material's
                    // atlas slot): phase-2 per-object textures ride the
                    // otherwise spare channel. Raw integer 0..255; the shader
                    // tests > 0.5 so an unset (0) override never fires
                    sl.tan_aspect = glm::vec4(tanOut, float(s.tex));
                    surfels[i] = sl;
                    isObj[i] = s.obj ? 1 : 0;
                    layerId[i] = s.obj ? s.layer : uint8_t(0);
                    if (s.obj)
                        ++locObject;
                    else
                        ++locTerrain;
                }
                terrainCount += locTerrain;
                objectCount += locObject;
            });
        for (auto& th : threads)
            th.join();
    }
    set.terrainCount = terrainCount.load();
    set.objectCount = objectCount.load();

    // Shadow penumbra + AO smoothing: the baked march verdict is binary per
    // surfel (draws shadow boundaries along disk shapes) and the baked AO
    // steps at staircase-tap granularity (mottles flat walls). Average both
    // over face-neighbouring surface surfels so all baked fields vary
    // continuously and adjacent same-color splats shade identically.
    // Reads snapshots (order-free, deterministic).
    {
        std::vector<float> shPrev(n), aoPrev(n);
        for (int i = 0; i < n; ++i) {
            shPrev[i] = surfels[i].bent_sh.w;
            aoPrev[i] = surfels[i].mat_ao.w;
        }
        // O(1) neighbour lookup: keys are sorted-unique, so a hash gives the
        // same index as the old lower_bound with no log factor. Built once,
        // read-only across threads. Same averaging, bit-identical results.
        std::unordered_map<uint64_t, int> keyToIdx;
        keyToIdx.reserve(size_t(n) * 2);
        for (int i = 0; i < n; ++i)
            keyToIdx.emplace(keys[i], i);
        std::atomic<int> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        const int faceDirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 },
                                     { 0, 1, 0 }, { 0, -1, 0 },
                                     { 0, 0, 1 }, { 0, 0, -1 } };
        constexpr int kBatch = 256;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                for (;;) {
                    const int begin = next.fetch_add(kBatch);
                    if (begin >= n)
                        break;
                    const int end = std::min(begin + kBatch, n);
                    for (int i = begin; i < end; ++i) {
                    int x, y, z;
                    unpackKey(keys[i], x, y, z);
                    float acc = shPrev[i];
                    float aoAcc = aoPrev[i];
                    float wsum = 1.0f;
                    for (auto& d : faceDirs) {
                        const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                        auto it = keyToIdx.find(nk);
                        if (it != keyToIdx.end()) {
                            const int j = it->second;
                            acc += shPrev[j];
                            aoAcc += aoPrev[j];
                            ++wsum;
                        }
                    }
                    surfels[i].bent_sh.w = acc / wsum;
                    surfels[i].mat_ao.w = aoAcc / wsum;
                    }
                }
            });
        for (auto& th : threads)
            th.join();
    }

    // Pack the exact owning layer AFTER AO smoothing, which averages w across
    // face neighbours and would otherwise mix ownership IDs together. The
    // shader contract is shared with common_surfel.glsl: AO + 8 + 16*layerId.
    // Unowned object geometry remains layer 0 and is intentionally excluded
    // from layer rotation (notably newly added live-edit cells).
    for (int i = 0; i < n; ++i)
        surfels[i].mat_ao.w = packSurfelAo(surfelBakedAo(surfels[i].mat_ao.w),
                                            layerId[i]);

    // ---- TRACE audit: how many surfels are geometrically unsupported? -------
    // A surfel whose 3x3x3 lattice neighbourhood holds no OTHER surfel has no
    // disk within one cell of it, so it cannot visually connect to anything and
    // renders as a detached blob. `keys` is still key-sorted here (the chunk
    // bucketing below destroys that order), so the probe is a binary search.
    // Sampled: this is a diagnostic, not a production pass.
    if (getenv("VF_TRACE")) {
        const int kProbe[26][3] = {
            {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
            {-1,-1, 0},{1,-1, 0},{-1, 1, 0},{1, 1, 0},
            {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1},
            {-1, 0, 0},{1, 0, 0},{0,-1, 0},{0, 1, 0},{0, 0,-1},{0, 0, 1}};
        const int latN = field.latN();
        size_t probed = 0, detached = 0, detachedLoneVoxel = 0;
        int shown = 0;
        for (int i = 0; i < n; i += 37) {
            int x, y, z;
            unpackKey(keys[i], x, y, z);
            ++probed;
            bool supported = false;
            for (int d = 0; d < 26 && !supported; ++d) {
                const int nx = x + kProbe[d][0], ny = y + kProbe[d][1],
                          nz = z + kProbe[d][2];
                if (nx < 0 || nx >= latN || ny < 0 || ny >= latN || nz < 0 ||
                    nz >= latN)
                    continue;
                const uint64_t nk = packKey(nx, ny, nz);
                auto it = std::lower_bound(keys.begin(), keys.end(), nk);
                if (it != keys.end() && *it == nk)
                    supported = true;
            }
            if (supported)
                continue;
            ++detached;
            // The user's exception: a genuinely lone solid voxel in space IS
            // meant to be visible. Count it separately.
            bool lone = true;
            for (int d = 0; d < 6 && lone; ++d) {
                static const int sd[6][3] = {{1,0,0},{-1,0,0},{0,1,0},
                                             {0,-1,0},{0,0,1},{0,0,-1}};
                const int nx = x + sd[d][0], ny = y + sd[d][1], nz = z + sd[d][2];
                if (nx < 0 || nx >= latN || ny < 0 || ny >= latN || nz < 0 ||
                    nz >= latN || field.sample(nx, ny, nz).d <= 0.0f)
                    lone = false;
            }
            if (lone)
                ++detachedLoneVoxel;
            if (shown < 10) {
                ++shown;
                spdlog::info("  detached surfel #{} cell ({},{},{}) mat {} r {:.3f} "
                             "pos ({:.2f},{:.2f},{:.2f}) loneVoxel={}",
                             shown, x, y, z, int(surfels[i].mat_ao.x + 0.5f),
                             surfels[i].pos_rU.w, surfels[i].pos_rU.x,
                             surfels[i].pos_rU.y, surfels[i].pos_rU.z, lone);
            }
        }
        spdlog::info("unsupported-surfel audit: {} detached of {} probed "
                     "({:.3f}%), of which {} are lone voxels",
                     detached, probed, probed ? 100.0 * detached / probed : 0.0,
                     detachedLoneVoxel);
    }

    std::vector<uint64_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    // precompute chunk ids once: the comparator runs O(n log n) times and
    // unpackKey+chunkIndex per comparison dominated the build
    std::vector<uint32_t> chunkOf(n);
    for (int i = 0; i < n; ++i) {
        const uint64_t k = keys[i];
        chunkOf[i] = chunkIndex(int((k >> 20) & 0x3FFu), int((k >> 10) & 0x3FFu),
                                int(k & 0x3FFu));
    }
    // counting sort by chunk (4096 buckets, O(n)) instead of std::sort:
    // surfels stay ordered by key within each chunk (stable)
    constexpr uint32_t kChunks = kSurfGridN * kSurfGridN * kSurfGridN;
    std::vector<uint32_t> counts(kChunks, 0);
    for (uint32_t c : chunkOf)
        ++counts[c];
    std::vector<uint32_t> starts(kChunks + 1, 0);
    for (uint32_t c = 0; c < kChunks; ++c)
        starts[c + 1] = starts[c] + counts[c];
    std::vector<uint32_t> cursor = starts;
    for (uint32_t i = 0; i < uint32_t(n); ++i)
        order[cursor[chunkOf[i]]++] = i;
    std::vector<Surfel> sorted(n);
    for (int i = 0; i < n; ++i) sorted[i] = surfels[order[i]];
    surfels = std::move(sorted);
    // base-only prefix sums over key chunks (rebuilt below if micros added)
    for (int i = 0; i < n; ++i) {
        int x, y, z;
        unpackKey(keys[order[i]], x, y, z);
        ++set.chunkRange[chunkIndex(x, y, z) + 1];
    }
    for (int c = 1; c <= kSurfGridN * kSurfGridN * kSurfGridN; ++c)
        set.chunkRange[c] += set.chunkRange[c - 1];

    // Per-chunk object presence (draw-time micro-distance selection): object
    // chunks keep their micro detail farther out than terrain-only chunks.
    set.objectChunks.assign(kChunks, 0);
    for (int i = 0; i < n; ++i)
        if (isObj[order[i]])
            set.objectChunks[chunkOf[order[i]]] = 1;

    // ---- hard-edge bridges -------------------------------------------------
    // One small, tangent-aligned splat per exposed face pair. They inherit the
    // parent's baked shading and owner, so this adds no CPU field marches and
    // no fragment-shader work. Keep them per chunk for the LOD/object path and
    // the later [base | edge | micro] interleave.
    std::vector<std::vector<Surfel>> edgeByChunk(kChunks);
    if (params.edgeFill && params.edgeShrink > 0.0f) {
        for (int i = 0; i < n; ++i) {
            const int source = order[i];
            // Face-expanded thin cells have several parent entries with the
            // same key. Their explicit footprints are not tightened, and one
            // bridge set per lattice cell is sufficient.
            if (i > 0 && keys[source] == keys[order[i - 1]])
                continue;
            if (!isObj[source] || edgeInfos[source].pairCount <= 0)
                continue;
            const int mat = int(surfels[i].mat_ao.x + 0.5f);
            if (mat == 8 || (mat >= 9 && mat <= 15))
                continue;
            int x, y, z;
            unpackKey(keys[source], x, y, z);
            const glm::vec3 cellCentre(-51.2f + (x + 0.5f) * voxel,
                                       -51.2f + (y + 0.5f) * voxel,
                                       -51.2f + (z + 0.5f) * voxel);
            std::vector<Surfel>& out = edgeByChunk[chunkOf[i]];
            const size_t before = out.size();
            appendEdgeBridges(cellCentre, surfels[i], edgeInfos[source],
                              baseR, params.edgeBridgeSize, out);
            if (params.cornerFill)
                appendCornerCaps(cellCentre, surfels[i], edgeInfos[source],
                                 baseR, params.edgeBridgeSize, out);
            if (out.size() != before) {
                ++set.edgeParentCount;
                const size_t added = out.size() - before;
                set.edgeBridgeCount += added;
                set.objectCount += added;
            }
        }
    }

    // ---- LOD rings: merged-terrain surfels per chunk (terrain cells only,
    // object-field winners keep their own base surfels). Blocks of
    // 2x2x2 (LOD1) / 4x4x4 (LOD2) lattice cells merge the already-shaded
    // base surfels: position/normal/bent/shadow/AO are area-weighted means
    // (cells are equal), material is the majority. The merged radius covers
    // the union of member footprints (half of the member-centre AABB
    // diagonal + mean member radius, with a per-ring coverage floor) so the
    // Gaussian disks keep overlapping after merging. Deterministic: members
    // accumulate in base-array order, blocks emit in sorted block-key order.
    std::vector<std::vector<Surfel>> lod1ByChunk, lod2ByChunk;
    if (params.lodRings && n > 0) {
        auto buildRing = [&](int shift, std::vector<std::vector<Surfel>>& byChunk) {
            const int blocksPerAxis = 64 >> shift; // 32 (2x) / 16 (4x)
            const float coverBase = shift == 1 ? 0.20f : 0.30f;
            struct LodGroup {
                glm::vec3 posSum { 0.0f };
                glm::vec3 posMin { 0.0f };
                glm::vec3 posMax { 0.0f };
                glm::vec3 nSum { 0.0f };
                glm::vec3 bentSum { 0.0f };
                float shSum = 0.0f, aoSum = 0.0f, rSum = 0.0f;
                int count = 0;
            };
            struct LodAcc {
                LodGroup mat[kPaletteN];
                int total = 0;
            };
            std::atomic<int> next{ 0 };
            unsigned hc = std::max(1u, std::thread::hardware_concurrency());
            std::vector<std::thread> threads;
            for (unsigned t = 0; t < hc; ++t)
                threads.emplace_back([&] {
                    std::unordered_map<uint32_t, LodAcc> acc;
                    for (;;) {
                        const uint32_t c = next.fetch_add(1);
                        if (c >= kChunks)
                            return;
                        acc.clear();
                        const uint32_t b0 = set.chunkRange[c];
                        const uint32_t b1 = set.chunkRange[c + 1];
                        for (uint32_t i = b0; i < b1; ++i) {
                            if (isObj[order[i]]) {
                                // Objects ride along UNMERGED: the LOD run
                                // replaces the whole chunk draw at draw
                                // time, so dropping them here would make
                                // trees vanish in LOD bands. Terrain-only
                                // merging keeps every object cell visible.
                                byChunk[c].push_back(surfels[i]);
                                continue;
                            }
                            int x, y, z;
                            unpackKey(keys[order[i]], x, y, z);
                            const uint32_t bid =
                                (((x & 63) >> shift) * blocksPerAxis +
                                 ((y & 63) >> shift)) * blocksPerAxis +
                                ((z & 63) >> shift);
                            const int m = glm::clamp(
                                int(surfels[i].mat_ao.x + 0.5f), 0, kPaletteN - 1);
                            auto it = acc.find(bid);
                            if (it == acc.end())
                                it = acc.emplace(bid, LodAcc {}).first;
                            LodAcc& a = it->second;
                            LodGroup& g = a.mat[m];
                            const glm::vec3 p(surfels[i].pos_rU);
                            if (g.count == 0) {
                                g.posSum = p;
                                g.posMin = g.posMax = p;
                            } else {
                                g.posSum += p;
                                g.posMin = glm::min(g.posMin, p);
                                g.posMax = glm::max(g.posMax, p);
                            }
                            g.nSum += glm::vec3(surfels[i].normal_rV);
                            g.bentSum += glm::vec3(surfels[i].bent_sh);
                            g.shSum += surfels[i].bent_sh.w;
                            g.aoSum += surfels[i].mat_ao.w;
                            g.rSum += surfels[i].pos_rU.w;
                            ++g.count;
                            ++a.total;
                        }
                        // Object parents ride along unmerged in every ring;
                        // their derived hard-edge bridges must ride with them
                        // or distant chunks would lose the coverage repair.
                        if (!edgeByChunk[c].empty())
                            byChunk[c].insert(byChunk[c].end(),
                                               edgeByChunk[c].begin(),
                                               edgeByChunk[c].end());
                        if (acc.empty())
                            continue;
                        // deterministic emission: sorted block ids
                        std::vector<uint32_t> bids;
                        bids.reserve(acc.size());
                        for (auto& e : acc)
                            bids.push_back(e.first);
                        std::sort(bids.begin(), bids.end());
                        for (uint32_t bid : bids) {
                            const LodAcc& a = acc[bid];
                            int first = -1, second = -1;
                            for (int m = 0; m < kPaletteN; ++m) {
                                if (!a.mat[m].count)
                                    continue;
                                if (first < 0 ||
                                    a.mat[m].count > a.mat[first].count) {
                                    second = first;
                                    first = m;
                                } else if (second < 0 ||
                                           a.mat[m].count > a.mat[second].count) {
                                    second = m;
                                }
                            }
                            auto emitGroup = [&](int m) {
                                const LodGroup& g = a.mat[m];
                                const float fc = float(g.count);
                                const glm::vec3 pos = g.posSum / fc;
                                const glm::vec3 nn = safeNormalize(g.nSum);
                                const float meanR = g.rSum / fc;
                                const float spread =
                                    0.5f * glm::length(g.posMax - g.posMin);
                                const float r = glm::min(
                                    0.5f, std::max(coverBase, spread + meanR));
                                Surfel sl;
                                sl.pos_rU = glm::vec4(pos, r);
                                sl.normal_rV = glm::vec4(nn, r);
                                sl.bent_sh = glm::vec4(
                                    safeNormalize(g.bentSum), g.shSum / fc);
                                sl.mat_ao =
                                    glm::vec4(float(m), kMaterialReflection[m].x,
                                              kMaterialReflection[m].y,
                                              glm::clamp(g.aoSum / fc, 0.0f, 1.0f));
                                sl.tan_aspect = glm::vec4(0.0f); // merged: isotropic
                                byChunk[c].push_back(sl);
                            };
                            if (first >= 0)
                                emitGroup(first);
                            // The dominant minority group gets its own disk so
                            // material borders (shorelines, snow/rock lines)
                            // stay readable after the merge; it sits on that
                            // group's own mean, not the block centre.
                            if (params.lodMaterialSplit && second >= 0 &&
                                a.mat[second].count >= 2 &&
                                a.mat[second].count * 4 >= a.total)
                                emitGroup(second);
                        }
                    }
                });
            for (auto& th : threads)
                th.join();
        };
        lod1ByChunk.resize(kChunks);
        buildRing(1, lod1ByChunk);
        lod2ByChunk.resize(kChunks);
        buildRing(2, lod2ByChunk);
        size_t l1 = 0, l2 = 0;
        for (uint32_t c = 0; c < kChunks; ++c) {
            l1 += lod1ByChunk[c].size();
            l2 += lod2ByChunk[c].size();
        }
        set.lod1Count = l1;
        set.lod2Count = l2;
    }

    // ---- micro-detail: texture texels become real micro-surfel geometry ----
    // Deterministic children of the sorted BASE parents. This runs before the
    // base+edge interleave below, so `surfels[i]` still matches `order[i]`.
    std::vector<Surfel> micros;
    std::vector<uint32_t> microCounts(kChunks, 0);
    size_t microTerrain = 0, microObject = 0;
    if (params.microDetail && n > 0) {
        micros.reserve(size_t(n) / 2);
        for (int i = 0; i < n; ++i) {
            // Face-expanded cells occupy several consecutive entries with the
            // same key; their micros are emitted once, from the first entry.
            if (i > 0 && keys[order[i]] == keys[order[i - 1]])
                continue;
            int x, y, z;
            unpackKey(keys[order[i]], x, y, z);
            const size_t before = micros.size();
            emitMicroSurfelsForCell(x, y, z, surfels[i], micros);
            const size_t added = micros.size() - before;
            if (added > 0) {
                microCounts[chunkOf[order[i]]] += uint32_t(added);
                if (field.sample(x, y, z).obj)
                    microObject += added;
                else
                    microTerrain += added;
            }
        }
    }

    // ---- interleave base parents and always-on hard-edge bridges ----------
    // Both segments stay in chunkRange, so the renderer keeps one contiguous
    // opaque draw and adds no per-edge draw call. edgeStart is metadata used by
    // live GPU seeding/patching; ordinary rendering only needs microStart.
    {
        std::vector<Surfel> combined;
        size_t edgeTotal = 0;
        for (const auto& v : edgeByChunk)
            edgeTotal += v.size();
        combined.reserve(surfels.size() + edgeTotal);
        std::vector<uint32_t> newRange(kChunks + 1, 0);
        std::vector<uint32_t> newEdgeStart(kChunks + 1, 0);
        for (uint32_t c = 0; c < kChunks; ++c) {
            const uint32_t b0 = set.chunkRange[c];
            const uint32_t b1 = set.chunkRange[c + 1];
            for (uint32_t i = b0; i < b1; ++i)
                combined.push_back(surfels[i]);
            newEdgeStart[c] = uint32_t(combined.size());
            combined.insert(combined.end(), edgeByChunk[c].begin(),
                            edgeByChunk[c].end());
            newRange[c + 1] = uint32_t(combined.size());
        }
        newEdgeStart[kChunks] = uint32_t(combined.size());
        surfels = std::move(combined);
        set.chunkRange = std::move(newRange);
        set.edgeStart = std::move(newEdgeStart);
    }

    if (!micros.empty()) {
        // Final per-chunk layout is [base | edge bridges | material micros].
        std::vector<Surfel> combined;
        combined.reserve(surfels.size() + micros.size());
        std::vector<uint32_t> newRange(kChunks + 1, 0);
        std::vector<uint32_t> newEdgeStart(kChunks + 1, 0);
        std::vector<uint32_t> microStart(kChunks + 1, 0);
        std::vector<uint32_t> mStarts(kChunks + 1, 0);
        for (uint32_t c = 0; c < kChunks; ++c)
            mStarts[c + 1] = mStarts[c] + microCounts[c];
        for (uint32_t c = 0; c < kChunks; ++c) {
            const uint32_t b0 = set.chunkRange[c], b1 = set.chunkRange[c + 1];
            for (uint32_t i = b0; i < b1; ++i)
                combined.push_back(surfels[i]);
            newEdgeStart[c] = uint32_t(combined.size());
            microStart[c] = uint32_t(combined.size());
            const uint32_t m0 = mStarts[c], m1 = mStarts[c + 1];
            for (uint32_t i = m0; i < m1; ++i)
                combined.push_back(micros[i]);
            newRange[c + 1] = uint32_t(combined.size());
        }
        newEdgeStart[kChunks] = uint32_t(combined.size());
        microStart[kChunks] = uint32_t(combined.size());
        surfels = std::move(combined);
        set.chunkRange = std::move(newRange);
        set.edgeStart = std::move(newEdgeStart);
        set.microStart = std::move(microStart);
        set.terrainCount += microTerrain;
        set.objectCount += microObject;
    }

    // ---- append LOD rings after the base+micro stream (absolute offsets) --
    if (!lod1ByChunk.empty()) {
        const uint32_t l1Start = uint32_t(surfels.size());
        set.lod1Range.assign(kChunks + 1, 0);
        for (uint32_t c = 0; c < kChunks; ++c) {
            set.lod1Range[c] = uint32_t(surfels.size());
            surfels.insert(surfels.end(), lod1ByChunk[c].begin(),
                           lod1ByChunk[c].end());
        }
        set.lod1Range[kChunks] = uint32_t(surfels.size());
        (void)l1Start;
    }
    if (!lod2ByChunk.empty()) {
        set.lod2Range.assign(kChunks + 1, 0);
        for (uint32_t c = 0; c < kChunks; ++c) {
            set.lod2Range[c] = uint32_t(surfels.size());
            surfels.insert(surfels.end(), lod2ByChunk[c].begin(),
                           lod2ByChunk[c].end());
        }
        set.lod2Range[kChunks] = uint32_t(surfels.size());
    }
    set.surfels = std::move(surfels);
    // ---- drop unsupported (floating) surfels ------------------------------
    // A splat no other splat reaches renders as a speck hanging in space next
    // to the surface it belongs to. Measured on an edited region: 2 of 229,747
    // (both seg=micro foliage, mat 8, radius 0.0143-0.0219 m) missed their
    // nearest neighbour by 2.7-4.2 mm -- at 2 cm across that gap is plainly
    // visible. They are suppressed UNLESS the cell is a genuinely single solid
    // voxel in space, which is the one case where a lone disk is the point.
    //
    // Scope: the base+edge+micro region only (chunkRange's span). The LOD rings
    // are appended after this point and are left alone - a merged ring disk is
    // a coarse stand-in for a whole block, and dropping one would punch a hole
    // at LOD distance rather than remove a speckle.
    if (params.dropFloating && !set.chunkRange.empty() &&
        set.chunkRange[kSurfGridN * kSurfGridN * kSurfGridN] <= set.surfels.size()) {
        const float halfF = 0.5f * vf::voxel::WORLD;
        const float vsF = voxel;
        auto cellKeyF = [](int cx, int cy, int cz) {
            return (uint64_t(std::uint32_t(cx)) << 20) | (uint32_t(cy) << 10) |
                   uint32_t(cz);
        };
        const uint32_t kChunks3 = kSurfGridN * kSurfGridN * kSurfGridN;
        const uint32_t span = set.chunkRange[kChunks3];
        std::unordered_map<uint64_t, std::vector<int>> bucket;
        bucket.reserve(span);
        for (uint32_t i = 0; i < span; ++i)
            bucket[cellKeyF(int((set.surfels[i].pos_rU.x + halfF) / vsF),
                           int((set.surfels[i].pos_rU.y + halfF) / vsF),
                           int((set.surfels[i].pos_rU.z + halfF) / vsF))]
                .push_back(int(i));
        static const int kProbeF[27][3] = {
            {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
            {-1,-1, 0},{1,-1, 0},{-1, 1, 0},{1, 1, 0},
            {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1},
            {-1, 0, 0},{1, 0, 0},{0,-1, 0},{0, 1, 0},{0, 0,-1},{0, 0, 1},{0, 0, 0}};
        std::vector<uint8_t> keep(span, 1);
        uint32_t dropped = 0;
        for (uint32_t i = 0; i < span; ++i) {
            const Surfel& a = set.surfels[i];
            const float ra = std::max(a.pos_rU.w, a.normal_rV.w);
            int ex = int((a.pos_rU.x + halfF) / vsF);
            int ey = int((a.pos_rU.y + halfF) / vsF);
            int ez = int((a.pos_rU.z + halfF) / vsF);
            bool touch = false;
            for (int d = 0; d < 27 && !touch; ++d) {
                auto bt = bucket.find(
                    cellKeyF(ex + kProbeF[d][0], ey + kProbeF[d][1], ez + kProbeF[d][2]));
                if (bt == bucket.end())
                    continue;
                for (int bi : bt->second) {
                    if (uint32_t(bi) == i)
                        continue;
                    const Surfel& b = set.surfels[size_t(bi)];
                    if (glm::distance(a.pos_rU, b.pos_rU) <
                        ra + std::max(b.pos_rU.w, b.normal_rV.w)) {
                        touch = true;
                        break;
                    }
                }
            }
            if (touch)
                continue;
            // Exception: a single solid voxel in space is meant to be visible.
            bool lone = field.sample(ex, ey, ez).d <= 0.0f;
            static const int sdF[6][3] = {{1,0,0},{-1,0,0},{0,1,0},
                                          {0,-1,0},{0,0,1},{0,0,-1}};
            const int latNF = field.latN();
            for (int d = 0; d < 6 && lone; ++d) {
                const int nx = ex + sdF[d][0], ny = ey + sdF[d][1], nz = ez + sdF[d][2];
                if (nx < 0 || nx >= latNF || ny < 0 || ny >= latNF || nz < 0 ||
                    nz >= latNF || field.sample(nx, ny, nz).d <= 0.0f)
                    lone = false;
            }
            if (!lone) {
                keep[i] = 0;
                ++dropped;
            }
        }
        if (dropped) {
            // Compact the kept entries and rebuild every per-chunk index array:
            // the layout is [base | edge bridges | material micros], so all
            // three offsets shift once anything is removed.
            std::vector<Surfel> out;
            out.reserve(span);
            std::vector<uint32_t> nRange(kChunks3 + 1, 0), nEdge(kChunks3 + 1, 0),
                                   nMicro(kChunks3 + 1, 0);
            for (uint32_t c = 0; c < kChunks3; ++c) {
                const uint32_t b0 = set.chunkRange[c], b1 = set.chunkRange[c + 1];
                const uint32_t e0 = std::min(set.edgeStart.empty() ? b1
                                                                 : set.edgeStart[c], b1);
                const uint32_t m0 = std::min(set.microStart.empty() ? b1
                                                                   : set.microStart[c], b1);
                const size_t base0 = out.size();
                for (uint32_t i = b0; i < e0; ++i) if (keep[i]) out.push_back(set.surfels[i]);
                const size_t edge0 = out.size();
                for (uint32_t i = e0; i < m0; ++i) if (keep[i]) out.push_back(set.surfels[i]);
                const size_t mic0 = out.size();
                for (uint32_t i = m0; i < b1; ++i) if (keep[i]) out.push_back(set.surfels[i]);
                nRange[c] = uint32_t(base0);
                nEdge[c] = uint32_t(edge0);
                nMicro[c] = uint32_t(mic0);
            }
            nRange[kChunks3] = uint32_t(out.size());
            nEdge[kChunks3] = uint32_t(out.size());
            nMicro[kChunks3] = uint32_t(out.size());
            // keep the tail (LOD rings + water) that starts at span
            out.insert(out.end(), set.surfels.begin() + span, set.surfels.end());
            set.surfels = std::move(out);
            set.chunkRange = std::move(nRange);
            set.edgeStart = std::move(nEdge);
            set.microStart = std::move(nMicro);
        }
        set.droppedFloating = dropped;
    }
    // ---- TRACE audit: geometric support over the FINAL surfel set ---------
    // The cell-adjacency probe above only proves a neighbouring CELL exists.
    // That is not support: on a one-cell-thick plate the front and back face
    // disks are ~0.2 m apart with 0.1 m radii, so they never touch, and a disk
    // can sit alone in space while still having neighbours in the lattice.
    // This buckets every FINAL surfel (base + edge bridges + micros + LOD
    // rings) by its cell and asks the real question: does any other disk's
    // footprint come within reach? Circumradius is used, so this
    // over-detects contact rather than under-detecting it.
    if (getenv("VF_TRACE")) {
        const auto& S = set.surfels;
        const float half = 0.5f * vf::voxel::WORLD;
        // Does every surfel in [chunkRange[c], chunkRange[c+1]) actually live
        // in chunk c? This is the invariant the live path depends on: it SEEDS
        // a chunk's cache from exactly that GPU slot (SplatPass::
        // readChunkSurfels), so a mis-bucketed slot poisons every live patch of
        // that chunk with a neighbour's geometry.
        {
            size_t wrong = 0, checked = 0;
            int firstBad = -1;
            for (uint32_t c = 0; c < kSurfGridN * kSurfGridN * kSurfGridN; ++c) {
                const uint32_t b0 = set.chunkRange[c], b1 = set.chunkRange[c + 1];
                for (uint32_t i = b0; i < b1 && i < S.size(); ++i) {
                    ++checked;
                    const int lx = int((S[i].pos_rU.x + half) / voxel);
                    const int ly = int((S[i].pos_rU.y + half) / voxel);
                    const int lz = int((S[i].pos_rU.z + half) / voxel);
                    // chunkIndex() already divides by the chunk size.
                    if (chunkIndex(lx, ly, lz) != int(c)) {
                        if (firstBad < 0)
                            firstBad = int(c);
                        ++wrong;
                    }
                }
            }
            spdlog::info("chunk-bucket audit: {} of {} surfels in the WRONG chunk "
                         "slot{}", wrong, checked,
                         firstBad >= 0 ? std::to_string(firstBad) : std::string());
        }
        auto cellOf = [&](const Surfel& s) {
            return packKey(int((s.pos_rU.x + half) / voxel),
                           int((s.pos_rU.y + half) / voxel),
                           int((s.pos_rU.z + half) / voxel));
        };
        std::unordered_map<uint64_t, std::vector<int>> bucket;
        bucket.reserve(S.size());
        for (size_t i = 0; i < S.size(); ++i)
            bucket[cellOf(S[i])].push_back(int(i));
        const int kProbe[26][3] = {
            {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
            {-1,-1, 0},{1,-1, 0},{-1, 1, 0},{1, 1, 0},
            {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1},
            {-1, 0, 0},{1, 0, 0},{0,-1, 0},{0, 1, 0},{0, 0,-1},{0, 0, 1}};
        size_t probed = 0, floating = 0;
        int shown = 0;
        std::vector<int> byMat(kPaletteN, 0);
        for (size_t i = 0; i < S.size(); i += 53) {
            const Surfel& a = S[i];
            int x, y, z;
            unpackKey(cellOf(a), x, y, z);
            const float ra = std::max(a.pos_rU.w, a.normal_rV.w);
            ++probed;
            bool touch = false;
            // 27 probes: the 26 neighbours PLUS the cell's own bucket. A micro
            // child's parent lives in the SAME lattice cell, so omitting it
            // reports every micro as floating.
            for (int d = 0; d < 27 && !touch; ++d) {
                const int dx = d < 26 ? kProbe[d][0] : 0;
                const int dy = d < 26 ? kProbe[d][1] : 0;
                const int dz = d < 26 ? kProbe[d][2] : 0;
                const uint64_t nk = packKey(x + dx, y + dy, z + dz);
                auto it = bucket.find(nk);
                if (it == bucket.end())
                    continue;
                for (int j : it->second) {
                    if (size_t(j) == i)
                        continue;
                    const Surfel& b = S[j];
                    const float rb = std::max(b.pos_rU.w, b.normal_rV.w);
                    if (glm::distance(a.pos_rU, b.pos_rU) < ra + rb) {
                        touch = true;
                        break;
                    }
                }
            }
            if (touch)
                continue;
            ++floating;
            const int m = int(a.mat_ao.x + 0.5f);
            if (m >= 0 && m < kPaletteN)
                ++byMat[m];
            if (shown < 12) {
                ++shown;
                // Nearest other surfel: separates "parent is ABSENT" (a real
                // emitter bug) from "parent is present but just out of reach"
                // (radius tuning). Also report same-cell occupancy, which is
                // where a micro's own parent would live.
                float best = 1e9f;
                float bestR = 0.0f;
                int sameCell = 0;
                for (int d = 0; d < 27; ++d) {
                    const int dx = d < 26 ? kProbe[d][0] : 0;
                    const int dy = d < 26 ? kProbe[d][1] : 0;
                    const int dz = d < 26 ? kProbe[d][2] : 0;
                    const uint64_t nk = packKey(x + dx, y + dy, z + dz);
                    auto jt = bucket.find(nk);
                    if (jt == bucket.end())
                        continue;
                    for (int j : jt->second) {
                        if (size_t(j) == i)
                            continue;
                        if (d == 26)
                            ++sameCell;
                        const Surfel& b = S[j];
                        const float dist = glm::distance(a.pos_rU, b.pos_rU);
                        if (dist < best) {
                            best = dist;
                            bestR = std::max(b.pos_rU.w, b.normal_rV.w);
                        }
                    }
                }
                spdlog::info("  FLOATING #{} cell ({},{},{}) mat {} r {:.3f} "
                             "pos ({:.2f},{:.2f},{:.2f}) | nearest {:.3f} (its r "
                             "{:.3f}, need {:.3f}) sameCell={}",
                             shown, x, y, z, m, a.pos_rU.w, a.pos_rU.x,
                             a.pos_rU.y, a.pos_rU.z, best, bestR, ra + bestR,
                             sameCell);
            }
        }
        spdlog::info("geometric-support audit: {} unconnected of {} probed "
                     "({:.4f}%)", floating, probed,
                     probed ? 100.0 * floating / probed : 0.0, byMat.size());
        std::string mats;
        for (int m = 0; m < kPaletteN; ++m)
            if (byMat[m])
                mats += " mat" + std::to_string(m) + "=" + std::to_string(byMat[m]);
        spdlog::info("  unconnected by material:{}", mats);
    }    const auto t1 = std::chrono::steady_clock::now();
    auto ms = [](const auto& a, const auto& b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    set.buildMs = float(ms(t0, t1));
    spdlog::info("surfelize: {} surfels ({} terrain, {} object, edge {} / {}, "
                 "lod1 {} lod2 {}, dropped-floating {}), {} ms (enum+surface {} ms, "
                 "shade+bucket {} ms)",
                 set.surfels.size(), set.terrainCount, set.objectCount,
                 set.edgeBridgeCount, set.edgeParentCount, set.lod1Count,
                 set.lod2Count, set.droppedFloating, set.buildMs, ms(t0, tEnum),
                 ms(tEnum, t1));
    return set;
}

namespace {

// Candidate surface cell for the store-based live surfelizer.
//
// One CELL can own several candidates: a cell whose mean normal cancels to zero
// (a one-cell-thick plate) or a thin corner is expanded into one candidate per
// exposed face, exactly as the bake does, each carrying that face's axis normal.
// `keys` is therefore not one-per-cell, and the repeated key marks the group.
struct SurfelCand {
    uint64_t key;
    glm::vec3 pos;
    glm::vec3 n;     // final normal: smoothed, or the axis normal of a face entry
    glm::vec3 rawN;  // pre-smoothing mean normal (the bake's meanNormal rule)
    StoreCell cell;
    float aspect = 1.0f;  // 1 = isotropic; >1 stretches along `tan`
    glm::vec3 tan { 0.0f };
    // Thin-structure footprint (see thinFootprintAt): explicit radii replace
    // the crease `aspect` rule when thinTall > 0.
    float thinTall = 0.0f;
    float thinNarrow = 0.0f;
    uint8_t layer = 0; // source .vxw owner, resolved by ChunkStore provenance
    EdgeInfo edge;      // exposed-face hard-edge pairs only
    unsigned exposedFaces = 0; // lattice faces whose neighbour is air
    bool faceEntry = false;    // per-face expansion: never smoothed
};

// Store-space twins of solidDepthCells/solidRunCells: same world-space probe
// points, resolved through ChunkStore::cellAt (which is bounds-safe and
// returns air outside the lattice).
inline bool storeSolidAt(const ChunkStore& store, glm::vec3 p)
{
    const int cx = int(std::floor((p.x + 0.5f * WORLD) / VOXEL));
    const int cy = int(std::floor((p.y + 0.5f * WORLD) / VOXEL));
    const int cz = int(std::floor((p.z + 0.5f * WORLD) / VOXEL));
    return store.cellAt(cx, cy, cz).solid;
}

// Store-space neighbour pass, in the bake's order. The 6 lattice neighbours
// are found by binary search over the key-sorted candidate list, exactly as
// buildSurfels looks them up in its own key array, so one cell yields the same
// normal on both paths:
//   1. normal smoothing (the bake's `smoothNormals` block): a non-expanded cell
//      averages its own normal with the RAW normals of the face neighbours
//      that are candidates too. Per-face entries are skipped - smoothing them
//      off their voxel face is what the bake refuses to do.
//   2. the optional anisotropy tangent, from that same raw-normal bend sum.
//   3. the hard-edge mask, from the exposed faces recorded at enumeration.
//   4. the thin-footprint probe, using the final normal.
void computeCandidateAnisotropy(const ChunkStore& store,
                                std::vector<SurfelCand>& cands,
                                const SurfelParams& params)
{
    if (cands.empty())
        return;
    const int dirs[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                             { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (SurfelCand& cd : cands) {
        int x, y, z;
        unpackKey(cd.key, x, y, z);
        cd.edge = cd.cell.obj ? edgeInfoFromMask(cd.exposedFaces) : EdgeInfo {};
        if (cd.faceEntry)
            continue; // an axis-aligned face entry has nothing to smooth
        if (params.smoothNormals) {
            glm::vec3 bendSum(0.0f);
            glm::vec3 acc = cd.n;
            float wsum = 1.0f;
            for (const auto& d : dirs) {
                const uint64_t nk = packKey(x + d[0], y + d[1], z + d[2]);
                auto it = std::lower_bound(
                    cands.begin(), cands.end(), nk,
                    [](const SurfelCand& c, uint64_t k) { return c.key < k; });
                if (it == cands.end() || it->key != nk)
                    continue;
                acc += it->rawN;
                ++wsum;
                bendSum += it->rawN - cd.rawN;
            }
            const glm::vec3 avg = acc / wsum;
            // A cancellation this deep is a symmetric exposure, not a surface:
            // keep the cell's own mean normal rather than fabricate a
            // direction. A fabricated +Y is exactly what turned untouched
            // cabin walls upwards after a live edit.
            cd.n = glm::dot(avg, avg) > 1e-12f ? safeNormalize(avg) : cd.rawN;
            if (params.anisotropy)
                anisotropyFromBend(bendSum, cd.n, cd.aspect, cd.tan);
        }
        if (params.anisotropy) {
            // Same long-thin rule as the bake, so a live edit inside a reed bed
            // matches the surrounding baked geometry (probed from the CELL
            // CENTRE, never the normal-offset surfel position).
            const ThinFootprint fp = thinFootprintAt(
                [&](glm::vec3 q) { return storeSolidAt(store, q); },
                cd.pos, cd.n, x, y, z);
            if (fp.thin) {
                cd.thinTall = fp.along;
                cd.thinNarrow = fp.across;
                cd.tan = fp.axis;
            }
        }
    }
}

// Enumerate the range's surface cells and derive each one's RAW normal with
// the bake's own rule (meanNormal): the mean of the outward directions of the
// exposed faces, out-of-lattice counting as air. A cell with no exposed face is
// buried and gets no entry.
//
// The normal MUST come from this rule and not from a local SDF gradient. A live
// stamp re-derives the whole edit AABB plus a margin, so every rule difference
// between the bake and this path is applied to cells the user never touched:
// measured on the cabin, a central difference of the quantised store SDF put
// 27.8 % of its object surfels more than 30 deg off the bake (mean 22.4 deg) and
// silently turned 1191 of them into straight-up disks, because the store's
// byte-quantised nearest-sampled distance cancels to exactly zero deep inside a
// thick wall. The occupancy rule reproduces the bake to within 1.2 % / 3.8 deg
// unsmoothed, and within 0.6 % / 1.1 deg once the neighbour pass smooths it.
//
// A cell whose exposed directions cancel to zero, or a thin corner of a thin
// object, is EXPANDED into one axis-aligned candidate per exposed face, as the
// bake does. Dropping it would delete the body of every one-cell-thick stem;
// inventing a single direction would render it as one diagonal disk and leave
// the flat faces it stands for uncovered.
// --- store-side heightfield normal -----------------------------------------
// The BAKE blends every terrain-top normal 0.55 of the way toward a two-scale
// gradient of the height TEXTURE, i.e. of a bilinearly interpolated FLOAT top
// field. The store path had no such blend, so it shaded terrain tops from
// exposed faces alone. That is a parity break in the direction this file's
// parity rule cares about (the live path must equal the bake): an identical
// slope shades smooth after a bake and faceted after a live edit, and a Smooth
// stamp - whose whole purpose is to fair the surface - produced geometry the
// normal estimator could not describe at all.
//
// The store has no height texture, so the top is found by walking DOWN from a
// hint cell to the first solid, non-object cell. Bounded, because the two-scale
// gradient only samples within +-0.35 m (4 cells) of the surfel, and the caller
// passes the surfel's own cell as the hint.
constexpr float kStoreTopMissing = -1e8f;

float storeTopAt(const ChunkStore& store, float wx, float wz, int hintY)
{
    const int n = store.latN();
    if (n <= 0)
        return kStoreTopMissing;
    const int cx = std::clamp(int(std::floor((wx + 0.5f * WORLD) / VOXEL)), 0, n - 1);
    const int cz = std::clamp(int(std::floor((wz + 0.5f * WORLD) / VOXEL)), 0, n - 1);
    const int yHi = std::min(n - 1, hintY + 2);
    const int yLo = std::max(0, hintY - 18);
    for (int y = yHi; y >= yLo; --y) {
        const StoreCell c = store.cellAt(cx, y, cz);
        if (c.solid && !c.obj)
            return (float(y) + 0.5f) * VOXEL - 0.5f * WORLD;
    }
    return kStoreTopMissing; // no terrain in the window
}

// Exactly the bake's two-scale construction (e = 0.35 m wide, 0.10 m fine,
// mixed 0.55), so the two paths agree on the same surface. Returns a zero
// vector when any tap is missing, which tells the caller to keep the face
// normal rather than blend toward a partial field.
glm::vec3 storeHeightfieldNormal(const ChunkStore& store, glm::vec3 p, int hintY)
{
    const float e = 0.35f, e2 = 0.10f;
    auto H = [&](float dx, float dz) {
        return storeTopAt(store, p.x + dx, p.z + dz, hintY);
    };
    const float wx0 = H(-e, 0.f), wx1 = H(e, 0.f);
    const float wz0 = H(0.f, -e), wz1 = H(0.f, e);
    const float fx0 = H(-e2, 0.f), fx1 = H(e2, 0.f);
    const float fz0 = H(0.f, -e2), fz1 = H(0.f, e2);
    if (wx0 <= kStoreTopMissing || wx1 <= kStoreTopMissing ||
        wz0 <= kStoreTopMissing || wz1 <= kStoreTopMissing ||
        fx0 <= kStoreTopMissing || fx1 <= kStoreTopMissing ||
        fz0 <= kStoreTopMissing || fz1 <= kStoreTopMissing)
        return glm::vec3(0.0f);
    const glm::vec3 nWide = safeNormalize(glm::vec3(wx0 - wx1, 2.0f * e, wz0 - wz1));
    const glm::vec3 nFine = safeNormalize(glm::vec3(fx0 - fx1, 2.0f * e2, fz0 - fz1));
    return safeNormalize(glm::mix(nFine, nWide, 0.55f));
}

std::vector<SurfelCand> collectChunkCandidates(const ChunkStore& store, int chunk,
                                               glm::ivec3 lo, glm::ivec3 hi,
                                               const SurfelParams& params)
{
    std::vector<SurfelCand> cands;
    if (chunk < 0 || chunk >= kChunkCount)
        return cands;
    int ccx, ccy, ccz;
    chunkCoordsOf(chunk, ccx, ccy, ccz);
    const int x0 = std::max(ccx * CHUNK_N, lo.x), x1 = std::min(ccx * CHUNK_N + CHUNK_N, hi.x);
    const int y0 = std::max(ccy * CHUNK_N, lo.y), y1 = std::min(ccy * CHUNK_N + CHUNK_N, hi.y);
    const int z0 = std::max(ccz * CHUNK_N, lo.z), z1 = std::min(ccz * CHUNK_N + CHUNK_N, hi.z);
    const int latN = store.latN();
    const int dirs[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                             { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
    for (int z = z0; z < z1; ++z)
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const StoreCell c = store.cellAt(x, y, z);
                // raw == 0 counts as solid here: the SVO DDA tests `sdf <= 0`
                if (c.sdfRaw > 0)
                    continue;
                const glm::vec3 wp(-51.2f + (x + 0.5f) * VOXEL,
                                   -51.2f + (y + 0.5f) * VOXEL,
                                   -51.2f + (z + 0.5f) * VOXEL);
                unsigned exposed = 0;
                glm::vec3 raw(0.0f);
                for (int d = 0; d < 6; ++d) {
                    const int nx = x + dirs[d][0], ny = y + dirs[d][1],
                              nz = z + dirs[d][2];
                    const bool air = nx < 0 || nx >= latN || ny < 0 ||
                                     ny >= latN || nz < 0 || nz >= latN ||
                                     store.cellAt(nx, ny, nz).sdfRaw > 0;
                    if (!air)
                        continue;
                    exposed |= 1u << d;
                    raw += glm::vec3(float(dirs[d][0]), float(dirs[d][1]),
                                     float(dirs[d][2]));
                }
                if (exposed == 0)
                    continue; // buried: no face is visible from outside
                const VoxelField::Sample provenance = store.sample(x, y, z);
                SurfelCand base;
                base.key = packKey(x, y, z);
                base.pos = wp;
                base.cell = c;
                base.exposedFaces = exposed;
                base.layer = provenance.obj ? provenance.layer : uint8_t(0);
                base.rawN = safeNormalize(raw); // a cancelling sum stays zero
                const bool degenerate = glm::dot(raw, raw) < 1e-6f;
                bool thinCorner = false;
                if (!degenerate && c.obj) {
                    const glm::vec3 an = glm::abs(base.rawN);
                    const int nonZero = (an.x > 1e-3f) + (an.y > 1e-3f) +
                                        (an.z > 1e-3f);
                    thinCorner = nonZero >= 2 &&
                                 thinCellAt(
                                     [&](glm::vec3 q) {
                                         return storeSolidAt(store, q);
                                     },
                                     wp);
                }
                if (degenerate || thinCorner) {
                    for (int d = 0; d < 6; ++d) {
                        if (!(exposed & (1u << d)))
                            continue;
                        SurfelCand face = base;
                        face.n = glm::vec3(float(dirs[d][0]), float(dirs[d][1]),
                                           float(dirs[d][2]));
                        face.faceEntry = true;
                        cands.push_back(face);
                    }
                    continue;
                }
                base.n = base.rawN;
                // Terrain tops blend toward the store's own two-scale
                // heightfield gradient, mirroring the bake's
                // terrainHeightfieldNormals block. Object cells are excluded:
                // their surfaces are not a height field, and the bake excludes
                // them too (it keys on `y == colTop`).
                if (params.terrainHeightfieldNormals && !c.obj &&
                    (exposed & (1u << 2))) { // air directly above
                    const glm::vec3 hn = storeHeightfieldNormal(store, wp, y);
                    if (glm::dot(hn, hn) > 1e-6f)
                        base.n = safeNormalize(
                            glm::mix(base.n, hn, params.heightfieldBlend));
                    // rawN is deliberately LEFT UNBLENDED: the bake applies the
                    // heightfield mix to `n` but its smoothing stage reads the
                    // separate `rawNormals[]`, so blending rawN here would make
                    // the store path disagree with the bake on the very stage
                    // the parity rule is about.
                }
                cands.push_back(base);
            }
    // Key-sorted, stable: the neighbour pass binary-searches this order, and a
    // cell's face entries must stay adjacent so one key lookup finds the group.
    std::stable_sort(cands.begin(), cands.end(),
                     [](const SurfelCand& a, const SurfelCand& b) {
                         return a.key < b.key;
                     });
    return cands;
}

void shadeCandidates(const ChunkStore& store, const std::vector<SurfelCand>& cands,
                     const SurfelParams& params, std::vector<Surfel>& out,
                     size_t begin, size_t end)
{
    const glm::vec3 sunDir = glm::normalize(params.sunDir);
    const float baseR = params.baseRadius;
    for (size_t i = begin; i < end; ++i) {
        const SurfelCand& cd = cands[i];
        const glm::vec3 pos = cd.pos + cd.n * (0.5f * VOXEL);
        float shadow = 1.0f;
        if (glm::dot(cd.n, sunDir) > 0.02f)
            shadow = shadowMarch(store, pos + cd.n * 0.3f, sunDir);
        float ao = 1.0f;
        glm::vec3 bent = cd.n;
        aoBake(store, pos + cd.n * 0.02f, cd.n, ao, bent);

        const uint8_t mat = cd.cell.mat;
        const float refl = kMaterialReflection[std::min(int(mat), kPaletteN - 1)].x;
        const float rough = kMaterialReflection[std::min(int(mat), kPaletteN - 1)].y;
        // Same foliage-growth rule as the bake: terrain canopy grows to seal
        // sparse coverage, object foliage keeps the base radius so clusters
        // stay separate and the branches between them stay visible.
        const float foliageGrow = (mat == 8 && !cd.cell.obj) ? 2.0f : 1.0f;
        // Match the full bake: only genuine hard-edge, non-thin opaque object
        // parents tighten. Smooth curvature and explicit thin footprints keep
        // their coverage radius.
        const bool hardEdge =
            cd.cell.obj && mat != 8 && !(mat >= 9 && mat <= 15) &&
            cd.edge.pairCount > 0 && cd.thinTall <= 0.0f;
        const float edgeFactor =
            hardEdge ? 1.0f - glm::clamp(params.edgeShrink, 0.0f, 1.0f)
                     : 1.0f;
        const float r = baseR * foliageGrow * edgeFactor;
        // Long-thin structures carry explicit radii (tall and narrow); the
        // crease rule only applies when they are absent. Hard-edge parents
        // stay isotropic; their separate bridges carry the elongation.
        float rU = r, rV = r;
        glm::vec3 tanOut(0.0f);
        if (cd.thinTall > 0.0f) {
            rU = cd.thinTall;
            rV = cd.thinNarrow;
            tanOut = cd.tan;
        } else if (!hardEdge && params.anisotropy && cd.aspect > 1.0f) {
            rU = r * cd.aspect;
            tanOut = cd.tan;
        }
        Surfel sl;
        sl.pos_rU = glm::vec4(pos, rU);
        sl.normal_rV = glm::vec4(cd.n, rV);
        sl.bent_sh = glm::vec4(bent, shadow);
        sl.mat_ao = glm::vec4(float(mat), refl, rough, packSurfelAo(ao, cd.layer));
        sl.tan_aspect = glm::vec4(tanOut, float(cd.cell.tags));
        out[i] = sl;
    }
}

void appendCandidateEdgeBridges(const std::vector<SurfelCand>& cands,
                                const std::vector<Surfel>& base,
                                const SurfelParams& params,
                                std::vector<uint64_t>& edgeKeys,
                                std::vector<Surfel>& edges)
{
    if (!params.edgeFill || params.edgeShrink <= 0.0f)
        return;
    for (size_t i = 0; i < cands.size(); ++i) {
        const SurfelCand& cd = cands[i];
        if (!cd.cell.obj || (cd.edge.pairCount <= 0 && cd.edge.cornerCount <= 0))
            continue;
        const int mat = int(base[i].mat_ao.x + 0.5f);
        if (mat == 8 || (mat >= 9 && mat <= 15))
            continue;
        const size_t before = edges.size();
        appendEdgeBridges(cd.pos, base[i], cd.edge, params.baseRadius,
                          params.edgeBridgeSize, edges);
        if (params.cornerFill)
            appendCornerCaps(cd.pos, base[i], cd.edge, params.baseRadius,
                             params.edgeBridgeSize, edges);
        for (size_t j = before; j < edges.size(); ++j)
            edgeKeys.push_back(cd.key);
    }
}

} // namespace

namespace {
inline void chunkBounds(int chunk, glm::ivec3& lo, glm::ivec3& hi)
{
    int cx, cy, cz;
    chunkCoordsOf(chunk, cx, cy, cz);
    lo = glm::ivec3(cx * CHUNK_N, cy * CHUNK_N, cz * CHUNK_N);
    hi = lo + glm::ivec3(CHUNK_N);
}
} // namespace

SurfelRange buildChunkSurfelsRange(const ChunkStore& store, int chunk, glm::ivec3 lo,
                                   glm::ivec3 hi, const SurfelParams& params)
{
    SurfelRange out;
    std::vector<SurfelCand> cands = collectChunkCandidates(store, chunk, lo, hi, params);
    out.keys.reserve(cands.size());
    for (const SurfelCand& c : cands)
        out.keys.push_back(c.key);
    computeCandidateAnisotropy(store, cands, params);
    out.surfels.resize(cands.size());
    shadeCandidates(store, cands, params, out.surfels, 0, cands.size());
    appendCandidateEdgeBridges(cands, out.surfels, params,
                               out.edgeKeys, out.edgeSurfels);
    return out;
}

std::vector<Surfel> buildChunkSurfels(const ChunkStore& store, int chunk,
                                      const SurfelParams& params)
{
    glm::ivec3 lo, hi;
    chunkBounds(chunk, lo, hi);
    return buildChunkSurfelsRange(store, chunk, lo, hi, params).surfels;
}

std::vector<std::vector<Surfel>> buildChunksSurfels(
    const ChunkStore& store, const std::vector<int>& chunks,
    const SurfelParams& params)
{
    std::vector<std::vector<Surfel>> out(chunks.size());
    if (chunks.empty())
        return out;

    // Phase 1: enumerate candidates (read-only store) in parallel per chunk.
    std::vector<std::vector<SurfelCand>> cands(chunks.size());
    {
        std::atomic<size_t> next{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        hc = std::min<unsigned>(hc, unsigned(chunks.size()));
        std::vector<std::thread> threads;
        threads.reserve(hc);
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= chunks.size())
                        return;
                    glm::ivec3 lo, hi;
                    chunkBounds(chunks[i], lo, hi);
                    cands[i] = collectChunkCandidates(store, chunks[i], lo, hi, params);
                    out[i].resize(cands[i].size());
                }
            });
        for (auto& th : threads)
            th.join();
    }

    // Phase 1.5: per-chunk neighbour pass (normal smoothing + edge mask +
    // anisotropy, same rules as the bake). Cheap and sequential; the list is
    // already key-sorted per chunk.
    for (size_t c = 0; c < cands.size(); ++c)
        computeCandidateAnisotropy(store, cands[c], params);

    // Phase 2: shade a flat (chunk, slice) task list: no nested pools, every
    // index writes exactly one output element (deterministic per chunk).
    struct Task {
        uint32_t chunk;
        uint32_t begin, end;
    };
    std::vector<Task> tasks;
    constexpr size_t kSlice = 256;
    for (uint32_t c = 0; c < chunks.size(); ++c)
        for (size_t b = 0; b < cands[c].size(); b += kSlice)
            tasks.push_back({ c, uint32_t(b),
                              uint32_t(std::min(cands[c].size(), b + kSlice)) });
    std::atomic<size_t> next{ 0 };
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    hc = std::min<unsigned>(hc, unsigned(tasks.size()));
    std::vector<std::thread> threads;
    threads.reserve(hc);
    for (unsigned t = 0; t < hc; ++t)
        threads.emplace_back([&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= tasks.size())
                    return;
                const Task& tk = tasks[i];
                shadeCandidates(store, cands[tk.chunk], params, out[tk.chunk],
                                tk.begin, tk.end);
            }
        });
    for (auto& th : threads)
        th.join();
    return out;
}

Surfel makeWaterSurfel(float wx, float wz, float spacing)
{
    Surfel s;
    const float r = spacing * 0.9f; // overlap for watertight cover
    s.pos_rU = glm::vec4(wx, WATER_LEVEL, wz, r);
    s.normal_rV = glm::vec4(0.0f, 1.0f, 0.0f, r);
    s.bent_sh = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); // unshadowed water
    s.mat_ao = glm::vec4(0.0f, 40.0f, 200.0f, 3.0f); // ao=1 +2 = water
    s.tan_aspect = glm::vec4(0.0f);                  // isotropic plane splat
    return s;
}

std::vector<Surfel> buildWaterSurfels(float spacing)
{
    std::vector<Surfel> out;
    if (spacing <= 0.0f)
        return out;
    const float half = 0.5f * WORLD;
    const int steps = int(WORLD / spacing);
    out.reserve(size_t(steps + 1) * size_t(steps + 1));
    for (int j = 0; j <= steps; ++j)
        for (int i = 0; i <= steps; ++i)
            out.push_back(makeWaterSurfel(-half + (i + 0.5f) * spacing,
                                          -half + (j + 0.5f) * spacing, spacing));
    return out;
}

std::vector<Surfel> buildMicroSurfels(const std::vector<uint64_t>& keys,
                                      const std::vector<Surfel>& base,
                                      const uint8_t* obj, size_t* objCount)
{
    std::vector<Surfel> micros;
    if (keys.size() != base.size() || base.empty())
        return micros;
    micros.reserve(base.size() / 2);
    for (size_t i = 0; i < base.size(); ++i) {
        // A face-expanded cell contributes several entries under one key; the
        // bake emits ONE micro set for it, so skip the repeats. Keys are
        // key-sorted, so equal keys are adjacent.
        if (i > 0 && keys[i] == keys[i - 1])
            continue;
        int x, y, z;
        unpackKey(keys[i], x, y, z);
        const size_t before = micros.size();
        emitMicroSurfelsForCell(x, y, z, base[i], micros);
        if (objCount && obj && obj[i] && micros.size() > before)
            *objCount += micros.size() - before;
    }
    return micros;
}

} // namespace vf::voxel
