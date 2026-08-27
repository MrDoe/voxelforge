#include "voxel/voxel_field.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <queue>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_map>

namespace vf::voxel {

namespace {

constexpr int kBlock = 4; // presence-block granularity (cells)

inline uint32_t cellKey(uint32_t x, uint32_t y, uint32_t z)
{
    return (x << 20) | (y << 10) | z;
}

struct Nb {
    int dx, dy, dz;
    float len;
};
inline const std::vector<Nb>& neighbours()
{
    static const std::vector<Nb> nb = [] {
        std::vector<Nb> v;
        v.reserve(26);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy && !dz)
                        continue;
                    v.push_back({dx, dy, dz, std::sqrt(float(dx * dx + dy * dy + dz * dz))});
                }
        return v;
    }();
    return nb;
}

} // namespace

void VoxelField::oinsert(uint32_t k, uint32_t v)
{
    if (m_omask == 0 || (m_stored + 1) * 2 >= m_omask) {
        // grow at 50% load: rehash into a table of double capacity (or the
        // initial 2k slots when the estimate was clamped)
        size_t ncap = m_omask ? (m_omask + 1) * 2 : 2048;
        std::vector<uint32_t> nkey(ncap, 0), nval(ncap, 0);
        size_t nmask = ncap - 1;
        for (size_t i = 0; i <= m_omask; ++i) {
            if (!m_okey[i])
                continue;
            uint32_t ok = m_okey[i] - 1u;
            size_t j = (size_t(ok) * 2654435761u) & nmask;
            while (nkey[j])
                j = (j + 1) & nmask;
            nkey[j] = ok + 1;
            nval[j] = m_oval[i];
        }
        m_okey.swap(nkey);
        m_oval.swap(nval);
        m_omask = nmask;
    }
    size_t i = oslot(k);
    while (m_okey[i]) {
        if (m_okey[i] - 1u == k) {
            m_oval[i] = v;
            return;
        }
        i = (i + 1) & m_omask;
    }
    m_okey[i] = k + 1;
    m_oval[i] = v;
    ++m_stored;
}

void VoxelField::build(const std::vector<VoxelRecord>& records,
                       const std::vector<int16_t>& colTop, const std::vector<uint8_t>& colMat,
                       const std::vector<uint32_t>& objCells,
                       const std::vector<uint8_t>& objMats)
{
    auto tStart = std::chrono::steady_clock::now();
    m_latN = int(WORLD / VOXEL);
    const int N = m_latN;

    // ---- terrain columns ---------------------------------------------------
    m_colTop = colTop;
    m_colMat = colMat;
    m_heightTex.resize(size_t(N) * N);
    for (size_t i = 0; i < m_heightTex.size(); ++i) {
        int16_t t = m_colTop[i];
        float topY = t >= 0 ? (-0.5f * WORLD + (float(t) + 1.0f) * VOXEL) : -1e30f;
        m_heightTex[i] = glm::vec2(topY, float(m_colMat[i]) / 255.0f);
    }

    // ---- group object cells into connected components (26-neighbourhood) ---
    struct Comp {
        int lo[3], hi[3];
        std::vector<uint32_t> cellIdx; // indices into objCells
    };
    std::vector<Comp> comps;
    {
        size_t cap = 1024;
        while (cap < objCells.size() * 2)
            cap <<= 1;
        std::vector<uint32_t> okey(cap, 0), oval(cap, 0);
        const size_t omask = cap - 1;
        auto oplace = [&](uint32_t k, uint32_t v) {
            size_t i = (size_t(k) * 2654435761u) & omask;
            while (okey[i]) {
                if (okey[i] - 1u == k)
                    return;
                i = (i + 1) & omask;
            }
            okey[i] = k + 1;
            oval[i] = v;
        };
        auto ofind = [&](uint32_t k) -> int {
            size_t i = (size_t(k) * 2654435761u) & omask;
            while (okey[i]) {
                if (okey[i] - 1u == k)
                    return int(oval[i]);
                i = (i + 1) & omask;
            }
            return -1;
        };
        for (size_t i = 0; i < objCells.size(); ++i)
            oplace(objCells[i], uint32_t(i));

        auto xyz = [](uint32_t k, int& x, int& y, int& z) {
            x = int(k >> 20);
            y = int((k >> 10) & 0x3FFu);
            z = int(k & 0x3FFu);
        };
        std::vector<uint32_t> comp(objCells.size(), 0xFFFFFFFFu);
        std::vector<uint32_t> queue;
        for (size_t s = 0; s < objCells.size(); ++s) {
            if (comp[s] != 0xFFFFFFFFu)
                continue;
            uint32_t id = uint32_t(comps.size());
            comps.emplace_back();
            Comp& c = comps.back();
            int x, y, z;
            xyz(objCells[s], x, y, z);
            c.lo[0] = c.hi[0] = x;
            c.lo[1] = c.hi[1] = y;
            c.lo[2] = c.hi[2] = z;
            comp[s] = id;
            queue.clear();
            queue.push_back(uint32_t(s));
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                int cx, cy, cz;
                xyz(objCells[queue[qi]], cx, cy, cz);
                if (cx < c.lo[0]) c.lo[0] = cx;
                if (cy < c.lo[1]) c.lo[1] = cy;
                if (cz < c.lo[2]) c.lo[2] = cz;
                if (cx > c.hi[0]) c.hi[0] = cx;
                if (cy > c.hi[1]) c.hi[1] = cy;
                if (cz > c.hi[2]) c.hi[2] = cz;
                c.cellIdx.push_back(queue[qi]);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy && !dz)
                                continue;
                            int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= N || ny >= N || nz >= N)
                                continue;
                            int ni = ofind(cellKey(uint32_t(nx), uint32_t(ny), uint32_t(nz)));
                            if (ni < 0 || comp[size_t(ni)] != 0xFFFFFFFFu)
                                continue;
                            comp[size_t(ni)] = id;
                            queue.push_back(uint32_t(ni));
                        }
            }
        }
    }

    // ---- signed distance transform per component ---------------------------
    constexpr int kMargin = 2;   // cells of context around the shell
    constexpr float kStore = 6.f; // store air band up to 6 voxels from solids
    // The Dijkstra grid must extend at least kStore beyond the solid bbox, else
    // air cells in the outer part of the air band fall outside the grid and are
    // never stored -> sample() falls back to the (huge) terrain distance just
    // above thin features, so the ray-marcher steps over them at a distance.
    const int kPad = int(kStore) + kMargin;
    const auto nb = neighbours();

    const size_t gBlocks = size_t(globalBlocks());
    m_objBlock.assign(gBlocks * gBlocks * gBlocks, 0);

    // hash capacity: rough lower-bound estimate, clamped small - oinsert()
    // grows the table on demand, so a too-small start only costs rehashes
    {
        size_t est = 1024;
        for (const Comp& c : comps)
            est += c.cellIdx.size() * 8 + 1024;
        size_t cap = 2048;
        while (cap < est && cap < (4u << 20))
            cap <<= 1;
        m_okey.assign(cap, 0);
        m_oval.assign(cap, 0);
        m_omask = cap - 1;
        m_stored = 0;
    }

    auto markObjBlock = [&](int x, int y, int z) {
        size_t bi = (size_t(z / kBlock) * globalBlocks() + size_t(y / kBlock)) *
                        globalBlocks() +
                    size_t(x / kBlock);
        m_objBlock[bi] = 1;
    };

    // ---- coarse object-only volume for GPU shadow marching -----------------
    // Filled inline while emitting field cells below: every stored cell is
    // splatted into its nearest texel, keeping the most occluding (smallest)
    // distance. Texels never touched stay +127 = clear.
    m_objVol.assign(size_t(kObjVolN) * kObjVolN * kObjVolN, int8_t(127));
    auto objVolSplat = [&](int x, int y, int z, int raw) {
        float d = float(raw) * VOXEL;
        if (d > kObjVolMax)
            return;
        auto toTexel = [](int c) {
            float w = -0.5f * WORLD + (float(c) + 0.5f) * VOXEL;
            return std::clamp(int((w / WORLD + 0.5f) * kObjVolN), 0, kObjVolN - 1);
        };
        int8_t enc = int8_t(std::clamp(int(std::lround(d / kObjVolMax * 127.0f)), -127, 127));
        size_t ti = (size_t(toTexel(z)) * kObjVolN + size_t(toTexel(y))) * kObjVolN +
                    size_t(toTexel(x));
        if (enc < m_objVol[ti])
            m_objVol[ti] = enc;
    };

    std::vector<float> distOcc, distAir;
    std::vector<uint8_t> occupied, exterior;
    std::vector<uint8_t> argmat;
    using QE = std::pair<float, uint32_t>; // dist, cell index
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;

    // ---- signed distance transform, parallel over components ---------------
    // Components are independent: workers keep private scratch buffers and
    // fill per-comp outputs; the shared hash / presence blocks / shadow volume
    // are merged serially in component order afterwards (deterministic).
    // Results are cached content-keyed: components whose cells (coords +
    // materials) are unchanged since the previous build are reused verbatim,
    // so small edits only pay Dijkstra for the affected neighbourhoods.
    struct Scratch {
        std::vector<float> distOcc, distAir;
        std::vector<uint8_t> occupied, exterior, argmat;
        std::vector<uint32_t> bfs;
        using QE = std::pair<float, uint32_t>; // dist, cell index
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    };
    auto compHash = [&](const Comp& c) {
        uint64_t h = c.cellIdx.size();
        for (uint32_t ci : c.cellIdx) {
            uint64_t x = (uint64_t(objCells[ci]) << 8) ^
                         (uint64_t(objMats[ci]) * 0xff51afd7ed558ccdull);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdull;
            x ^= x >> 33;
            h += x; // order-independent accumulate
        }
        return h;
    };
    std::vector<CompOut> outs(comps.size());
    std::vector<uint64_t> hashes(comps.size());
    std::unordered_map<uint64_t, size_t> prevLookup;
    if (m_hasPrev)
        for (size_t i = 0; i < m_prevHashes.size(); ++i)
            prevLookup.emplace(m_prevHashes[i], i);
    std::vector<size_t> todo;
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        hashes[ci] = compHash(comps[ci]);
        auto it = prevLookup.find(hashes[ci]);
        if (it != prevLookup.end()) {
            outs[ci] = std::move(m_prevOuts[it->second]);
            prevLookup.erase(it);
        } else {
            todo.push_back(ci);
        }
    }

    auto processComp = [&](const Comp& c, Scratch& s, CompOut& out) {
        const int nx = c.hi[0] - c.lo[0] + 1 + 2 * kPad;
        const int ny = c.hi[1] - c.lo[1] + 1 + 2 * kPad;
        const int nz = c.hi[2] - c.lo[2] + 1 + 2 * kPad;
        const long long vol = (long long)nx * ny * nz;
        if (vol > 32'000'000) { // safety valve (~128 MB of float scratch)
            return false;
        }
        const int gx0 = c.lo[0] - kPad, gy0 = c.lo[1] - kPad, gz0 = c.lo[2] - kPad;
        const size_t cells = size_t(vol);

        s.occupied.assign(cells, 0);
        s.argmat.assign(cells, 0);
        for (uint32_t ci : c.cellIdx) {
            uint32_t k = objCells[ci];
            int x = int(k >> 20), y = int((k >> 10) & 0x3FFu), z = int(k & 0x3FFu);
            size_t i = (size_t(z - gz0) * ny + size_t(y - gy0)) * nx + size_t(x - gx0);
            s.occupied[i] = 1;
            s.argmat[i] = objMats[ci];
        }

        auto idx = [&](int lx, int ly, int lz) {
            return (size_t(lz) * ny + size_t(ly)) * nx + size_t(lx);
        };

        // pass 1: distance to nearest occupied cell (+ its material)
        s.distOcc.assign(cells, 1e30f);
        s.pq = {};
        for (size_t i = 0; i < cells; ++i)
            if (s.occupied[i]) {
                s.distOcc[i] = 0.f;
                s.pq.push({0.f, uint32_t(i)});
            }
        while (!s.pq.empty()) {
            auto [dcur, i] = s.pq.top();
            s.pq.pop();
            if (dcur > s.distOcc[i])
                continue;
            int lz = int(i / (size_t(nx) * ny)), ly = int((i / nx) % ny), lx = int(i % nx);
            for (const Nb& n : nb) {
                int ax = lx + n.dx, ay = ly + n.dy, az = lz + n.dz;
                if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                    continue;
                float nd = dcur + n.len;
                size_t j = idx(ax, ay, az);
                if (nd < s.distOcc[j]) {
                    s.distOcc[j] = nd;
                    s.argmat[j] = s.argmat[i]; // inherit nearest record's material
                    s.pq.push({nd, uint32_t(j)});
                }
            }
        }

        // exterior air flood from the grid boundary through unoccupied cells
        s.exterior.assign(cells, 0);
        {
            s.bfs.clear();
            auto visit = [&](size_t i) {
                if (!s.occupied[i] && !s.exterior[i]) {
                    s.exterior[i] = 1;
                    s.bfs.push_back(uint32_t(i));
                }
            };
            for (int ly = 0; ly < ny; ++ly)
                for (int lx = 0; lx < nx; ++lx) {
                    visit(idx(lx, ly, 0));
                    visit(idx(lx, ly, nz - 1));
                }
            for (int lz = 1; lz < nz - 1; ++lz)
                for (int lx = 0; lx < nx; ++lx) {
                    visit(idx(lx, 0, lz));
                    visit(idx(lx, ny - 1, lz));
                }
            for (int lz = 1; lz < nz - 1; ++lz)
                for (int ly = 1; ly < ny - 1; ++ly) {
                    visit(idx(0, ly, lz));
                    visit(idx(nx - 1, ly, lz));
                }
            for (size_t qi = 0; qi < s.bfs.size(); ++qi) {
                int lz = int(s.bfs[qi] / (size_t(nx) * ny)),
                    ly = int((s.bfs[qi] / nx) % ny), lx = int(s.bfs[qi] % nx);
                for (const Nb& n : nb) {
                    int ax = lx + n.dx, ay = ly + n.dy, az = lz + n.dz;
                    if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                        continue;
                    visit(idx(ax, ay, az));
                }
            }
        }

        // pass 2: distance from solid cells to the nearest exterior-air cell
        s.distAir.assign(cells, 255.f);
        s.pq = {};
        for (size_t i = 0; i < cells; ++i)
            if (s.exterior[i]) {
                s.distAir[i] = 0.f;
                s.pq.push({0.f, uint32_t(i)});
            }
        while (!s.pq.empty()) {
            auto [dcur, i] = s.pq.top();
            s.pq.pop();
            if (dcur > s.distAir[i])
                continue;
            int lz = int(i / (size_t(nx) * ny)), ly = int((i / nx) % ny), lx = int(i % nx);
            for (const Nb& n : nb) {
                int ax = lx + n.dx, ay = ly + n.dy, az = lz + n.dz;
                if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                    continue;
                float nd = dcur + n.len;
                size_t j = idx(ax, ay, az);
                if (nd < s.distAir[j]) {
                    s.distAir[j] = nd;
                    s.pq.push({nd, uint32_t(j)});
                }
            }
        }

        // collect: signed field cells (merged into the shared state later)
        out.keys.clear();
        out.vals.clear();
        for (int lz = 0; lz < nz; ++lz)
            for (int ly = 0; ly < ny; ++ly)
                for (int lx = 0; lx < nx; ++lx) {
                    size_t i = idx(lx, ly, lz);
                    bool solid = s.occupied[i] || !s.exterior[i];
                    float dCell =
                        solid ? -(s.distAir[i] * VOXEL) : +(s.distOcc[i] * VOXEL);
                    if (!solid && dCell > kStore * VOXEL)
                        continue;
                    int raw = int(std::lround(dCell / VOXEL));
                    raw = std::clamp(raw, -127, 127);
                    uint32_t wx = gx0 + lx, wy = gy0 + ly, wz = gz0 + lz;
                    if (wx >= uint32_t(N) || wy >= uint32_t(N) || wz >= uint32_t(N))
                        continue;
                    out.keys.push_back(cellKey(wx, wy, wz));
                    out.vals.push_back(uint32_t(uint8_t(raw & 0xFF)) |
                                       (uint32_t(s.argmat[i]) << 8));
                }
        return true;
    };

    size_t skipped = 0;
    if (!todo.empty()) {
        std::atomic<size_t> next{ 0 };
        std::atomic<size_t> skippedAtomic{ 0 };
        unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < hc; ++t)
            threads.emplace_back([&] {
                Scratch s;
                for (;;) {
                    size_t ti = next.fetch_add(1);
                    if (ti >= todo.size())
                        return;
                    size_t ci = todo[ti];
                    if (!processComp(comps[ci], s, outs[ci]))
                        skippedAtomic.fetch_add(1);
                }
            });
        for (auto& th : threads)
            th.join();
        skipped = skippedAtomic.load();
    }

    // deterministic merge into hash + presence blocks + shadow volume
    for (const CompOut& o : outs) {
        for (size_t i = 0; i < o.keys.size(); ++i) {
            uint32_t k = o.keys[i];
            oinsert(k, o.vals[i]);
            int x = int(k >> 20), y = int((k >> 10) & 0x3FFu), z = int(k & 0x3FFu);
            markObjBlock(x, y, z);
            objVolSplat(x, y, z, int(int8_t(o.vals[i] & 0xFF)));
        }
    }
    if (skipped)
        spdlog::warn("voxel_field: {} oversized component(s) skipped", skipped);

    m_prevOuts = std::move(outs);
    m_prevHashes = std::move(hashes);
    m_hasPrev = true;

    m_built = true;
    spdlog::info("voxel_field: {} components, {} object cells stored ({:.0f} ms)",
                 comps.size(), m_stored,
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - tStart)
                     .count());
}

float VoxelField::smoothTerrainY(float wx, float wz) const
{
    // continuous lattice coordinate of the sample point; texel i is centred on
    // cell i, matching the shader's heightAt() uv mapping
    float fx = (wx + 0.5f * WORLD) / VOXEL - 0.5f;
    float fz = (wz + 0.5f * WORLD) / VOXEL - 0.5f;
    int x0 = int(std::floor(fx)), z0 = int(std::floor(fz));
    float tx = fx - float(x0), tz = fz - float(z0);
    auto const clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int x1 = clampi(x0 + 1, 0, m_latN - 1), z1 = clampi(z0 + 1, 0, m_latN - 1);
    x0 = clampi(x0, 0, m_latN - 1);
    z0 = clampi(z0, 0, m_latN - 1);
    float h00 = m_heightTex[size_t(z0) * m_latN + size_t(x0)].x;
    float h10 = m_heightTex[size_t(z0) * m_latN + size_t(x1)].x;
    float h01 = m_heightTex[size_t(z1) * m_latN + size_t(x0)].x;
    float h11 = m_heightTex[size_t(z1) * m_latN + size_t(x1)].x;
    return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
}

bool VoxelField::anyTerrainNear(int cx, int cz) const
{
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            int x = cx + dx, z = cz + dz;
            if (x >= 0 && z >= 0 && x < m_latN && z < m_latN &&
                m_colTop[size_t(z) * m_latN + size_t(x)] >= 0)
                return true;
        }
    return false;
}

VoxelField::Sample VoxelField::sample(int cx, int cy, int cz) const
{
    Sample out;
    out.d = 1e9f;
    out.mat = 0;
    out.obj = false;
    if (cx < 0 || cy < 0 || cz < 0 || cx >= m_latN || cy >= m_latN || cz >= m_latN)
        return out;

    // terrain: vertical distance to the bilinear-smoothed column surface
    if (anyTerrainNear(cx, cz)) {
        float wx = -0.5f * WORLD + (float(cx) + 0.5f) * VOXEL;
        float wz = -0.5f * WORLD + (float(cz) + 0.5f) * VOXEL;
        float py = -0.5f * WORLD + (float(cy) + 0.5f) * VOXEL;
        out.d = py - smoothTerrainY(wx, wz);
        int ix = std::clamp(cx, 0, m_latN - 1), iz = std::clamp(cz, 0, m_latN - 1);
        out.mat = terrainMat(ix, iz);
    }

    // objects: sparse signed field (overrides terrain when closer)
    uint32_t v;
    if (ofind(cellKey(uint32_t(cx), uint32_t(cy), uint32_t(cz)), v)) {
        float d = float(int8_t(v & 0xFF)) * VOXEL;
        if (d < out.d) {
            out.d = d;
            out.mat = uint8_t(v >> 8);
            out.obj = true;
        }
    }
    return out;
}

VoxelField::Sample VoxelField::sampleWorld(glm::vec3 p) const
{
    int cx = int(std::floor((p.x + 0.5f * WORLD) / VOXEL));
    int cy = int(std::floor((p.y + 0.5f * WORLD) / VOXEL));
    int cz = int(std::floor((p.z + 0.5f * WORLD) / VOXEL));
    return sample(cx, cy, cz);
}

} // namespace vf::voxel
