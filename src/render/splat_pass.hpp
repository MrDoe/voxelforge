#pragma once
#include "rhi/context.hpp"
#include "render/raymarch_pass.hpp" // RaymarchPush (shared push layout)
#include <glm/glm.hpp>
#include <vector>

namespace vf {

struct SplatVertexData {
    std::vector<glm::vec4> posRadius;  // xyz + radius
    std::vector<glm::vec4> colors;     // legacy baked (kept for compat)
    std::vector<glm::vec4> albedoAO;   // rgb albedo, a = AO
    std::vector<glm::vec4> normalMat;  // xyz normal (= shortest axis v), w = matId
    std::vector<glm::vec4> shadeParams; // xyz specular tint s, w = roughness ρ (GaussianShader)
    std::vector<float> shadow;          // per-splat shadow factor 0=lit 1=shadowed (minimal raytracing)
};

// Renders surface point-splats as Gaussian sprites into the offscreen image.
// v0 of the M7 gaussian-splat path: isotropic, unsorted, additive-look.
class SplatPass {
public:
    bool init(const Context& ctx, VkFormat colorFormat, const SplatVertexData& data);
    void destroy();

    void setOutputView(VkImageView view) { m_outView = view; }
    void record(VkCommandBuffer cmd, VkExtent2D extent, const RaymarchPush& push) const;
    size_t count() const { return m_count; }
    void updateSorting(const glm::vec3& camPos, const glm::vec3& camFwd);

private:
    bool createEnvCubemap();
    void destroyEnvCubemap();
    // HDR cubemap helpers
    static glm::vec3 cubemapDir(int face, float u, float v);
    static glm::vec3 sampleCubemapDir(const glm::vec3& dir,
                                      const std::vector<std::vector<glm::vec4>>& faces,
                                      int baseSize);
    bool loadOrGenerateEnvFaces(std::vector<std::vector<glm::vec4>>& outFaces, int size);

    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkBuffer m_buf = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    VkImageView m_outView = VK_NULL_HANDLE;
    size_t m_count = 0;
    std::vector<glm::vec4> m_origPos;
    std::vector<glm::vec4> m_origCol;
    std::vector<glm::vec4> m_origAlbedoAO;
    std::vector<glm::vec4> m_origNormalMat;
    std::vector<glm::vec4> m_origShadeParams;
    std::vector<float> m_origShadow;
    // sorting cache for high-density (avoid per-frame O(N log N) when camera static)
    glm::vec3 m_lastSortPos{1e9f};
    glm::vec3 m_lastSortFwd{0,0,1};
    uint64_t m_lastSortFrame = 0;

    // GaussianShader HDR env cubemap (6×64, GGX prefiltered mips)
    VkImage m_envCube = VK_NULL_HANDLE;
    VmaAllocation m_envAlloc = VK_NULL_HANDLE;
    VkImageView m_envView = VK_NULL_HANDLE;
    VkSampler m_envSampler = VK_NULL_HANDLE;
    uint32_t m_envMips = 1;
};

} // namespace vf
