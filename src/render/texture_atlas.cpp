#include "render/texture_atlas.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/scenery.hpp"
#include "voxel/worldfile.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace vf {

TexAtlas::~TexAtlas() { destroy(); }

void TexAtlas::destroy()
{
    if (m_ctx) {
        if (m_sampler)
            vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
        destroyBuffer(*m_ctx, m_tableBuf);
        destroyImage3D(*m_ctx, m_image);
    }
    m_sampler = VK_NULL_HANDLE;
    m_image = {};
    m_tableBuf = {};
    m_ctx = nullptr;
}

void TexAtlas::resample(const std::vector<uint8_t>& src, int sw, int sh,
                        std::vector<uint8_t>& dst)
{
    const int N = int(kTexSize);
    dst.assign(size_t(N) * N * 4, 0);
    if (sw <= 0 || sh <= 0)
        return;
    if (sw == N && sh == N) {
        std::copy(src.begin(), src.end(), dst.begin());
        return;
    }
    // box filter: each atlas texel averages the source block covering it.
    // Handles both downscale and (degenerate) upscale by averaging the
    // source texels mapped into the destination texel's footprint.
    for (int y = 0; y < N; ++y) {
        const float fy0 = float(y) / N, fy1 = float(y + 1) / N;
        for (int x = 0; x < N; ++x) {
            const float fx0 = float(x) / N, fx1 = float(x + 1) / N;
            int sx0 = int(std::floor(fx0 * sw)), sx1 = std::max(int(std::ceil(fx1 * sw)), sx0 + 1);
            int sy0 = int(std::floor(fy0 * sh)), sy1 = std::max(int(std::ceil(fy1 * sh)), sy0 + 1);
            sx0 = std::clamp(sx0, 0, sw - 1);
            sx1 = std::clamp(sx1, 1, sw);
            sy0 = std::clamp(sy0, 0, sh - 1);
            sy1 = std::clamp(sy1, 1, sh);
            int r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    const size_t i = (size_t(sy) * sw + sx) * 4;
                    r += src[i + 0]; g += src[i + 1]; b += src[i + 2]; a += src[i + 3];
                    ++n;
                }
            uint8_t* o = &dst[(size_t(y) * N + x) * 4];
            o[0] = uint8_t((r + n / 2) / n);
            o[1] = uint8_t((g + n / 2) / n);
            o[2] = uint8_t((b + n / 2) / n);
            o[3] = uint8_t((a + n / 2) / n);
        }
    }
}

bool TexAtlas::load(const Context& ctx, const std::string& manifestPath)
{
    m_ctx = &ctx;

    std::vector<voxel::worldfile::TextureBinding> bindings;
    const bool haveTable = voxel::worldfile::loadTextureManifest(manifestPath, bindings);
    // Rebuild the slot table from scratch on every load: a material whose
    // entry was removed (GUI "palette") or whose file is missing must fall
    // back to the palette, so a stale slot from a previous load must never
    // survive. With no table at all the table stays empty and the palette
    // path stays bit-exact; the atlas still must exist (the binding is
    // declared in every pipeline layout).
    for (int m = 0; m < int(kLayers); ++m) {
        m_slot[m] = -1;
        m_scale[m] = kDefaultScale;
        m_emis[m] = 0.0f;
        m_mean[m] = glm::vec3(0.0f);
        m_cardLayer[m] = -1;
    }
    m_tableDirty = true;
    const bool disabled = haveTable && bindings.empty() ? false :
                          (getenv("VF_TEXTURES") && std::atoi(getenv("VF_TEXTURES")) == 0);

    // decode + resample every declared texture into its material's layer
    std::vector<std::vector<uint8_t>> layers(kLayers);
    std::string dir = manifestPath;
    {
        const size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    }
    for (const auto& b : bindings) {
        // VF_TEXTURES=0 skips the decode ENTIRELY. Decoding and then only
        // clearing the slot table leaked the texture through the per-cell
        // override: gTexOv wins over matTex in texSlotFor, so override cells
        // kept sampling the decoded layer and the hatch left 0.83% of the
        // hero frame showing the very checker it promises to remove
        // (texture_check vf_off arm). Undecoded, every layer keeps the
        // neutral filler - byte-identical to the no-table palette arm.
        if (disabled)
            break;
        if (b.mat < 0 || b.mat >= int(kLayers))
            continue;
        std::vector<uint8_t> raw;
        int w = 0, h = 0;
        // absolute paths (tests) are used verbatim; relative ones resolve
        // against the manifest directory, then the SHARED asset dir (the
        // texture library is material-identity, not scenery content)
        const std::string path =
            (!b.file.empty() && (b.file[0] == '/' || b.file[1] == ':'))
                ? b.file
                : voxel::scenery::resolveShared(dir + "/", b.file);
        if (!voxel::loadPngRGBA8(path, raw, w, h)) {
            spdlog::warn("texture atlas: '{}' for material {} failed - palette fallback",
                         b.file, b.mat);
            continue;
        }
        resample(raw, w, h, layers[size_t(b.mat)]);
        m_slot[b.mat] = b.mat;
        m_scale[b.mat] = b.scale > 0.02f ? b.scale : kDefaultScale;
        // emission is bound to the texture, not the material id: only after a
        // successful decode, so a missing PNG neither glows nor lights
        m_emis[b.mat] = b.emissive ? (b.emissiveScale > 0.0f ? b.emissiveScale : 1.0f)
                                   : 0.0f;
        spdlog::info("texture atlas: material {} <- {} ({}x{}, {:.2f} m/tile{})",
                     b.mat, b.file, w, h, m_scale[b.mat],
                     m_emis[b.mat] > 0.0f ? ", emissive" : "");
    }
    // Photoreal foliage cards. The card is a property of the TEXTURE binding:
    // a row in the "textures" table may carry a "card" name (e.g. "grass"),
    // and the card grows only over that binding's atlas slot (which is the
    // material index of the row). The shader follows the effectively-sampled
    // slot (per-cell override included), so a card rides the texture it
    // belongs to rather than the cell's material id. VF_TEXTURES=0 skips the
    // load and leaves m_cardLayer at -1, so the bit-exact hatch holds.
    if (!disabled) {
        const std::string cardsJson =
            voxel::scenery::resolveShared(dir + "/", "textures/cards/cards.json");
        std::string cardDir = cardsJson;
        {
            const size_t s = cardDir.find_last_of("/\\");
            cardDir = s == std::string::npos ? std::string() : cardDir.substr(0, s + 1);
        }
        std::vector<vf::voxel::worldfile::CardLibEntry> lib;
        vf::voxel::worldfile::loadCardLibraryFull(cardsJson, lib);

        // For each texture binding that carries a card, resolve its layer/file.
        struct Resolved { int mat = -1; int layer = -1; std::string file; };
        std::vector<Resolved> resolved;
        for (const auto& b : bindings) {
            if (b.card.empty() || b.mat < 0 || b.mat >= int(kLayers))
                continue;
            for (const auto& c : lib) {
                if (c.name == b.card && c.layer >= 0 && c.layer < int(kLayers)
                    && !c.file.empty()) {
                    Resolved r;
                    r.mat = b.mat;
                    r.layer = c.layer;
                    r.file = c.file;
                    resolved.push_back(std::move(r));
                    break;
                }
            }
        }
        // decode each distinct card layer once
        for (const auto& r : resolved) {
            if (r.layer < 0 || r.layer >= int(kLayers))
                continue;
            bool alreadyLoaded = false;
            for (const auto& other : resolved) {
                if (&other == &r) break;
                if (other.layer == r.layer) { alreadyLoaded = true; break; }
            }
            if (!alreadyLoaded) {
                const std::string path = cardDir + r.file;
                std::vector<uint8_t> raw;
                int cw = 0, ch = 0;
                if (!voxel::loadPngRGBA8(path, raw, cw, ch)) {
                    spdlog::warn("texture atlas: foliage card '{}' missing - "
                                 "no card for texture on material {}", r.file, r.mat);
                    continue;
                }
                resample(raw, cw, ch, layers[size_t(r.layer)]);
                spdlog::info("texture atlas: foliage card '{}' -> layer {} "
                             "(texture on material {})", r.file, r.layer, r.mat);
            }
            m_cardLayer[r.mat] = r.layer;
        }
    }

    // Legacy fallback: old manifests carry a top-level "foliageCards" table
    // keyed by ground material. Treat each entry as a card on that material's
    // (possibly absent) texture binding, so existing sceneries keep rendering
    // while migrating. The shader's sampled-slot gate still decides whether
    // the card actually grows, so an untextured material's card is inert.
    if (!disabled) {
        std::vector<voxel::worldfile::ResolvedFoliageCard> legacy;
        const std::string cardsJson2 =
            voxel::scenery::resolveShared(dir + "/", "textures/cards/cards.json");
        std::string cardDir2 = cardsJson2;
        {
            const size_t s = cardDir2.find_last_of("/\\");
            cardDir2 = s == std::string::npos ? std::string() : cardDir2.substr(0, s + 1);
        }
        if (voxel::worldfile::loadFoliageCardManifest(manifestPath, cardsJson2, legacy)) {
            for (const auto& c : legacy) {
                if (c.mat < 0 || c.mat >= int(kLayers) || c.layer < 0 || c.layer >= int(kLayers))
                    continue;
                if (m_cardLayer[c.mat] >= 0)
                    continue; // already bound via the textures table
                bool alreadyLoaded = (layers[size_t(c.layer)].size() > 0);
                if (!alreadyLoaded) {
                    // find the file in the library again - loadFoliageCardManifest
                    // already gave us c.file
                    const std::string path = cardDir2 + c.file;
                    std::vector<uint8_t> raw;
                    int cw = 0, ch = 0;
                    if (voxel::loadPngRGBA8(path, raw, cw, ch)) {
                        resample(raw, cw, ch, layers[size_t(c.layer)]);
                        spdlog::info("texture atlas: legacy foliage card '{}' -> layer {} "
                                     "(ground material {})", c.file, c.layer, c.mat);
                    }
                }
                m_cardLayer[c.mat] = c.layer;
            }
        }
    }

    if (disabled)
        for (int& s : m_slot)
            s = -1;
    // VF_TEXTURES=0 is the bit-exact escape hatch: it must disable emission
    // as well as sampling, or an emissive binding would still add light.
    if (disabled)
        for (float& e : m_emis)
            e = 0.0f;
    m_tableDirty = true;

    // Mean colour of every decoded layer (linear 0..1): the COLOUR a derived
    // point light uses for an emissive material, so a cyan-glowing texture
    // lights the room cyan instead of in the palette's ember orange. A layer
    // with no texture keeps black and therefore derives no light.
    for (uint32_t m = 0; m < kLayers; ++m) {
        m_mean[m] = glm::vec3(0.0f);
        if (layers[m].empty())
            continue;
        double sr = 0.0, sg = 0.0, sb = 0.0;
        const size_t px = size_t(kTexSize) * kTexSize;
        for (size_t i = 0; i < px; ++i) {
            sr += layers[m][4 * i + 0];
            sg += layers[m][4 * i + 1];
            sb += layers[m][4 * i + 2];
        }
        const float inv = 1.0f / (255.0f * float(px));
        m_mean[m] = glm::vec3(float(sr), float(sg), float(sb)) * inv;
    }

    // one-time resources: fixed layer count keeps the view stable across
    // reloads, so the descriptor only needs a single write.
    if (m_image.img == VK_NULL_HANDLE) {
        m_mips = 1;
        for (uint32_t s = kTexSize; s > 1; s >>= 1)
            ++m_mips;
        m_image = makeTexture2DArray(ctx, kTexSize, kTexSize, kLayers, m_mips,
                                     VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!m_image.img)
            return false;

        VkSamplerCreateInfo si { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = float(m_mips);
        if (vkCreateSampler(ctx.device(), &si, nullptr, &m_sampler) != VK_SUCCESS) {
            spdlog::critical("texture atlas: sampler creation failed");
            destroy();
            return false;
        }

        m_tableBuf = makeBuffer(ctx, sizeof(float) * 4 * kLayers,
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                VMA_MEMORY_USAGE_CPU_TO_GPU, true);
        if (!m_tableBuf.buf) {
            spdlog::critical("texture atlas: table UBO failed");
            destroy();
            return false;
        }
    }

    // build the full mip-0 staging buffer: declared layers get their texture,
    // every other layer gets a neutral mid-grey filler (never sampled: its
    // slot is -1).
    const size_t layerBytes = size_t(kTexSize) * kTexSize * 4;
    std::vector<uint8_t> staging(layerBytes * kLayers, 128);
    for (uint32_t m = 0; m < kLayers; ++m)
        if (!layers[m].empty())
            std::copy(layers[m].begin(), layers[m].end(),
                      staging.begin() + size_t(m) * layerBytes);

    Buffer stagingBuf = makeBuffer(ctx, VkDeviceSize(staging.size()),
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VMA_MEMORY_USAGE_CPU_ONLY, true);
    if (!stagingBuf.buf || !stagingBuf.mapped) {
        spdlog::critical("texture atlas: staging buffer failed");
        destroy();
        return false;
    }
    std::memcpy(stagingBuf.mapped, staging.data(), staging.size());

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        // upload mip 0 for every layer
        transitionImage(cmd, m_image.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        kLayers);
        for (uint32_t m = 0; m < kLayers; ++m) {
            VkBufferImageCopy c {};
            c.bufferOffset = VkDeviceSize(m) * VkDeviceSize(layerBytes);
            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m, 1 };
            c.imageExtent = { kTexSize, kTexSize, 1 };
            vkCmdCopyBufferToImage(cmd, stagingBuf.buf, m_image.img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        // blit the mip chain per layer (box filter via the blit hardware)
        uint32_t w = kTexSize;
        for (uint32_t mip = 1; mip < m_mips; ++mip) {
            const uint32_t nw = std::max(w >> 1, 1u);
            for (uint32_t m = 0; m < kLayers; ++m) {
                VkImageBlit bl {};
                bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, m, 1 };
                bl.srcOffsets[0] = { 0, 0, 0 };
                bl.srcOffsets[1] = { int(w), int(w), 1 };
                bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, m, 1 };
                bl.dstOffsets[0] = { 0, 0, 0 };
                bl.dstOffsets[1] = { int(nw), int(nw), 1 };
                vkCmdBlitImage(cmd, m_image.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               m_image.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                               VK_FILTER_LINEAR);
            }
            w = nw;
        }
        transitionImage(cmd, m_image.img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT, kLayers);
    });
    destroyBuffer(ctx, stagingBuf);

    // refresh the UBO table
    if (m_tableBuf.mapped) {
        float tab[kLayers * 4] = {};
        fillTable(tab);
        std::memcpy(m_tableBuf.mapped, tab, sizeof(tab));
    }
    m_tableDirty = false;
    return true;
}

void TexAtlas::fillTable(float* outNx4) const
{
    for (int m = 0; m < int(kLayers); ++m) {
        float* r = &outNx4[m * 4];
        r[0] = float(m_slot[m]);
        r[1] = m_scale[m];
        r[2] = m_emis[m]; // emissive scale, 0 = the texture does not emit
        r[3] = float(m_cardLayer[m]); // authored foliage card, -1 = none
    }
}

} // namespace vf
