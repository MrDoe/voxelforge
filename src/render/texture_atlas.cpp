#include "render/texture_atlas.hpp"
#include "voxel/heightmap.hpp"
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
        if (b.mat < 0 || b.mat >= int(kLayers))
            continue;
        std::vector<uint8_t> raw;
        int w = 0, h = 0;
        // absolute paths (tests) are used verbatim; relative ones resolve
        // against the manifest directory (assets/)
        const std::string path =
            (!b.file.empty() && (b.file[0] == '/' || b.file[1] == ':'))
                ? b.file
                : dir + b.file;
        if (!voxel::loadPngRGBA8(path, raw, w, h)) {
            spdlog::warn("texture atlas: '{}' for material {} failed - palette fallback",
                         b.file, b.mat);
            continue;
        }
        resample(raw, w, h, layers[size_t(b.mat)]);
        m_slot[b.mat] = b.mat;
        m_scale[b.mat] = b.scale > 0.02f ? b.scale : kDefaultScale;
        spdlog::info("texture atlas: material {} <- {} ({}x{}, {:.2f} m/tile)",
                     b.mat, b.file, w, h, m_scale[b.mat]);
    }
    if (disabled)
        for (int& s : m_slot)
            s = -1;
    m_tableDirty = true;

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
        r[2] = 0.0f;
        r[3] = 0.0f;
    }
}

} // namespace vf
