// The sidebar's render section: the render-flag bits, the surfel/LOD knobs, and the splat tuning.
#include "app/app.hpp"

#include "app/sun_angles.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/sun_time.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include <spdlog/spdlog.h>

#include <imgui_impl_vulkan.h>

namespace vf {
namespace app {

void App::drawPanelRender()
{
    sectionHeader("RENDERING", "F switches renderer");

    if (ImGui::BeginTable("RendererButtons", 2, ImGuiTableFlags_SizingStretchSame)) {
        const bool splats = m_renderMode == RenderMode::Splats;
        if (actionButton("Gaussian surfels", splats, ImVec2(-1.0f, 32.0f)))
            m_renderMode = RenderMode::Splats;
        ImGui::TableNextColumn();
        if (actionButton("SVO reference", !splats, ImVec2(-1.0f, 32.0f)))
            m_renderMode = RenderMode::Svo;
        ImGui::EndTable();
    }

    ImGui::SeparatorText("View");
    const glm::vec3 fwd = m_camera.forward();
    float heading = glm::degrees(std::atan2(fwd.x, fwd.z));
    if (heading < 0.0f)
        heading += 360.0f;
    const char* cardinal = (heading < 22.5f || heading >= 337.5f) ? "N" :
                          (heading < 67.5f) ? "NE" :
                          (heading < 112.5f) ? "E" :
                          (heading < 157.5f) ? "SE" :
                          (heading < 202.5f) ? "S" :
                          (heading < 247.5f) ? "SW" :
                          (heading < 292.5f) ? "W" : "NW";
    ImGui::Text("Pos  %.1f, %.1f, %.1f", m_camera.pos.x, m_camera.pos.y,
                m_camera.pos.z);
    ImGui::Text("Head %.0f deg %s", heading, cardinal);
    ImGui::TextDisabled("%.1f ms CPU  |  frame %.1f ms", m_avgMs, m_lastFrameMs);
    ImGui::TextDisabled("gpu geo %.1f  post %.1f  fx %.1f  taa %.1f",
                        m_profAvg[0], m_profAvg[1], m_profAvg[2], m_profAvg[3]);

    // ---- Sun: the one control for day / dusk / night -----------------------
    // m_sunDir is the whole source of truth (it rides the per-frame push
    // constant), so the sky and direct light follow the drag immediately;
    // what lags is the CPU-baked per-surfel SUN SHADOW, which is rebuilt on
    // release rather than per tick - a full world reload during a drag would
    // stall the frame loop. Angles are read back out of m_sunDir rather than
    // kept in parallel floats, so --sun / world.json "sun" and this slider can
    // never disagree about where the sun is.
    ImGui::SeparatorText("Sun");
    {
        float elev = glm::degrees(std::asin(glm::clamp(m_sunDir.y, -1.0f, 1.0f)));
        float azim = glm::degrees(std::atan2(m_sunDir.x, m_sunDir.z));
        if (azim < 0.0f)
            azim += 360.0f;

        // Coarse day/night switch; the sliders below stay the fine control.
        // The active button is derived from m_sunDir rather than a parallel
        // flag, so dragging the sliders can never leave the highlight lying -
        // the same single-source-of-truth rule the angles above already use.
        // Active state and the P hotkey both go through sunIsNight(), so the
        // highlight cannot disagree with the key about the current phase.
        const bool isNight = sunIsNight(elev);
        if (ImGui::BeginTable("SunPhaseButtons", 2, ImGuiTableFlags_SizingStretchSame)) {
            if (actionButton("Day", !isNight, ImVec2(-1.0f, 28.0f)))
                setSunPhase(false);
            ImGui::TableNextColumn();
            if (actionButton("Night", isNight, ImVec2(-1.0f, 28.0f)))
                setSunPhase(true);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("P toggles  |  session only, world.json untouched");

        // Write-only clock: HH:MM commits through setSunTime (setSunAngles +
        // one reload), exactly like the preset buttons. The readout below is
        // a best-effort inverse of m_sunDir with "~" when the sun sits off
        // the clock arc (presets, slider drags); it never feeds a write back.
        // Note: submitting a displayed read-back always sets the ARC value,
        // so Enter on the default sun's "15:52" moves it to 31.8 deg.
        ImGui::SetNextItemWidth(-1.0f);
        static char sunTimeBuf[8] = "";
        if (ImGui::InputText("Time##sunTime", sunTimeBuf, sizeof(sunTimeBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            float h = 0.0f;
            if (parseSunTime(sunTimeBuf, h))
                setSunTime(h);
            else
                spdlog::warn("sun time '{}' ignored (use HH:MM)", sunTimeBuf);
        }
        {
            float th = 0.0f;
            bool approx = false;
            sunTimeForAngles(elev, azim, th, approx);
            char disp[8] = "";
            formatSunTime(th, disp, sizeof(disp));
            if (approx)
                ImGui::TextDisabled("~%s (sun is off the clock arc)", disp);
            else
                ImGui::TextDisabled("%s", disp);
        }

        ImGui::SetNextItemWidth(-1.0f);
        const bool elevEdit =
            ImGui::SliderFloat("Elevation##sunElev", &elev, -60.0f, 90.0f, "%.1f deg");
        const bool elevRel = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(-1.0f);
        const bool azimEdit =
            ImGui::SliderFloat("Azimuth##sunAzim", &azim, 0.0f, 360.0f, "%.1f deg");
        const bool azimRel = ImGui::IsItemDeactivatedAfterEdit();

        if (elevEdit || azimEdit) {
            setSunAngles(elev, azim); // shared conversion, see run.cpp
        }
        if (elevRel || azimRel)
            requestWorldReload(); // rebake sun shadows with the new sun

        const char* phase = elev > kSunDayElevThreshold ? "Day"
                           : elev > kSunNightElevThreshold ? "Dusk / dawn"
                                                            : "Night";
        ImGui::TextDisabled("%s  |  %.0f deg up, %.0f deg", phase, elev, azim);
        if (sunIsNight(elev))
            ImGui::TextDisabled("Moon + authored lights only (bit 8 keeps caves dark)");
    }

    ImGui::SeparatorText("Lights");
    {
        // Global emitter budget N + per-pixel nearest-K: both re-upload the
        // binding-25 UBO only, so dragging never triggers a world reload.
        // The per-pixel cost ceiling is K visibility marches, not N.
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("Emitter budget##lightBudget", &m_lightBudget, 16, 256,
                             "%d")) {
            m_lightBudget = std::clamp(m_lightBudget, 1, 256);
            uploadLightSources();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Global emitter cap: authored first, derived fill. "
                              "UBO patch only, no reload.");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("Per-pixel lights##lightK", &m_lightK, 1, 8, "%d")) {
            m_lightK = std::clamp(m_lightK, 1, 8);
            uploadLightSources();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Nearest-K shaded per pixel: cost ceiling is K "
                              "marches, independent of the budget.");
    }

    if (m_hasSelection) {
        ImGui::SeparatorText("Selection");
        ImGui::Text("Voxel %d, %d, %d", m_selectedHit.voxel.x,
                    m_selectedHit.voxel.y, m_selectedHit.voxel.z);
        if (m_selectedLayer.empty())
            ImGui::TextDisabled("Terrain / unowned live geometry");
        else
            ImGui::TextColored(kAccent, "Layer %s", m_selectedLayer.c_str());
    } else {
        ImGui::SeparatorText("Selection");
        ImGui::TextWrapped("Ctrl + LMB picks a voxel and resolves its exact .vxw owner.");
    }
    if (m_hoverHit.hit && m_hoverHit.object) {
        const std::string hoverLayer = m_layers.layerFile(m_hoverHit.layer);
        if (!hoverLayer.empty())
            ImGui::TextDisabled("Hover owner: %s", hoverLayer.c_str());
    }

    ImGui::SeparatorText("Effects");
    auto flagToggle = [&](const char* label, int mask) {
        bool enabled = (m_renderFlags & mask) != 0;
        if (ImGui::Checkbox(label, &enabled)) {
            if (enabled)
                m_renderFlags |= mask;
            else
                m_renderFlags &= ~mask;
        }
    };
    if (ImGui::BeginTable("RenderToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
        flagToggle("Ambient occlusion", 1 << 0);
        ImGui::TableNextColumn();
        flagToggle("Shadows", 1 << 1);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Flora shading", 1 << 2);
        ImGui::TableNextColumn();
        flagToggle("Water", 1 << 3);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Reflections (G)", 1 << 5);
        ImGui::TableNextColumn();
        flagToggle("SSAO (H)", 1 << 6);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        flagToggle("Detail normals (B)", 1 << 7);
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("TAA (N)", &m_taaEnabled))
            m_taaFirstFrame = true;
        ImGui::EndTable();
    }
    // Fog / motion blur / DoF are separate opt-in members, not render-flag
    // bits (bit 4 is the unused outline flag and stays unexposed).
    ImGui::Checkbox("Volumetric fog (J)", &m_volFogEnabled);
    ImGui::Checkbox("Motion blur (K)", &m_motionBlurEnabled);
    ImGui::Checkbox("Depth of field (L)", &m_dofEnabled);

    ImGui::SeparatorText("Surfels");
    if (m_renderMode == RenderMode::Splats) {
        ImGui::SetNextItemWidth(-1.0f);
        float radius = m_splatPass.radiusScale();
        if (ImGui::SliderFloat("Splat radius##radius", &radius, 0.5f, 2.0f, "%.2f"))
            m_splatPass.setRadiusScale(radius);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Sharp-edge fit##edgeShrink", &m_edgeShrink, 0.0f,
                           0.8f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            requestWorldReload();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reduce only opaque parents on genuine voxel edges; "
                              "smooth curvature keeps full coverage. Rebuilds.");
        if (ImGui::Checkbox("Interpolate crease splats", &m_edgeFill))
            requestWorldReload();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 4.0f, "%.2f");

    if (m_scenePreview) {
        if (!m_sceneTexId)
            m_sceneTexId = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                m_uiSampler, m_offscreen.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ImGui::SeparatorText("Scene preview");
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Image(m_sceneTexId, ImVec2(w, w * 9.0f / 16.0f));
    }

    if (ImGui::CollapsingHeader("Keyboard")) {
        ImGui::BulletText("WASD move; Q/E down/up");
        ImGui::BulletText("RMB drag: look; wheel: speed");
        ImGui::BulletText("Ctrl+LMB: pick exact voxel / layer");
        ImGui::BulletText("Tab: collapse sidebar; Ctrl+1..6: section");
        ImGui::BulletText("Rotate: click object, then drag a trackball ring");
        ImGui::BulletText("C: arm brush; F: renderer; N: TAA");
        ImGui::BulletText("B/G/H/J/K/L: visual effects");
        ImGui::BulletText("[/]: splat radius; M: Move brush");
        ImGui::BulletText("Ctrl+Z: undo last stroke");
        ImGui::BulletText("Window close button: quit");
    }
}

} // namespace app
} // namespace vf
