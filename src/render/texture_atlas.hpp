#pragma once
#include <glm/glm.hpp>
// TexAtlas: optional PNG texture overrides for surfel shading.
//
// world.json may declare a top-level "textures" table (see
// worldfile::loadTextureManifest) mapping a material id to a PNG. The atlas
// is a fixed-size 2D texture array with one layer per material id
// (0..kPaletteN-1); layer == material id, so the shader indexes it directly
// without a per-surfel indirection. Materials without an entry keep their
// palette albedo (their layer holds a neutral filler and the UBO slot is -1,
// so the texture branch never fires).
//
// The layer count is fixed for the process lifetime, so the VkImageView is
// stable and the descriptor only needs writing once. A world reload
// re-uploads pixel data (and re-blits the mip chain) but never recreates the
// image. Sampling is triplanar in world space (see texTriplanar in
// common_base.glsl), so the texture is a pure function of (position, normal)
// - bit-exact across rebuilds and identical in both backends.
//
// Scale is metres per texture tile; the default texture size is fixed so
// sources of any resolution are resampled into the atlas.
//
// `VF_TEXTURES=0` disables the feature (all slots -> -1) without freeing the
// resources, which keeps the descriptor valid.
#include "rhi/context.hpp"
#include "rhi/resources.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace vf {

class TexAtlas {
public:
    static constexpr uint32_t kTexSize = 512;   // texels per atlas layer side
    static constexpr uint32_t kLayers = 21;     // one per palette material id
    static constexpr float kDefaultScale = 0.5f; // metres per tile

    TexAtlas()
    {
        // all-zero aggregate init would leave slots 1..16 at layer 0 and the
        // scales at 0; every material starts on the palette path
        for (uint32_t m = 0; m < kLayers; ++m) {
            m_slot[m] = -1;
            m_scale[m] = kDefaultScale;
        }
    }
    ~TexAtlas();

    TexAtlas(const TexAtlas&) = delete;
    TexAtlas& operator=(const TexAtlas&) = delete;

    // Parse + decode every PNG declared in the manifest and (re)upload the
    // atlas. Safe to call repeatedly (world reload); the image and view are
    // created once. Returns false only if the Vulkan resources could not be
    // made - a missing/bad PNG logs a warning and falls back to the palette
    // for that material.
    bool load(const Context& ctx, const std::string& manifestPath);

    bool valid() const { return m_image.img != VK_NULL_HANDLE; }
    VkImageView view() const { return m_image.view; }
    VkSampler sampler() const { return m_sampler; }
    VkBuffer tableUbo() const { return m_tableBuf.buf; }

    // mat -> (slot, scale) packed as the shader's matTex[mId]: x = layer or
    // -1 (no texture), y = metres per tile, z = emissive scale (0 = the
    // texture does not emit).
    void fillTable(float* outNx4) const;

    // Emission strength of the texture bound to `mat` (0 = none). The shader
    // reads the same value from the table's .z; the CPU side needs it to turn
    // emissive materials into point lights.
    float emissiveScale(int mat) const {
        return (mat >= 0 && mat < int(kLayers)) ? m_emis[mat] : 0.0f;
    }
    // Mean colour of the texture bound to `mat` (black when untextured) -
    // the light COLOUR a derived point light uses, so a cyan glow lights the
    // room cyan rather than in the palette's ember orange.
    glm::vec3 meanColor(int mat) const {
        return (mat >= 0 && mat < int(kLayers)) ? m_mean[mat] : glm::vec3(0.0f);
    }

    // Release the GPU resources. Must be called before the Vulkan device dies
    // (App::destroy calls it explicitly); the destructor calls it too, so a
    // late teardown is harmless but a *device-first* teardown is a validation
    // error, not a leak.
    void destroy();

private:
    // Resample a decoded RGBA8 source into the fixed-size atlas layer with a
    // simple box filter (downscale) or nearest (upscale).
    static void resample(const std::vector<uint8_t>& src, int sw, int sh,
                         std::vector<uint8_t>& dst);

    const Context* m_ctx = nullptr;
    Image3D m_image {};
    VkSampler m_sampler = VK_NULL_HANDLE;
    Buffer m_tableBuf {};
    uint32_t m_mips = 1;
    bool m_tableDirty = true;
    // per-material: layer index (== mat id) or -1, and metres per tile
    int m_slot[kLayers] = { -1 };
    float m_scale[kLayers] = { kDefaultScale };
    // per-material emission (0 = non-emissive) and mean colour of its
    // texture, both fed from the manifest's "emissive"/"emissiveScale"
    float m_emis[kLayers] = { 0.0f };
    glm::vec3 m_mean[kLayers] = {};
};

} // namespace vf
