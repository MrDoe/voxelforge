// The studio's BONE GIZMO: three rings at the selected bone's joint, and the
// keyframe a drag writes.
//
// WHY A GIZMO AND NOT JUST FIELDS. Posing a skeleton by typing Euler angles is
// guesswork: the user cannot see which way +20 degrees of X bends the neck.
// Dragging a ring is the same interaction the object trackball already uses
// (`ui/gizmo_math.hpp`), so there is ONE ring vocabulary in the app.
//
// WHAT A DRAG WRITES. A keyframe, at the playhead, into the target's clip -
// the same `BoneKey { t, rotDeg }` that `MicroClip::sample` hands to
// `composeBones`. The Euler is edited in place, so the pose on screen IS the
// pose stored: there is no separate "preview" transform to drift from the
// data (the object trackball needs one because a world layer's pose lives in
// the manifest; a bone's lives in the clip).
//
// THE SEEDING RULE. The first edit at a time with no key seeds the new key
// from the pose already authored there (`eulerAt`, which mirrors sample()'s
// clamp/interpolate rule). Seeding from zero instead would SNAP the bone to
// rest the moment the user touched a ring, which reads as a bug.
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace vf {
namespace app {

namespace {

// The authored Euler at `t`, via the ONE blend rule the rig uses. Kept as a
// thin wrapper so the writer and `MicroClip::sample` cannot disagree about
// what "the pose there" means.
glm::vec3 eulerAt(const std::vector<vf::voxel::BoneKey>& keys, float t)
{
    return vf::voxel::sampleTrackEuler(keys, t);
}

} // namespace

bool App::studioBoneGizmo(glm::vec2& centre, float& radius)
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

    // LOGICAL window pixels: GLFW cursor coordinates and ImGui's DisplaySize
    // are logical while framebufferSize() is physical, and picking must use
    // the former on a high-DPI display.
    const glm::ivec2 fb(int(ImGui::GetIO().DisplaySize.x),
                        int(ImGui::GetIO().DisplaySize.y));
    const float tanHalf = tanHalfFov60();
    centre = projectScreen(a, cam, tanHalf, fb);
    const glm::vec2 pc = projectScreen(c, cam, tanHalf, fb);
    // Size the rings from the bone's own length ON SCREEN, so a short ear bone
    // gets a small gizmo and the spine a large one - a fixed pixel radius
    // either swallows the animal or is unusable on a small bone.
    radius = glm::clamp(glm::length(pc - centre) * 1.15f, 26.f, 140.f);
    return true;
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
    k.rotDeg = eulerAt(track->keys, t); // seed from the pose, never from zero
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

bool App::studioBoneEulerAt(uint32_t boneId, float t, glm::vec3& out)
{
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

bool App::studioBeginBoneDrag(int boneIndex, TrackballHandle handle,
                              glm::vec2 mouse, float radius)
{
    auto* b = studioBehaviour();
    if (!b || boneIndex < 0 || size_t(boneIndex) >= b->rig.bones.size())
        return false;
    if (handle == TrackballHandle::None)
        return false;
    auto* clip = studioEnsureClip();
    if (!clip)
        return false;
    m_studio.gizmoDragBone = boneIndex;
    m_studio.gizmoDragBoneId = b->rig.bones[size_t(boneIndex)].id;
    m_studio.gizmoHandle = handle;
    m_studio.gizmoLastMouse = mouse;
    m_studio.gizmoRadius = radius;
    // The clip clock must not run under the drag, or the key would land at a
    // time the user is not looking at.
    m_studio.playing = false;
    b->clipPlaying = false;
    b->clipTime = m_studio.time;
    // Create the key NOW, so the pose is already authored where the drag will
    // write and the ring has something to move.
    studioKeyAt(*clip, b->rig.bones[size_t(boneIndex)].id, m_studio.time);
    b->animateBones();
    m_taaFirstFrame = true;
    spdlog::info("studio: bone '{}' ring {} drag at t={:.3f}",
                 b->rig.bones[size_t(boneIndex)].name, int(handle),
                 m_studio.time);
    return true;
}

void App::studioApplyBoneDrag(glm::vec2 delta)
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
        m_studio.gizmoHandle = TrackballHandle::None;
        m_studio.gizmoRadius = 0.f;
        return;
    }
    auto* k = studioKeyAt(clip, b->rig.bones[si].id, m_studio.time);
    if (!k)
        return;
    // The object trackball's drag law: 120 degrees across a ring radius, so a
    // small ring is not twitchy and a large one is not sluggish. The radius is
    // the one captured at the drag's START - recomputing it would change the
    // sensitivity under the user's hand.
    const float dpp = 120.f /
        (glm::pi<float>() * std::max(m_studio.gizmoRadius, 48.f));
    switch (m_studio.gizmoHandle) {
    case TrackballHandle::Yaw:   // the circle
        k->rotDeg.y += delta.x * dpp;
        break;
    case TrackballHandle::Pitch: // the wide, flat ring
        k->rotDeg.x -= delta.y * dpp;
        break;
    case TrackballHandle::Roll:  // the tall ring
        k->rotDeg.z += delta.x * dpp;
        break;
    default:
        return;
    }
    // Re-sample THIS frame, so the dragged bone follows the pointer without a
    // frame of lag and the pose on screen is exactly the pose stored.
    b->clipTime = m_studio.time;
    b->animateBones();
    m_taaFirstFrame = true;
}

} // namespace app
} // namespace vf