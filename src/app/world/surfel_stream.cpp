// The surfel stream: re-bake it from the live field, bucket the world-wide
// water plane per chunk, and patch the height texture after a live edit.
#include "app/app.hpp"

#include <numeric>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::rebuildSurfels()
{
    if (!m_layers.loaded())
        return;
    vkDeviceWaitIdle(m_ctx.device());
    vf::voxel::SurfelParams sp;
    sp.edgeShrink = m_edgeShrink;
    sp.edgeFill = m_edgeFill;
    sp.sunDir = glm::vec3(m_sunDir);
    // micro-detail: texture texels as real micro-surfel geometry (moss,
    // pebbles, bark relief, leaflets). VF_MICRO=0 disables for perf/debug.
    sp.microDetail = m_microDetail;
    // LOD rings: baked 2x2x2 / 4x4x4 merged-terrain surfel runs per chunk;
    // the renderer picks a ring per chunk by distance (VF_LOD1/VF_LOD2).
    sp.lodRings = true;
    if (const char* e = getenv("VF_LOD"))
        sp.lodRings = atoi(e) != 0;
    // material-split LOD merging (VF_LOD_SPLIT=0: single majority disk)
    sp.lodMaterialSplit = true;
    if (const char* e = getenv("VF_LOD_SPLIT"))
        sp.lodMaterialSplit = atoi(e) != 0;
    // anisotropic footprints: disks stretch along local creases (VF_ANISO=0)
    sp.anisotropy = true;
    if (const char* e = getenv("VF_ANISO"))
        sp.anisotropy = atoi(e) != 0;
    // debug/experiment overrides for the surfel bake (default = tuned values)
    if (const char* e = getenv("VF_SURFEL_SMOOTH"))
        sp.smoothNormals = atoi(e) != 0;
    if (const char* e = getenv("VF_SURFEL_HFBLEND"))
        sp.terrainHeightfieldNormals = atoi(e) != 0;
    vf::voxel::SurfelSet set = vf::voxel::buildSurfels(m_layers.field(), sp);
    // The water is ONE fixed-level plane subdivided into a world-wide 0.2 m
    // grid of coplanar surfels (coverage only - the shader intersects the
    // analytic plane per fragment). The depth test against the opaque prepass
    // hides the cells standing on dry land and objects, so a live dig below
    // the level is water with no per-column bookkeeping and every water
    // fragment shades identically. Bucket the run per chunk (deterministic
    // order) so the draw code can frustum-cull.
    m_waterSurfels = vf::voxel::buildWaterSurfels();
    const uint32_t waterStart = uint32_t(set.surfels.size());
    m_waterRel = updateWaterBuckets();
    std::vector<uint32_t> waterRange(m_waterRel.size(), waterStart);
    for (size_t i = 0; i < m_waterRel.size(); ++i)
        waterRange[i] = waterStart + m_waterRel[i];
    set.surfels.resize(set.surfels.size() + m_waterSurfels.size());
    std::copy(m_waterSurfels.begin(), m_waterSurfels.end(),
              set.surfels.begin() + waterStart);
    // chunkRange only covers opaque surfels; waterRange buckets the trailing
    // water run per chunk for frustum-culled water draws; microStart splits
    // each chunk into base + micro-detail for distance culling
    m_splatPass.setSurfels(set.surfels.data(),
                           set.surfels.size() * sizeof(vf::voxel::Surfel),
                           set.surfels.size(), set.chunkRange, waterStart, waterRange,
                           set.microStart, set.edgeStart, set.lod1Range, set.lod2Range,
                           set.objectChunks);
    spdlog::info("splat backend: {} surfels ({} water, {} edge bridges on {} parents), "
                 "{} chunks, lod1 {} lod2 {}",
                 set.surfels.size(), m_waterSurfels.size(), set.edgeBridgeCount,
                 set.edgeParentCount,
                 set.chunkRange.empty() ? 0 : set.chunkRange.size() - 1,
                 set.lod1Count, set.lod2Count);
}

std::vector<uint32_t> App::updateWaterBuckets()
{
    constexpr uint32_t kChunks = 16 * 16 * 16;
    m_waterRel.assign(kChunks + 1, 0);
    auto chunkOf = [](const vf::voxel::Surfel& s) {
        const float px = s.pos_rU.x, py = s.pos_rU.y, pz = s.pos_rU.z;
        const auto ax = std::clamp(int(std::floor((px + 51.2f) / 6.4f)), 0, 15);
        const auto ay = std::clamp(int(std::floor((py + 51.2f) / 6.4f)), 0, 15);
        const auto az = std::clamp(int(std::floor((pz + 51.2f) / 6.4f)), 0, 15);
        return uint32_t(vf::voxel::chunkIndexOf(ax, ay, az));
    };
    if (m_waterSurfels.empty())
        return m_waterRel;
    std::vector<uint32_t> order(m_waterSurfels.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return chunkOf(m_waterSurfels[a]) < chunkOf(m_waterSurfels[b]);
    });
    std::vector<vf::voxel::Surfel> sorted;
    sorted.reserve(m_waterSurfels.size());
    uint32_t open = chunkOf(m_waterSurfels[order[0]]);
    m_waterRel[open] = 0;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint32_t c = chunkOf(m_waterSurfels[order[k]]);
        if (c != open) {
            for (uint32_t f = open + 1; f <= c; ++f)
                m_waterRel[f] = uint32_t(k);
            open = c;
        }
        sorted.push_back(m_waterSurfels[order[k]]);
    }
    for (uint32_t f = open + 1; f < m_waterRel.size(); ++f)
        m_waterRel[f] = uint32_t(order.size());
    m_waterSurfels.swap(sorted);
    return m_waterRel;
}

void App::patchHeightTexture(int x0, int z0, int x1, int z1, int riseCells)
{
    auto& store = m_layers.store();
    const int latN = store.latN();
    if (latN <= 0 || m_heightCpu.size() != size_t(latN) * latN)
        return;
    x0 = std::clamp(x0, 0, latN - 1); x1 = std::clamp(x1, 0, latN - 1);
    z0 = std::clamp(z0, 0, latN - 1); z1 = std::clamp(z1, 0, latN - 1);
    if (x1 < x0 || z1 < z0)
        return;
    const float half = 0.5f * vf::voxel::WORLD;
    int topMost = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            // current (kept in sync) top -> lattice, then scan past the rise
            const float oldTop = m_heightCpu[size_t(z) * latN + size_t(x)].x;
            int y = (oldTop > -1e29f)
                        ? int(std::floor((oldTop + half) / vf::voxel::VOXEL)) - 1
                        : latN - 1;
            y = std::min(latN - 1, y + std::max(riseCells, 0));
            float topY = -1e30f;
            uint8_t mat = 0;
            for (; y >= 0; --y) {
                const vf::voxel::StoreCell c = store.cellAt(x, y, z);
                if (c.solid && !c.obj) {
                    topY = -half + (float(y) + 1.0f) * vf::voxel::VOXEL;
                    mat = c.mat;
                    break;
                }
            }
            m_heightCpu[size_t(z) * latN + size_t(x)] =
                glm::vec2(topY, float(mat) / 255.0f);
            topMost = std::max(topMost, y);
        }
    if (m_heightImg.img == VK_NULL_HANDLE)
        return;
    const uint32_t w = uint32_t(x1 - x0 + 1), h = uint32_t(z1 - z0 + 1);
    std::vector<glm::vec2> rect(size_t(w) * h);
    for (uint32_t z = 0; z < h; ++z)
        std::memcpy(rect.data() + size_t(z) * w,
                    m_heightCpu.data() + size_t(z0 + z) * latN + size_t(x0),
                    size_t(w) * sizeof(glm::vec2));
    if (!vf::uploadSubImage3D(m_ctx, m_heightImg, rect.data(), uint32_t(x0),
                              uint32_t(z0), w, h, sizeof(glm::vec2)))
        spdlog::warn("live edit: height-texture patch {}x{} at ({},{}) failed", w, h,
                     x0, z0);
}

} // namespace app
} // namespace vf
