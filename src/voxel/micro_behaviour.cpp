// micro_behaviour.cpp - the walk and the graze.
#include "voxel/micro_behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace vf::voxel {

namespace {

// How long the graze takes to lean down and to straighten up (seconds), and
// how long it stays down before it wanders off again.
constexpr float kGrazeEase = 2.5f;
constexpr float kGrazeHold = 3.0f;

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
    // do not graze in lockstep. A COMMAND overrides it: the animal keeps
    // walking when it is told to, and keeps grazing when it is told to
    // (the AI can say "let the alpaca run" and it runs).
    if (command == Command::Walk) {
        state = State::Walk;
    } else if (command == Command::Graze) {
        state = State::Graze;
    } else if (command == Command::Stop) {
        state = State::Graze;
        stateT = 0.f;
    } else if (state == State::Walk) {
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
    // A pending target (the collision response) wins over the wander, and
    // the turn is RATE LIMITED: the animal swings its body round instead of
    // snapping, and the wander resumes when it has arrived.
    if (hasHeadingTarget) {
        float d = headingTarget - heading;
        while (d > 3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        const float maxTurn = 1.1f * dt;   // rad/s
        if (std::abs(d) <= maxTurn) {
            heading = headingTarget;
            hasHeadingTarget = false;
        } else {
            heading += (d > 0.f ? maxTurn : -maxTurn);
        }
    } else {
        heading += (0.45f * std::sin(wander * 0.6f + seed) + 1.1f * pull) * dt;
    }
    if (heading > 3.14159265f) heading -= 6.28318531f;
    if (heading < -3.14159265f) heading += 6.28318531f;

    // The walk: the offset advances along the heading. The gait phase is
    // driven by the DISTANCE travelled, not by the time, so the feet keep
    // their rhythm when the animal turns or slows. A STOP freezes it (the
    // animal stands, it does not skate).
    if (walking && command != Command::Stop) {
        const float step = speed * dt;
        offset.x += std::sin(heading) * step;
        offset.z += std::cos(heading) * step;
        gait += (step / std::max(0.05f, stride)) * 6.28318531f;
    }

    // The graze: a bow that eases in over the first second and out over the
    // last. The blend is the smoothstep, so the animal leans down and
    // straightens up rather than jerking.
    // The graze eases IN and OUT over a couple of seconds each: an animal
    // that snaps its head down reads as a glitch, one that leans down
    // slowly reads as an animal deciding to eat.
    float graze = 0.f;
    if (state == State::Graze) {
        const float inT = smoothstep01(stateT / kGrazeEase);
        const float outT = smoothstep01((kGrazeHold + 5.0f * hash01(seed + 2.0f) -
                                         stateT) / kGrazeEase);
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

void MicroBehaviour::pinPose(glm::vec3 offsetIn, float headingDeg,
                              float gaitPhase, float grazeBlend)
{
    state = State::Walk;
    offset = offsetIn;
    heading = glm::radians(headingDeg);
    gait = gaitPhase;
    grazeOverride = glm::clamp(grazeBlend, 0.f, 1.f);
    // The same yaw the walk applies; the sway/bob are gait-driven and are
    // deliberately left out - a pinned pose must be exactly reproducible.
    rot = axisRotation(glm::vec3(0, 1, 0), heading);
    animateBones();
}

void MicroBehaviour::animateBones()
{
    // The GAIT, driven by the rig's roles.
    //
    // The legs swing about their joints, in DIAGONAL pairs (a walk, not a
    // trot): the pairs come from the rig's own leg order, which the
    // derivation sorted along the spine, so the front-left swings with the
    // back-right. The head nods with the stride and dips further when the
    // animal grazes; the tail sways.
    if (rig.bones.empty())
        return;
    const size_t n = rig.bones.size();
    if (boneRot.size() != n) {
        boneRot.assign(n, glm::mat3(1.f));
        boneXform.assign(n, glm::vec3(0.f));
        boneJoint.resize(n);
        for (size_t i = 0; i < n; ++i)
            boneJoint[i] = rig.bones[i].a;
    }
    // The frame: up and forward come from the derivation, and the side axis
    // (across the body) is their cross product.
    const glm::vec3 up = rig.up;
    glm::vec3 fwd = rig.forward;
    glm::vec3 side = glm::cross(up, fwd);
    if (glm::length(side) < 1e-5f)
        side = glm::vec3(1.f, 0.f, 0.f);
    side = glm::normalize(side);
    fwd = glm::normalize(glm::cross(side, up));

    // The legs, sorted along the spine, so the pairs are diagonal.
    std::vector<size_t> legs;
    for (size_t i = 0; i < n; ++i)
        if (rig.bones[i].role == BoneRole::Leg)
            legs.push_back(i);
    std::sort(legs.begin(), legs.end(), [&](size_t a, size_t b) {
        const float fa = glm::dot(rig.bones[a].a, fwd);
        const float fb = glm::dot(rig.bones[b].a, fwd);
        if (std::abs(fa - fb) > 1e-3f)
            return fa > fb; // the front legs first
        return glm::dot(rig.bones[a].a, side) > glm::dot(rig.bones[b].a, side);
    });

    // How many links the front extension is made of (Neck links + the Head
    // tip). The bend is spread over them, so a three-link neck curves instead
    // of hinging at the shoulder; a rig with one Head bone divides by one and
    // behaves exactly as before.
    int neckLinks = 0;
    for (size_t i = 0; i < n; ++i)
        if (rig.bones[i].role == BoneRole::Neck ||
            rig.bones[i].role == BoneRole::Head)
            ++neckLinks;
    const float stride = gait;
    // How far down the animal is leaning (0 = standing, 1 = fully bowed).
    // A pinned pose overrides it, so a screenshot of the graze is exact.
    const float grazeBlend =
        (grazeOverride >= 0.f)
            ? grazeOverride
            : ((state == State::Graze)
                   ? std::min(smoothstep01(stateT / kGrazeEase),
                              smoothstep01((kGrazeHold +
                                            5.0f * hash01(seed + 2.0f) -
                                            stateT) /
                                           kGrazeEase))
                   : 0.f);
    // The head/neck chain's TOTAL tip angle. The links share it, so the neck
    // curves; see the Neck/Head case below.
    const float headTotal = 0.06f * std::sin(gait * 2.f) + grazeBlend * 0.70f;
    for (size_t i = 0; i < n; ++i) {
        const MicroBone& b = rig.bones[i];
        float angle = 0.f;
        glm::vec3 axis = side;
        switch (b.role) {
        case BoneRole::Leg: {
            // The diagonal pairs: the first and the last of the sorted legs
            // swing together, the middle two together.
            size_t rank = 0;
            for (size_t k = 0; k < legs.size(); ++k)
                if (legs[k] == i)
                    rank = k;
            const bool phase0 = (rank == 0) || (rank + 1 == legs.size());
            const float ph = phase0 ? 0.f : 3.14159265f;
            // The swing is about the leg's own axis across the body, and it
            // is larger for the leg that is BEHIND (it has to catch up).
            angle = std::sin(stride + ph) * 0.32f;
            // A grazing animal STANDS on four feet: it does not freeze
            // mid-stride. Ease the leg swing out as the graze blends in, so
            // all four hooves plant while the head lowers.
            angle *= (1.f - grazeBlend);
            break;
        }
        case BoneRole::Spine: {
            // The BOW. The spine's segment starts at the body's BACK, so a
            // pitch about it drops the FRONT - the animal leans over its
            // front legs to reach the grass, the way a grazing quadruped
            // does. Without it the head can only dip by the length of its
            // neck (0.32 m on the alpaca), which is nowhere near the ground.
            // The body stays LEVEL while grazing: a spine bow tips the body and
            // lifts the back feet off the ground (rigid legs cannot extend), so
            // the animal would no longer stand on four feet. The head reaches
            // down through the neck instead.
            angle = 0.0f;
            axis = side;
            break;
        }
        case BoneRole::Neck:
        case BoneRole::Head: {
            // The neck reaches down and forward, on top of the bow. The angle
            // is SHARED by the chain's links (the tip's total is their sum),
            // so the bend is a curve along the neck, not a hinge at its base.
            angle = headTotal / float(std::max(1, neckLinks));
            axis = side;
            break;
        }
        case BoneRole::Tail: {
            angle = 0.22f * std::sin(stride + 1.1f);
            axis = up;
            break;
        }
        default:
            angle = 0.f;
            break;
        }
        boneJoint[i] = b.a;
        // The LOCAL transform: a rotation about the bone's own joint, in the
        // rest frame.
        const glm::mat3 R = axisRotation(axis, angle);
        glm::vec3 t = b.a - R * b.a;
        // CHAIN it: the bone's world transform is its parent's composed with
        // its own. Without this the head keeps turning about its REST joint
        // while the body bows under it - the neck visibly tears away from
        // the shoulders (which is what the user saw).
        const int parent = b.parent;
        if (parent >= 0 && size_t(parent) < n) {
            const glm::mat3 Rp = boneRot[size_t(parent)];
            const glm::vec3 tp = boneXform[size_t(parent)];
            boneRot[i] = Rp * R;
            boneXform[i] = Rp * t + tp;
        } else {
            boneRot[i] = R;
            boneXform[i] = t;
        }
    }
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
