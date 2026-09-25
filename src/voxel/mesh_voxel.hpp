#pragma once
// Solid voxelization of a triangle mesh onto the world's VOXEL lattice.
//
// Used by tools/mesh_to_voxel.cpp (the offline STL/OBJ -> .vxw converter) and
// by the vf_mcp `import_mesh` tool. Both need the same guarantee: a mesh
// becomes a set of VoxelRecords whose cells are SOLID, because the loader
// flood-fills an object component to a solid only when its shell is
// watertight - and a rasterised 1-voxel shell is not (see the hollow-tube
// regression in tools/tree_gen.cpp's emit loop). So we emit the full solid
// volume here and let the caller write records straight out.
//
// Algorithm:
//   1. Shell pass - every triangle is rasterised against a VOXEL-sized cube
//      grid via a separating-axis box/triangle overlap test (13 axes). Any
//      cube the triangle touches is marked shell. Neighbouring triangles
//      share edge cubes, so the shell is closed at one-voxel resolution.
//   2. Interior pass - a 6-neighbour flood fill of empty cells starting at
//      the grid border. Every cell not reached and not shell is interior, and
//      becomes solid. A hole larger than one voxel lets the fill escape: the
//      interior comes back empty, which the caller reports (the object would
//      render hollow in that case - repair the mesh or scale it up).
//
// Header-only on purpose: unit-testable from tests/ without a new TU.
#include "voxel/common.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <queue>
#include <string>
#include <vector>

namespace vf::voxel {

// One triangle with its surface appearance. `mat` is a palette id; when
// `hasColor` is set the explicit sRGB byte triple wins over kPalette[mat].
struct MeshTri {
    glm::vec3 v[3];
    uint8_t mat = 4;
    bool hasColor = false;
    uint8_t r = 0, g = 0, b = 0;
};

// Cell flags: 0 = air, 1 = shell (a triangle passes through the cube),
// 2 = interior (enclosed by the shell). Only shell+interior are emitted as
// records when `solid` is requested; `solid=false` emits shell only.
struct VoxelizedMesh {
    int x0 = 0, y0 = 0, z0 = 0; // lattice origin of the grid's min corner
    int nx = 0, ny = 0, nz = 0;
    std::vector<uint8_t> cell; // see flags above
    std::vector<uint8_t> mat;  // per-cell palette id (shell+interior)
    std::vector<uint8_t> r, g, b; // per-cell colour when the triangle had one
    bool leak = false;         // interior fill escaped (non-watertight mesh)

    size_t size3() const { return size_t(nx) * size_t(ny) * size_t(nz); }
    size_t idx(int x, int y, int z) const
    {
        return (size_t(z) * size_t(ny) + size_t(y)) * size_t(nx) + size_t(x);
    }
    bool inside(int x, int y, int z) const
    {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }
    int solidCount() const
    {
        int n = 0;
        for (uint8_t c : cell)
            if (c != 0)
                ++n;
        return n;
    }
};

struct MeshVoxelOptions {
    bool solid = true; // fill the interior (default); false = thin shell only
    // Hard cap on grid cells: the world is 1024^3 and this buffer holds 5
    // bytes/cell, so an oversized mesh would be a memory blow-up rather than
    // a slow conversion. The caller scales the mesh down with --fit.
    size_t maxCells = size_t(64) * 1024 * 1024;
};

// Separating-axis overlap between a VOXEL cube centred at `c` and triangle t.
// 13 axes: 3 box faces, 1 triangle face, 9 edge cross products.
inline bool triCubeOverlap(const glm::vec3& c, const glm::vec3& h, const MeshTri& t)
{
    const glm::vec3 v0 = t.v[0] - c, v1 = t.v[1] - c, v2 = t.v[2] - c;
    const glm::vec3 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;

    auto range = [&](const glm::vec3& axis, float& mn, float& mx) {
        float p0 = glm::dot(v0, axis), p1 = glm::dot(v1, axis), p2 = glm::dot(v2, axis);
        mn = glm::min(p0, glm::min(p1, p2));
        mx = glm::max(p0, glm::max(p1, p2));
    };
    // box face axes: triangle interval must meet [-h, h] per component
    {
        float mn, mx;
        range(glm::vec3(1, 0, 0), mn, mx);
        if (mx < -h.x || mn > h.x)
            return false;
        range(glm::vec3(0, 1, 0), mn, mx);
        if (mx < -h.y || mn > h.y)
            return false;
        range(glm::vec3(0, 0, 1), mn, mx);
        if (mx < -h.z || mn > h.z)
            return false;
    }
    // triangle face axis: box projection must straddle the plane
    {
        const glm::vec3 n = glm::cross(e0, e1);
        const float d = glm::dot(n, v0);
        const float r = glm::dot(glm::abs(n), h);
        if (d > r || d < -r)
            return false;
    }
    // 9 edge pairs: box edge (X/Y/Z) x triangle edge
    const glm::vec3 axes[9] = {
        glm::cross(glm::vec3(1, 0, 0), e0), glm::cross(glm::vec3(1, 0, 0), e1),
        glm::cross(glm::vec3(1, 0, 0), e2), glm::cross(glm::vec3(0, 1, 0), e0),
        glm::cross(glm::vec3(0, 1, 0), e1), glm::cross(glm::vec3(0, 1, 0), e2),
        glm::cross(glm::vec3(0, 0, 1), e0), glm::cross(glm::vec3(0, 0, 1), e1),
        glm::cross(glm::vec3(0, 0, 1), e2),
    };
    for (const glm::vec3& a : axes) {
        float mn, mx;
        range(a, mn, mx);
        const float r = glm::dot(glm::abs(a), h);
        if (mn > r || mx < -r)
            return false;
    }
    return true;
}

// Twice the signed area of a triangle (squared length of the face normal) -
// the degenerate test: zero means the three vertices are collinear.
inline float triArea2(const MeshTri& t)
{
    const glm::vec3 n = glm::cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
    return glm::dot(n, n);
}

// Voxelize `tris` (already in world metres, anywhere in space) onto the VOXEL
// lattice. Returns false when the mesh is empty, degenerate, or too large for
// the cell budget (err then explains it). On a leaky mesh returns true and
// sets out.leak - the caller should warn, since the object renders hollow.
inline bool voxelizeMesh(const std::vector<MeshTri>& tris, VoxelizedMesh& out,
                         const MeshVoxelOptions& opt, std::string* err)
{
    out = VoxelizedMesh {};
    if (tris.empty()) {
        if (err)
            *err = "mesh has no triangles";
        return false;
    }
    glm::vec3 lo(1e30f), hi(-1e30f);
    int usable = 0;
    for (const auto& t : tris) {
        if (triArea2(t) <= 1e-12f)
            continue; // skip degenerate slivers (they would mark whole boxes)
        ++usable;
        for (const glm::vec3& p : t.v)
            for (int k = 0; k < 3; ++k) {
                lo[k] = glm::min(lo[k], p[k]);
                hi[k] = glm::max(hi[k], p[k]);
            }
    }
    if (usable == 0) {
        if (err)
            *err = "mesh has only degenerate (zero-area) triangles";
        return false;
    }

    // grid covers [lo, hi] with a one-cell margin so a mesh flush with the
    // bounds still has an air ring around it for the flood fill to walk
    const glm::vec3 pad(VOXEL);
    out.x0 = int(std::floor((lo.x - pad.x) / VOXEL));
    out.y0 = int(std::floor((lo.y - pad.y) / VOXEL));
    out.z0 = int(std::floor((lo.z - pad.z) / VOXEL));
    const int x1 = int(std::floor((hi.x + pad.x) / VOXEL));
    const int y1 = int(std::floor((hi.y + pad.y) / VOXEL));
    const int z1 = int(std::floor((hi.z + pad.z) / VOXEL));
    out.nx = x1 - out.x0 + 1;
    out.ny = y1 - out.y0 + 1;
    out.nz = z1 - out.z0 + 1;
    if (out.size3() > opt.maxCells) {
        if (err) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "mesh voxelizes to %dx%dx%d (%.0f M cells) - over the "
                          "%.0f M budget; scale it down with --fit",
                          out.nx, out.ny, out.nz, out.size3() / 1e6f,
                          opt.maxCells / 1e6f);
            *err = buf;
        }
        return false;
    }
    const size_t n = out.size3();
    out.cell.assign(n, 0);
    out.mat.assign(n, 0);
    out.r.assign(n, 0);
    out.g.assign(n, 0);
    out.b.assign(n, 0);

    // ---- shell pass -------------------------------------------------------
    const glm::vec3 h(VOXEL * 0.5f);
    for (const auto& t : tris) {
        if (triArea2(t) <= 1e-12f)
            continue;
        glm::vec3 tlo(t.v[0]), thi(t.v[0]);
        for (int k = 1; k < 3; ++k)
            for (int a = 0; a < 3; ++a) {
                tlo[a] = glm::min(tlo[a], t.v[k][a]);
                thi[a] = glm::max(thi[a], t.v[k][a]);
            }
        int cx0 = int(std::floor((tlo.x - h.x) / VOXEL)) - out.x0;
        int cy0 = int(std::floor((tlo.y - h.y) / VOXEL)) - out.y0;
        int cz0 = int(std::floor((tlo.z - h.z) / VOXEL)) - out.z0;
        int cx1 = int(std::floor((thi.x + h.x) / VOXEL)) - out.x0;
        int cy1 = int(std::floor((thi.y + h.y) / VOXEL)) - out.y0;
        int cz1 = int(std::floor((thi.z + h.z) / VOXEL)) - out.z0;
        cx0 = glm::clamp(cx0, 0, out.nx - 1);
        cy0 = glm::clamp(cy0, 0, out.ny - 1);
        cz0 = glm::clamp(cz0, 0, out.nz - 1);
        cx1 = glm::clamp(cx1, 0, out.nx - 1);
        cy1 = glm::clamp(cy1, 0, out.ny - 1);
        cz1 = glm::clamp(cz1, 0, out.nz - 1);
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cy = cy0; cy <= cy1; ++cy)
                for (int cx = cx0; cx <= cx1; ++cx) {
                    const glm::vec3 c(out.x0 + cx, out.y0 + cy, out.z0 + cz);
                    if (!triCubeOverlap(c * VOXEL, h, t))
                        continue;
                    const size_t i = out.idx(cx, cy, cz);
                    out.cell[i] = 1;
                    out.mat[i] = t.mat;
                    if (t.hasColor) {
                        out.r[i] = t.r;
                        out.g[i] = t.g;
                        out.b[i] = t.b;
                    }
                }
    }

    if (!opt.solid)
        return true;

    // ---- interior pass: flood the exterior air from the grid border --------
    std::vector<uint8_t> ext(n, 0);
    std::queue<int> q;
    auto markExt = [&](int x, int y, int z) {
        const size_t i = out.idx(x, y, z);
        if (ext[i] || out.cell[i])
            return;
        ext[i] = 1;
        q.push((z << 20) | (y << 10) | x); // grid dims fit in 20 bits each
    };
    for (int x = 0; x < out.nx; ++x) {
        for (int y = 0; y < out.ny; ++y) {
            markExt(x, y, 0);
            markExt(x, y, out.nz - 1);
        }
        for (int z = 0; z < out.nz; ++z) {
            markExt(x, 0, z);
            markExt(x, out.ny - 1, z);
        }
    }
    for (int y = 0; y < out.ny; ++y)
        for (int z = 0; z < out.nz; ++z) {
            markExt(0, y, z);
            markExt(out.nx - 1, y, z);
        }
    while (!q.empty()) {
        const int p = q.front();
        q.pop();
        const int x = p & 1023, y = (p >> 10) & 1023, z = (p >> 20) & 1023;
        if (x + 1 < out.nx)
            markExt(x + 1, y, z);
        if (x > 0)
            markExt(x - 1, y, z);
        if (y + 1 < out.ny)
            markExt(x, y + 1, z);
        if (y > 0)
            markExt(x, y - 1, z);
        if (z + 1 < out.nz)
            markExt(x, y, z + 1);
        if (z > 0)
            markExt(x, y, z - 1);
    }
    int interior = 0;
    for (size_t i = 0; i < n; ++i)
        if (out.cell[i] == 0 && !ext[i]) {
            out.cell[i] = 2;
            ++interior;
        }
    out.leak = interior == 0;
    if (!out.leak) {
        // Propagate the shell's appearance inward (a BFS from every shell
        // cell through interior cells) so a hollow-walled model keeps one
        // material through its filled core instead of defaulting to 0.
        std::vector<uint8_t> got(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (out.cell[i] == 1)
                got[i] = 1;
        std::queue<int> mq;
        auto spread = [&](int x, int y, int z, uint8_t m, uint8_t r, uint8_t g,
                          uint8_t b) {
            if (!out.inside(x, y, z))
                return;
            const size_t j = out.idx(x, y, z);
            if (got[j] || out.cell[j] != 2)
                return;
            got[j] = 1;
            out.mat[j] = m;
            out.r[j] = r;
            out.g[j] = g;
            out.b[j] = b;
            mq.push((z << 20) | (y << 10) | x);
        };
        for (int z = 0; z < out.nz; ++z)
            for (int y = 0; y < out.ny; ++y)
                for (int x = 0; x < out.nx; ++x) {
                    const size_t i = out.idx(x, y, z);
                    if (out.cell[i] != 1)
                        continue;
                    spread(x + 1, y, z, out.mat[i], out.r[i], out.g[i], out.b[i]);
                    spread(x - 1, y, z, out.mat[i], out.r[i], out.g[i], out.b[i]);
                    spread(x, y + 1, z, out.mat[i], out.r[i], out.g[i], out.b[i]);
                    spread(x, y - 1, z, out.mat[i], out.r[i], out.g[i], out.b[i]);
                    spread(x, y, z + 1, out.mat[i], out.r[i], out.g[i], out.b[i]);
                    spread(x, y, z - 1, out.mat[i], out.r[i], out.g[i], out.b[i]);
                }
        while (!mq.empty()) {
            const int p = mq.front();
            mq.pop();
            const int x = p & 1023, y = (p >> 10) & 1023, z = (p >> 20) & 1023;
            const size_t i = out.idx(x, y, z);
            const uint8_t m = out.mat[i];
            spread(x + 1, y, z, m, out.r[i], out.g[i], out.b[i]);
            spread(x - 1, y, z, m, out.r[i], out.g[i], out.b[i]);
            spread(x, y + 1, z, m, out.r[i], out.g[i], out.b[i]);
            spread(x, y - 1, z, m, out.r[i], out.g[i], out.b[i]);
            spread(x, y, z + 1, m, out.r[i], out.g[i], out.b[i]);
            spread(x, y, z - 1, m, out.r[i], out.g[i], out.b[i]);
        }
    }
    return true;
}

// Turn a voxelized mesh into VoxelRecords placed with bottom-center on the
// lattice `anchor` (the same convention EditableWorld::importLayer uses: the
// anchor cell is the object's bottom-center). Interior cells inherit the
// shell's material/appearance (VoxelField treats the whole component as one
// solid anyway). Returns the record count and reports lattice clamps.
inline size_t meshToRecords(const VoxelizedMesh& m, glm::ivec3 anchor,
                            std::vector<VoxelRecord>& out, int* clamped)
{
    out.clear();
    if (clamped)
        *clamped = 0;
    if (m.nx == 0 || m.ny == 0 || m.nz == 0)
        return 0;
    // Place by the SOLID AABB, not the padded grid: the grid carries a
    // one-cell air ring for the flood fill, and the mesh min can land
    // anywhere inside a cell, so keying on the grid origin would float the
    // object up to a voxel and off-center it.
    int lo[3] = { m.nx, m.ny, m.nz };
    int hi[3] = { -1, -1, -1 };
    for (int z = 0; z < m.nz; ++z)
        for (int y = 0; y < m.ny; ++y)
            for (int x = 0; x < m.nx; ++x)
                if (m.cell[m.idx(x, y, z)] != 0)
                    for (int a = 0; a < 3; ++a) {
                        const int c = (a == 0 ? x : (a == 1 ? y : z));
                        lo[a] = glm::min(lo[a], c);
                        hi[a] = glm::max(hi[a], c);
                    }
    if (hi[0] < 0)
        return 0;
    const int ox = anchor.x - (lo[0] + hi[0]) / 2; // centred footprint
    const int oy = anchor.y - lo[1];              // base on the anchor
    const int oz = anchor.z - (lo[2] + hi[2]) / 2;
    for (int z = 0; z < m.nz; ++z)
        for (int y = 0; y < m.ny; ++y)
            for (int x = 0; x < m.nx; ++x) {
                const size_t i = m.idx(x, y, z);
                if (m.cell[i] == 0)
                    continue;
                const int lx = ox + x, ly = oy + y, lz = oz + z;
                if (lx < 0 || ly < 0 || lz < 0 || lx >= 1024 || ly >= 1024 || lz >= 1024) {
                    if (clamped)
                        ++(*clamped);
                    continue;
                }
                if (m.r[i] || m.g[i] || m.b[i])
                    out.push_back(makeVoxelRecord(lx, ly, lz, m.mat[i], m.r[i],
                                                  m.g[i], m.b[i]));
                else
                    out.push_back(makeVoxelRecord(lx, ly, lz, m.mat[i]));
            }
    return out.size();
}

} // namespace vf::voxel
