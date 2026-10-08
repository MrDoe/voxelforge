// GPU uploads derived from the loaded world: the terrain height texture
// (rg32f = top world Y + material), the coarse object volume the splat
// water path marches for shadows, and the 64^3 irradiance volume (binding 26).
#include "app/app.hpp"
#include "voxel/irradiance_volume.hpp"

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

// Coarse irradiance volume for binding 26: bake the emitter set (the SAME
// LightUBO the direct term consumes, passed in by uploadLightSources at its
// tail so the two can never disagree about which lights exist) and upload it
// as a sampled 64^3 RGBA32F. Three invariants, all learned the hard way here:
//
//  1. The image is created ONCE and only ever re-uploaded. Recreating would
//     free the VkImageView the descriptor set already holds - the same defect
//     that made repeated world toggles read freed terrain memory. So the
//     zeros go in first, and the descriptor writes live INSIDE the create
//     branch, meaning a view can never be bound before it has contents, and
//     no reload ever rewrites the descriptor.
//  2. RGBA32F, not the 2 MB RGBA16F the header notes as the obvious size: the
//     bake's cells are already glm::vec4, so this upload is a byte copy with
//     NO packing step that could transpose a channel or flip a sign. This
//     feature already shipped one coordinate-frame defect that every test
//     agreed with; a second one hiding in a format conversion is not a risk
//     worth 2 MiB at 64^3.
//  3. uploadToImage3D ends in SHADER_READ_ONLY_OPTIMAL, which is exactly the
//     layout the descriptor declares - no extra transition (the height and
//     objvol images need one because they are storage images bound as
//     GENERAL; this one is sampled).
void App::uploadIrradianceVolume(const vf::voxel::worldfile::LightUBO& lights)
{
    constexpr int kN = vf::voxel::IrradianceVolume::kN;
    constexpr size_t kCells = size_t(kN) * size_t(kN) * size_t(kN);

    if (m_irrImg.img == VK_NULL_HANDLE) {
        m_irrImg = vf::makeImage3D(m_ctx, uint32_t(kN), uint32_t(kN), uint32_t(kN),
                                   VK_FORMAT_R32G32B32A32_SFLOAT,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!m_irrImg.img) {
            spdlog::error("irradiance volume: image creation failed - binding 26 "
                          "left unbound, the term reads as absent");
            return;
        }
        // Defined content before the view can be referenced. An all-zero
        // volume makes irradianceVolume() read exactly 0.0, which sends the
        // shader down its fallback branch to the old analytic stand-in - so
        // "not baked yet" renders as the pre-feature frame rather than black.
        const std::vector<glm::vec4> zeros(kCells, glm::vec4(0.0f));
        if (!vf::uploadToImage3D(m_ctx, m_irrImg, zeros.data(),
                                 zeros.size() * sizeof(glm::vec4))) {
            spdlog::error("irradiance volume: zero upload failed - binding 26 "
                          "left unbound, the term reads as absent");
            return;
        }
        // Written exactly once, here: both descriptor sets get a view that is
        // guaranteed populated. Reloads re-upload through the same view.
        m_splatPass.setIrrVolView(m_irrImg.view);
        m_svoPass.setIrrVolView(m_irrImg.view);
    }

    vf::voxel::IrradianceBakeStats st;
    const vf::voxel::IrradianceVolume vol =
        vf::voxel::buildIrradianceVolume(m_layers.field(), lights,
                                         glm::vec3(m_sunDir), &st);
    if (vol.size() != kCells) {
        spdlog::error("irradiance volume: bake produced {} of {} cells - "
                      "keeping the zero upload (term reads as absent)",
                      vol.size(), kCells);
        return;
    }
    if (!vf::uploadToImage3D(m_ctx, m_irrImg, vol.cells.data(),
                             vol.cells.size() * sizeof(glm::vec4))) {
        spdlog::error("irradiance volume: upload failed");
        return;
    }
    // Logged at this call site rather than inside the bake (the bake is
    // shared with tests that must not depend on the logger), and always -
    // a full 16/16 must be readable in the log, never inferred, because a
    // truncated emitter otherwise has no symptom anywhere.
    spdlog::info("irradiance volume: {}x{}x{} cells, {} emitters seen / {} used, "
                 "{} cells lit",
                 kN, kN, kN, st.seen, st.used, st.cellsLit);
}

} // namespace app
} // namespace vf
