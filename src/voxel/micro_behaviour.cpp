// micro_behaviour.cpp - the walk and the graze.
#include "voxel/micro_behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace vf::voxel {

namespace {

// A deterministic 0..1 value from a seed, so a behaviour is reproducible:
// the same animal grazes at the same moments on every run (a gate that
// measures the motion needs that).
float hash01(float s)
{
    const float x = std::fmod(std::sin(s * 12.9898f) * 43758.5453f, 1.0f);
    return x < 0.f ? x + 1.f : x;
}

// A smooth 0..1 ramp (the Hermite smoothstep). Used to blend the graze in
// and out: a state that snaps on looks like a glitch, a state that eases in
// looks like an animal deciding to stop.
float smoothstep01(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// A rotation about an axis (Rodrigues' formula), written out rather than
// taken from glm: the extension header is not part of the core build, and
// three rotations do not justify a dependency.
glm::mat3 axisRotation(glm::vec3 axis, float angle)
{
    axis = glm::normalize(axis);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.f - c;
    const glm::vec3 k = axis;
    return glm::mat3(
        t * k.x * k.x + c,        t * k.x * k.y + s * k.z,  t * k.x * k.z - s * k.y,
        t * k.x * k.y - s * k.z,  t * k.y * k.y + c,        t * k.y * k.z + s * k.x,
        t * k.x * k.z + s * k.y,  t * k.y * k.z - s * k.x,  t * k.z * k.z + c);
}

} // namespace

void MicroBehaviour::reset(glm::vec3 pos, float seedIn)
{
    anchor = pos;
    seed = seedIn;
    state = State::Walk;
    stateT = 0.f;
    gait = 0.f;
    wander = hash01(seedIn) * 6.28f;
    // Start facing a random-ish direction, so two animals do not set off
    // in the same direction.
    heading = hash01(seedIn + 3.7f) * 6.28f;
    offset = glm::vec3(0.f);
    rot = glm::mat3(1.f);
}

void MicroBehaviour::update(float dt)
{
    // A long frame (a reload, a stall) must not teleport the animal: the
    // step is clamped, so a 5 s hitch advances the walk by one frame's
    // worth, not by five seconds'.
    dt = std::min(dt, 0.1f);
    stateT += dt;
    wander += dt;

    // The state machine. The durations come from the seed, so two alpacas
    // do not graze in lockstep.
    if (state == State::Walk) {
        if (stateT > 5.0f + 6.0f * hash01(seed + 1.0f)) {
            state = State::Graze;
            stateT = 0.f;
        }
    } else {
        if (stateT > 3.0f + 5.0f * hash01(seed + 2.0f)) {
            state = State::Walk;
            stateT = 0.f;
        }
    }
    const bool walking = state == State::Walk;

    // The heading: a slow wander, plus a pull back toward the anchor when
    // the animal has strayed too far. Both terms are smooth, so the turn
    // eases rather than snapping.
    const float dist = std::sqrt(offset.x * offset.x + offset.z * offset.z);
    float pull = 0.f;
    if (dist > radius && dist > 1e-4f) {
        // The direction home, as a yaw.
        const float home = std::atan2(-offset.x, -offset.z);
        float d = home - heading;
        while (d > 3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        pull = std::clamp(d, -1.2f, 1.2f);
    }
    heading += (0.45f * std::sin(wander * 0.6f + seed) + 1.1f * pull) * dt;
    if (heading > 3.14159265f) heading -= 6.28318531f;
    if (heading < -3.14159265f) heading += 6.28318531f;

    // The walk: the offset advances along the heading. The gait phase is
    // driven by the DISTANCE travelled, not by the time, so the feet keep
    // their rhythm when the animal turns or slows.
    if (walking) {
        const float step = speed * dt;
        offset.x += std::sin(heading) * step;
        offset.z += std::cos(heading) * step;
        gait += (step / std::max(0.05f, stride)) * 6.28318531f;
    }

    // The graze: a bow that eases in over the first second and out over the
    // last. The blend is the smoothstep, so the animal leans down and
    // straightens up rather than jerking.
    float graze = 0.f;
    if (state == State::Graze) {
        const float inT = smoothstep01(stateT / 1.0f);
        const float outT = smoothstep01((3.0f + 5.0f * hash01(seed + 2.0f) -
                                         stateT) / 1.0f);
        graze = std::min(inT, outT);
    }

    // The transform. The rotation is a yaw (the heading), a roll (the gait
    // sway) and a pitch (the graze bow), about the object's pivot - the
    // same bottom-centre pivot the placement uses.
    const float sway = walking ? std::sin(gait) * sway : 0.f;
    const float bob = walking ? std::sin(gait * 2.f) * bob : 0.f;
    const float pitch = graze * grazePitch;

    const glm::mat3 yaw = axisRotation(glm::vec3(0, 1, 0), heading);
    const glm::mat3 roll = axisRotation(glm::vec3(0, 0, 1), sway);
    const glm::mat3 tilt = axisRotation(glm::vec3(1, 0, 0), pitch);
    rot = yaw * roll * tilt;
    offset.y = bob;
}

glm::vec3 MicroBehaviour::toLocal(glm::vec3 p) const
{
    // The inverse of the animated transform: the offset comes off first,
    // then the rotation about the pivot.
    p -= offset;
    return pivot + glm::transpose(rot) * (p - pivot);
}

glm::vec3 MicroBehaviour::dirToLocal(glm::vec3 d) const
{
    return glm::transpose(rot) * d;
}

glm::vec3 MicroBehaviour::toWorld(glm::vec3 p) const
{
    // The forward transform: the rotation about the pivot first, then the
    // offset.
    p = pivot + rot * (p - pivot);
    return p + offset;
}

} // namespace vf::voxel
