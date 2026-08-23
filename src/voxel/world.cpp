#include "voxel/world.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <algorithm>

namespace vf::voxel {

namespace {
// Landscape slopes can exceed Lipschitz-1 locally (channel banks); inflate
// the conservative classification margin so empty/solid cells stay safe.
inline float classifyMargin(glm::vec3 cellSize) { return 0.75f * glm::length(cellSize); }
constexpr int kMaxDepth = 3; // chunk 64 -> 32 -> 16 -> brick 8
} // namespace

void World::build()
{
    auto t0 = std::chrono::steady_clock::now();
    const size_t numChunks = size_t(GRID_N) * GRID_N * GRID_N;

    std::vector<int32_t> chunkGrid(numChunks, -1);

    struct Pool {
        std::vector<uint32_t> childBase, payload, handles, bricks;
        uint32_t allocNode()
        {
            payload.push_back(0);
            childBase.push_back(uint32_t(handles.size()));
            handles.resize(handles.size() + 8, kEmptyHandle);
            return uint32_t((payload.size() - 1) << 2);
        }
        uint32_t emitBrick(const uint32_t* data)
        {
            bricks.insert(bricks.end(), data, data + BRICK_VOXELS);
            return uint32_t(((bricks.size() / BRICK_VOXELS) - 1) << 2 | 1);
        }
    };
    std::vector<Pool> pools(numChunks);

    auto buildChunk = [&](size_t ci) {
        int cz = int(ci / (GRID_N * GRID_N));
        int cy = int((ci / GRID_N) % GRID_N);
        int cx = int(ci % GRID_N);
        glm::vec3 cmin(glm::vec3(cx, cy, cz) * CHUNK_M + glm::vec3(-0.5f * WORLD));

        std::function<uint32_t(glm::vec3, glm::vec3, int)> rec =
            [&](glm::vec3 mn, glm::vec3 sz, int d) -> uint32_t {
            float margin = classifyMargin(sz);
            float lo = 1e30f, hi = -1e30f;
            for (int i = 0; i < 8; ++i) {
                glm::vec3 c = mn + sz * glm::vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
                float dv = scene(c).d;
                lo = glm::min(lo, dv);
                hi = glm::max(hi, dv);
            }
            if (lo > margin)
                return kEmptyHandle;
            if (hi < -margin)
                return kSolidHandle;

            if (d == kMaxDepth) {
                uint32_t data[BRICK_VOXELS];
                for (int bz = 0; bz < BRICK_N; ++bz)
                    for (int by = 0; by < BRICK_N; ++by)
                        for (int bx = 0; bx < BRICK_N; ++bx) {
                            glm::vec3 p =
                                mn + glm::vec3(bx + 0.5f, by + 0.5f, bz + 0.5f) *
                                         (sz / float(BRICK_N));
                            SceneSample s = scene(p);
                            uint32_t sb = encodeSnormByte(
                                glm::clamp(s.d / VOXEL, -127.0f, 127.0f));
                            const glm::vec3& c = kPalette[s.mat];
                            data[(bz * BRICK_N + by) * BRICK_N + bx] =
                                uint32_t(c.r * 255.0f) | (uint32_t(c.g * 255.0f) << 8) |
                                (uint32_t(c.b * 255.0f) << 16) | (sb << 24);
                        }
                return pools[ci].emitBrick(data);
            }

            Pool& pool = pools[ci];
            uint32_t nodeH = pool.allocNode();
            uint32_t base = pool.childBase[nodeIndexOf(nodeH)];
            uint32_t validMask = 0, solidMask = 0;
            for (int i = 0; i < 8; ++i) {
                glm::vec3 cmn =
                    mn + glm::vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1) * (sz * 0.5f);
                uint32_t ch = rec(cmn, sz * 0.5f, d + 1);
                pool.handles[base + i] = ch;
                if (ch != kEmptyHandle)
                    validMask |= 1u << i;
                if (ch == kSolidHandle)
                    solidMask |= 1u << i;
            }
            if (solidMask == 0xFF)
                return kSolidHandle;
            if (validMask == 0)
                return kEmptyHandle;
            pool.payload[nodeIndexOf(nodeH)] = validMask | (solidMask << 8);
            return nodeH;
        };

        chunkGrid[ci] = int32_t(rec(cmin, glm::vec3(CHUNK_M), 0));
    };

    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{ 0 };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < hc; ++t)
        threads.emplace_back([&] {
            for (;;) {
                size_t ci = next.fetch_add(1);
                if (ci >= numChunks)
                    return;
                buildChunk(ci);
            }
        });
    for (auto& t : threads)
        t.join();

    // Deterministic merge.
    // Node handles encode NODE indices -> shifted by node counts.
    // Brick handles encode BRICK indices -> shifted by brick counts.
    // childBase values index the flat handle pool -> shifted by handle counts.
    m_gpu.chunkGrid = std::move(chunkGrid);
    size_t nodeOff = 0, hOff = 0, bOff = 0;
    for (size_t ci = 0; ci < numChunks; ++ci) {
        Pool& p = pools[ci];
        for (uint32_t& h : p.handles) {
            if (handleIsNode(h))
                h += uint32_t(nodeOff << 2);
            else if (handleIsBrick(h))
                h += uint32_t(bOff << 2);
        }
        uint32_t r = uint32_t(m_gpu.chunkGrid[ci]);
        if (handleIsNode(r))
            r += uint32_t(nodeOff << 2);
        else if (handleIsBrick(r))
            r += uint32_t(bOff << 2);
        m_gpu.chunkGrid[ci] = r == kEmptyHandle ? -1 : int32_t(r);
        for (uint32_t& cb : p.childBase)
            cb += uint32_t(hOff);

        m_gpu.handles.insert(m_gpu.handles.end(), p.handles.begin(), p.handles.end());
        m_gpu.childBase.insert(m_gpu.childBase.end(), p.childBase.begin(), p.childBase.end());
        m_gpu.payload.insert(m_gpu.payload.end(), p.payload.begin(), p.payload.end());
        m_gpu.bricks.insert(m_gpu.bricks.end(), p.bricks.begin(), p.bricks.end());
        nodeOff += p.payload.size();
        hOff += p.handles.size();
        bOff += p.bricks.size() / BRICK_VOXELS;
    }

    m_stats.nodes = m_gpu.payload.size();
    m_stats.bricks = m_gpu.bricks.size() / BRICK_VOXELS;
    m_stats.activeChunks = size_t(std::count_if(m_gpu.chunkGrid.begin(), m_gpu.chunkGrid.end(),
                                                [](int32_t v) { return v >= 0; }));
    m_stats.buildSeconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

float World::sample(glm::vec3 p) const
{
    glm::ivec3 cc = glm::ivec3(floor((p - m_worldMin) / CHUNK_M));
    if (glm::any(glm::lessThan(cc, glm::ivec3(0))) ||
        glm::any(glm::greaterThanEqual(cc, glm::ivec3(GRID_N))))
        return 4.0f;
    int32_t root = m_gpu.chunkGrid[size_t(cc.z) * GRID_N * GRID_N + size_t(cc.y) * GRID_N + cc.x];
    if (root == -1)
        return 4.0f; // only -1 marks an absent chunk (kSolidHandle == -2!)
    uint32_t h = uint32_t(root);

    glm::vec3 mn = m_worldMin + glm::vec3(cc) * CHUNK_M;
    glm::vec3 sz(CHUNK_M);

    while (true) {
        if (h == kEmptyHandle)
            return 4.0f;
        if (h == kSolidHandle)
            return -VOXEL;
        if (handleIsBrick(h)) {
            uint32_t bi = brickIndexOf(h);
            float cellSz = (sz / float(BRICK_N)).x;
            glm::vec3 g = (p - mn) / cellSz - 0.5f;
            g = glm::clamp(g, glm::vec3(0.0f), glm::vec3(BRICK_N - 1.001f));
            glm::ivec3 i0(g);
            glm::vec3 f = g - glm::vec3(i0);
            auto at = [&](glm::ivec3 c) {
                c = glm::clamp(c, glm::ivec3(0), glm::ivec3(BRICK_N - 1));
                uint32_t v = m_gpu.bricks[bi * BRICK_VOXELS +
                                          size_t(c.z) * BRICK_N * BRICK_N +
                                          size_t(c.y) * BRICK_N + c.x];
                return decodeSnormByte(v >> 24) * VOXEL;
            };
            float c00 = glm::mix(at({ i0.x, i0.y, i0.z }), at({ i0.x + 1, i0.y, i0.z }), f.x);
            float c10 = glm::mix(at({ i0.x, i0.y + 1, i0.z }), at({ i0.x + 1, i0.y + 1, i0.z }), f.x);
            float c01 = glm::mix(at({ i0.x, i0.y, i0.z + 1 }), at({ i0.x + 1, i0.y, i0.z + 1 }), f.x);
            float c11 = glm::mix(at({ i0.x, i0.y + 1, i0.z + 1 }),
                                 at({ i0.x + 1, i0.y + 1, i0.z + 1 }), f.x);
            return glm::mix(glm::mix(c00, c10, f.y), glm::mix(c01, c11, f.y), f.z);
        }

        uint32_t ni = nodeIndexOf(h);
        uint32_t pl = m_gpu.payload[ni];
        uint32_t base = m_gpu.childBase[ni];
        glm::vec3 half = sz * 0.5f;
        glm::vec3 rel = p - mn;
        int oct = (rel.x >= half.x ? 1 : 0) | (rel.y >= half.y ? 2 : 0) |
                  (rel.z >= half.z ? 4 : 0);
        if ((pl >> (8 + oct)) & 1)
            return -VOXEL;
        uint32_t ch = m_gpu.handles[base + oct];
        if (ch == kEmptyHandle)
            return 4.0f;
        if (ch == kSolidHandle)
            return -VOXEL;
        h = ch;
        mn += glm::vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * half;
        sz = half;
    }
}

World::Stats World::stats() const { return m_stats; }

} // namespace vf::voxel
