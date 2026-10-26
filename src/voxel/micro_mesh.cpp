// micro_mesh.cpp - mesh + skin deformation for micro objects. See the header
// for WHY this replaces the rest-field-through-inverse-transform design.
#include "voxel/micro_mesh.hpp"

#include "voxel/common.hpp"
#include "voxel/mesh_import.hpp"

#include <algorithm>
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
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        posed[t] = mesh.tris[t];
        for (int k = 0; k < 3; ++k) {
            const size_t vi = t * 3 + size_t(k);
            const int32_t b = vi < mesh.bone.size() ? mesh.bone[vi] : -1;
            if (b >= 0 && size_t(b) < boneRot.size() &&
                size_t(b) < boneXform.size())
                posed[t].v[k] = boneRot[size_t(b)] * mesh.rest[vi] +
                                boneXform[size_t(b)];
            else
                posed[t].v[k] = mesh.rest[vi];
        }
    }
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
                int best = -1;
                float bestD = std::numeric_limits<float>::max();
                for (size_t b = 0; b < rig.bones.size(); ++b) {
                    const MicroBone& bone = rig.bones[b];
                    // Un-bone the POSED point with THIS bone's transform, then
                    // measure against THIS bone's rest segment. A bone that
                    // does not own the point maps it somewhere far from its
                    // own segment, which is exactly the discrimination the
                    // shader's fixed rule uses.
                    glm::vec3 rest = p;
                    if (b < boneRot.size() && b < boneXform.size())
                        rest = glm::transpose(boneRot[b]) *
                               (p - boneXform[b]);
                    const float d = distToSegment(rest, bone.a, bone.b);
                    if (d < bestD) {
                        bestD = d;
                        best = int(b);
                    }
                }
                grid.bone[i] = best;
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
                const glm::ivec3 c = glm::ivec3(
                    glm::floor((rp - rest.origin) / rest.voxel));

                // Nearest REST solid within 2 cells. A cell that grew during
                // the pose can land where the rest body never had a surface, so
                // a miss is expected; the fallback material then keeps the mesh
                // from carrying a hole of palette-0 albedo.
                bool got = false;
                size_t ri = 0;
                for (int r = 0; r <= 2 && !got; ++r)
                    for (int dz = -r; dz <= r && !got; ++dz)
                        for (int dy = -r; dy <= r && !got; ++dy)
                            for (int dx = -r; dx <= r && !got; ++dx) {
                                const glm::ivec3 q = c + glm::ivec3(dx, dy, dz);
                                if (!rest.inside(q.x, q.y, q.z))
                                    continue;
                                const size_t k = rest.idx(q.x, q.y, q.z);
                                if (!rest.solid[k])
                                    continue;
                                got = true;
                                ri = k;
                            }
                if (!got) {
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

} // namespace vf::voxel
