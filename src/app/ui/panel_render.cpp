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
    sectionHeader("F switches renderer");

    if (groupHeader("render.backend", "BACKEND")) {
        if (ImGui::BeginTable("RendererButtons", 2, ImGuiTableFlags_SizingStretchSame)) {
            const bool splats = m_renderMode == RenderMode::Splats;
            if (actionButton("Gaussian surfels", splats, ImVec2(-1.0f, kUiRowH + 2.0f)))
                m_renderMode = RenderMode::Splats;
            ImGui::TableNextColumn();
            if (actionButton("SVO reference", !splats, ImVec2(-1.0f, kUiRowH + 2.0f)))
                m_renderMode = RenderMode::Svo;
            ImGui::EndTable();
        }
        hintLine("Gaussian surfels are the production path; the SVO backend is "
                 "the pixel reference.");
        groupEnd();
    }

    if (groupHeader("render.view", "VIEW")) {
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
        // Readouts as DATA: label left, value right, so the numbers line up
        // in a column and can be compared between frames.
        keyValue("Position", "%.1f, %.1f, %.1f", m_camera.pos.x, m_camera.pos.y,
                 m_camera.pos.z);
        keyValue("Heading", "%.0f deg %s", heading, cardinal);
        keyValue("Frame", "%.1f ms", m_avgMs);
        keyValue("GPU", "geo %.1f · post %.1f · fx %.1f · taa %.1f", m_profAvg[0],
                 m_profAvg[1], m_profAvg[2], m_profAvg[3]);
        // The movement speed lives HERE, as an explicit setting with a
        // readout. It used to be the mouse wheel, which changed it with no
        // indication anywhere: a scroll over the sidebar silently made the
        // camera faster or slower, and nothing on screen said which.
        controlRow("Speed", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##camSpeed", &m_camera.speed, 0.5f, 40.0f,
                               "%.1f m/s");
        });
        hintLine("Shift flies 3.5x faster, Ctrl 4x slower.");
        groupEnd();
    }

    // ---- Sun: the one control for day / dusk / night -----------------------
    // m_sunDir is the whole source of truth (it rides the per-frame push
    // constant), so the sky and direct light follow the drag immediately;
    // what lags is the CPU-baked per-surfel SUN SHADOW, which is rebuilt on
    // release rather than per tick - a full world reload during a drag would
    // stall the frame loop. Angles are read back out of m_sunDir rather than
    // kept in parallel floats, so --sun / world.json "sun" and this slider can
    // never disagree about where the sun is.
    if (groupHeader("render.sun", "SUN")) {
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
            if (actionButton("Day", !isNight, ImVec2(-1.0f, kUiRowH - 2.0f)))
                setSunPhase(false);
            ImGui::TableNextColumn();
            if (actionButton("Night", isNight, ImVec2(-1.0f, kUiRowH - 2.0f)))
                setSunPhase(true);
            ImGui::EndTable();
        }
        hintLine("P toggles  |  session only, world.json untouched");

        // Write-only clock: HH:MM commits through setSunTime (setSunAngles +
        // one reload), exactly like the preset buttons. The readout below is
        // a best-effort inverse of m_sunDir with "~" when the sun sits off
        // the clock arc (presets, slider drags); it never feeds a write back.
        // Note: submitting a displayed read-back always sets the ARC value,
        // so Enter on the default sun's "15:52" moves it to 31.8 deg.
        static char sunTimeBuf[8] = "";
        controlRow("Time", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##sunTime", sunTimeBuf, sizeof(sunTimeBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                float h = 0.0f;
                if (parseSunTime(sunTimeBuf, h))
                    setSunTime(h);
                else
                    spdlog::warn("sun time '{}' ignored (use HH:MM)", sunTimeBuf);
            }
        });
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

        bool elevEdit = false, elevRel = false, azimEdit = false, azimRel = false;
        controlRow("Elevation", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            elevEdit = ImGui::SliderFloat("##sunElev", &elev, -60.0f, 90.0f,
                                         "%.1f deg");
            elevRel = ImGui::IsItemDeactivatedAfterEdit();
        });
        controlRow("Azimuth", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            azimEdit = ImGui::SliderFloat("##sunAzim", &azim, 0.0f, 360.0f,
                                          "%.1f deg");
            azimRel = ImGui::IsItemDeactivatedAfterEdit();
        });

        if (elevEdit || azimEdit) {
            setSunAngles(elev, azim); // shared conversion, see run.cpp
        }
        if (elevRel || azimRel)
            requestWorldReload(); // rebake sun shadows with the new sun

        const char* phase = elev > kSunDayElevThreshold ? "Day"
                           : elev > kSunNightElevThreshold ? "Dusk / dawn"
                                                            : "Night";
        keyValueColored("Phase", kAccent, "%s", phase);
        keyValue("Elevation", "%.0f deg", elev);
        keyValue("Azimuth", "%.0f deg", azim);
        if (sunIsNight(elev))
            hintLine("Moon + authored lights only (bit 8 keeps caves dark)");
        groupEnd();
    }

    if (groupHeader("render.lights", "LIGHTS")) {
        // Global emitter budget N + per-pixel nearest-K: both re-upload the
        // binding-25 UBO only, so dragging never triggers a world reload.
        // The per-pixel cost ceiling is K visibility marches, not N.
        controlRow("Emitter budget", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##lightBudget", &m_lightBudget, 16, 256, "%d")) {
                m_lightBudget = std::clamp(m_lightBudget, 1, 256);
                uploadLightSources();
            }
        });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Global emitter cap: authored first, derived fill. "
                              "UBO patch only, no reload.");
        controlRow("Per-pixel lights", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##lightK", &m_lightK, 1, 8, "%d")) {
                m_lightK = std::clamp(m_lightK, 1, 8);
                uploadLightSources();
            }
        });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Nearest-K shaded per pixel: cost ceiling is K "
                              "marches, independent of the budget.");
        groupEnd();
    }

    if (groupHeader("render.selection", "SELECTION")) {
        if (m_hasSelection) {
            keyValue("Voxel", "%d, %d, %d", m_selectedHit.voxel.x,
                     m_selectedHit.voxel.y, m_selectedHit.voxel.z);
            if (m_selectedLayer.empty())
                ImGui::TextDisabled("Terrain / unowned live geometry");
            else
                ImGui::TextColored(kAccent, "Layer %s", m_selectedLayer.c_str());
        } else {
            hintLine("Ctrl + LMB picks a voxel and resolves its exact .vxw owner.");
        }
        if (m_hoverHit.hit && m_hoverHit.object) {
            const std::string hoverLayer = m_layers.layerFile(m_hoverHit.layer);
            if (!hoverLayer.empty())
                ImGui::TextDisabled("Hover owner: %s", hoverLayer.c_str());
        }
        groupEnd();
    }

    if (groupHeader("render.effects", "EFFECTS")) {
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
        groupEnd();
    }

    if (groupHeader("render.surfels", "SURFELS")) {
        if (m_renderMode == RenderMode::Splats) {
            float radius = m_splatPass.radiusScale();
            controlRow("Splat radius", [&] {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##radius", &radius, 0.5f, 2.0f, "%.2f"))
                    m_splatPass.setRadiusScale(radius);
            });
            controlRow("Sharp-edge fit", [&] {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##edgeShrink", &m_edgeShrink, 0.0f, 0.8f,
                                   "%.2f");
            });
            if (ImGui::IsItemDeactivatedAfterEdit())
                requestWorldReload();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reduce only opaque parents on genuine voxel edges; "
                                  "smooth curvature keeps full coverage. Rebuilds.");
            if (ImGui::Checkbox("Interpolate crease splats", &m_edgeFill))
                requestWorldReload();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bridge the crease between a shrunk parent and its "
                                  "neighbour. Off by default: with edgeShrink 0.25 "
                                  "turning this off moves ~1.2%% of pixels by >12 at "
                                  "the house view (a coverage/complexity trade, not "
                                  "a free off switch). Rebuilds.");
            controlRow("Crease splat size", [&] {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##edgeSize", &m_edgeBridgeSize, 0.0f, 2.0f,
                                   "%.2f");
            });
            if (ImGui::IsItemDeactivatedAfterEdit())
                requestWorldReload();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Size of edge/corner fill splats. Rebuilds.");
            if (ImGui::Checkbox("Fill corner splats", &m_cornerFill))
                requestWorldReload();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cap corner vertices the parent disk does not "
                                  "reach (thin shells, concave corners); covered "
                                  "convex corners are skipped. Off by default - "
                                  "measured no visible change on the hamlet "
                                  "(stays inside the rerun noise floor). Rebuilds.");
        } else {
            hintLine("Splat tuning applies to the Gaussian backend; the SVO "
                     "reference draws the same world differently.");
        }
        controlRow("Exposure", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##exposure", &m_exposure, 0.1f, 4.0f, "%.2f");
        });
        groupEnd();
    }
    // ---- Fur coat: the hair cards over the micro objects ------------------
    // The coat is a CPU product (the cards are grown from the field's
    // surface cells), so every control here REBUILDS it - and it applies on
    // RELEASE, not per tick, because growing the alpaca's coat costs a few
    // hundred milliseconds.
    if (groupHeader("render.fur", "FUR COAT", false)) {
        bool furDirty = false;
        controlRow("Enabled", [&] {
            if (ImGui::Checkbox("##furEnabled", &m_furEnabled))
                furDirty = true;
        });
        hintLine("The hair cards over a micro object. Off is the bit-exact "
                 "escape hatch: the pass never dispatches.");
        controlRow("Density", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##furPerCell", &m_furPerCell, 1, 12,
                                 "%d / cell"))
                furDirty = true;
        });
        hintLine("Cards per surface cell. Two left gaps at close range.");
        controlRow("Strand length", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##furLen", &m_furLength, 0.008f, 0.060f,
                                   "%.0f mm", ImGuiSliderFlags_Logarithmic))
                furDirty = true;
        });
        controlRow("Clump width", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##furWid", &m_furWidth, 0.001f, 0.012f,
                                   "%.1f mm", ImGuiSliderFlags_Logarithmic))
                furDirty = true;
        });
        controlRow("Curl", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##furCurl", &m_furCurl, 0.0f, 0.8f, "%.2f"))
                furDirty = true;
        });
        hintLine("How far the strand's centre line wanders, as a fraction of "
                 "its length. Measured against the CLUMP's width: a wave "
                 "that wanders many widths sideways is a smear, not a curl.");
        controlRow("Curl cycles", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##furCycles", &m_furCycles, 0.2f, 3.0f,
                                   "%.2f"))
                furDirty = true;
        });
        controlRow("Lift", [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##furTilt", &m_furTilt, 0.0f, 70.0f,
                                   "%.0f deg"))
                furDirty = true;
        });
        hintLine("How far the strand stands off the surface. A steep strand "
                 "reads as a spike; wool lies close to the body.");
        if (furDirty) {
            rebuildFurCards();
            hintLine("Coat rebuilt.");
        }
        groupEnd();
    }

    if (m_scenePreview) {
        if (groupHeader("render.preview", "SCENE PREVIEW")) {
            if (!m_sceneTexId)
                m_sceneTexId = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                    m_uiSampler, m_offscreen.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::Image(m_sceneTexId, ImVec2(w, w * 9.0f / 16.0f));
            groupEnd();
        }
    }

    // The keyboard reference is the one group that starts CLOSED: it is a
    // lookup, not a control, and 11 lines of it would push everything else
    // down the pane.
    if (groupHeader("render.keys", "KEYBOARD", false)) {
        ImGui::BulletText("WASD move; Q/E down/up");
        ImGui::BulletText("RMB drag: look; wheel: scroll the sidebar");
        ImGui::BulletText("Shift: 3.5x faster; Ctrl: 4x slower");
        ImGui::BulletText("Ctrl+LMB: pick exact voxel / layer");
        ImGui::BulletText("Tab: View/Edit; Ctrl+1..6: section");
        ImGui::BulletText("Rotate: click object, then drag a trackball ring");
        ImGui::BulletText("C: arm brush; F: renderer; N: TAA");
        ImGui::BulletText("B/G/H/J/K/L: visual effects");
        ImGui::BulletText("[/]: splat radius; M: Move brush");
        ImGui::BulletText("Ctrl+Z: undo last stroke");
        ImGui::BulletText("Window close button: quit");
        groupEnd();
    }
}

} // namespace app
} // namespace vf
