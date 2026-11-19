#pragma once
// Ground foliage cards: thin vertical photographic blades standing on the
// terrain, rendered by the FurPass's raster pipeline (one instanced quad per
// card) and composited through the same accumulation targets as fur.
//
// This is the SAME primitive the micro objects use for fur: a real raster card
// with per-fragment alpha and a G-buffer depth test, not a surfel shading
// trick. The card's atlas layer is the one bound to the ground material in the
// scenery's "foliageCards" table, so no material or card is hardcoded here.
#include <functional>
#include <vector>
#include <cstdint>

#include <glm/glm.hpp>

namespace vf::voxel {

// 64 B, four vec4s, matching the shader's gCards[] layout.
struct alignas(16) GrassCard {
    // xyz = world-space root, w = shared atlas layer for the photo card
    glm::vec4 rootLayer;
    // xyz = strand direction * length (metres), w = card width (metres)
    glm::vec4 dirWidth;
    // xyz = surface normal, w = peak alpha
    glm::vec4 normAlpha;
    // rgb = per-card tone, w = ground material id
    glm::vec4 tintMat;
};

// Build crossed cards over every terrain cell whose ground material has a
// foliage-card binding. `layerForMat(mat)` returns the atlas layer or -1.
// Returns an empty vector when no material is bound (the pass stays inert).
std::vector<GrassCard> buildGrassCards(const std::function<int(int)>& layerForMat,
                                       float spacing = 0.15f, int perCell = 3);

} // namespace vf::voxel
