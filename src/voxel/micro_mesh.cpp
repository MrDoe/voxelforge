// micro_mesh.cpp - mesh + skin deformation for micro objects. See the header
// for WHY this replaces the rest-field-through-inverse-transform design.
#include "voxel/micro_mesh.hpp"

#include "voxel/common.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/world.hpp" // BRICK_N, for the dirty-brick set

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace vf::voxel {
namespace {

float distToSegment(glm::vec3 p, glm::vec3 a, glm::vec3 b)
{
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 <= 1e-12f)
        return glm::length(p - a);
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.f, 1.f);
    return glm::length(p - (a + t * ab));
}

// Möller–Trumbore, no culling: parity does not care which way a triangle
// faces, only whether the ray crosses it. Returns false on a miss; `tOut` is
// the distance along `rd` (not required to be unit length).
bool rayTriangleT(glm::vec3 ro, glm::vec3 rd, const MeshTri& t, float& tOut)
{
    const glm::vec3 e1 = t.v[1] - t.v[0], e2 = t.v[2] - t.v[0];
    const glm::vec3 p = glm::cross(rd, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f)
        return false;
    const float inv = 1.f / det;
    const glm::vec3 tv = ro - t.v[0];
    const float u = glm::dot(tv, p) * inv;
    if (u < 0.f || u > 1.f)
        return false;
    const glm::vec3 q = glm::cross(tv, e1);
    const float v = glm::dot(rd, q) * inv;
    if (v < 0.f || u + v > 1.f)
        return false;
    const float tt = glm::dot(e2, q) * inv;
    if (tt <= 0.f)
        return false;
    tOut = tt;
    return true;
}

// The nearest REST solid cell to a point in the rest frame, searched out to
// two cells. A cell that GREW during the pose can land where the rest body
// never had a surface, so a miss is expected - the caller keeps a fallback
// material rather than a hole of palette-0 albedo. The scan order is part of
// the result: a full rebuild and a delta update must pick the same rest cell
// for the same point, or the appearance of a newly solid cell would depend on
// which path built it.
size_t nearestRestSolid(const MicroField& rest, glm::vec3 rp)
{
    const glm::ivec3 c =
        glm::ivec3(glm::floor((rp - rest.origin) / rest.voxel));
    for (int r = 0; r <= 2; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const glm::ivec3 q = c + glm::ivec3(dx, dy, dz);
                    if (!rest.inside(q.x, q.y, q.z))
                        continue;
                    const size_t k = rest.idx(q.x, q.y, q.z);
                    if (rest.solid[k])
                        return k;
                }
    return SIZE_MAX;
}

// ONE triangle's pose. The delta deformer re-poses only the triangles a moved
// bone touches, and it must land on EXACTLY the vertices a full pose would:
// a triangle whose posed position differs by one ulp from the full path's
// would leave a one-cell seam in the parity delta that never heals. So both
// paths call this.
void poseTri(const MicroMesh& mesh, size_t t,
             const std::vector<glm::mat3>& boneRot,
             const std::vector<glm::vec3>& boneXform, MeshTri& out)
{
    out = mesh.tris[t];
    for (int k = 0; k < 3; ++k) {
        const size_t vi = t * 3 + size_t(k);
        const int32_t b = vi < mesh.bone.size() ? mesh.bone[vi] : -1;
        if (b >= 0 && size_t(b) < boneRot.size() &&
            size_t(b) < boneXform.size())
            out.v[k] = boneRot[size_t(b)] * mesh.rest[vi] +
                       boneXform[size_t(b)];
        else
            out.v[k] = mesh.rest[vi];
    }
}

} // namespace

bool loadMicroMesh(const std::string& meshPath, const std::string& skinPath,
                   MicroMesh& out, std::string* err)
{
    auto fail = [&](const std::string& msg) {
        if (err)
            *err = msg;
        return false;
    };
    out = MicroMesh {};

    std::string readErr;
    if (!readMeshFile(meshPath, out.tris, /*defaultMat*/ 4, readErr))
        return fail("micro mesh: " + readErr);
    if (out.tris.empty())
        return fail("micro mesh: no triangles in " + meshPath);

    // The STL reader emits one triangle per facet and no shared vertices, so
    // the vertex list IS the triangle corner list. Building it once keeps the
    // skin's vertex indices and the triangle corners in one order.
    out.rest.reserve(out.tris.size() * 3);
    for (size_t t = 0; t < out.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
            out.rest.push_back(out.tris[t].v[k]);
    out.bone.assign(out.rest.size(), -1);

    // FRAME. A micro mesh is a side-car of a LAYER: the layer's records start
    // at the object's min corner and so must the mesh. Exporters that write
    // raw lattice indices (the alpaca's STL starts at cell (5,0,8), because
    // the voxel cells it was extracted from were tagged in lattice space)
    // leave a constant shift that clips one side of the surface against the
    // field's grid and lets the interior flood fill escape. Normalise once
    // here so every consumer works in the one frame the records use.
    {
        glm::vec3 mn(std::numeric_limits<float>::max());
        for (const glm::vec3& v : out.rest)
            mn = glm::min(mn, v);
        if (glm::length(mn) > 1e-6f) {
            out.restOffset = -mn;
            for (glm::vec3& v : out.rest)
                v += out.restOffset;
            for (MeshTri& t : out.tris)
                for (int k = 0; k < 3; ++k)
                    t.v[k] += out.restOffset;
        }
    }

    // A missing skin is legal: a rigid object is a mesh with no bones.
    std::ifstream in(skinPath);
    if (!in) {
        out.loaded = true;
        return true;
    }
    std::string line;
    size_t declaredVerts = 0, declaredBones = 0;
    bool haveCounts = false;
    std::vector<std::pair<size_t, int>> pairs;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line[0] == '#') {
            // "# bones: spine leg0 ..." - names only, for diagnostics. They
            // are matched to the rig by INDEX, exactly like the skin's bone
            // ids (the cache and the skin are written from the same rig).
            const std::string tag = "# bones:";
            if (line.compare(0, tag.size(), tag) == 0) {
                std::istringstream ls(line.substr(tag.size()));
                std::string name;
                while (ls >> name)
                    out.boneNames.push_back(name);
            }
            continue;
        }
        std::istringstream ls(line);
        if (!haveCounts) {
            if (!(ls >> declaredVerts >> declaredBones))
                return fail("micro skin: expected '<vertexCount> <boneCount>' "
                            "in " + skinPath);
            haveCounts = true;
            continue;
        }
        long long vi = -1;
        int bi = -1;
        if (!(ls >> vi >> bi))
            return fail("micro skin: bad line '" + line + "' in " + skinPath);
        if (vi < 0)
            return fail("micro skin: negative vertex index in " + skinPath);
        pairs.emplace_back(size_t(vi), bi);
    }
    if (!haveCounts)
        return fail("micro skin: no header in " + skinPath);

    // A count mismatch is LOUD on purpose. Silently skinning the first N
    // vertices would deform the wrong parts of the mesh, and that looks like a
    // rig bug rather than a stale side-car.
    if (declaredVerts != out.rest.size())
        return fail("micro skin: declares " + std::to_string(declaredVerts) +
                    " vertices but the mesh has " +
                    std::to_string(out.rest.size()) +
                    " (stale side-car? regenerate the skin)");
    for (const auto& pr : pairs) {
        if (pr.first >= out.bone.size())
            return fail("micro skin: vertex index out of range in " + skinPath);
        out.bone[pr.first] = pr.second;
    }
    if (declaredBones > 64)
        return fail("micro skin: implausible bone count");
    out.loaded = true;
    return true;
}

void poseMicroMesh(const MicroMesh& mesh, const std::vector<glm::mat3>& boneRot,
                   const std::vector<glm::vec3>& boneXform,
                   std::vector<MeshTri>& posed)
{
    posed.resize(mesh.tris.size());
    for (size_t t = 0; t < mesh.tris.size(); ++t)
        poseTri(mesh, t, boneRot, boneXform, posed[t]);
}

bool voxelizeMicroMesh(const std::vector<MeshTri>& posed, glm::ivec3 dim,
                       float voxel, glm::vec3 origin, MicroMeshGrid& out,
                       std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    out = MicroMeshGrid {};
    if (posed.empty())
        return fail("voxelizeMicroMesh: no triangles");
    if (dim.x <= 0 || dim.y <= 0 || dim.z <= 0)
        return fail("voxelizeMicroMesh: empty grid");
    if (!(voxel > 0.f) || !std::isfinite(voxel))
        return fail("voxelizeMicroMesh: voxel must be positive and finite");

    out.dim = dim;
    out.voxel = voxel;
    out.origin = origin;
    const size_t n = out.size3();
    if (n > kMicroMaxCells * 4)
        return fail("voxelizeMicroMesh: grid over the cell budget");

    // ---- solidity: crossing PARITY along +x, one scanline per (y,z) row ----
    //
    // Winding is author data, and it is routinely inconsistent: the alpaca's
    // own side-car - generated from the voxels, so it should be perfect - has
    // 26 055 of its 117 392 shared edges traversed in the SAME direction by
    // both adjacent triangles, i.e. about a third of the surface is inverted.
    // Any "which side does the normal face" rule therefore marks a third of
    // the surface cells on the wrong side; the shell grows one-cell holes and
    // the interior flood fill escapes through them (measured with the
    // normal-sign rule: 15.7 % rest agreement, 12 328 cells solid in air).
    //
    // Parity does not look at the winding at all: a closed surface is crossed
    // an odd number of times from a point inside it. The ray runs along +x
    // with a small off-axis jitter so it cannot graze an edge or a vertex, and
    // because a row is shared by every cell along it the whole grid costs one
    // pass instead of one ray per cell.
    //
    // A cell's parity says which side of the SURFACE it is on, which is not
    // quite the same as "inside the body" once bones interpenetrate (a posed
    // leg can enter the belly): the even-odd rule turns such an overlap into
    // an enclosed air pocket. The fill below closes exactly those pockets,
    // because none of them is reachable from the grid border.
    const glm::vec3 rayDir = glm::normalize(glm::vec3(1.f, 1e-4f, 2e-4f));
    std::vector<uint8_t> solid(n, 0);
    {
        // Per-(y,z) row buckets: the +x ray at a fixed (y,z) can only meet a
        // triangle whose (y,z) AABB contains it.
        const int rows = dim.y * dim.z;
        std::vector<uint32_t> rowCounts(size_t(rows) + 1, 0);
        std::vector<glm::ivec2> lo2(posed.size()), hi2(posed.size());
        auto cellOf = [&](float p, int axis) {
            return int(glm::floor((p - origin[axis]) / voxel));
        };
        for (size_t t = 0; t < posed.size(); ++t) {
            glm::vec3 mn(std::numeric_limits<float>::max()), mx(-mn);
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) {
                    mn[a] = std::min(mn[a], posed[t].v[k][a]);
                    mx[a] = std::max(mx[a], posed[t].v[k][a]);
                }
            lo2[t] = glm::ivec2(std::max(0, cellOf(mn.y, 1) - 1),
                                std::max(0, cellOf(mn.z, 2) - 1));
            hi2[t] = glm::ivec2(std::min(dim.y - 1, cellOf(mx.y, 1) + 1),
                                std::min(dim.z - 1, cellOf(mx.z, 2) + 1));
            hi2[t].x = std::max(hi2[t].x, lo2[t].x);
            hi2[t].y = std::max(hi2[t].y, lo2[t].y);
            for (int iy = lo2[t].x; iy <= hi2[t].x; ++iy)
                for (int iz = lo2[t].y; iz <= hi2[t].y; ++iz)
                    ++rowCounts[size_t(iy) * dim.z + iz + 1];
        }
        for (int i = 0; i < rows; ++i)
            rowCounts[size_t(i) + 1] += rowCounts[size_t(i)];
        std::vector<uint32_t> rowItems(rowCounts[rows], 0);
        {
            std::vector<uint32_t> cur(rowCounts.begin(), rowCounts.end() - 1);
            for (size_t t = 0; t < posed.size(); ++t)
                for (int iy = lo2[t].x; iy <= hi2[t].x; ++iy)
                    for (int iz = lo2[t].y; iz <= hi2[t].y; ++iz)
                        rowItems[cur[size_t(iy) * dim.z + iz]++] = uint32_t(t);
        }

        std::vector<float> crossings;
        crossings.reserve(64);
        for (int iz = 0; iz < dim.z; ++iz)
            for (int iy = 0; iy < dim.y; ++iy) {
                const size_t row = size_t(iy) * dim.z + iz;
                const glm::vec3 ro(
                    origin.x, origin.y + (float(iy) + 0.5f) * voxel,
                    origin.z + (float(iz) + 0.5f) * voxel);
                crossings.clear();
                for (uint32_t k = rowCounts[row]; k < rowCounts[row + 1]; ++k) {
                    const MeshTri& t = posed[rowItems[k]];
                    float tt = 0.f;
                    if (triArea2(t) > 1e-16f &&
                        rayTriangleT(ro, rayDir, t, tt))
                        crossings.push_back(ro.x + tt * rayDir.x);
                }
                std::sort(crossings.begin(), crossings.end());
                size_t k = 0;
                for (int ix = 0; ix < dim.x; ++ix) {
                    const float xc = origin.x + (float(ix) + 0.5f) * voxel;
                    while (k < crossings.size() && crossings[k] < xc)
                        ++k;
                    if (k & 1)
                        solid[out.idx(ix, iy, iz)] = 1;
                }
            }
    }

    // ---- the fill closes ENCLOSED air (posed self-overlap) ----------------
    // Air that the border cannot reach is not "outside the body": the only
    // way a pocket like that forms is two surfaces overlapping (a bone
    // rotated into the body) or an authored hollow. Both are solid for a
    // body, so seal them. This is also what makes the result independent of
    // the even-odd rule's behaviour at an intersection.
    //
    // The result BEFORE the fill is kept as `raw`: it is the only state from
    // which a delta can be applied, because the fill itself is global (a
    // sealed pocket cannot be told from authored air by looking at a cell).
    out.raw = solid;
    {
        std::vector<uint8_t> ext(n, 0);
        std::vector<uint32_t> q;
        q.reserve(n / 4 + 16);
        auto push = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y ||
                z >= dim.z)
                return;
            const size_t i = out.idx(x, y, z);
            if (ext[i] || solid[i])
                return;
            ext[i] = 1;
            q.push_back(uint32_t(i));
        };
        for (int z = 0; z < dim.z; ++z)
            for (int y = 0; y < dim.y; ++y) {
                push(0, y, z);
                push(dim.x - 1, y, z);
            }
        for (int z = 0; z < dim.z; ++z)
            for (int x = 0; x < dim.x; ++x) {
                push(x, 0, z);
                push(x, dim.y - 1, z);
            }
        for (int y = 0; y < dim.y; ++y)
            for (int x = 0; x < dim.x; ++x) {
                push(x, y, 0);
                push(x, y, dim.z - 1);
            }
        for (size_t head = 0; head < q.size(); ++head) {
            const uint32_t i = q[head];
            const int x = int(i % uint32_t(dim.x));
            const int y = int((i / uint32_t(dim.x)) % uint32_t(dim.y));
            const int z = int(i / (uint32_t(dim.x) * uint32_t(dim.y)));
            push(x + 1, y, z);
            push(x - 1, y, z);
            push(x, y + 1, z);
            push(x, y - 1, z);
            push(x, y, z + 1);
            push(x, y, z - 1);
        }
        for (size_t i = 0; i < n; ++i)
            if (!solid[i] && !ext[i])
                solid[i] = 1; // an enclosed pocket
    }

    // ---- stats: the surface LAYER and the interior are both reported ------
    out.solid = std::move(solid);
    out.bone.assign(n, -1);
    out.shellCells = 0;
    out.interiorCells = 0;
    for (int z = 0; z < dim.z; ++z)
        for (int y = 0; y < dim.y; ++y)
            for (int x = 0; x < dim.x; ++x) {
                const size_t i = out.idx(x, y, z);
                if (!out.solid[i])
                    continue;
                bool exposed = false;
                for (int k = 0; k < 6 && !exposed; ++k) {
                    static const int dx[6] = { 1, -1, 0, 0, 0, 0 };
                    static const int dy[6] = { 0, 0, 1, -1, 0, 0 };
                    static const int dz[6] = { 0, 0, 0, 0, 1, -1 };
                    const int nx = x + dx[k], ny = y + dy[k], nz = z + dz[k];
                    exposed = !out.inside(nx, ny, nz) ||
                              !out.solid[out.idx(nx, ny, nz)];
                }
                if (exposed)
                    ++out.shellCells;
                else
                    ++out.interiorCells;
            }
    // `leak` now means "the mesh enclosed nothing on this grid", which is the
    // only failure the fill cannot repair (the surface fell outside the box).
    out.leak = out.shellCells == 0;
    return true;
}

int microBoneAtPosed(const MicroRig& rig, glm::vec3 p,
                     const std::vector<glm::mat3>& boneRot,
                     const std::vector<glm::vec3>& boneXform)
{
    int best = -1;
    float bestD = std::numeric_limits<float>::max();
    for (size_t b = 0; b < rig.bones.size(); ++b) {
        const MicroBone& bone = rig.bones[b];
        // Un-bone the POSED point with THIS bone's transform, then measure
        // against THIS bone's rest segment. A bone that does not own the point
        // maps it somewhere far from its own segment, which is exactly the
        // discrimination the shader's rule uses.
        glm::vec3 rest = p;
        if (b < boneRot.size() && b < boneXform.size())
            rest = glm::transpose(boneRot[b]) * (p - boneXform[b]);
        const float d = distToSegment(rest, bone.a, bone.b);
        if (d < bestD) {
            bestD = d;
            best = int(b);
        }
    }
    return best;
}

void assignMicroMeshBones(MicroMeshGrid& grid, const MicroRig& rig,
                          const std::vector<glm::mat3>& boneRot,
                          const std::vector<glm::vec3>& boneXform)
{
    if (grid.bone.size() != grid.size3())
        grid.bone.assign(grid.size3(), -1);
    if (rig.bones.empty())
        return;
    for (int z = 0; z < grid.dim.z; ++z)
        for (int y = 0; y < grid.dim.y; ++y)
            for (int x = 0; x < grid.dim.x; ++x) {
                const size_t i = grid.idx(x, y, z);
                if (!grid.solid[i])
                    continue;
                const glm::vec3 p =
                    grid.origin +
                    (glm::vec3(float(x), float(y), float(z)) + 0.5f) *
                        grid.voxel;
                grid.bone[i] = microBoneAtPosed(rig, p, boneRot, boneXform);
            }
}

bool buildMicroFieldFromMesh(const MicroMeshGrid& grid,
                             const std::vector<glm::mat3>& boneRot,
                             const std::vector<glm::vec3>& boneXform,
                             const MicroField& rest, MicroField& out,
                             MeshFieldStats* stats, std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (grid.size3() == 0)
        return fail("buildMicroFieldFromMesh: empty grid");
    if (grid.solid.size() != grid.size3())
        return fail("buildMicroFieldFromMesh: occupancy does not match the grid");
    if (rest.size3() == 0)
        return fail("buildMicroFieldFromMesh: no rest field to transfer from");

    out = MicroField {};
    out.voxel = grid.voxel;
    out.dim = grid.dim;
    out.origin = grid.origin;
    const size_t n = grid.size3();
    out.solid = grid.solid;
    out.mat.assign(n, 4); // rock: the STL's own material, a safe fallback
    out.r.assign(n, 0);
    out.g.assign(n, 0);
    out.b.assign(n, 0);
    out.refl.assign(n, 0);
    out.rough.assign(n, 0);
    out.tex.assign(n, 0);

    MeshFieldStats st;
    st.solidCells = 0;
    for (int z = 0; z < grid.dim.z; ++z)
        for (int y = 0; y < grid.dim.y; ++y)
            for (int x = 0; x < grid.dim.x; ++x) {
                const size_t i = grid.idx(x, y, z);
                if (!grid.solid[i])
                    continue;
                ++st.solidCells;
                const glm::vec3 p =
                    grid.origin +
                    (glm::vec3(float(x), float(y), float(z)) + 0.5f) *
                        grid.voxel;
                const int32_t b = grid.bone.size() == n ? grid.bone[i] : -1;
                glm::vec3 rp = p;
                if (b >= 0 && size_t(b) < boneRot.size() &&
                    size_t(b) < boneXform.size())
                    rp = glm::transpose(boneRot[size_t(b)]) *
                         (p - boneXform[size_t(b)]);

                // Nearest REST solid within 2 cells.
                const size_t ri = nearestRestSolid(rest, rp);
                if (ri == SIZE_MAX) {
                    ++st.fallbacks;
                    continue;
                }
                ++st.transferred;
                out.mat[i] = std::min(rest.mat[ri], uint8_t(kPaletteN - 1));
                out.r[i] = rest.r[ri];
                out.g[i] = rest.g[ri];
                out.b[i] = rest.b[ri];
                out.refl[i] = rest.refl[ri];
                out.rough[i] = rest.rough[ri];
                out.tex[i] = rest.tex[ri];
            }

    buildMicroSdf(out);
    if (stats)
        *stats = st;
    return true;
}

// ---------------------------------------------------------------------------
// Delta deformer (P2)
// ---------------------------------------------------------------------------
namespace {

using DefClock = std::chrono::steady_clock;

double defMsSince(DefClock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(DefClock::now() - t0)
        .count();
}

// The scanline ray the voxelizer uses. Shared so a crossing's position here is
// bit-identical to a full voxelization's - the delta is an XOR of crossings,
// and a last-bit difference would leave a one-cell seam that never heals.
const glm::vec3& parityRay()
{
    static const glm::vec3 r = glm::normalize(glm::vec3(1.f, 1e-4f, 2e-4f));
    return r;
}

struct MeshRowRange {
    int y0 = 0, y1 = -1, z0 = 0, z1 = -1;
};

// The (y,z) rows a triangle can possibly cross, using the SAME padded bbox the
// full voxelizer files it under. A row outside this box cannot produce a
// crossing, so testing exactly these rows is not an approximation.
MeshRowRange rowRangeOfBox(glm::vec3 mn, glm::vec3 mx, glm::ivec3 dim,
                           float voxel, glm::vec3 origin);
MeshRowRange rowRangeOfOne(const MeshTri& t, glm::ivec3 dim, float voxel,
                           glm::vec3 origin);
bool rowInRange(const MeshRowRange& r, int iy, int iz);

MeshRowRange rowRangeOfTwo(const MeshTri& a, const MeshTri& b, glm::ivec3 dim,
                           float voxel, glm::vec3 origin)
{
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-mn);
    const MeshTri* both[2] = { &a, &b };
    for (const MeshTri* t : both)
        for (int k = 0; k < 3; ++k)
            for (int c = 0; c < 3; ++c) {
                mn[c] = std::min(mn[c], t->v[k][c]);
                mx[c] = std::max(mx[c], t->v[k][c]);
            }
    return rowRangeOfBox(mn, mx, dim, voxel, origin);
}

// One triangle's own rows. The delta uses this as a cheap PRE-TEST: a row
// outside a triangle's box cannot hold its crossing, so skipping the ray test
// there is free, and a limb that swings across the body pays only for the rows
// each of its two positions actually covers instead of the whole swept box.
MeshRowRange rowRangeOfOne(const MeshTri& t, glm::ivec3 dim, float voxel,
                           glm::vec3 origin)
{
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-mn);
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], t.v[k][c]);
            mx[c] = std::max(mx[c], t.v[k][c]);
        }
    return rowRangeOfBox(mn, mx, dim, voxel, origin);
}

bool rowInRange(const MeshRowRange& r, int iy, int iz)
{
    return iy >= r.y0 && iy <= r.y1 && iz >= r.z0 && iz <= r.z1;
}

MeshRowRange rowRangeOfBox(glm::vec3 mn, glm::vec3 mx, glm::ivec3 dim,
                           float voxel, glm::vec3 origin)
{
    auto cellOf = [&](float p, int axis) {
        return int(glm::floor((p - origin[axis]) / voxel));
    };
    MeshRowRange r;
    r.y0 = std::max(0, cellOf(mn.y, 1) - 1);
    r.y1 = std::min(dim.y - 1, cellOf(mx.y, 1) + 1);
    r.z0 = std::max(0, cellOf(mn.z, 2) - 1);
    r.z1 = std::min(dim.z - 1, cellOf(mx.z, 2) + 1);
    return r;
}

bool rowCrossingX(const MeshTri& t, float voxel, glm::vec3 origin, int iy,
                  int iz, float& xOut)
{
    const glm::vec3 ro(origin.x, origin.y + (float(iy) + 0.5f) * voxel,
                       origin.z + (float(iz) + 0.5f) * voxel);
    float tt = 0.f;
    if (!(triArea2(t) > 1e-16f) || !rayTriangleT(ro, parityRay(), t, tt))
        return false;
    xOut = ro.x + tt * parityRay().x;
    return true;
}

// The first cell whose CENTRE is strictly beyond `x` along +x. The comparison
// is the same float expression the voxelizer's `crossings[k] < xc` uses, so a
// crossing landing exactly on a centre cannot be counted on one side by the
// delta and the other by a full rebuild.
int firstCellBeyond(float x, float voxel, float originX, int dimX)
{
    int ix = std::clamp(int(std::floor((x - originX) / voxel)) - 1, 0, dimX);
    while (ix < dimX && !(originX + (float(ix) + 0.5f) * voxel > x))
        ++ix;
    return ix;
}

void copyAppearance(const MicroField& rest, size_t ri, MicroField& out, size_t i)
{
    out.mat[i] = std::min(rest.mat[ri], uint8_t(kPaletteN - 1));
    out.r[i] = rest.r[ri];
    out.g[i] = rest.g[ri];
    out.b[i] = rest.b[ri];
    out.refl[i] = rest.refl[ri];
    out.rough[i] = rest.rough[ri];
    out.tex[i] = rest.tex[ri];
}

} // namespace

MicroUnionGrid microUnionGrid(const MicroUnionBounds& bounds, float voxel,
                              int margin)
{
    MicroUnionGrid g;
    g.voxel = voxel;
    if (!(voxel > 0.f) || bounds.empty())
        return g;
    // Snap to the mesh's own lattice: the object's min corner IS the lattice
    // origin, so a cell index means the same thing here as in the rest field.
    const glm::ivec3 loCell = glm::ivec3(glm::floor(bounds.lo / voxel));
    const glm::ivec3 hiCell = glm::ivec3(glm::ceil(bounds.hi / voxel));
    g.origin = glm::vec3(loCell - glm::ivec3(margin)) * voxel;
    g.dim = (hiCell - loCell) + glm::ivec3(2 * margin);
    g.dim = glm::max(g.dim, glm::ivec3(2 * margin + 1));
    return g;
}

size_t MicroDeformer::solidCount() const
{
    size_t n = 0;
    for (uint8_t s : field.solid)
        n += s ? 1 : 0;
    return n;
}

bool MicroDeformer::init(const MicroMesh& m, const MicroRig& r,
                         const MicroField& restField, MicroUnionGrid grid,
                         MicroDeltaStats* stats, std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (m.tris.empty())
        return fail("MicroDeformer::init: no triangles");
    if (restField.size3() == 0)
        return fail("MicroDeformer::init: no rest field");
    if (grid.dim.x <= 0 || grid.dim.y <= 0 || grid.dim.z <= 0)
        return fail("MicroDeformer::init: empty grid");
    // The enclosure flood packs a cell into ten bits an axis (1024 cells =
    // 8.2 m at the 8 mm micro lattice) to keep three divisions out of its hot
    // loop; a micro object is a detail asset, so this is a sanity bound.
    if (grid.dim.x > 1024 || grid.dim.y > 1024 || grid.dim.z > 1024)
        return fail("MicroDeformer::init: grid axis exceeds 1024 cells");
    if (!(grid.voxel > 0.f))
        return fail("MicroDeformer::init: voxel must be positive");

    const auto t0 = DefClock::now();
    mesh = m;
    rig = r;
    rest = restField;

    field = MicroField {};
    field.voxel = grid.voxel;
    field.dim = grid.dim;
    field.origin = grid.origin;

    MicroMeshGrid vg;
    if (!voxelizeMicroMesh(mesh.tris, grid.dim, grid.voxel, grid.origin, vg,
                           err))
        return false;
    if (vg.raw.size() != vg.size3())
        return fail("MicroDeformer::init: the voxelizer kept no parity");
    parity = std::move(vg.raw);

    // The rest pose's bones, appearance and SDF: the SAME full-build path, so
    // the deformer's first frame is exactly the field a full rebuild makes.
    assignMicroMeshBones(vg, rig, {}, {});
    MeshFieldStats fst;
    if (!buildMicroFieldFromMesh(vg, {}, {}, rest, field, &fst, err))
        return false;
    bone = std::move(vg.bone);
    posed = mesh.tris;

    const size_t n = field.size3();
    ext.assign(n, 0);
    queue.reserve(n / 4 + 16);
    dSolid.assign(n, 0);
    dAir.assign(n, 0);
    sdfBuckets.assign(size_t(kMicroSdfMaxSteps) + 2, {});
    regionStamp.assign(n, 0);
    triStamp.assign(mesh.tris.size(), 0);
    stamp = 0;

    const size_t nb = rig.bones.size();
    boneTris.assign(nb, {});
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        int32_t uniq[3];
        int uc = 0;
        for (int k = 0; k < 3; ++k) {
            const size_t vi = t * 3 + size_t(k);
            const int32_t b = vi < mesh.bone.size() ? mesh.bone[vi] : -1;
            if (b < 0 || size_t(b) >= nb)
                continue;
            bool dup = false;
            for (int j = 0; j < uc; ++j)
                if (uniq[j] == b)
                    dup = true;
            if (dup)
                continue;
            uniq[uc++] = b;
            boneTris[size_t(b)].push_back(uint32_t(t));
        }
    }
    lastRot.assign(nb, glm::mat3(1.f));
    lastXform.assign(nb, glm::vec3(0.f));

    // Brick de-duplication for the P3 upload set.
    const int bx = (field.dim.x + BRICK_N - 1) / BRICK_N;
    const int by = (field.dim.y + BRICK_N - 1) / BRICK_N;
    const int bz = (field.dim.z + BRICK_N - 1) / BRICK_N;
    brickStamp.assign(size_t(bx) * size_t(by) * size_t(bz), 0);
    brickEpoch = 0;
    dirtyBricks.clear();

    initialized = true;
    if (stats) {
        *stats = MicroDeltaStats {};
        stats->totalMs = defMsSince(t0);
    }
    return true;
}

bool MicroDeformer::updatePose(const std::vector<glm::mat3>& boneRot,
                               const std::vector<glm::vec3>& boneXform,
                               MicroDeltaStats* stats, std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!initialized || field.size3() == 0)
        return fail("MicroDeformer::updatePose before init");

    const auto tAll = DefClock::now();
    MicroDeltaStats st;
    const glm::ivec3 dim = field.dim;
    const float voxel = field.voxel;
    const size_t nb = rig.bones.size();
    if (lastRot.size() != nb || lastXform.size() != nb) {
        lastRot.assign(nb, glm::mat3(1.f));
        lastXform.assign(nb, glm::vec3(0.f));
    }
    changedCells.clear();

    // ---- 1. pose only the triangles a MOVED bone touches -----------------
    //
    // The skin is rigid per vertex, so a triangle whose three bones all stood
    // still is bit-identical to its previous pose. Re-posing all 78 k
    // triangles costs ~10 ms; the neck of a grazing animal touches a few
    // thousand, and the legs none.
    const auto tPose = DefClock::now();
    std::vector<uint8_t> boneMoved(nb, 0);
    bool anyMoved = false;
    for (size_t b = 0; b < nb; ++b) {
        const glm::mat3 R = b < boneRot.size() ? boneRot[b] : glm::mat3(1.f);
        const glm::vec3 T =
            b < boneXform.size() ? boneXform[b] : glm::vec3(0.f);
        const bool moved = R != lastRot[b] || T != lastXform[b];
        boneMoved[b] = moved ? 1 : 0;
        if (moved) {
            lastRot[b] = R;
            lastXform[b] = T;
            anyMoved = true;
        }
    }
    if (stamp == 0xFFFFFFFFu) {
        std::fill(triStamp.begin(), triStamp.end(), 0u);
        stamp = 0;
    }
    ++stamp;
    movedList.clear();
    movedOld.clear();
    if (anyMoved) {
        for (size_t b = 0; b < nb; ++b) {
            if (!boneMoved[b])
                continue;
            for (uint32_t t : boneTris[b]) {
                if (triStamp[t] == stamp)
                    continue;
                triStamp[t] = stamp;
                movedList.push_back(t);
                movedOld.push_back(OldTri { t, posed[t] });
            }
        }
        for (uint32_t t : movedList)
            poseTri(mesh, t, boneRot, boneXform, posed[t]);
    }
    st.movedTris = movedList.size();
    st.poseMs = defMsSince(tPose);

    // ---- 2. even-odd delta, per scanline row ------------------------------
    //
    // A moved triangle's contribution to a row is ONE crossing that moved from
    // xOld to xNew. Removing a crossing at xOld toggles the parity of every
    // cell centre beyond it; adding one at xNew toggles the same tail back, so
    // the net is the cells between the two crossings. Collect those intervals
    // for every row and XOR them BEFORE applying: when a triangle enters or
    // leaves a row its tail runs to the grid edge, and it is the neighbour
    // entering/leaving with it that cancels the tail. Applying per triangle
    // would flip the whole +x tail of the row and then require the seal to
    // clean up a body-shaped mess.
    const auto tDelta = DefClock::now();
    events.clear();
    for (size_t m = 0; m < movedList.size(); ++m) {
        const MeshTri& oldT = movedOld[m].tri;
        const MeshTri& newT = posed[movedList[m]];
        const MeshRowRange rr =
            rowRangeOfTwo(oldT, newT, dim, voxel, field.origin);
        if (rr.y1 < rr.y0 || rr.z1 < rr.z0)
            continue;
        const MeshRowRange ro = rowRangeOfOne(oldT, dim, voxel, field.origin);
        const MeshRowRange rn = rowRangeOfOne(newT, dim, voxel, field.origin);
        for (int iz = rr.z0; iz <= rr.z1; ++iz)
            for (int iy = rr.y0; iy <= rr.y1; ++iy) {
                // The union box was the swept box, which for a swinging limb is
                // hundreds of rows of which it covers a handful. Test the OLD
                // and NEW boxes first: two integer compares instead of a ray
                // cast, and the events are identical (a row inside neither box
                // has no crossing at either position).
                const bool oIn = rowInRange(ro, iy, iz);
                const bool nIn = rowInRange(rn, iy, iz);
                if (!oIn && !nIn)
                    continue;
                float xo = 0.f, xn = 0.f;
                const bool hasOld =
                    oIn && rowCrossingX(oldT, voxel, field.origin, iy, iz, xo);
                const bool hasNew =
                    nIn && rowCrossingX(newT, voxel, field.origin, iy, iz, xn);
                if (!hasOld && !hasNew)
                    continue;
                const int a = hasOld ? firstCellBeyond(xo, voxel, field.origin.x,
                                                       dim.x)
                                     : dim.x;
                const int b = hasNew ? firstCellBeyond(xn, voxel, field.origin.x,
                                                       dim.x)
                                     : dim.x;
                if (a == b)
                    continue;
                const uint32_t row =
                    uint32_t(iy) * uint32_t(dim.z) + uint32_t(iz);
                events.push_back(RowEvent { row, int32_t(std::min(a, b)) });
                events.push_back(RowEvent { row, int32_t(std::max(a, b)) });
            }
    }
    st.rowEvents = events.size();

    // Counting sort by row. The event list runs to hundreds of thousands of
    // entries per frame while the row table is fixed and dense, so two linear
    // passes replace the comparison sort that used to dominate this stage.
    const uint32_t rowCount = uint32_t(dim.y) * uint32_t(dim.z);
    rowStart.assign(size_t(rowCount) + 1, 0);
    for (const RowEvent& e : events)
        ++rowStart[size_t(e.row) + 1];
    for (uint32_t r = 0; r < rowCount; ++r)
        rowStart[size_t(r) + 1] += rowStart[size_t(r)];
    rowOrder.resize(events.size());
    {
        std::vector<uint32_t> cursor(rowStart.begin(), rowStart.end() - 1);
        for (const RowEvent& e : events)
            rowOrder[cursor[size_t(e.row)]++] = e;
    }

    size_t flips = 0;
    // The AABB of everything the parity touched. The seal uses it to bound its
    // flood (see stage 3), and it is an UPPER bound on where a solid cell can
    // have appeared: interval cells that XOR back to air are inside it too.
    glm::ivec3 flipLo(std::numeric_limits<int>::max());
    glm::ivec3 flipHi(std::numeric_limits<int>::min());
    {
        uint32_t curRow = 0xFFFFFFFFu;
        size_t rowBase = 0;
        int x0 = 0, state = 0;
        auto runTo = [&](int xEnd) {
            if (!state || xEnd <= x0)
                return;
            for (int x = x0; x < xEnd; ++x)
                parity[rowBase + size_t(x)] ^= 1;
            flips += size_t(xEnd - x0);
            // Decoded once per RUN (a few thousand per frame), never per cell.
            const int ry = int(curRow / uint32_t(dim.z));
            const int rz = int(curRow % uint32_t(dim.z));
            flipLo = glm::min(flipLo, glm::ivec3(x0, ry, rz));
            flipHi = glm::max(flipHi, glm::ivec3(xEnd - 1, ry, rz));
        };
        for (const RowEvent& e : rowOrder) {
            if (e.row != curRow) {
                runTo(dim.x); // the previous row's tail
                state = 0;
                x0 = 0;
                curRow = e.row;
                const int iz = int(curRow % uint32_t(dim.z));
                const int iy = int(curRow / uint32_t(dim.z));
                rowBase = field.idx(0, iy, iz);
            }
            const int x = std::clamp(int(e.x), 0, dim.x);
            if (state && x > x0)
                runTo(x);
            state ^= 1;
            x0 = x;
        }
        runTo(dim.x);
    }
    st.parityFlips = flips;
    st.deltaMs = defMsSince(tDelta);

    // A pose that flipped no cell leaves the occupancy (and so the seal and
    // the SDF) untouched - the deltas are functions of occupancy only.
    if (flips == 0) {
        st.totalMs = defMsSince(tAll);
        if (stats)
            *stats = st;
        return true;
    }

    // ---- 3. the enclosure seal, over the body's box (exact) --------------
    //
    // "Not reachable from the grid border" is the one genuinely GLOBAL
    // property, and it is what makes a posed self-overlap (a leg pressed into
    // the belly) solid instead of an air pocket the tracer enters and loses
    // the surface in.
    //
    // It does NOT need the whole grid. A cell outside the solid AABB is
    // provably reachable: the box is convex and holds every solid cell, so
    // marching along any axis on which the cell is outside never re-enters the
    // box (hence never meets anything solid) and runs straight to the border.
    // The union grid is sized for EVERY clip and the animal fills 8 % of it, so
    // flooding from the border visits 1.7 M cells to answer a question that
    // only involves the ~300 k inside the body's box: seed the flood on the six
    // faces of the AABB inflated by one and let the outside be implicitly open.
    //
    // The AABB is (the extent before this update) x (the parity flips), which
    // bounds every solid cell afterwards: unflipped solids were already inside
    // the old extent, and a cell the seal closes is adjacent to solid, so it is
    // inside the inflated box too - which is also why an enclosed cell can
    // never sit ON the inflated face (there the outward neighbour is air).
    const auto tSeal = DefClock::now();
    glm::ivec3 bo = glm::min(field.solidMin, flipLo) - 1;
    glm::ivec3 bh = glm::max(field.solidMax, flipHi) + 1;
    if (field.solidMin.x > field.solidMax.x)
        bo = flipLo - 1, bh = flipHi + 1;
    bo = glm::max(bo, glm::ivec3(0));
    bh = glm::min(bh, dim - 1);
    if (bo.x > bh.x || bo.y > bh.y || bo.z > bh.z) {
        st.sealMs = defMsSince(tSeal);
        st.totalMs = defMsSince(tAll);
        if (stats)
            *stats = st;
        return true;
    }
    for (int z = bo.z; z <= bh.z; ++z)
        for (int y = bo.y; y <= bh.y; ++y)
            std::memset(&ext[field.idx(bo.x, y, z)], 0, size_t(bh.x - bo.x + 1));
    queue.clear();
    // Packed coordinates. The flood visits ~500 k cells and decoding a linear
    // index costs three integer divisions per cell - which is most of this
    // stage, since each is a variable divisor. Ten bits an axis is 1024 cells
    // (8.2 m at the 8 mm micro lattice), enforced in init().
    auto pushAir = [&](int x, int y, int z) {
        if (x < bo.x || y < bo.y || z < bo.z || x > bh.x || y > bh.y || z > bh.z)
            return;
        const size_t i = field.idx(x, y, z);
        if (ext[i] || parity[i])
            return;
        ext[i] = 1;
        queue.push_back(uint32_t(x) | (uint32_t(y) << 10) | (uint32_t(z) << 20));
    };
    for (int z = bo.z; z <= bh.z; ++z)
        for (int y = bo.y; y <= bh.y; ++y) {
            pushAir(bo.x, y, z);
            pushAir(bh.x, y, z);
        }
    for (int z = bo.z; z <= bh.z; ++z)
        for (int x = bo.x; x <= bh.x; ++x) {
            pushAir(x, bo.y, z);
            pushAir(x, bh.y, z);
        }
    for (int y = bo.y; y <= bh.y; ++y)
        for (int x = bo.x; x <= bh.x; ++x) {
            pushAir(x, y, bo.z);
            pushAir(x, y, bh.z);
        }
    for (size_t h = 0; h < queue.size(); ++h) {
        const int x = int(queue[h] & 1023u);
        const int y = int((queue[h] >> 10) & 1023u);
        const int z = int(queue[h] >> 20);
        pushAir(x + 1, y, z);
        pushAir(x - 1, y, z);
        pushAir(x, y + 1, z);
        pushAir(x, y - 1, z);
        pushAir(x, y, z + 1);
        pushAir(x, y, z - 1);
    }
    st.sealMs = defMsSince(tSeal);

    // ---- 4. apply the seal, and give the new cells a bone + a material ---
    const auto tSweep = DefClock::now();
    size_t added = 0, removed = 0, appT = 0, appF = 0, sealed = 0;
    glm::ivec3 aLo(std::numeric_limits<int>::max());
    glm::ivec3 aHi(std::numeric_limits<int>::min());
    glm::ivec3 rLo(std::numeric_limits<int>::max());
    glm::ivec3 rHi(std::numeric_limits<int>::min());
    // Only the box can hold a solid cell (before OR after: `removed` cells are
    // flipped cells, and the flipped cells are inside it), so the sweep does
    // not have to look outside - and outside, `ext` is stale by design.
    for (int z = bo.z; z <= bh.z; ++z)
        for (int y = bo.y; y <= bh.y; ++y)
            for (int x = bo.x; x <= bh.x; ++x) {
                const size_t i = field.idx(x, y, z);
                const uint8_t s = uint8_t(parity[i] || !ext[i]);
                if (s == field.solid[i])
                    continue;
                field.solid[i] = s;
                // A cell the parity calls solid is a surface the mesh emits; a
                // cell only the seal closes is a POCKET (a posed self-overlap).
                // Counting them apart is what says whether a frame's work is
                // the surface or the fill.
                if (s && !parity[i])
                    ++sealed;
                const glm::ivec3 cell(x, y, z);
                // The one set a renderer must re-read (P3), and the one the
                // distance band is a function of: the SOLID change, i.e. the
                // parity flips plus whatever the seal closed or opened.
                changedCells.push_back(cell);
                if (s) {
                    ++added;
                    aLo = glm::min(aLo, cell);
                    aHi = glm::max(aHi, cell);
                    const glm::vec3 p = field.origin +
                                        (glm::vec3(float(x), float(y), float(z)) +
                                         0.5f) *
                                            voxel;
                    const int b = microBoneAtPosed(rig, p, boneRot, boneXform);
                    bone[i] = b;
                    glm::vec3 rp = p;
                    if (b >= 0 && size_t(b) < boneRot.size() &&
                        size_t(b) < boneXform.size())
                        rp = glm::transpose(boneRot[size_t(b)]) *
                             (p - boneXform[size_t(b)]);
                    const size_t ri = nearestRestSolid(rest, rp);
                    if (ri == SIZE_MAX) {
                        ++appF;
                    } else {
                        ++appT;
                        copyAppearance(rest, ri, field, i);
                    }
                } else {
                    ++removed;
                    bone[i] = -1;
                    rLo = glm::min(rLo, cell);
                    rHi = glm::max(rHi, cell);
                }
            }
    st.added = added;
    st.removed = removed;
    st.appearanceTransferred = appT;
    st.appearanceFallbacks = appF;
    st.sealAdded = sealed;

    // The solid extent is what the footprint, the ground clamp and the union
    // grid read; a stale one sinks or floats the animal.
    if (added) {
        field.solidMin = glm::min(field.solidMin, aLo);
        field.solidMax = glm::max(field.solidMax, aHi);
    }
    if (removed && field.solidMin.x <= field.solidMax.x) {
        auto planeHasSolid = [&](int axis, int plane) {
            glm::ivec3 lo(0), hi(dim - 1);
            lo[axis] = hi[axis] = plane;
            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x)
                        if (field.solid[field.idx(x, y, z)])
                            return true;
            return false;
        };
        for (int axis = 0; axis < 3; ++axis) {
            if (rLo[axis] == field.solidMin[axis])
                while (field.solidMin[axis] <= field.solidMax[axis] &&
                       !planeHasSolid(axis, field.solidMin[axis]))
                    ++field.solidMin[axis];
            if (rHi[axis] == field.solidMax[axis])
                while (field.solidMin[axis] <= field.solidMax[axis] &&
                       !planeHasSolid(axis, field.solidMax[axis]))
                    --field.solidMax[axis];
        }
    }
    st.sweepMs = defMsSince(tSweep);

    // ---- 5. the signed-distance band, over the affected region ------------
    const auto tSdf = DefClock::now();
    refreshSdf(st);
    st.sdfMs = defMsSince(tSdf);

    st.totalMs = defMsSince(tAll);
    if (stats)
        *stats = st;
    return true;
}

void MicroDeformer::refreshSdf(MicroDeltaStats& st)
{
    const glm::ivec3 dim = field.dim;
    const float voxel = field.voxel;
    const int K = kMicroSdfMaxSteps;
    dirtyBricks.clear();
    st.sdfCells = 0;
    st.dirtyBricks = 0;
    if (changedCells.empty())
        return;

    // ---- 1. the affected region ------------------------------------------
    //
    // A cell's stored distance is +-(min(chebSteps, K) - 0.5) voxels, and the
    // step count is a pure Chebyshev transform of the SOLID SET, so the value
    // can only differ where a solid cell within K changed: the K-dilation of
    // `changedCells`. Outside it the capped value is bit-identical, which is
    // what makes this a delta instead of a rebuild. (The box this replaces
    // took the BOUNDING BOX of the change, which for a tilting body is the
    // whole animal. The dilation is still large for a whole-body pose - the
    // cost scales with the swept SURFACE plus K, not with a box - so K is the
    // lever that decides this stage.)
    const auto tRegion = DefClock::now();
    regionEpoch += 2; // +0 marks the region, +1 its seed ring
    if (regionEpoch > 0xFFFFFF00u) {
        std::fill(regionStamp.begin(), regionStamp.end(), 0u);
        regionEpoch = 2;
    }
    const uint32_t inRegion = regionEpoch;
    const uint32_t inRing = regionEpoch + 1;
    region.clear();
    regionFrontier.clear();
    regionNext.clear();
    auto mark = [&](int x, int y, int z, std::vector<glm::ivec3>& dst) {
        if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z)
            return;
        const size_t i = field.idx(x, y, z);
        if (regionStamp[i] == inRegion)
            return;
        regionStamp[i] = inRegion;
        dst.push_back(glm::ivec3(x, y, z));
    };
    for (const glm::ivec3& c : changedCells)
        mark(c.x, c.y, c.z, regionNext);
    for (int d = 0;; ++d) {
        region.insert(region.end(), regionNext.begin(), regionNext.end());
        regionFrontier.swap(regionNext);
        regionNext.clear();
        if (d >= K)
            break; // regionFrontier now holds the outer, distance-K shell
        for (const glm::ivec3& c : regionFrontier)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dx || dy || dz)
                            mark(c.x + dx, c.y + dy, c.z + dz, regionNext);
    }
    const size_t regionCount = region.size();

    // ---- 2. the seed ring ------------------------------------------------
    //
    // The BFS below cannot see sources outside the region, and the nearest
    // source to a region cell may well be out there. The fix is the standard
    // distance-transform boundary condition: a cell K+1 or more from EVERY
    // changed cell has an exact stored distance, because no cell within K of
    // it changed class. Seeding those with their stored value is therefore
    // exact, and every path that leaves the region crosses one of them.
    //
    // A cell adjacent to a region cell but NOT in the region is at distance
    // K+1 by construction (at distance <= K it would have been marked), so the
    // ring is exactly the shell around the distance-K frontier - no separate
    // scan of the whole region is needed.
    //
    // (This is the bug that made the first, box-clamped version disagree with
    // a full rebuild by one voxel over 38 k cells: a seed ring clamped against
    // the grid border is closer than K+1 to the change, so its stored value is
    // stale. Here the ring is defined by distance to the change, never by a
    // box, so the property holds by construction.)
    for (const glm::ivec3& c : regionFrontier)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy && !dz)
                        continue;
                    const int x = c.x + dx, y = c.y + dy, z = c.z + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y ||
                        z >= dim.z)
                        continue;
                    const size_t i = field.idx(x, y, z);
                    if (regionStamp[i] >= inRegion)
                        continue;
                    regionStamp[i] = inRing;
                    region.push_back(glm::ivec3(x, y, z));
                }
    st.regionMs = defMsSince(tRegion);

    // ---- 3. the two transforms, over region + ring -----------------------
    //
    // Distances are uint8 (0 = unreached, d+1 = at distance d): the band is
    // capped at K, so a byte covers the range, and this stage is memory-bound.
    //
    // EACH TRANSFORM ONLY CARRIES VALUES FOR ONE CLASS, and the shortest path
    // to the nearest opposite cell never leaves that class: if a shorter
    // path crossed a cell of the OPPOSITE class, that cell would be nearer
    // than the cell the distance was measured to. So
    //   - dSolid (air -> nearest solid) travels through AIR only, and
    //   - dAir   (solid -> nearest air)  travels through SOLID only,
    // which is why the second one costs a fraction of the first: the body is
    // ~8 % of the grid. The first step out of a source still has to reach the
    // other class, so a distance-0 cell always expands.
    const auto tBfs = DefClock::now();
    auto at = [&](const glm::ivec3& c) { return field.idx(c.x, c.y, c.z); };

    // dSolid: air cells only.
    {
        for (auto& bucket : sdfBuckets)
            bucket.clear();
        for (const glm::ivec3& c : region)
            dSolid[at(c)] = 0;
        for (size_t r = 0; r < regionCount; ++r) {
            const glm::ivec3 c = region[r];
            if (field.solid[at(c)]) {
                dSolid[at(c)] = 1;
                sdfBuckets[0].push_back(c);
            }
        }
        for (size_t r = regionCount; r < region.size(); ++r) {
            const glm::ivec3 c = region[r];
            const size_t i = at(c);
            int d = 0;
            if (!field.solid[i]) {
                const float raw = field.sdf[i] / voxel + 0.5f;
                d = std::clamp(int(std::lround(raw)), 0, K);
            }
            dSolid[i] = uint8_t(d + 1);
            sdfBuckets[size_t(d)].push_back(c);
        }
        for (int d = 0; d <= K; ++d) {
            auto& bucket = sdfBuckets[size_t(d)];
            for (size_t h = 0; h < bucket.size(); ++h) {
                const glm::ivec3 c = bucket[h];
                if (int(dSolid[at(c)]) != d + 1)
                    continue; // improved since it was queued
                const uint8_t nd = uint8_t(d + 2);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy && !dz)
                                continue;
                            const int x = c.x + dx, y = c.y + dy, z = c.z + dz;
                            if (x < 0 || y < 0 || z < 0 || x >= dim.x ||
                                y >= dim.y || z >= dim.z)
                                continue;
                            const size_t ni = field.idx(x, y, z);
                            if (regionStamp[ni] < inRegion)
                                continue; // outside the affected set
                            if (field.solid[ni])
                                continue; // carries no air distance
                            if (dSolid[ni] != 0 && dSolid[ni] <= nd)
                                continue;
                            dSolid[ni] = nd;
                            if (d + 1 <= K)
                                sdfBuckets[size_t(d + 1)].push_back(
                                    glm::ivec3(x, y, z));
                        }
            }
        }
    }

    // dAir: solid cells only.
    {
        for (auto& bucket : sdfBuckets)
            bucket.clear();
        for (const glm::ivec3& c : region)
            dAir[at(c)] = 0;
        // The first solid layer: a solid region cell that touches air anywhere
        // (a region boundary seed does not matter - the test is on the FIELD's
        // occupancy, so it is the same class test the BFS would make).
        for (size_t r = 0; r < regionCount; ++r) {
            const glm::ivec3 c = region[r];
            const size_t i = at(c);
            if (!field.solid[i])
                continue;
            bool touchesAir = false;
            for (int dz = -1; dz <= 1 && !touchesAir; ++dz)
                for (int dy = -1; dy <= 1 && !touchesAir; ++dy)
                    for (int dx = -1; dx <= 1 && !touchesAir; ++dx) {
                        const int x = c.x + dx, y = c.y + dy, z = c.z + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= dim.x ||
                            y >= dim.y || z >= dim.z)
                            continue;
                        if (!field.solid[field.idx(x, y, z)])
                            touchesAir = true;
                    }
            if (touchesAir) {
                dAir[i] = 2; // distance 1
                sdfBuckets[1].push_back(c);
            }
        }
        for (size_t r = regionCount; r < region.size(); ++r) {
            const glm::ivec3 c = region[r];
            const size_t i = at(c);
            int d = 0;
            if (field.solid[i]) {
                const float raw = -field.sdf[i] / voxel + 0.5f;
                d = std::clamp(int(std::lround(raw)), 0, K);
            }
            dAir[i] = uint8_t(d + 1);
            sdfBuckets[size_t(d)].push_back(c);
        }
        for (int d = 0; d <= K; ++d) {
            auto& bucket = sdfBuckets[size_t(d)];
            for (size_t h = 0; h < bucket.size(); ++h) {
                const glm::ivec3 c = bucket[h];
                if (int(dAir[at(c)]) != d + 1)
                    continue;
                const uint8_t nd = uint8_t(d + 2);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy && !dz)
                                continue;
                            const int x = c.x + dx, y = c.y + dy, z = c.z + dz;
                            if (x < 0 || y < 0 || z < 0 || x >= dim.x ||
                                y >= dim.y || z >= dim.z)
                                continue;
                            const size_t ni = field.idx(x, y, z);
                            if (regionStamp[ni] < inRegion)
                                continue;
                            if (!field.solid[ni])
                                continue; // carries no solid depth
                            if (dAir[ni] != 0 && dAir[ni] <= nd)
                                continue;
                            dAir[ni] = nd;
                            if (d + 1 <= K)
                                sdfBuckets[size_t(d + 1)].push_back(
                                    glm::ivec3(x, y, z));
                        }
            }
        }
    }
    st.bfsMs = defMsSince(tBfs);

    // ---- 4. write the band, and derive the dirty bricks ------------------
    const auto tWrite = DefClock::now();
    const int bricksX = (dim.x + BRICK_N - 1) / BRICK_N;
    const int bricksY = (dim.y + BRICK_N - 1) / BRICK_N;
    ++brickEpoch;
    if (brickEpoch == 0) {
        std::fill(brickStamp.begin(), brickStamp.end(), 0u);
        brickEpoch = 1;
    }
    for (const glm::ivec3& c : region) {
        const size_t i = at(c);
        const bool solid = field.solid[i] != 0;
        const int ds =
            dSolid[i] ? std::min(int(dSolid[i]) - 1, K) : K;
        int da = dAir[i] ? std::min(int(dAir[i]) - 1, K) : K;
        // buildMicroSdf runs its AIR BFS on a grid PADDED with one air ring,
        // so a solid cell's depth is also bounded by its distance to that
        // ring - min(x+1, dim.x-x) on each axis. Reproduce it here or every
        // solid cell near a face disagrees with a full rebuild.
        if (solid) {
            const int tPad =
                std::min(std::min(c.x + 1, dim.x - c.x),
                         std::min(std::min(c.y + 1, dim.y - c.y),
                                  std::min(c.z + 1, dim.z - c.z)));
            da = std::min(da, tPad);
        }
        field.sdf[i] = solid ? -(float(da) - 0.5f) * voxel
                             : (float(ds) - 0.5f) * voxel;

        const size_t bi = (size_t(c.z / BRICK_N) * size_t(bricksY) +
                           size_t(c.y / BRICK_N)) *
                              size_t(bricksX) +
                          size_t(c.x / BRICK_N);
        if (brickStamp[bi] != brickEpoch) {
            brickStamp[bi] = brickEpoch;
            dirtyBricks.push_back(uint32_t(bi));
        }
    }
    st.writeMs = defMsSince(tWrite);
    st.sdfCells = region.size();
    st.dirtyBricks = dirtyBricks.size();
}

} // namespace vf::voxel
