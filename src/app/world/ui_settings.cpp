// The world.json "ui" block as a live control surface.
//
// vf_mcp (and any other out-of-process author) cannot poke App members - it is
// a separate stdio process - but it CAN write the manifest, and the app polls
// that manifest's mtime every ~0.5 s (frame/run_poll.cpp). So UI settings are
// made MCP-changeable by declaring them as DATA: a flat, all-optional "ui"
// object whose fields mirror the sidebar's Render and Edit controls.
//
// Two guards make this safe to run next to a live GUI:
//  * a field is applied only when the file PARSED value differs from the
//    member's CURRENT value, and rebuilds fire only on an actual change, so
//    a GUI drag that moved a knob is never snapped back by an unrelated
//    manifest edit (the same lesson the session-only sun toggle learned);
//  * the seen-value snapshot (m_seenUi / m_seenSun*) distinguishes "the file
//    changed" from "the file exists" - an unchanged file never re-applies,
//    so Day/Night (P) and every other session-only toggle stay put until an
//    author actually writes a new value.
//
// Startup applies the block BEFORE the env overrides (applyStartupEnvOverrides
// right after it in frame/run.cpp), so an explicit VF_* flag still wins for a
// scripted A/B run - the manifest is authored intent, env is this-run intent.
#include "app/app.hpp"

#include "voxel/common.hpp"
#include "voxel/scenery.hpp"
#include "voxel/worldfile.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>

namespace vf {
namespace app {

namespace {

constexpr float kAbsent = vf::voxel::worldfile::UiSettings::kUiAbsent;

bool present(float v) { return v != kAbsent && std::isfinite(v); }
int  toInt(float v) { return int(std::lround(v)); }
bool toBool(float v) { return toInt(v) != 0; }

// The active scenery's folder name, for comparing against ui.scenery.
std::string currentSceneryName()
{
    return std::filesystem::path(vf::voxel::scenery::root())
        .filename()
        .string();
}

} // namespace

void App::seedUiSunFromDir()
{
    // Derive the angles from m_sunDir - the single source of truth (same
    // convention the Render panel's readout uses), never parallel floats.
    m_seenSunElev = std::asin(std::clamp(m_sunDir.y, -1.0f, 1.0f)) *
                    (180.0f / float(M_PI));
    float a = std::atan2(m_sunDir.x, m_sunDir.z) * (180.0f / float(M_PI));
    if (a < 0.0f)
        a += 360.0f;
    m_seenSunAzim = a;
}

void App::resetUiSettingsState()
{
    m_seenUi = vf::voxel::worldfile::UiSettings();
    seedUiSunFromDir();
}

void App::applyManifestSun()
{
    float e = 0.f, a = 0.f;
    if (!vf::voxel::worldfile::loadSunManifest(m_manifestPath, e, a))
        return; // no (or half-written) sun block: nothing authored to follow
    if (m_seenSunElev > -1e29f && std::fabs(e - m_seenSunElev) < 0.01f &&
        std::fabs(a - m_seenSunAzim) < 0.01f)
        return; // file unchanged since we last saw it: never snap a live toggle
    m_seenSunElev = e;
    m_seenSunAzim = a;
    setSunAngles(e, a);
    // Baked per-surfel sun shadows only follow on a reload (the same reason
    // setSunPhase/setSunTime request one) - skip it during startup, where the
    // world load that follows already bakes with the new direction.
    requestWorldReload();
    spdlog::info("ui: scene sun -> elev {:.1f} azim {:.1f} (world.json)", e, a);
}

void App::applyUiSettingsFromManifest(bool heavy)
{
    vf::voxel::worldfile::UiSettings u;
    if (!vf::voxel::worldfile::loadUiManifest(m_manifestPath, u))
        return;

    bool grassDirty = false;   // photo-cards bit flipped
    bool worldDirty = false;   // a surfel-geometry knob moved (rebuilds)
    bool furDirty = false;     // a structural coat knob moved
    bool sssDirty = false;     // composite uniform only
    bool lightsDirty = false;  // budget/K moved (re-upload)
    int  applied = 0;

    auto num = [&](float file, float& member, float lo, float hi) {
        if (!present(file))
            return;
        const float v = std::clamp(file, lo, hi);
        if (v != member) {
            member = v;
            ++applied;
        }
    };
    auto boolean = [&](float file, bool& member) {
        if (!present(file))
            return;
        const bool v = toBool(file);
        if (v != member) {
            member = v;
            ++applied;
        }
    };

    // ---- render flags / backend / toggles ----------------------------------
    if (present(u.renderFlags)) {
        const int f = toInt(u.renderFlags);
        if (f != m_renderFlags) {
            if (((f ^ m_renderFlags) & (1 << 13)) != 0)
                grassDirty = true;
            m_renderFlags = f;
            ++applied;
        }
    }
    if (!u.backend.empty()) {
        RenderMode m = m_renderMode;
        if (u.backend == "splats" || u.backend == "splat")
            m = RenderMode::Splats;
        else if (u.backend == "svo")
            m = RenderMode::Svo;
        else if (u.backend == "dgs")
            m = RenderMode::Dgs;
        if (m != m_renderMode) {
            m_renderMode = m;
            m_taaFirstFrame = true;
            ++applied;
        }
    }
    boolean(u.taa, m_taaEnabled);
    if (present(u.taa))
        m_taaFirstFrame = true;
    boolean(u.volFog, m_volFogEnabled);
    boolean(u.motionBlur, m_motionBlurEnabled);
    boolean(u.dof, m_dofEnabled);
    boolean(u.linearFx, m_linearFx);
    if (present(u.autoExposure)) {
        const bool v = toBool(u.autoExposure);
        if (v != m_autoExposure) {
            m_autoExposure = v;
            // post.comp multiplies by the metered value only with bit 10 set.
            if (v)
                m_renderFlags |= (1 << 10);
            else
                m_renderFlags &= ~(1 << 10);
            ++applied;
        }
    }
    num(u.exposure, m_exposure, 0.1f, 4.0f);
    if (present(u.tonemapLook)) {
        const int v = std::clamp(toInt(u.tonemapLook), 0, 2);
        if (v != m_tonemapLook) { m_tonemapLook = v; ++applied; }
    }
    num(u.ssaoStrength, m_ssaoStrength, 0.0f, 1.0f);
    num(u.ssaoRadius, m_ssaoRadius, 0.1f, 4.0f);
    boolean(u.ssaoBlur, m_ssaoBlur);
    if (present(u.ssaoDebug)) {
        const int v = toInt(u.ssaoDebug);
        if (v != m_ssaoDebug) { m_ssaoDebug = v; ++applied; }
    }

    // ---- surfel tuning (the SURFELS group; the geometry knobs rebuild) -----
    if (present(u.splatRadius)) {
        const float v = std::clamp(u.splatRadius, 0.5f, 2.0f);
        if (v != m_splatPass.radiusScale()) {
            m_splatPass.setRadiusScale(v);
            ++applied;
        }
    }
    if (present(u.edgeBlend)) {
        const float v = std::clamp(u.edgeBlend, 0.0f, 1.0f);
        if (v != m_splatPass.edgeBlend()) {
            m_splatPass.setEdgeBlend(v);
            ++applied;
        }
    }
    auto rebuildKnob = [&](float file, float& member, float lo, float hi) {
        if (!present(file))
            return;
        const float v = std::clamp(file, lo, hi);
        if (v != member) {
            member = v;
            worldDirty = true;
            ++applied;
        }
    };
    rebuildKnob(u.edgeShrink, m_edgeShrink, 0.0f, 0.8f);
    rebuildKnob(u.edgeBridgeSize, m_edgeBridgeSize, 0.0f, 2.0f);
    if (present(u.edgeFill)) {
        const bool v = toBool(u.edgeFill);
        if (v != m_edgeFill) { m_edgeFill = v; worldDirty = true; ++applied; }
    }
    if (present(u.cornerFill)) {
        const bool v = toBool(u.cornerFill);
        if (v != m_cornerFill) { m_cornerFill = v; worldDirty = true; ++applied; }
    }

    // ---- point-light budget -------------------------------------------------
    if (present(u.lightBudget)) {
        const int v = std::clamp(toInt(u.lightBudget), 1, vf::voxel::worldfile::kMaxLights);
        if (v != m_lightBudget) { m_lightBudget = v; lightsDirty = true; ++applied; }
    }
    if (present(u.lightK)) {
        const int v = std::clamp(toInt(u.lightK), 1, vf::voxel::worldfile::kLightKMax);
        if (v != m_lightK) { m_lightK = v; lightsDirty = true; ++applied; }
    }

    // ---- camera (session view; applied only when the FILE changed, so the
    //      poller never fights WASD - an unchanged block is inert) -----------
    num(u.camSpeed, m_camera.speed, 0.5f, 40.0f);
    if (present(u.camX) && present(u.camY) && present(u.camZ)) {
        m_camera.pos = glm::vec3(u.camX, u.camY, u.camZ);
        ++applied;
    }
    if (present(u.camYaw)) {
        m_camera.yaw = glm::radians(u.camYaw);
        ++applied;
    }
    if (present(u.camPitch)) {
        m_camera.pitch = glm::radians(std::clamp(u.camPitch, -89.0f, 89.0f));
        ++applied;
    }

    // ---- fur coat (structural keys rebuild the cards; sss is a uniform) ----
    boolean(u.furEnabled, m_furEnabled);
    if (present(u.furEnabled))
        furDirty = true; // toggling changes what (if anything) is drawn
    if (present(u.furPerCell)) {
        const int v = std::clamp(toInt(u.furPerCell), 1, 12);
        if (v != m_furPerCell) { m_furPerCell = v; furDirty = true; ++applied; }
    }
    num(u.furLength, m_furLength, 0.002f, 0.2f);
    num(u.furWidth, m_furWidth, 0.0005f, 0.05f);
    num(u.furCurl, m_furCurl, 0.0f, 1.5f);
    num(u.furCycles, m_furCycles, 0.0f, 6.0f);
    num(u.furTilt, m_furTilt, 0.0f, 89.0f);
    if (present(u.furLength) || present(u.furWidth) || present(u.furCurl) ||
        present(u.furCycles) || present(u.furTilt))
        furDirty = true;
    num(u.furSss, m_furSss, 0.0f, 4.0f);
    if (present(u.furSss))
        sssDirty = true;

    // ---- edit brush (the EDIT group) -----------------------------------------
    if (!u.brushMode.empty()) {
        using B = EditBrush;
        const char* names[] = {"carve", "add",  "delete",
                               "paint", "smooth", "rotate", "move"};
        for (int i = 0; i <= int(B::Move); ++i) {
            if (u.brushMode == names[i]) {
                // chooseEditMode is the ONE place a mode changes (it refuses
                // while a rotate/move stage is in flight) - mirror of the
                // hotkeys and the sidebar buttons.
                if (B(i) != m_editBrush) {
                    chooseEditMode(B(i));
                    ++applied;
                }
                break;
            }
        }
    }
    if (present(u.brushWidth))
        setBrushVoxels(toInt(u.brushWidth));
    if (present(u.brushDepth))
        setBrushDepthVoxels(toInt(u.brushDepth));
    if (present(u.brushMaterial)) {
        const int v = std::clamp(toInt(u.brushMaterial), 0, int(vf::voxel::kPaletteN) - 1);
        if (uint8_t(v) != m_editMat) { m_editMat = uint8_t(v); ++applied; }
    }
    num(u.smoothStrength, m_smoothStrength, 0.0f, 1.0f);
    if (!u.falloffCurve.empty()) {
        using C = vf::voxel::EditableWorld::FalloffCurve;
        for (int i = 0; i < int(C::Count); ++i) {
            const C c = C(i);
            if (u.falloffCurve == vf::voxel::EditableWorld::falloffCurveName(c)) {
                if (c != m_editFalloffCurve) {
                    m_editFalloffCurve = c;
                    ++applied;
                }
                break;
            }
        }
    }
    boolean(u.smoothPreserveVolume, m_smoothPreserveVolume);

    // ---- UI text size (high-res displays; atlas rebuild between frames) -----
    const float oldFontScale = m_uiFontScale;
    num(u.uiFontScale, m_uiFontScale, 0.8f, 2.0f);
    const bool fontDirty = oldFontScale != m_uiFontScale;

    // ---- scenery switch ------------------------------------------------------
    // Rides the SAME path the sidebar uses: set the pending request, let the
    // poll apply it BETWEEN frames (a switch must never land mid-record - see
    // the 2026-11-19 deadlock note in run_poll.cpp).
    if (!u.scenery.empty() && u.scenery != m_seenUi.scenery &&
        u.scenery != currentSceneryName()) {
        m_pendingScenerySwitch = u.scenery;
        spdlog::info("ui: scenery switch requested -> '{}'", u.scenery);
    }

    m_seenUi = u;

    // A visible count on BOTH paths: an applied-but-silent settings block is
    // indistinguishable from an ignored one, and this project's rule is that
    // a feature without a log line has no positive firing test.
    if (applied > 0)
        spdlog::info("ui: applied {} setting(s) from world.json{}", applied,
                     heavy ? "" : " (startup; env overrides may follow)");

    // Font scale must also be applied at startup: applyUiSettings runs AFTER
    // initImGui (which built the atlas with the default scale), so the
    // manifest's persisted value would otherwise be ignored until a live edit.
    if (fontDirty)
        rebuildUiFont();

    if (!heavy)
        return; // startup: members are enough, the init path does the uploads

    if (grassDirty)
        rebuildGrassCards();
    if (furDirty)
        rebuildFurCards();
    else if (sssDirty)
        m_furPass.setSss(m_furSss);
    if (lightsDirty)
        uploadLightSources();
    if (worldDirty)
        requestWorldReload();
}

} // namespace app
} // namespace vf
