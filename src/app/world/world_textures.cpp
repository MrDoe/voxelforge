// GPU uploads derived from the loaded world: the terrain height texture
// (rg32f = top world Y + material) and the coarse object volume the splat
// water path marches for shadows.
#include "app/app.hpp"

namespace vf {
namespace app {

bool App::uploadTerrainTexture()
{
    const std::vector<glm::vec2>& htx = m_layers.field().heightTexture();
    const uint32_t lat = uint32_t(m_layers.field().latN());
    // The height texture is constant-size (latN x latN); create it once and
    // only re-upload contents on reload. Recreating would invalidate the
    // VkImageView bound by the (once-written) descriptor set, causing the
    // whole scene to read freed memory and crash on repeated toggles.
    if (m_heightImg.img == VK_NULL_HANDLE) {
        m_heightImg = vf::makeImage3D(m_ctx, lat, lat, 1,
                                      VK_FORMAT_R32G32_SFLOAT,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_heightImg.img)
            return false;
    }
    if (!vf::uploadToImage3D(m_ctx, m_heightImg, htx.data(),
                             htx.size() * sizeof(glm::vec2)))
        return false;
    m_heightCpu = htx; // mirror for live-edit patches (patchHeightTexture)
    m_ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_heightImg.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    });
    m_svoPass.setHeightmapView(m_heightImg.view);
    return true;
}

bool App::uploadObjVolTexture()
{
    const auto& ov = m_layers.field().objectVolume();
    const int n = vf::voxel::VoxelField::kObjVolN;
    // Constant-size (kObjVolN^3) volume; create once and re-upload only.
    // Recreating would free the VkImageView still referenced by the (once-
    // written) descriptor set, producing stale reads and memory exceptions.
    if (m_objVolImg.img == VK_NULL_HANDLE) {
        m_objVolImg = vf::makeImage3D(m_ctx, uint32_t(n), uint32_t(n), uint32_t(n),
                                      VK_FORMAT_R8_SNORM,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT);
        if (!m_objVolImg.img)
            return false;
    }
    if (!vf::uploadToImage3D(m_ctx, m_objVolImg, ov.data(), ov.size()))
        return false;
    m_ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_objVolImg.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    });
    m_svoPass.setObjVolumeView(m_objVolImg.view);
    return true;
}

} // namespace app
} // namespace vf
