// micro_behaviour.hpp - an autonomous behaviour for a MICRO object.
//
// A micro object is a separate SVO world on its own fine lattice, placed by
// an explicit world position. That position is STATIC, so a high-detail
// animal stands like a prop. This is the behaviour layer that lets it move:
// the object walks on its own, with a gait, and stops to graze.
//
// The behaviour is a CPU-side state machine that produces a TRANSFORM (an
// offset and a rotation about the object's pivot) - NOT a re-bake. The field
// is baked once; the transform is applied where the field is sampled, so a
// walk costs nothing per frame and the object keeps its exact geometry.
//
// The motion is CONTINUOUS on purpose. A walker that hops in quantised
// steps, or that teleports from one spot to the next, reads as a glitch;
// here the gait phase, the heading and the graze blend are all smooth
// functions of time, so every frame is a small step from the one before it.
//
// The behaviour is suspended while the user manipulates the object (a
// staged move or a staged rotation), because then the transform is the
// user's, not the animal's.
#pragma once

#include "voxel/micro_rig.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace vf::voxel {

struct MicroBehaviour {
    enum class State {
        Walk,   // it wanders, with a gait
        Graze,  // it has stopped and is grazing
    };

    // What the behaviour is TOLD to do. Auto is the animal's own choice (it
    // walks and grazes as it pleases); the others are an instruction - from
    // the user or from the AI ("let the alpaca run") - and it holds until it
    // is changed.
    enum class Command {
        Auto,   // the animal decides
        Walk,   // keep walking
        Graze,  // stop and graze
        Stop,   // stand still
    };
    Command command = Command::Auto;

    // The configuration. The defaults suit a 1.3 m animal; a manifest can
    // override them through the layer's "behaviour" block.
    float speed = 0.20f;       // m/s - a calm alpaca walk
    float radius = 1.2f;       // the wander radius around the anchor (m)
    float bob = 0.004f;        // the gait bob (m, peak) - kept small so the
                               // back stays level (a real alpaca does not
                               // bounce); the ground clamp handles slopes
    float sway = 0.025f;       // the gait sway (radians, peak) - gentle roll
    float grazePitch = 0.0f;   // the graze bow (radians, peak); 0 keeps all
                               // four feet planted (rigid legs cannot extend)
    float stride = 0.90f;      // the distance per gait cycle (m)

    // HEAD TURNING. The head can look in a direction independently of the
    // body's yaw - toward a target, an obstacle, or the direction of travel.
    // It is a yaw relative to the body, smoothed so the head eases rather
    // than snapping. The neck links share it, like the pitch.
    float headTurn = 0.f;      // target (radians, relative to body)
    float headTurnCur = 0.f;   // smoothed current (radians)

    // The runtime state.
    State state = State::Walk;
    float stateT = 0.f;       // the seconds in the current state
    float gait = 0.f;         // the gait phase (radians)
    float heading = 0.f;      // the wander heading (radians)
    // A heading the animal is TURNING TOWARD, at a limited rate. The
    // collision response sets it (a direction the animal must try) instead
    // of jumping the heading: a jump is applied EVERY frame while the
    // obstacle is still there, and the animal spins in place.
    float headingTarget = 0.f;
    bool hasHeadingTarget = false;
    float wander = 0.f;       // the wander phase (drives the heading)
    float seed = 0.f;         // the per-object phase offset
    glm::vec3 anchor { 0.f }; // the home position (the placed position)
    glm::vec3 offset { 0.f }; // the animated offset from the anchor
    glm::mat3 rot { 1.f };    // the animated rotation about the pivot
    glm::vec3 pivot { 0.f };  // the rotation pivot (the bottom-centre)
    bool active = false;      // the layer asked for a behaviour

    // A PINNED graze blend (0 = standing, 1 = fully bowed), or -1 to take it
    // from the state machine. The deterministic pose hook sets it: the blend
    // otherwise depends on stateT and the object's seed, so a "grazing"
    // screenshot could not be reproduced.
    float grazeOverride = -1.f;

    // The SKELETON. The rig is DERIVED from the object's own voxels (so a
    // newly imported model animates too) and the gait below drives it by
    // ROLE, not by a hand-written bone list.
    MicroRig rig;
    // The per-bone transform, filled by animateBones(): the CHAINED rotation
    // and the translation that goes with it. A bone's transform is its
    // parent's composed with its own rotation about its joint, so a limb
    // follows the body it hangs off (without this the head keeps turning
    // about its rest joint while the body bows under it, and the neck tears
    // away from the shoulders).
    std::vector<glm::mat3> boneRot;
    std::vector<glm::vec3> boneXform;   // the translation of each transform
    std::vector<glm::vec3> boneJoint;

    // Set the behaviour up for an object placed at `pos`. `seed` makes two
    // animals of the same kind behave differently (they do not graze in
    // lockstep and they do not turn the same way).
    void reset(glm::vec3 pos, float seed);

    // Advance by dt seconds and recompute the transform.
    void update(float dt);

    // The GAIT: the legs swing in diagonal pairs, the head nods, the tail
    // sways. It is driven by the rig's roles, so it works on any rig - an
    // imported cat animates the same way an imported horse does.
    void animateBones();

    // PIN a pose: the transform, the gait phase and (optionally) the graze
    // blend, with no state machine and no wandering. A headless `--shot` of an
    // animated object is otherwise non-deterministic (the behaviour advances
    // with the frame clock before the shot), so a shader A/B has to freeze it
    // first.
    void pinPose(glm::vec3 offsetIn, float headingDeg, float gaitPhase,
                 float grazeBlend = 0.f);

    // The INVERSE of the animated transform: a world point (or a direction)
    // into the object's own frame. The pick needs it - a moving object is
    // picked where it IS, not where it was baked.
    glm::vec3 toLocal(glm::vec3 p) const;
    glm::vec3 dirToLocal(glm::vec3 d) const;
    // The forward transform: the object's own frame back into the world.
    glm::vec3 toWorld(glm::vec3 p) const;
};

} // namespace vf::voxel
