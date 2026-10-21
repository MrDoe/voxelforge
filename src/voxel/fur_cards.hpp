#pragma once
// Fur cards: transparent hair-clump sprites for a micro-voxel object.
//
// A "card" is the classic real-time fur primitive: a small flat quad that
// stands OUT of the surface, with a soft alpha profile across its width and a
// taper along its length. One card stands for a clump of tens of strands, so
// the cost scales with clumps, not with hairs.
//
// WHY NOT THE WORLD'S SPLATS. The world surfels are disks that lie IN the
// surface tangent plane, and the opaque base seal (the nearest fragment
// written at alpha 1) is exactly what makes a lone silhouette splat read as a
// hard-edged blob. A card is neither: it is a long, thin, mostly transparent
// sliver standing off the surface, so a coat of them reads as fuzz.
//
// The fur lives in its OWN pass for the same reason the micro objects live in
// their own pass: the fur is TRANSLUCENT, and the world's opaque pipeline has
// no room for that. Compositing the fur over the already-opaque body means
// the sky only shows where a card extends past the silhouette - which is
// exactly what fur should do - and no opaque pass has to change.
//
// The cards are generated in the field's LOCAL frame (the frame the field's
// cells live in), so the layer's world placement and the live rotation
// preview are applied by the shader exactly as the micro overlay applies them
// to the field. The two therefore never drift apart.
#include "voxel/micro_field.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace vf::voxel {

// One hair-clump card: 96 B, six vec4s (the same shape as the world's
// Surfel, for the same reason - one record, no side tables).
struct alignas(16) FurCard {
    // xyz = the root, in the object's LOCAL frame (m); w = the card's length.
    glm::vec4 rootLen;
    // xyz = the strand direction (unit, local); w = the card's width.
    glm::vec4 dirWidth;
    // xyz = the surface normal at the root (local); w = the card's peak alpha.
    glm::vec4 nrmAlpha;
    // rgb = the record's own albedo - IGNORED for a TEXTURED material, which
    // takes its colour from the atlas (the same rule the world follows); w =
    // the card's twist about its own strand (radians).
    glm::vec4 tint;
    // x = the baked AO, y = the baked sun shadow, z = the material id, w = the
    // flags (reserved).
    glm::vec4 shade;
    // The CURL: x = the amplitude (m), y = the cycles over the strand's
    // length, z = the phase (radians), w = spare.
    //
    // A straight ribbon reads as a SPIKE: a needle sticking out of the
    // surface. Wool is curly, so the strand's CENTRE LINE wanders across the
    // ribbon's plane in a wave that starts at the root (where the strand is
    // anchored) and grows toward the tip. The ribbon's PLANE is unchanged,
    // so the ray/plane intersection is still exact - only the centre line
    // moves, which costs one sine.
    glm::vec4 curl;
};

// The uniform grid the shader walks with a DDA. The cells are BUCKETS: every
// card is filed in every cell its bounding box touches, so a ray that clips
// the card still finds it.
struct FurGrid {
    glm::vec3 cell { 0.04f };  // the cell size (m)
    glm::ivec3 dim { 0 };      // the cells per axis
    glm::vec3 origin { 0.f };  // the LOCAL position of cell (0,0,0)'s corner
};

// One micro object's cards + its grid.
struct FurObject {
    std::vector<FurCard> cards;
    // The bucket table: cellStart has dim+1 entries (the offsets), cellItems
    // holds the card indices, concatenated over the cells.
    std::vector<uint32_t> cellStart;
    std::vector<uint32_t> cellItems;
    FurGrid grid;
    // The object's world AABB min (= the field's origin + the layer's world
    // offset). The shader's local frame is relative to it.
    glm::vec3 worldMin { 0.f };
    // The live preview state, mirrored from the micro overlay so the fur
    // follows the object it belongs to.
    glm::vec3 pivot { 0.f };
    glm::mat3 rot { 1.f };
    bool rotActive = false;
    glm::vec3 preview { 0.f };
    bool previewActive = false;
};

// How the fur grows.
struct FurParams {
    // The card length (m). Alpaca fleece clumps are a few centimetres.
    float length = 0.026f;
    // The card width (m). A clump is a sliver, not a disk.
    float width = 0.0045f;
    // The cards per surface cell. Four is the measured sweet spot: two left
    // gaps (the coat was sparse at close range), and the extra cards bought
    // very little beyond that. Six, with the wider clumps above, is what the
    // softest measured coat used.
    int perCell = 6;
    // A card's peak alpha.
    float alpha = 0.34f;
    // The strand's TILT out of the surface (degrees). A steep strand reads
    // as a spike; wool lies closer to the body.
    float tiltMin = 12.0f;
    float tiltMax = 38.0f;
    // The CURL: how far the centre line wanders, as a fraction of the
    // strand's length, and how many cycles it makes over it.
    //
    // The amplitude has to be a LARGE fraction of the length to read at all:
    // at 0.16 the wave is a couple of pixels across on a strand fifteen
    // pixels long, and the coat still read as spikes. At 0.45 the wave is
    // about half the strand's length, and the coat reads as wool - the
    // measured high-frequency noise fell from 16.1 to 12.7, BELOW the bare
    // body's 13.5, because a curled strand spreads its ink instead of
    // concentrating it in a needle.
    float curlAmp = 0.45f;
    float curlCycles = 1.0f;
    // The grid's cell size (m). Bigger than the card length, so a card
    // overlaps one or two cells.
    float cellSize = 0.040f;
    // The sun direction (toward the sun), for the baked per-card shadow.
    glm::vec3 sunDir { 0.f, 1.f, 0.f };
    // The materials that grow fur. 21 is `fleece`.
    std::vector<uint8_t> materials { 21 };
};

// Build the fur cards for one micro field.
//
// The cards are generated in the field's LOCAL frame. `out.worldMin` is left
// at zero here: the caller fills it with the object's world AABB min (the
// field's origin plus the layer's world offset), which is what turns the
// local frame into the world frame.
//
// Returns false with `err` when the field is empty or carries no fur
// material at all.
bool buildFurCards(const MicroField& field, const FurParams& params, FurObject& out,
                   std::string* err);

}  // namespace vf::voxel
