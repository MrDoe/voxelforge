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

#include <glm/glm.hpp>

namespace vf::voxel {

struct MicroBehaviour {
    enum class State {
        Walk,   // it wanders, with a gait
        Graze,  // it has stopped and is grazing
    };

    // The configuration. The defaults suit a 1.3 m animal; a manifest can
    // override them through the layer's "behaviour" block.
    float speed = 0.22f;       // m/s
    float radius = 1.2f;       // the wander radius around the anchor (m)
    float bob = 0.014f;        // the gait bob (m, peak)
    float sway = 0.030f;       // the gait sway (radians, peak)
    float grazePitch = 0.30f;  // the graze bow (radians, peak)
    float stride = 0.85f;      // the distance per gait cycle (m)

    // The runtime state.
    State state = State::Walk;
    float stateT = 0.f;       // the seconds in the current state
    float gait = 0.f;         // the gait phase (radians)
    float heading = 0.f;      // the wander heading (radians)
    float wander = 0.f;       // the wander phase (drives the heading)
    float seed = 0.f;         // the per-object phase offset
    glm::vec3 anchor { 0.f }; // the home position (the placed position)
    glm::vec3 offset { 0.f }; // the animated offset from the anchor
    glm::mat3 rot { 1.f };    // the animated rotation about the pivot
    glm::vec3 pivot { 0.f };  // the rotation pivot (the bottom-centre)
    bool active = false;      // the layer asked for a behaviour

    // Set the behaviour up for an object placed at `pos`. `seed` makes two
    // animals of the same kind behave differently (they do not graze in
    // lockstep and they do not turn the same way).
    void reset(glm::vec3 pos, float seed);

    // Advance by dt seconds and recompute the transform.
    void update(float dt);

    // The INVERSE of the animated transform: a world point (or a direction)
    // into the object's own frame. The pick needs it - a moving object is
    // picked where it IS, not where it was baked.
    glm::vec3 toLocal(glm::vec3 p) const;
    glm::vec3 dirToLocal(glm::vec3 d) const;
    // The forward transform: the object's own frame back into the world.
    glm::vec3 toWorld(glm::vec3 p) const;
};

} // namespace vf::voxel
