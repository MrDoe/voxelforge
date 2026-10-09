#pragma once
#include <glm/glm.hpp>
#include "rhi/context.hpp"
#include "rhi/resources.hpp"

namespace vf {

// Previous-frame camera basis, shared by every pass that reprojects history
// against it: TAA's resolve and the motion-blur velocity field.
//
// It MUST be the basis of the camera that rendered the PREVIOUS frame, so the
// app writes it at the END of a recorded frame (on both the headless and
// interactive paths) and seeds it once at startup. The default-constructed
// value is NOT a valid camera - it is pos = origin with right = +X, up = +Y,
// fwd = +Z and tanHalfFov = 1.0 - i.e. a camera sitting at the world origin
// looking down +Z. Reprojecting against it makes every pixel read as having
// travelled a long way, which is exactly the "motion blur smears the whole
// frame while nothing moves" symptom.
struct PrevCam {
    glm::vec3 pos = {0.0f, 0.0f, 0.0f};
    glm::vec3 right = {1.0f, 0.0f, 0.0f};
    glm::vec3 up = {0.0f, 1.0f, 0.0f};
    glm::vec3 fwd = {0.0f, 0.0f, 1.0f};
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
};
using TaaPrevCam = PrevCam; // historical name; both passes read the same state

class TaaPass {
public:
    bool init(const Context& ctx);
    void destroy();

    // current = offscreen after raymarch (GENERAL), history = previous frame's output (GENERAL),
    // out = resolve target (GENERAL), gpos = world-pos G-buffer (GENERAL, rgba16f)
    void updateDescriptors(VkImageView currentView, VkImageView historyView, VkImageView outView, VkImageView gposView);
    void record(VkCommandBuffer cmd, uint32_t width, uint32_t height, float historyBlend, bool firstFrame,
                const TaaPrevCam& prev) const;

private:
    const Context* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

} // namespace vf
