// Coarse IRRADIANCE volume: the indirect term that replaces the old
// albedo-tinted stand-in, so a room lit by an actual emitter is tinted by that
// emitter instead of by nothing.
//
// WHAT THIS IS NOT: this is NOT global illumination. It is DIRECT irradiance
// arriving at each cell from the light set, with visibility, plus a sky term.
// There is no propagation between bounces - light does not bounce off the
// floor and come back. One bounce is a separate, more expensive bake and is
// deliberately out of scope. Calling this "GI" would be exactly the kind of
// claim-that-outruns-the-evidence this repo keeps paying for.
//
// WHY A VOLUME AND NOT PER-FRAGMENT TRACING: the splat fragment shader already
// runs twice per covered pixel (opaque base pass at depth EQUAL, then the
// blended band pass) and a per-fragment occlusion march is already up to 32
// dependent texture taps. Tracing at fragment rate multiplies an
// already-expensive pass for a term that is inherently low-frequency. A 64^3
// grid is 1.6 m cells: coarse enough that the term reads as smooth indirect
// light (which is what it is), fine enough that a hearth is roughly one cell.
//
// THE EMITTER SET IS THE SHADER'S LIGHTUBO SLOTS, not a private list. It is
// built from the same 16 slots `applyLights` consumes, authored first then
// derived-from-emissive, so the two paths cannot disagree about which lights
// exist. The consequence is that truncation is SHARED: when the 16 slots fill,
// `applyLights` and this volume degrade together. That is consistent, but a
// dropped emitter here is invisible in the frame with no symptom of its own,
// so `buildIrradianceVolume` reports how many emitters it saw and used and the
// caller logs both. A full 16/16 must be visible in the log, never inferred.
#pragma once

#include "voxel/common.hpp"
#include "voxel/voxel_field.hpp"
#include "voxel/worldfile.hpp"

#include <vector>

namespace vf::voxel {

struct IrradianceVolume {
    // 64^3 over the 102.4 m world = 1.6 m cells, 4 MB as RGBA32F (the upload is
    // a byte-copy of cells.data(), no packing step).
    static constexpr int kN = 64;
    static constexpr float kCellM = WORLD / float(kN);
    // THE COORDINATE FRAME IS ORIGIN-CENTRED, and both the CPU bake and the
    // shader MUST agree on it: cell (x,y,z) covers the world box
    // [x*kCell - WORLD/2, (x+1)*kCell - WORLD/2) on each axis. The world spans
    // -WORLD/2..+WORLD/2, NOT 0..WORLD - VoxelField::sampleWorld is the
    // authority (int((p + 0.5f*WORLD) / VOXEL)), and the authored terrain
    // confirms it: heightmap.hpp's kHmMinMeters is -8.0f, so world coordinates
    // are routinely negative.
    //
    // This is not a formality. The bake shipped once with cell centres at
    // (i+0.5)*kCell (i.e. 0..WORLD) while the shader sampled in the centred
    // frame, misregistering every cell by WORLD/2 = 51.2 m = 32 cells, and the
    // test suite did not notice for exactly one reason: its own helper
    // reproduced the same uncentred formula, so bake and test agreed with each
    // other and neither had an external reference point. Any change to the
    // cell->world mapping must change `irrCellCentre` in tests/test_world.cpp
    // in the same commit, and the frame test there is what pins it.
    static constexpr float kOriginOffset = WORLD * 0.5f;

    // xyz = irradiance arriving at the cell centre, w = sky-visibility
    // fraction in [0,1]. Shading blends emitter light against sky light with
    // w so an enclosed cell does not receive full daylight ambient just
    // because the volume has no geometry information about its own cell.
    std::vector<glm::vec4> cells;

    // ---- THE UPLOAD CONTRACT, pinned because nothing else can check it -------
    //
    // The upload does NO packing: it hands `cells.data()` to
    // vkCmdCopyBufferToImage as a tightly-packed RGBA32F image. That is the
    // right choice precisely because it means there is no second expression
    // capable of disagreeing with this one - the earlier failure mode in this
    // file was two copies of a coordinate formula that agreed with each other.
    // But "right by convention" is not "enforced", so these static_asserts make
    // it enforced. If the cell type ever gains padding, or the format changes,
    // this file refuses to build instead of shipping a volume whose bytes are
    // silently misaligned.
    //
    // ORDER: x fastest, then y, then z - Vulkan's tightly-packed copy order, so
    // the linear index is (z*n + y)*n + x. This matches `chunkIndexOf`'s
    // canonical z-major convention used by the SVO/surfel/store paths; do NOT
    // reintroduce a second convention here.
    //
    // The same index appears in three places and they must agree:
    //   - buildIrradianceVolume's write      (irradiance_volume.cpp)
    //   - the test's irrIndex read           (tests/test_world.cpp)
    //   - the shader's texel fetch           (common_irradiance.glsl)
    static constexpr size_t indexOf(int x, int y, int z) {
        return (size_t(z) * size_t(kN) + size_t(y)) * size_t(kN) + size_t(x);
    }
    static constexpr size_t kBytes = size_t(kN) * size_t(kN) * size_t(kN) * 16;

    // A byte-copy to RGBA32F is only legal if the element is exactly 16 bytes
    // with no padding and no tail. Checked at compile time, so changing the
    // cell type is a build error rather than a misaligned upload.
    static_assert(sizeof(glm::vec4) == 16,
                  "the upload byte-copies cells.data() as tightly-packed "
                  "RGBA32F, so the cell type must be exactly 16 bytes");
    static_assert(alignof(glm::vec4) == 4 || alignof(glm::vec4) >= 4,
                  "cell alignment must divide into a 4-byte texel component");
    // std::vector gives no inter-element padding for a trivially copyable type,
    // so kBytes is the true upload size - but only if the element is 16 B, which
    // is the assert above. Kept as one expression so the upload's image height
    // cannot drift from the allocation.
    static_assert(kBytes == size_t(kN) * size_t(kN) * size_t(kN) *
                                sizeof(glm::vec4));

    bool empty() const { return cells.empty(); }
    size_t size() const { return cells.size(); }
};

// How the emitter budget went, so truncation can be logged instead of
// guessed at. `seen` is how many non-degenerate slots the UBO carried,
// `used` how many the bake actually accumulated (it may drop a light whose
// radius reaches no cell centre).
struct IrradianceBakeStats {
    int seen = 0;
    int used = 0;
    int cellsLit = 0;   // cells that received any emitter contribution
};

// `sunDir` is the direction TOWARD the sun (the same convention as the rest of
// the lighting code). The sky term is deliberately simple and unlit-by-the-sun
// in its magnitude: it exists so a cell that sees the sky is not pitch black
// before the analytic sky irradiance term is added at shading time, and so the
// w channel has something meaningful to say.
//
// Visibility uses the SAME fields and the SAME rule as the splat path's
// `lightVisibilitySplat` (height texture + coarse object volume, with the
// both-endpoints-buried shortcut), deliberately rather than the exact lattice:
// this feeds the splat backend, so matching what the splat path believes about
// occlusion keeps the indirect and direct terms consistent with each other. If
// the two disagreed, a lamp would visibly stop lighting the floor exactly where
// the two visibility models disagree.
IrradianceVolume buildIrradianceVolume(const VoxelField& field,
                                       const worldfile::LightUBO& lights,
                                       glm::vec3 sunDir,
                                       IrradianceBakeStats* stats = nullptr);

} // namespace vf::voxel