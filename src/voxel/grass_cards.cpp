#include "voxel/grass_cards.hpp"
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"

#include <cmath>
#include <random>

namespace vf::voxel {

std::vector<GrassCard> buildGrassCards(const std::function<int(int)>& layerForMat,
                                       float spacing, int perCell)
{
    std::vector<GrassCard> out;
    const HeightMap& hm = sharedHeightmap();
    if (!hm.loaded() || perCell <= 0)
        return out;

    // Cache the layer lookup for the small palette: one call per material,
    // not one per terrain cell.
    int layerOf[256];
    for (int& l : layerOf) l = -1;
    for (int m = 0; m < 256; ++m)
        layerOf[m] = layerForMat(m);

    const float half = WORLD * 0.5f;
    // UNGRIDDED placement. A regular cell grid - even with jitter - leaves a
    // visible lattice at close range (the reference has no such pattern).
    // Sample positions uniformly at random and keep those whose ground
    // material has a binding; the rejected ones cost one height lookup.
    const int attempts = std::max(1, int(WORLD * WORLD / (spacing * spacing)) * perCell);
    std::mt19937 rng(7331);
    auto unit = [&]() { return float(rng()) / float(rng.max()); };

    for (int i = 0; i < attempts; ++i) {
        const float x = unit() * WORLD - half;
        const float z = unit() * WORLD - half;
        const float H = hm.sample(x, z);
        const int mat = int(materialAt(x, z, H));
        const int layer = layerOf[mat & 255];
        if (layer < 0 || H < WATER_LEVEL + 0.02f)
            continue;

        const glm::vec2 g = hm.gradient(x, z);
        const glm::vec3 nrm = glm::normalize(glm::vec3(-g.x, 1.0f, -g.y));
        const float ang = unit() * 6.2831853f;
        // Wide length/width ranges: a meadow has tall seed heads and short
        // cut blades side by side, not a uniform carpet.
        const float len = 0.24f + 0.78f * unit() * unit();
        const float wid = 0.018f + 0.050f * unit();
        const float lean = 0.05f + 0.28f * unit();
        const glm::vec3 dir = glm::vec3(std::cos(ang) * lean, 1.0f,
                                        std::sin(ang) * lean) * len;
        const glm::vec3 root { x, H - 0.02f, z };
        // Per-channel tone: a clump is not a clone. Red drifts toward dry
        // straw, blue stays low for green foliage, and all three vary
        // independently so the meadow keeps its colour spread.
        GrassCard c;
        c.rootLayer = glm::vec4(root, float(layer));
        c.dirWidth = glm::vec4(dir, wid);
        c.normAlpha = glm::vec4(nrm, 0.78f + 0.18f * unit());
        // Wide, independent per-channel spread: some clumps stay deep green,
        // others drift to dry gold/straw (the reference meadow is full of
        // sun-bleached blades). The same rule is data, not a material id.
        c.tintMat = glm::vec4(0.55f + 1.15f * unit(),
                              0.62f + 0.60f * unit(),
                              0.35f + 0.75f * unit(), float(mat));
        out.push_back(c);
    }
    return out;
}

} // namespace vf::voxel
