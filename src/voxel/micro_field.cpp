// micro_field.cpp - dense fine-lattice field + signed distance for microvoxel
// objects. See micro_field.hpp for the design and the why.
#include "voxel/micro_field.hpp"
#include "voxel/common.hpp"
#include "voxel/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf::voxel {

glm::ivec3 MicroField::cellOf(glm::vec3 p) const
{
    glm::vec3 f = glm::floor((p - origin) / voxel);
    return glm::ivec3(f);
}

bool MicroField::solidAtWorld(glm::vec3 p) const
{
    glm::ivec3 c = cellOf(p);
    if (!inside(c.x, c.y, c.z))
        return false;
    return solid[idx(c.x, c.y, c.z)] != 0;
}

float MicroField::sdfAtWorld(glm::vec3 p) const
{
    glm::ivec3 c = cellOf(p);
    if (!inside(c.x, c.y, c.z))
        return std::numeric_limits<float>::max();
    return sdf[idx(c.x, c.y, c.z)];
}

float MicroField::sampleSdf(glm::vec3 p) const
{
    // Trilinear over cell centres, clamped at the border so an outside query
    // degrades to the edge value rather than reading garbage. The SVO tracer
    // walks a SDF to sphere-step, so a smooth field matters more than speed.
    glm::vec3 g = (p - origin) / voxel - 0.5f;
    glm::ivec3 i = glm::ivec3(glm::floor(g));
    glm::vec3 f = g - glm::vec3(i);
    float acc = 0.0f;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                glm::ivec3 c = i + glm::ivec3(dx, dy, dz);
                if (!inside(c.x, c.y, c.z))
                    continue;
                float w = (dx ? f.x : 1.f - f.x) * (dy ? f.y : 1.f - f.y) *
                          (dz ? f.z : 1.f - f.z);
                acc += w * sdf[idx(c.x, c.y, c.z)];
            }
    return acc;
}

// ---------------------------------------------------------------------------
// Signed distance: a 6-neighbour BFS per side (from solid, from air), each
// giving an exact grid-space Chebyshev-ish distance, then combined into a
// signed field. A micro object is tiny (a vase is ~13 k cells) so the exact,
// obviously-correct BFS is cheaper and far less error-prone than the world's
// jump-flood EDT -- and correctness of the SDF is load-bearing here: the tracer
// sphere-steps on it, so an under-estimate makes the tracer tunnel and an
// over-estimate makes it stall. BFS is never wrong either way.
// ---------------------------------------------------------------------------
namespace {

struct BfsDist {
    std::vector<float> dist;
    void run(const std::vector<uint8_t>& occ, glm::ivec3 dim, bool seedOnOccupied)
    {
        const size_t n = occ.size();
        const float INF = std::numeric_limits<float>::infinity();
        dist.assign(n, INF);
        std::vector<uint32_t> queue;
        queue.reserve(n);
        const int dx[6] = { 1, -1, 0, 0, 0, 0 };
        const int dy[6] = { 0, 0, 1, -1, 0, 0 };
        const int dz[6] = { 0, 0, 0, 0, 1, -1 };
        for (int z = 0; z < dim.z; ++z)
            for (int y = 0; y < dim.y; ++y)
                for (int x = 0; x < dim.x; ++x) {
                    const size_t i = (size_t(z) * size_t(dim.y) + size_t(y)) *
                                         size_t(dim.x) + size_t(x);
                    if ((occ[i] != 0) == seedOnOccupied) {
                        dist[i] = 0.0f;
                        queue.push_back(uint32_t(i));
                    }
                }
        // The queue GROWS during the walk, so the bound must be re-read each
        // iteration. Capturing the seed count up front (the tempting `const
        // size_t lim = queue.size()`) silently truncates the BFS to one level:
        // only the seeds are ever expanded, every deeper cell stays INF. That
        // reads as "deep inside the object the distance clamps to the ceiling",
        // which is exactly how it showed up here.
        for (size_t head = 0; head < queue.size(); ++head) {
            const uint32_t cur = queue[head];
            const int cx = int(cur % uint32_t(dim.x));
            const int cy = int((cur / uint32_t(dim.x)) % uint32_t(dim.y));
            const int cz = int(cur / (uint32_t(dim.x) * uint32_t(dim.y)));
            for (int d = 0; d < 6; ++d) {
                const int nx = cx + dx[d], ny = cy + dy[d], nz = cz + dz[d];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= dim.x || ny >= dim.y ||
                    nz >= dim.z)
                    continue;
                const size_t ni = (size_t(nz) * size_t(dim.y) + size_t(ny)) *
                                      size_t(dim.x) + size_t(nx);
                if (dist[ni] == INF) {
                    dist[ni] = dist[cur] + 1.0f;
                    queue.push_back(uint32_t(ni));
                }
            }
        }
    }
};

} // namespace

bool buildMicroField(const std::vector<VoxelRecord>& records,
                     const WorldFileMeta& meta, MicroField& out,
                     std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (records.empty())
        return fail("micro layer has no records");
    if (!(meta.voxelSize > 0.0f) || !std::isfinite(meta.voxelSize))
        return fail("micro layer meta.voxelSize must be a positive finite size");

    // Derive the grid extent from the record extents. Records are in the
    // layer's own coordinates, so the grid is tight around them and cell (0,0,0)
    // is the layer's min corner: the micro world is positioned entirely by its
    // explicit world `origin`, never by the world lattice.
    glm::ivec3 lo(INT32_MAX), hi(INT32_MIN);
    for (const VoxelRecord& r : records) {
        lo.x = std::min(lo.x, int(r.x));
        lo.y = std::min(lo.y, int(r.y));
        lo.z = std::min(lo.z, int(r.z));
        hi.x = std::max(hi.x, int(r.x));
        hi.y = std::max(hi.y, int(r.y));
        hi.z = std::max(hi.z, int(r.z));
    }
    out.voxel = meta.voxelSize;
    out.dim = hi - lo + 1;
    if (out.dim.x <= 0 || out.dim.y <= 0 || out.dim.z <= 0)
        return fail("micro layer has an empty grid extent");
    if (out.size3() > kMicroMaxCells)
        return fail("micro layer grid exceeds the cell budget");

    // The origin is left at 0 here: the layer's local frame is what the field
    // stores, and placement into the world is applied by the caller when it
    // sets `origin` (transformRecords-equivalent for the fine lattice). Records
    // are rebased so index 0 is the grid min corner.
    out.origin = glm::vec3(0.0f);
    const size_t n = out.size3();
    out.solid.assign(n, 0);
    out.mat.assign(n, 0);
    out.r.assign(n, 0);
    out.g.assign(n, 0);
    out.b.assign(n, 0);
    out.refl.assign(n, 0);
    out.rough.assign(n, 0);
    out.tex.assign(n, 0);
    for (const VoxelRecord& rec : records) {
        const int x = int(rec.x) - lo.x, y = int(rec.y) - lo.y,
                  z = int(rec.z) - lo.z;
        const size_t i = out.idx(x, y, z);
        out.solid[i] = 1;
        out.mat[i] = std::min(rec.materialId, uint8_t(kPaletteN - 1));
        out.r[i] = rec.r;
        out.g[i] = rec.g;
        out.b[i] = rec.b;
        out.refl[i] = rec.reflectivity;
        out.rough[i] = rec.roughness;
        out.tex[i] = rec.reserved;
    }

    // Signed distance: BFS from the solid set (distance to surface from air)
    // and from the air set (depth from a solid cell), combined by the cell's
    // own occupancy. Padded by a one-cell air ring so surface distances are
    // measured to the object, not to the grid border.
    {
        const glm::ivec3 pd = out.dim + glm::ivec3(2);
        std::vector<uint8_t> pad(size_t(pd.x) * size_t(pd.y) * size_t(pd.z), 0);
        auto pidx = [&](int x, int y, int z) {
            return (size_t(z) * size_t(pd.y) + size_t(y)) * size_t(pd.x) +
                   size_t(x);
        };
        for (int z = 0; z < out.dim.z; ++z)
            for (int y = 0; y < out.dim.y; ++y)
                for (int x = 0; x < out.dim.x; ++x)
                    pad[pidx(x + 1, y + 1, z + 1)] = out.solid[out.idx(x, y, z)];

        BfsDist fromSolid, fromAir;
        fromSolid.run(pad, pd, true);   // 0 on solid, grows into air
        fromAir.run(pad, pd, false);    // 0 on air, grows into solid

        out.sdf.assign(n, 0.0f);
        for (int z = 0; z < out.dim.z; ++z)
            for (int y = 0; y < out.dim.y; ++y)
                for (int x = 0; x < out.dim.x; ++x) {
                    const size_t i = out.idx(x, y, z);
                    // Signed distance: air cells hold the distance to the surface
                // (positive), solid cells the depth below it (negative). The
                // -0.5 offsets the half-cell so the zero crossing lands on the
                // object's real boundary rather than one cell inside, which is
                // what keeps the brick's trilinear field continuous across the
                // surface. Getting the sign wrong here is invisible in a
                // screenshot but makes every tracer step straight through the
                // object, so it is asserted in tests/test_micro_surfel.cpp.
                if (out.solid[i])
                    out.sdf[i] = -(std::min(fromAir.dist[pidx(x + 1, y + 1, z + 1)],
                                            float(kMicroSdfMaxVoxels)) - 0.5f) *
                                 out.voxel;
                else
                    out.sdf[i] = (std::min(fromSolid.dist[pidx(x + 1, y + 1, z + 1)],
                                           float(kMicroSdfMaxVoxels)) - 0.5f) *
                                 out.voxel;
                }
    }
    return true;
}

int microGridN(glm::vec3 extent, float voxel)
{
    // One chunk spans BRICK_N (8) bricks; a brick spans BRICK_N (8) voxels, so
    // a chunk holds 8*8 = 64 voxels on a side. Round the object extent UP to a
    // whole number of chunks so the grid always contains it.
    const float chunkM = 16.0f * voxel;
    const float maxE = std::max({ extent.x, extent.y, extent.z });
    if (!(maxE > 0.0f) || !(chunkM > 0.0f))
        return 1;
    return std::max(1, int(std::ceil(maxE / chunkM)));
}

void fillMicroBrick(const MicroField& f, uint32_t* data, int bx0, int by0, int bz0)
{
    for (int bz = 0; bz < BRICK_N; ++bz)
        for (int by = 0; by < BRICK_N; ++by)
            for (int bx = 0; bx < BRICK_N; ++bx) {
                const int cx = bx0 + bx, cy = by0 + by, cz = bz0 + bz;
                uint32_t w[2] = { 0u, 0u };
                if (f.inside(cx, cy, cz)) {
                    const size_t i = f.idx(cx, cy, cz);
                    const uint8_t mat = f.mat[i];
                    const float d = f.sdf[i];
                    // Same quantisation contract as the world bake: the sdf byte
                    // is a signed VOXEL count, decoded by multiplying by this
                    // world's voxel size in the shader. Sign convention matches
                    // too: negative inside, and the hard clamps keep a cell more
                    // than 127 voxels away from any surface at the int8
                    // ceiling rather than wrapping.
                    int sraw = int(d / f.voxel);
                    sraw = std::clamp(sraw, -kMicroSdfMaxVoxels, kMicroSdfMaxVoxels);
                    // bit 7 of the material byte = object surface (drives the
                    // SVO gradient normal and brick material).
                    w[0] = uint32_t(f.r[i]) | (uint32_t(f.g[i]) << 8) |
                           (uint32_t(f.b[i]) << 16) |
                           (uint32_t(uint8_t(sraw)) << 24);
                    w[1] = 255u | (uint32_t(f.refl[i]) << 8) |
                           (uint32_t(f.rough[i]) << 16) |
                           (uint32_t(mat | 0x80u) << 24);
                } else {
                    // Outside the authored grid: air at the max distance, so a
                    // chunk that overhangs the object still marches cleanly.
                    w[0] = 0u | (uint32_t(uint8_t(kMicroSdfMaxVoxels)) << 24);
                    w[1] = 255u | (uint32_t(kMicroSdfMaxVoxels) << 16);
                }
                const size_t bi = (size_t(bz) * BRICK_N + size_t(by)) * BRICK_N +
                                   size_t(bx);
                data[bi * 2] = w[0];
                data[bi * 2 + 1] = w[1];
            }
}

} // namespace vf::voxel