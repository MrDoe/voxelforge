// The studio's BONE GIZMO: three rings that are the projected circles of the
// selected bone's OWN axes, and the keyframe a drag writes.
//
// WHY A GIZMO AND NOT JUST FIELDS. Posing a skeleton by typing Euler angles is
// guesswork: the user cannot see which way +20 degrees of X bends the neck.
//
// WHY THE FIRST VERSION'S RINGS WERE WRONG. It reused the object trackball's
// three FIXED screen shapes (a circle, a wide flat ellipse, a tall ellipse).
// Those encode "yaw / pitch / roll" for an object whose pose is authored in the
// WORLD frame, where a screen-space metaphor is at least arguable. A bone is
// arbitrarily oriented, so a fixed screen ellipse claims a plane the bone does
// not have: which ring "was" X depended on where the camera stood, and dragging
// it rotated about a different axis than the one drawn. Now each ring is the
// projection of the real circle - centre at the joint, normal along the bone's
// own axis, radius a fraction of the bone's length - so the ring the user sees
// IS the axis it rotates. Hit-testing is therefore point-to-projected-polyline,
// which also works when a ring is nearly edge-on (a fixed-shape test cannot).
//
// WHAT A DRAG WRITES. The pointer is intersected with the grabbed ring's PLANE
// and its angle around the joint is tracked; the delta is composed onto the
// bone's local rotation as `R * axisRotation(localAxis, dAngle)` - rotating
// about the bone's own axis, exactly as drawn - and decomposed back into the
// clip's Euler with `boneEulerFromMatrix`. The result is stored as a
// `BoneKey` at the playhead, the same value `MicroClip::sample` poses from, so
// the pose on screen IS the pose stored.
//
// THE SEEDING RULE. The first edit at a time with no key seeds the new key
// from the pose already authored there (`sampleTrackEuler`). Seeding from zero
// would SNAP the bone to rest the moment the user touched a ring.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "voxel/micro_clip.hpp"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace vf {
namespace app {

namespace {

constexpr int kRingSegments = 64;


// A right-handed rotation about a world axis. glm's `rotate` is right-handed,
// which is what the rings' angle convention assumes; the clip's Euler
// convention (`boneEulerMatrix`) is NOT (rx enters negated), so using the
// matrix form here keeps the two independent.
glm::mat3 axisRotationRH(glm::vec3 axis, float radians)
{
    return glm::mat3(glm::rotate(glm::mat4(1.f), radians, axis));
}

// The pointer's ray, the same construction splat.vert and the fur shader use.
glm::vec3 pointerRay(glm::vec2 px, const vf::Camera& cam, float tanHalf,
                     const glm::ivec2& fb, glm::vec3& origin)
{
    const float x = (fb.x > 0) ? (px.x / float(fb.x)) * 2.f - 1.f : 0.f;
    const float y = (fb.y > 0) ? (px.y / float(fb.y)) * 2.f - 1.f : 0.f;
    const float aspect = (fb.y > 0) ? float(fb.x) / float(fb.y) : 1.f;
    origin = cam.pos;
    return glm::normalize(cam.forward() + x * tanHalf * aspect * cam.right() -
                          y * tanHalf * cam.up());
}

// Where the pointer hits the plane, and its angle in the plane's (U,V) basis.
bool planeAngle(glm::vec2 px, const glm::vec3& centre, const glm::vec3& n,
                const glm::vec3& u, const glm::vec3& v, const vf::Camera& cam,
                float tanHalf, const glm::ivec2& fb, float& outAngle)
{
    glm::vec3 origin;
    const glm::vec3 rd = pointerRay(px, cam, tanHalf, fb, origin);
    const float denom = glm::dot(rd, n);
    if (std::abs(denom) < 1e-4f)
        return false; // the ray is parallel to the ring's plane
    const float t = glm::dot(centre - origin, n) / denom;
    if (t <= 0.f)
        return false; // the plane is behind the camera
    const glm::vec3 p = origin + rd * t;
    const glm::vec3 d = p - centre;
    outAngle = std::atan2(glm::dot(d, v), glm::dot(d, u));
    return true;
}

float wrapPi(float a)
{
    while (a > glm::pi<float>()) a -= 2.f * glm::pi<float>();
    while (a < -glm::pi<float>()) a += 2.f * glm::pi<float>();
    return a;
}

} // namespace

glm::ivec2 App::viewportLogical() const
{
    // LOGICAL window pixels: GLFW cursor coordinates and ImGui's DisplaySize
    // are logical while framebufferSize() is physical, and picking must use
    // the former on a high-DPI display.
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x > 0.f && ds.y > 0.f)
        return glm::ivec2(int(ds.x), int(ds.y));
    // Before the first ImGui frame DisplaySize is UNSET (it reads back
    // NEGATIVE), and a headless --shot may never build a UI frame at all. A
    // negative size gives the projection a negative aspect and puts every
    // gizmo off-screen, so fall back to the requested surface size.
    return glm::ivec2(std::max(1, m_args.width), std::max(1, m_args.height));
}

bool App::studioBoneGizmo(BoneGizmo& out)
{
    auto* b = studioBehaviour();
    if (!m_studio.active || !b || b->rig.bones.empty())
        return false;
    if (m_studio.selectedBone < 0 ||
        size_t(m_studio.selectedBone) >= b->rig.bones.size())
        return false;
    // The pose is only meaningful once animateBones() has run; before that
    // boneRot is empty and a gizmo at the origin would point at nothing.
    if (b->boneRot.size() != b->rig.bones.size())
        return false;
    if (b->boneXform.size() != b->rig.bones.size())
        return false;

    const size_t si = size_t(m_studio.selectedBone);
    const vf::voxel::MicroBone& mb = b->rig.bones[si];
    // The SAME chain the skeleton overlay draws with: the posed segment, then
    // the behaviour's whole-object transform. A gizmo computed from the rest
    // segment would sit where the bone used to be as soon as anything moved.
    const glm::mat3 R = b->renderRot();
    const glm::vec3 pv = b->pivot;
    auto toWorld = [&](glm::vec3 p) {
        return pv + R * (p - pv) + b->renderOffset();
    };
    const glm::vec3 a = toWorld(b->boneRot[si] * mb.a + b->boneXform[si]);
    const glm::vec3 c = toWorld(b->boneRot[si] * mb.b + b->boneXform[si]);
    const vf::Camera& cam = m_camera;
    if (glm::dot(a - cam.pos, cam.forward()) < 0.05f)
        return false;
    if (glm::dot(c - cam.pos, cam.forward()) < 0.05f)
        return false;

    out.centre = a;
    // The bone's OWN frame, carried through the pose chain and the object
    // transform. `boneRot` is a rotation, and so is the object's, so these stay
    // orthonormal - which is what lets the rings be circles.
    const glm::mat3 axes = R * b->boneRot[si];
    for (int k = 0; k < 3; ++k) {
        glm::vec3 e(0.f);
        e[k] = 1.f;
        out.axis[k] = glm::normalize(axes * e);
    }
    // A ring the size of the bone, so a short ear bone gets a small gizmo and
    // the spine a large one, clamped so it is always grabbable.
    const float boneLen = glm::length(c - a);
    out.radius = glm::clamp(boneLen * 0.55f, 0.02f, 0.60f);

    const glm::ivec2 fb = viewportLogical();
    const float tanHalf = tanHalfFov60();
    out.centrePx = projectScreen(a, cam, tanHalf, fb);
    const glm::vec2 edge =
        projectScreen(a + out.radius * out.axis[1], cam, tanHalf, fb);
    out.radiusPx = glm::length(edge - out.centrePx);
    return true;
}

glm::vec2 App::studioRingPoint(const BoneGizmo& g, int axis, float t)
{
    const int i1 = (axis + 1) % 3;
    const int i2 = (axis + 2) % 3;
    const glm::vec3 p = g.centre + g.radius *
        (std::cos(t) * g.axis[i1] + std::sin(t) * g.axis[i2]);
    return projectScreen(p, m_camera, tanHalfFov60(), viewportLogical());
}

int App::studioBoneRingAt(const BoneGizmo& g, glm::vec2 p, float slop)
{
    float best = slop;
    int hit = -1;
    for (int axis = 0; axis < 3; ++axis) {
        glm::vec2 prev = studioRingPoint(g, axis, 0.f);
        for (int i = 1; i <= kRingSegments; ++i) {
            const float t = 6.2831853f * float(i) / float(kRingSegments);
            const glm::vec2 cur = studioRingPoint(g, axis, t);
            const float d = screenSegmentDistance(p, prev, cur);
            if (d < best) {
                best = d;
                hit = axis;
            }
            prev = cur;
        }
    }
    return hit;
}

vf::voxel::MicroClip* App::studioEnsureClip()
{
    auto* b = studioBehaviour();
    auto* clips = studioClips();
    if (!b || !clips)
        return nullptr;
    if (b->clip)
        return const_cast<vf::voxel::MicroClip*>(b->clip);
    // No clip: the rig is on the procedural gait and there is nowhere to put
    // a key. Create one rather than refusing the edit, and select it so the
    // panel and the transport show what the drag is writing into.
    auto clip = std::make_unique<vf::voxel::MicroClip>();
    clip->name = "studio";
    clip->description = "authored in the studio (created to hold a bone key)";
    clip->layer = m_studio.layerFile;
    clip->length = 2.f;
    clip->fps = 30.f;
    clip->loop = true;
    clip->clock = vf::voxel::ClipClock::Time;
    clip->speed = 0.20f;
    clip->stride = 0.90f;
    clip->blendIn = 0.20f;
    // startClip stores the POINTER; the unique_ptr's move into the vector does
    // not move the object, so the pointer stays valid.
    b->startClip(clip.get());
    b->clipPlaying = false;
    clips->push_back(std::move(clip));
    m_studio.selectedClip = int(clips->size()) - 1;
    spdlog::info("studio: created clip 'studio' to hold a bone key");
    return const_cast<vf::voxel::MicroClip*>(b->clip);
}

vf::voxel::BoneKey* App::studioKeyAt(vf::voxel::MicroClip& clip,
                                     uint32_t boneId, float t)
{
    vf::voxel::BoneTrack* track = nullptr;
    for (auto& tr : clip.tracks) {
        if (tr.boneId == boneId) {
            track = &tr;
            break;
        }
    }
    if (!track) {
        clip.tracks.push_back({});
        track = &clip.tracks.back();
        track->boneId = boneId;
    }
    // Quantise to the clip's own frame grid. A drag emits hundreds of mouse
    // motions and every one of them must land on the SAME key - writing a key
    // per pixel of motion would make the clip unusable and the timeline a
    // smear.
    const float step = (clip.fps > 1e-3f) ? 1.f / clip.fps : 1.f / 30.f;
    t = std::round(t / step) * step;
    for (auto& k : track->keys) {
        if (std::abs(k.t - t) < 0.5f * step)
            return &k; // the key already exists: the drag UPDATES it
    }
    vf::voxel::BoneKey k;
    k.t = t;
    // SEED from the pose already on screen, or the first edit would snap the
    // bone to zero and destroy the pose the user is looking at.
    k.rotDeg = vf::voxel::sampleTrackEuler(track->keys, t);
    k.ease = vf::voxel::ClipEase::Smooth;
    track->keys.push_back(k);
    std::sort(track->keys.begin(), track->keys.end(),
              [](const vf::voxel::BoneKey& a, const vf::voxel::BoneKey& b) {
                  return a.t < b.t;
              });
    // Re-find after the sort: the vector moved the element.
    for (auto& kk : track->keys) {
        if (std::abs(kk.t - t) < 0.5f * step)
            return &kk;
    }
    return nullptr;
}

void App::studioUploadRig()
{
    auto* b = studioBehaviour();
    if (!b)
        return;
    std::vector<glm::vec3> segA, segB;
    std::vector<float> radius;
    segA.reserve(b->rig.bones.size());
    segB.reserve(b->rig.bones.size());
    radius.reserve(b->rig.bones.size());
    for (const auto& bone : b->rig.bones) {
        segA.push_back(bone.a);
        segB.push_back(bone.b);
        radius.push_back(bone.radius);
    }
    const int mi = m_studio.targetIndex;
    if (mi >= 0)
        m_microSvoPass.setBoneSegments(size_t(mi), segA, segB, radius);
    // The FUR pass keeps its segments in the fur object's own frame (the
    // field's origin plus the layer's world offset), not the rig's.
    if (mi >= 0 && size_t(mi) < m_furIndexOfMicro.size()) {
        const int fi = m_furIndexOfMicro[size_t(mi)];
        if (fi >= 0 && size_t(fi) < m_furObjects.size()) {
            vf::voxel::FurObject& fo = m_furObjects[size_t(fi)];
            std::vector<glm::vec3> fa, fb;
            fa.reserve(segA.size());
            fb.reserve(segB.size());
            for (size_t i = 0; i < segA.size(); ++i) {
                fa.push_back(segA[i] - fo.rigFrame);
                fb.push_back(segB[i] - fo.rigFrame);
            }
            m_furPass.setBoneSegments(size_t(fi), fa, fb, radius);
            // Keep the CPU copy in step: a later rebuild (the coat's shape
            // sliders) re-reads these.
            fo.boneSegA = fa;
            fo.boneSegB = fb;
            fo.boneRadius = radius;
        }
    }
}

bool App::studioSetBoneLength(int boneIndex, float metres)
{
    auto* b = studioBehaviour();
    if (!b || boneIndex < 0 || size_t(boneIndex) >= b->rig.bones.size())
        return false;
    auto& bone = b->rig.bones[size_t(boneIndex)];
    const glm::vec3 d = bone.b - bone.a;
    const glm::vec3 dir = (glm::length(d) > 1e-6f) ? glm::normalize(d)
                                                   : glm::vec3(0.f, -1.f, 0.f);
    // The JOINT stays put: `a` is the pivot the clip's local rotation turns
    // the bone about, so moving it would change the pose's anchor, which is a
    // different (and much riskier) edit than "this bone is too short".
    bone.b = bone.a + dir * std::max(metres, 0.005f);
    b->animateBones();
    studioUploadRig();
    m_taaFirstFrame = true;
    return true;
}

bool App::studioMoveBone(int boneIndex, glm::vec3 delta)
{
    auto* b = studioBehaviour();
    if (!b || boneIndex < 0 || size_t(boneIndex) >= b->rig.bones.size())
        return false;
    auto& bone = b->rig.bones[size_t(boneIndex)];
    const glm::vec3 oldA = bone.a;
    // The JOINT moves and the TIP stays, so the bone's LENGTH and DIRECTION
    // change IMPLICITLY - that is the resize. Moving both endpoints rigidly
    // would need a separate length edit for every placement change, which is
    // the wrong way round for a rig derived from voxels.
    bone.a += delta;
    // A CONNECTED chain stays connected: when the parent's tip WAS this joint,
    // it follows, so the parent stretches instead of tearing away.
    const int pi = bone.parent;
    if (pi >= 0 && size_t(pi) < b->rig.bones.size()) {
        auto& parent = b->rig.bones[size_t(pi)];
        if (glm::length(parent.b - oldA) < 1e-4f)
            parent.b = bone.a;
    }
    b->animateBones();
    studioUploadRig();
    m_taaFirstFrame = true;
    return true;
}

bool App::studioBoneCentreAt(const BoneGizmo& g, glm::vec2 p, float slop)
{
    return glm::length(p - g.centrePx) <= slop;
}

bool App::studioBeginBoneTranslate(int boneIndex, const BoneGizmo& g,
                                   glm::vec2 mouse)
{
    auto* b = studioBehaviour();
    if (!b || boneIndex < 0 || size_t(boneIndex) >= b->rig.bones.size())
        return false;
    m_studio.gizmoDragBone = boneIndex;
    m_studio.gizmoDragBoneId = b->rig.bones[size_t(boneIndex)].id;
    m_studio.gizmoTranslating = true;
    m_studio.gizmoLastMouse = mouse;
    m_studio.gizmoCentre = g.centre; // the depth the slide is scaled at
    // As with a rotation drag: the clip clock must not run under the hand.
    m_studio.playing = false;
    b->clipPlaying = false;
    b->clipTime = m_studio.time;
    spdlog::info("studio: bone '{}' translate drag at t={:.3f}",
                 b->rig.bones[size_t(boneIndex)].name, m_studio.time);
    return true;
}

void App::studioApplyBoneTranslate(glm::vec2 mouse)
{
    if (m_studio.gizmoDragBone < 0 || !m_studio.gizmoTranslating)
        return;
    auto* b = studioBehaviour();
    if (!b)
        return;
    const size_t si = size_t(m_studio.gizmoDragBone);
    if (si >= b->rig.bones.size() ||
        b->rig.bones[si].id != m_studio.gizmoDragBoneId) {
        m_studio.gizmoDragBone = -1;
        m_studio.gizmoTranslating = false;
        return;
    }
    const glm::vec2 d = mouse - m_studio.gizmoLastMouse;
    m_studio.gizmoLastMouse = mouse;
    if (glm::length(d) < 1e-4f)
        return;
    // Slide in the CAMERA plane, scaled so the joint stays under the pointer:
    // world-per-pixel at the joint's depth. Screen y grows DOWN, hence -up.
    const float depth =
        glm::dot(m_studio.gizmoCentre - m_camera.pos, m_camera.forward());
    if (depth <= 0.01f)
        return;
    const glm::ivec2 fb = viewportLogical();
    const float wpp = 2.f * depth * tanHalfFov60() /
                      float(std::max(1, fb.y));
    const glm::vec3 delta = m_camera.right() * (d.x * wpp) -
                            m_camera.up() * (d.y * wpp);
    studioMoveBone(int(si), delta);
}

bool App::studioBoneEulerAt(uint32_t boneId, float t, glm::vec3& out){
    auto* b = studioBehaviour();
    if (!b || !b->clip)
        return false;
    for (const auto& tr : b->clip->tracks) {
        if (tr.boneId != boneId)
            continue;
        if (tr.keys.empty())
            return false;
        out = vf::voxel::sampleTrackEuler(tr.keys, t);
        return true;
    }
    return false;
}

bool App::studioBeginBoneDrag(int boneIndex, int axis, const BoneGizmo& g,
                              glm::vec2 mouse)
{
    auto* b = studioBehaviour();
    if (!b || boneIndex < 0 || size_t(boneIndex) >= b->rig.bones.size())
        return false;
    if (axis < 0 || axis > 2)
        return false;
    auto* clip = studioEnsureClip();
    if (!clip)
        return false;
    float angle = 0.f;
    const glm::ivec2 fb = viewportLogical();
    const float tanHalf = tanHalfFov60();
    const int i1 = (axis + 1) % 3;
    const int i2 = (axis + 2) % 3;
    if (!planeAngle(mouse, g.centre, g.axis[axis], g.axis[i1], g.axis[i2],
                    m_camera, tanHalf, fb, angle))
        return false; // the pointer is not on the ring's plane at all

    m_studio.gizmoDragBone = boneIndex;
    m_studio.gizmoDragBoneId = b->rig.bones[size_t(boneIndex)].id;
    m_studio.gizmoDragAxis = axis;
    m_studio.gizmoCentre = g.centre;
    m_studio.gizmoPlaneN = g.axis[axis];
    m_studio.gizmoPlaneU = g.axis[i1];
    m_studio.gizmoPlaneV = g.axis[i2];
    m_studio.gizmoLastAngle = angle;
    m_studio.gizmoRadiusPx = g.radiusPx;
    // The clip clock must not run under the drag, or the key would land at a
    // time the user is not looking at.
    m_studio.playing = false;
    b->clipPlaying = false;
    b->clipTime = m_studio.time;
    // Create the key NOW, so the pose is already authored where the drag will
    // write and the ring has something to move.
    studioKeyAt(*clip, m_studio.gizmoDragBoneId, m_studio.time);
    b->animateBones();
    m_taaFirstFrame = true;
    spdlog::info("studio: bone '{}' axis {} drag at t={:.3f}",
                 b->rig.bones[size_t(boneIndex)].name, axis, m_studio.time);
    return true;
}

void App::studioApplyBoneDrag(glm::vec2 mouse)
{
    if (m_studio.gizmoDragBone < 0)
        return;
    auto* b = studioBehaviour();
    if (!b || !b->clip)
        return;
    auto& clip = *const_cast<vf::voxel::MicroClip*>(b->clip);
    const size_t si = size_t(m_studio.gizmoDragBone);
    if (si >= b->rig.bones.size())
        return;
    // The rig may have been reloaded mid-drag: refuse rather than write a key
    // onto whichever bone now occupies the index.
    if (b->rig.bones[si].id != m_studio.gizmoDragBoneId) {
        spdlog::warn("studio: bone drag abandoned (rig changed under it)");
        m_studio.gizmoDragBone = -1;
        m_studio.gizmoDragAxis = -1;
        return;
    }
    const glm::ivec2 fb = viewportLogical();
    const float tanHalf = tanHalfFov60();
    float angle = 0.f;
    if (!planeAngle(mouse, m_studio.gizmoCentre, m_studio.gizmoPlaneN,
                    m_studio.gizmoPlaneU, m_studio.gizmoPlaneV, m_camera,
                    tanHalf, fb, angle))
        return; // the pointer left the plane (parallel ray): keep the pose
    const float delta = wrapPi(angle - m_studio.gizmoLastAngle);
    m_studio.gizmoLastAngle = angle;
    if (std::abs(delta) < 1e-6f)
        return;
    auto* k = studioKeyAt(clip, m_studio.gizmoDragBoneId, m_studio.time);
    if (!k)
        return;
    // Compose about the bone's OWN axis, in the frame the local rotation acts
    // in, then decompose back to the clip's Euler. `R * dR` keeps the axis
    // direction `R * e` fixed - the axis the ring was drawn for.
    const glm::mat3 R = vf::voxel::boneEulerMatrix(k->rotDeg);
    glm::vec3 e(0.f);
    e[m_studio.gizmoDragAxis] = 1.f;
    const glm::mat3 Rn = R * axisRotationRH(e, delta);
    k->rotDeg = vf::voxel::boneEulerFromMatrix(Rn);
    // Re-sample THIS frame, so the dragged bone follows the pointer without a
    // frame of lag and the pose on screen is exactly the pose stored.
    b->clipTime = m_studio.time;
    b->animateBones();
    m_taaFirstFrame = true;
}

} // namespace app
} // namespace vf
