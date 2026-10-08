// Per-frame brush preview: tint the splats the next stamp would affect, then
// feed the selected/hover highlight to the shader. The tint is a preview of the
// CPU volume, not of the result - the growth it shows has no splats yet.
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// The brush hover preview -> splat backend, and the highlight feeds. Two
// adjacent blocks in the frame body: the first decides what the next stamp
// would touch, the second publishes the selection/hover outline. They are kept
// together because both describe "where the pointer is", and splitting them
// would put the two halves of one interaction in two files.
void App::updateBrushPreview()
{
    const float tanHalfFov = tanHalfFov60();
    // The preview follows the live pick when the pointer is in the scene, and
    // otherwise falls back to the last scene pick (m_latchedHover) so resizing
    // the brush from the sidebar does not make the preview blink out. The
    // latch is a PREVIEW feed only: applyEditLive still requires a live hit,
    // so a latched preview can never stamp.
    const vf::voxel::PickHit& previewHit =
        m_hoverHit.hit ? m_hoverHit : m_latchedHover;
    // skin: surfels sit at cell centre + 0.05 m along their normal, so
    // grow the volume a little past the cell centres the CPU rasterizer
    // selects (see App::applyEditLive). Hoisted to function scope because the
    // depth indicator below extends the same reach.
    constexpr float kBrushSkin = 0.06f;
    // The volume tint, kept so the depth indicator can be coloured like the
    // volume it measures (warm = Carve, green = Add).
    glm::vec4 volumeTint(0.f);
    // Brush hover preview -> splat backend: tint the splats whose centre
    // lies inside the volume the next stamp would affect - the carve
    // cylinder, the delete/paint ball, the add growth (whose tint shows the
    // surface patch the raised shell will bury), or the conservative blue
    // footprint used by terrain Smooth.
    {
        glm::vec4 vol(0.f), axis(0.f), tint(0.f);
        m_gizmoValid = false;
        if (m_editActive && m_editBrush == EditBrush::Move &&
            !m_moveLayer.empty()) {
            const uint8_t layerId = m_layers.layerId(m_moveLayer);
            const vf::voxel::WorldAABB b = m_layers.layerBox(m_moveLayer);
            if (b.valid() && layerId != 0) {
                const glm::vec3 c = 0.5f * (b.lo + b.hi);
                const glm::vec3 he = 0.5f * (b.hi - b.lo) + kBrushSkin;
                vol = glm::vec4(c, 0.f);
                axis = glm::vec4(he, 0.f);
                tint = glm::vec4(0.20f, 0.85f, 0.55f, 0.22f);
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const glm::ivec2 fbExt =
                    display.x > 0.f && display.y > 0.f
                        ? glm::ivec2(int(display.x), int(display.y))
                        : glm::ivec2(int(m_swapchain.extent().width),
                                     int(m_swapchain.extent().height));
                m_gizmoValid = trackballScreenForBox(
                    b, m_camera, tanHalfFov, fbExt,
                    m_gizmoCentre, m_gizmoRadius);
                static bool moveProbeLogged = false;
                if (m_gizmoValid && getenv("VF_TEST_MOVE_PROBE") &&
                    !moveProbeLogged) {
                    moveProbeLogged = true;
                    const glm::vec3 c = 0.5f * (b.lo + b.hi);
                    const auto probe = [&](MoveAxis axis) {
                        const MoveAxisLine line = moveAxisScreenLine(
                            c, axis, m_camera, tanHalfFov, fbExt,
                            m_gizmoRadius);
                        return int(moveAxisHandleAt(
                            line.b, c, m_camera, tanHalfFov, fbExt,
                            m_gizmoRadius));
                    };
                    spdlog::info("move probe: hitX={} hitY={} hitZ={}",
                                 probe(MoveAxis::X), probe(MoveAxis::Y),
                                 probe(MoveAxis::Z));
                }
            }
        } else if (m_editActive && m_editBrush == EditBrush::Rotate &&
                   !m_rotateLayer.empty()) {
            // One click activates the owner; keep its full layer tinted
            // while the bounds-centered trackball remains available.
            const uint8_t layerId = m_layers.layerId(m_rotateLayer);
            const vf::voxel::WorldAABB b = m_layers.layerBox(m_rotateLayer);
            if (b.valid() && layerId != 0) {
                const glm::vec3 c = 0.5f * (b.lo + b.hi);
                const glm::vec3 he = 0.5f * (b.hi - b.lo) + kBrushSkin;
                vol = glm::vec4(c, 0.f);
                axis = glm::vec4(he, 0.f);
                tint = glm::vec4(0.20f, 0.75f, 1.00f, 0.35f);
                // ImGui mouse/draw coordinates are logical display pixels;
                // use them for the gizmo so high-DPI windows do not split
                // the ring hit-test from the rendered trackball.
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const glm::ivec2 fbExt =
                    display.x > 0.f && display.y > 0.f
                        ? glm::ivec2(int(display.x), int(display.y))
                        : glm::ivec2(int(m_swapchain.extent().width),
                                     int(m_swapchain.extent().height));
                m_gizmoValid = trackballScreenForBox(
                    b, m_camera, tanHalfFov, fbExt,
                    m_gizmoCentre, m_gizmoRadius);
                m_rotateRadius = m_gizmoValid ? m_gizmoRadius : 0.f;
                static bool trackballProbeLogged = false;
                if (m_gizmoValid && getenv("VF_TEST_TRACKBALL_PROBE") &&
                    !trackballProbeLogged) {
                    trackballProbeLogged = true;
                    const float r = m_gizmoRadius;
                    const int yaw = int(trackballHandleAt(
                        m_gizmoCentre + glm::vec2(r * 0.8f, r * 0.6f),
                        m_gizmoCentre, r));
                    const int pitch = int(trackballHandleAt(
                        m_gizmoCentre + glm::vec2(0.f, r * 0.42f),
                        m_gizmoCentre, r));
                    const int roll = int(trackballHandleAt(
                        m_gizmoCentre + glm::vec2(r * 0.42f, 0.f),
                        m_gizmoCentre, r));
                    const int centre = int(trackballHandleAt(
                        m_gizmoCentre, m_gizmoCentre, r));
                    spdlog::info(
                        "trackball probe: centre=({:.3f},{:.3f}) radius={:.3f} "
                        "hitYaw={} hitPitch={} hitRoll={} hitCentre={} "
                        "bounds=({:.3f},{:.3f},{:.3f})-({:.3f},{:.3f},{:.3f}) "
                        "camPos=({:.3f},{:.3f},{:.3f}) camYaw={:.5f} camPitch={:.5f}",
                        m_gizmoCentre.x, m_gizmoCentre.y, r,
                        yaw, pitch, roll, centre,
                        b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y, b.hi.z,
                        m_camera.pos.x, m_camera.pos.y, m_camera.pos.z,
                        m_camera.yaw, m_camera.pitch);
                }
                if (getenv("VF_TRACE") && m_frameIdx == 1)
                    spdlog::info("trackball: layer={} box={} id={} valid={} centre=({:.1f},{:.1f}) radius={:.1f} bounds=({:.2f},{:.2f},{:.2f})-({:.2f},{:.2f},{:.2f}) campos=({:.2f},{:.2f},{:.2f}) yaw={:.3f} pitch={:.3f}",
                                 m_rotateLayer, b.valid(), unsigned(layerId),
                                 m_gizmoValid, m_gizmoCentre.x, m_gizmoCentre.y,
                                 m_gizmoRadius, b.lo.x, b.lo.y, b.lo.z,
                                 b.hi.x, b.hi.y, b.hi.z,
                                 m_camera.pos.x, m_camera.pos.y, m_camera.pos.z,
                                 m_camera.yaw, m_camera.pitch);
            }
        } else if (m_editActive && previewHit.hit) {
            {
                const glm::vec3 c = vf::voxel::voxelCenter(previewHit.voxel);
                glm::vec3 n = previewHit.normal;
                if (glm::length(n) < 1e-3f)
                    n = glm::vec3(0.f, 1.f, 0.f);
                n = glm::normalize(n);
                const float r = m_editDiameter * 0.5f + kBrushSkin;
                if (brushIsPerVoxel() && (m_editBrush == EditBrush::Add ||
                                          m_editBrush == EditBrush::Carve)) {
                    // per-voxel: the tint is the single cell the stamp would
                    // touch (a 1-voxel box, not the depth-extent volume), so
                    // the highlight IS the target. The hover outline in the
                    // post pass marks the picked voxel itself.
                    const glm::ivec3 t = perVoxelTarget(n);
                    vol = glm::vec4(vf::voxel::voxelCenter(t), 0.f);
                    // box form: bAxis.xyz is the half extent on ALL THREE
                    // axes (bAxis.w == 0), so it must be set per axis
                    const float he = 0.5f * vf::voxel::VOXEL + kBrushSkin * 0.5f;
                    axis = glm::vec4(he, he, he, 0.f);
                    // A 1-voxel volume cannot over-tint the scene, so the
                    // strength goes up: at 0.55 a single cell measured only
                    // dG=+3, which reads as nothing next to the outline.
                    tint = (m_editBrush == EditBrush::Add)
                               ? glm::vec4(0.30f, 0.95f, 0.40f, 0.90f)
                               : glm::vec4(1.00f, 0.45f, 0.10f, 0.90f);
                } else if (m_editBrush == EditBrush::Carve) {
                    // the exact volume makeOrientedCylinder emits: a cylinder
                    // reaching kCarveTopMargin above the hit cell, `depth`
                    // long along -normal (the top margin is what opens the
                    // surface, so the preview must include it)
                    const glm::vec3 a = -n;
                    const float margin =
                        vf::voxel::EditableWorld::kCarveTopMargin;
                    const float half = (m_editDepth + margin) * 0.5f + kBrushSkin;
                    vol = glm::vec4(c + a * ((m_editDepth - margin) * 0.5f), r);
                    axis = glm::vec4(a, half);
                    tint = glm::vec4(1.00f, 0.45f, 0.10f, 0.45f); // warm = cut
                } else if (m_editBrush == EditBrush::Delete) {
                    vol = glm::vec4(c, r);
                    axis = glm::vec4(0.f, 1.f, 0.f, 0.f); // ball
                    tint = glm::vec4(1.00f, 0.12f, 0.10f, 0.55f); // red = remove
                } else if (m_editBrush == EditBrush::Add) {
                    // makeDome emits an extruded disk plus a rounded lip;
                    // encode that growth volume as a negative axis half
                    // length for the shader dome case, with the brush skin
                    // folded into the reach.
                    axis = glm::vec4(n, -(m_editDepth + kBrushSkin));
                    vol = glm::vec4(c, r);
                    tint = glm::vec4(0.30f, 0.95f, 0.40f, 0.50f); // green = raise
                } else if (m_editBrush == EditBrush::Smooth) {
                    // Smooth relaxes terrain columns or an object surface
                    // rather than a fixed 3D volume. A conservative ball
                    // gives the user a clear blue footprint without
                    // claiming that the relaxation touches every cell at
                    // exactly the same height.
                    vol = glm::vec4(c, r);
                    axis = glm::vec4(0.f, 1.f, 0.f, 0.f);
                    tint = glm::vec4(0.20f, 0.68f, 1.00f, 0.45f);
                } else { // Paint: preview the chosen material colour
                    vol = glm::vec4(c, r);
                    axis = glm::vec4(0.f, 1.f, 0.f, 0.f); // ball
                    const glm::vec3 pc = vf::voxel::kPalette[std::min<int>(m_editMat, 16)];
                    tint = glm::vec4(pc, 0.55f);
                }
            }
        }
        // Add/Carve/Delete/Paint carry the falloff CURVE index so the tint
        // marks the tapered volume the stamp actually emits. Smooth has no
        // tapered volume (its preview is a conservative footprint ball).
        const float brushFalloff =
            (m_editBrush == EditBrush::Add || m_editBrush == EditBrush::Carve ||
             m_editBrush == EditBrush::Delete || m_editBrush == EditBrush::Paint)
                ? float(m_editFalloffCurve)
                : 0.0f;
        m_splatPass.setBrush(
            vol, axis, tint,
            (m_editBrush == EditBrush::Rotate && !m_rotateLayer.empty())
                ? m_layers.layerId(m_rotateLayer)
                : ((m_editBrush == EditBrush::Move && !m_moveLayer.empty())
                       ? m_layers.layerId(m_moveLayer)
                       : uint8_t(0)),
            brushFalloff);
        // The SVO reference backend gets the same volume. It used to have no
        // preview at all (setBrush fed only m_splatPass), so in --mode svo
        // every brush-size change was invisible. SVO shades a raymarch HIT
        // POINT rather than a per-surfel centre, so it tests the volume
        // against the hit - a strictly closer match to the CPU cell set than
        // the splat backend's per-surfel approximation.
        m_svoPass.setBrush(
            vol, axis, tint,
            (m_editBrush == EditBrush::Rotate && !m_rotateLayer.empty())
                ? m_layers.layerId(m_rotateLayer)
                : ((m_editBrush == EditBrush::Move && !m_moveLayer.empty())
                       ? m_layers.layerId(m_moveLayer)
                       : uint8_t(0)),
            brushFalloff);
        volumeTint = tint;
    }

    // Depth indicator. The tint can only mark EXISTING surfels, and the extra
    // depth of a Carve cylinder lies below the surface (solid material) while
    // an Add dome grows into empty air - neither has a surfel to light up.
    // Measured: the Carve mask was bit-identical (2920 px) at depths
    // 0.1/0.5/2.0/12.0 m, i.e. the Depth slider changed nothing on screen.
    // So the depth axis gets its own marker: the volume's FAR end (Carve
    // below the hit, Add above it) plus a tick at the hit plane, drawn by the
    // post pass from world positions - no CPU projection, so the trackball
    // projection maths stays untouched.
    m_depthMarker = glm::vec4(0.f);
    m_depthFar = glm::vec4(0.f);
    m_depthNear = glm::vec4(0.f);
    if (m_editActive && previewHit.hit &&
        (m_editBrush == EditBrush::Carve || m_editBrush == EditBrush::Add) &&
        !brushIsPerVoxel()) {
        glm::vec3 n = previewHit.normal;
        if (glm::length(n) < 1e-3f)
            n = glm::vec3(0.f, 1.f, 0.f);
        n = glm::normalize(n);
        const glm::vec3 c = vf::voxel::voxelCenter(previewHit.voxel);
        // Carve reaches kCarveTopMargin ABOVE the hit as well as `depth`
        // below it, so the marker spans the whole carved extent. Add grows
        // only upward, from the hit plane.
        const bool carve = m_editBrush == EditBrush::Carve;
        const float margin =
            carve ? vf::voxel::EditableWorld::kCarveTopMargin : 0.f;
        const glm::vec3 far = carve ? c - n * (m_editDepth + kBrushSkin)
                                    : c + n * (m_editDepth + kBrushSkin);
        const glm::vec3 near = c - n * margin;
        // xyz = world point, w = 1 marks this end as the end that MOVES with
        // the Depth slider (the far one). The post pass draws a line between
        // them plus a tick at each, so depth reads as a visible extent.
        m_depthMarker = glm::vec4(c, 1.f);
        m_depthFar = glm::vec4(far, 1.f);
        m_depthNear = glm::vec4(near, 0.f);
        // rgb of the volume's own tint, so the line matches the volume tint
        // (warm for Carve, green for Add) instead of a second colour scheme.
        m_depthTint = glm::vec4(volumeTint.r, volumeTint.g, volumeTint.b, 1.f);
    }

    // highlight feeds: selected (strong) + hover (faint) -> shader UBO
    {
        glm::vec4 selFeed(0.f), hovFeed(0.f);
        if (m_hasSelection)
            selFeed = glm::vec4(vf::voxel::voxelCenter(m_selectedHit.voxel), 1.f);
        if (previewHit.hit)
            hovFeed = glm::vec4(vf::voxel::voxelCenter(previewHit.voxel), 1.f);
        m_postPass.setSelection(selFeed);
        m_postPass.setHover(hovFeed);
        m_postPass.setDepthMarker(m_depthMarker, m_depthFar, m_depthNear,
                                  m_depthTint);
    }
}

} // namespace app
} // namespace vf
