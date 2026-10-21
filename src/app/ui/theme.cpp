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
    // A modern editor skin: one cool slate family, one cyan accent, and
    // FOUR surface levels (window < raised < control < overlay) so a control
    // can be told apart from the panel it sits in without a border doing all
    // the work. The accent is reserved for STATE - what is active, what is
    // armed, what is on - and is never used for a label, because a page where
    // everything is highlighted highlights nothing.
    //
    // The tokens stay local to ImGui: the world and the renderers are
    // unaffected by the editor skin.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.CellPadding = ImVec2(7.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 10.0f;
    style.GrabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 12.0f;
    ImVec4* colors = style.Colors;
    // surfaces: window < raised < control < overlay
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.067f, 0.086f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.071f, 0.086f, 0.110f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.086f, 0.102f, 0.129f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.137f, 0.169f, 0.212f, 1.00f);
    // text: primary / secondary / tertiary
    colors[ImGuiCol_Text] = ImVec4(0.902f, 0.929f, 0.953f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.427f, 0.478f, 0.541f, 1.00f);
    // controls
    colors[ImGuiCol_FrameBg] = ImVec4(0.090f, 0.106f, 0.133f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.122f, 0.153f, 0.192f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.149f, 0.192f, 0.239f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.067f, 0.086f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.098f, 0.137f, 0.176f, 1.00f);
    // the group band: a raised surface, one step above the panel
    colors[ImGuiCol_Header] = ImVec4(0.102f, 0.129f, 0.165f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.133f, 0.176f, 0.220f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.157f, 0.208f, 0.259f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.110f, 0.137f, 0.173f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.122f, 0.294f, 0.353f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.145f, 0.392f, 0.471f, 1.00f);
    // accent: state only
    colors[ImGuiCol_CheckMark] = ImVec4(0.204f, 0.804f, 0.925f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.180f, 0.616f, 0.722f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.204f, 0.804f, 0.925f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.129f, 0.157f, 0.196f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.090f, 0.110f, 0.141f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.137f, 0.169f, 0.212f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.071f, 0.086f, 0.110f, 1.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.086f, 0.106f, 0.133f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.122f, 0.294f, 0.353f, 0.65f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.204f, 0.804f, 0.925f, 0.65f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.204f, 0.804f, 0.925f, 1.00f);
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
