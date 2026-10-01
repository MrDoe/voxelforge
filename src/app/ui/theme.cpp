// ImGui bring-up: the context, the dense editor theme, the GLFW/Vulkan
// backends, the font atlas, and the linear sampler the HUD's live scene
// thumbnail samples.
#include "app/app.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The theme tokens stay local to ImGui, so neither the world nor the renderers
// are affected by the editor skin.
void App::initImGui()
{
    const Args& args = m_args;
    // ImGui ---------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    // Dense professional editor theme: midnight surfaces, one cyan action
    // colour, restrained borders. Tokens stay local to ImGui so rendering
    // and world content are unaffected.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.CellPadding = ImVec2(7.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 10.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.055f, 0.090f, 0.96f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.070f, 0.110f, 0.96f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.070f, 0.110f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.30f, 0.40f, 0.55f);
    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.94f, 0.98f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.57f, 0.67f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.070f, 0.105f, 0.155f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.085f, 0.190f, 0.245f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.075f, 0.310f, 0.385f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.035f, 0.055f, 0.090f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.055f, 0.145f, 0.205f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.065f, 0.160f, 0.220f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.080f, 0.310f, 0.390f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.090f, 0.390f, 0.480f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.075f, 0.145f, 0.215f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.080f, 0.300f, 0.380f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.090f, 0.420f, 0.510f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.13f, 0.83f, 0.93f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.65f, 0.76f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.18f, 0.88f, 0.96f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.30f, 0.40f, 0.65f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.060f, 0.105f, 0.155f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.16f, 0.25f, 0.34f, 0.75f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.040f, 0.065f, 0.100f, 1.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.050f, 0.080f, 0.120f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.08f, 0.36f, 0.45f, 0.55f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.13f, 0.83f, 0.93f, 0.65f);
    ImGui_ImplGlfw_InitForVulkan(m_window.handle(), true);

    ImGui_ImplVulkan_InitInfo vi {};
    vi.Instance = m_ctx.instance();
    vi.PhysicalDevice = m_ctx.physicalDevice();
    vi.Device = m_ctx.device();
    vi.QueueFamily = m_ctx.graphicsFamily();
    vi.Queue = m_ctx.graphicsQueue();
    vi.MinImageCount = 3;
    vi.ImageCount = uint32_t(m_swapchain.imageCount());
    vi.DescriptorPoolSize = 128;
    vi.UseDynamicRendering = true;
    vi.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    vi.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineRenderingCreateInfo.pColorAttachmentFormats = m_swapchain.formatPtr();
    vi.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS)
            spdlog::error("ImGui Vulkan backend error {}", int(r));
    };
    ImGui_ImplVulkan_Init(&vi);
    ImGui_ImplVulkan_CreateFontsTexture();

    // linear sampler for UI textures (e.g. the live scene-preview thumbnail)
    VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx.device(), &sci, nullptr, &m_uiSampler);
    m_scenePreview = getenv("VF_SCENE_PREVIEW") != nullptr;

    // AI chat
    if (!m_chatInitialized) {
        m_chatUi.init(args.llmUrl, args.llmModel);
        m_chatInitialized = true;
    }
}

} // namespace app
} // namespace vf
