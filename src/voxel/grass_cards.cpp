#include "voxel/grass_cards.hpp"
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"

#include <cmath>
#include <cstdint>
#include <random>

namespace vf::voxel {
namespace {

// Per-candidate randomness. The old code drew one sequential mt19937 stream,
// which made a card's position depend on every candidate before it — so a
// partial rebuild could not reproduce the same cards without walking (and
// shading) the whole world. A hash keyed by the candidate index makes each
// card independent of its neighbours: a brush-sized region can be regenerated
// exactly, with the same positions the full bake produced.
float hash01(uint32_t i, uint32_t salt)
{
    uint32_t h = i * 747796405u + salt * 2891336453u + 7331u;
    h = (h ^ (h >> 16)) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    h ^= h >> 16;
    return float(h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

} // namespace

std::vector<GrassCard> buildGrassCards(const GrassCardLayerFn& layerForCard,
                                       float spacing, int perCell, const glm::vec4* region)
{
    std::vector<GrassCard> out;
    const HeightMap& hm = sharedHeightmap();
    if (!hm.loaded() || perCell <= 0)
        return out;

    const float half = WORLD * 0.5f;
    // UNGRIDDED placement. A regular cell grid - even with jitter - leaves a
    // visible lattice at close range (the reference has no such pattern).
    // Sample positions uniformly at random and keep those whose ground
    // material has a binding; the rejected ones cost one height lookup.
    const int attempts = std::max(1, int(WORLD * WORLD / (spacing * spacing)) * perCell);
    for (int i = 0; i < attempts; ++i) {
        const uint32_t id = uint32_t(i);
        const float x = hash01(id, 0u) * WORLD - half;
        const float z = hash01(id, 1u) * WORLD - half;
        // Region-only rebuild: skip the expensive height/material/field
        // lookups for every candidate outside the painted rectangle. The
        // hash keys are index-based, so the kept cards are bit-identical to
        // the ones a full rebuild would emit at these very positions.
        if (region && (x < region->x || x > region->z ||
                       z < region->y || z > region->w))
            continue;
        // The live texture/store decides both the layer and the root height.
        // Nothing here is baked into the scene: change the texture binding or
        // paint the ground and the next rebuild moves the grass with it.
        float H = 0.0f;
        int mat = 0;
        const int layer = layerForCard(x, z, 0, H, mat);
        if (layer < 0 || H < WATER_LEVEL + 0.02f)
            continue;

        const glm::vec2 g = hm.gradient(x, z);
        const glm::vec3 nrm = glm::normalize(glm::vec3(-g.x, 1.0f, -g.y));
        const float ang = hash01(id, 2u) * 6.2831853f;
        // Wide length/width ranges: a meadow has tall seed heads and short
        // cut blades side by side, not a uniform carpet.
        const float len = 0.24f + 0.78f * hash01(id, 3u) * hash01(id, 3u);
        const float wid = 0.018f + 0.050f * hash01(id, 4u);
        const float lean = 0.05f + 0.28f * hash01(id, 5u);
        const glm::vec3 dir = glm::vec3(std::cos(ang) * lean, 1.0f,
                                        std::sin(ang) * lean) * len;
        const glm::vec3 root { x, H - 0.02f, z };
        // Per-channel tone: a clump is not a clone. Red drifts toward dry
        // straw, blue stays low for green foliage, and all three vary
        // independently so the meadow keeps its colour spread.
        GrassCard c;
        c.rootLayer = glm::vec4(root, float(layer));
        c.dirWidth = glm::vec4(dir, wid);
        c.normAlpha = glm::vec4(nrm, 0.78f + 0.18f * hash01(id, 6u));
        // Wide, independent per-channel spread: some clumps stay deep green,
        // others drift to dry gold/straw (the reference meadow is full of
        // sun-bleached blades). The same rule is data, not a material id.
        c.tintMat = glm::vec4(0.55f + 1.15f * hash01(id, 7u),
                              0.62f + 0.60f * hash01(id, 8u),
                              0.35f + 0.75f * hash01(id, 9u), float(mat));
        out.push_back(c);
    }
    return out;
}

} // namespace vf::voxel
