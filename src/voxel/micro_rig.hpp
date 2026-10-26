// micro_rig.hpp - a SKELETON derived from an object's own geometry.
//
// A micro object is a set of voxels on its own fine lattice, and until now
// it was RIGID: the whole object could be moved, turned or scaled, but its
// parts could not move relative to each other. A rig is the bone structure
// that makes a living animal possible.
//
// THE RIG IS DERIVED, NOT AUTHORED. Nothing here knows what an alpaca is:
// the bones are found in the object's own voxels, so a newly imported model
// (an STL turned into a micro layer) animates the moment it is loaded, with
// no per-object code and no per-object asset. An authored rig could override
// this, but the default has to work on its own.
//
// The derivation is deliberately generic and geometric:
//
//   * the SPINE is the object's longest axis (a PCA-free approximation: the
//     longest AABB axis), and the CORE is the middle band of it - the mass
//     of the body;
//   * the LIMBS are the vertical clusters hanging BELOW the core - the legs;
//   * the HEAD is the mass beyond the core's front, along the spine;
//   * the TAIL is the mass beyond the core's back.
//
// A bone is a SEGMENT (a start and an end in the object's own frame) plus
// the role it plays. A segment is more general than a pivot: a leg is a
// segment, the neck is a segment, and the joint a bone rotates about is the
// segment's END that touches the core.
//
// The WEIGHTS are not stored: a point belongs to the bone whose segment is
// nearest, with the seams placed where the two bones meet INSIDE the body
// (so the seam never shows on the silhouette).
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vf::voxel {

// What a bone IS, which is what the animation drives.
enum class BoneRole {
    Spine,  // the body: the root of the hierarchy
    Leg,    // a limb below the core
    Head,   // the head: the TIP of the front extension
    Tail,   // the tail: the back extension
    Neck,   // a link of the front extension BELOW the head (the chain)
};

struct MicroBone {
    // Stable id, assigned at derivation (index + 1) and preserved across
    // rig edits (insert/delete/reparent). Clip tracks reference this.
    // 0 means "unset" (derived rigs from old caches); M1 assigns on derive.
    uint32_t id = 0;
    // Human-readable name (e.g. "leg0", "head"). Written to the override
    // JSON for readability; never used for lookup.
    std::string name;
    // The segment, in the object's own frame (metres, from the layer's min
    // corner - the frame the records use).
    glm::vec3 a { 0.f };
    glm::vec3 b { 0.f };
    // Capsule radius: the median distance of the surface cells this bone
    // owns to the bone's segment. Used by the shader's bone-ownership test
    // (microBoneAt / furBoneAt) to subtract a per-bone thickness, so a point
    // is attributed to the bone whose capsule it lies inside rather than the
    // geometrically nearest segment. Per-bone (NOT uniform) - a uniform
    // radius made flips worse.
    float radius = 0.f;
    BoneRole role = BoneRole::Spine;
    // Which leg of the pair (0..3), so the gait can pair them diagonally. A
    // non-leg bone keeps it at -1.
    int legIndex = -1;
    // The bone this one hangs off (-1 = the root). A skeleton is a CHAIN: a
    // child's transform has to be composed with its parent's, or the head
    // keeps turning about its REST joint while the body bows under it, and
    // the neck tears away from the shoulders.
    int parent = -1;
};

struct MicroRig {
    std::vector<MicroBone> bones;
    // The frame the derivation found, so the animation can walk along the
    // spine and the gait can tell front from back.
    glm::vec3 center { 0.f };    // the object's centre
    glm::vec3 up { 0.f, 1.f, 0.f };      // the up axis
    glm::vec3 forward { 0.f, 0.f, 1.f }; // the spine's direction
    float length = 0.f;        // the spine's length
    int legCount = 0;

    bool valid() const { return !bones.empty(); }
    // The bone whose segment is nearest a point, or -1.
    int boneAt(glm::vec3 p) const;
};

// Derive a rig from a micro object's field. `field` must be the object's
// own field (the cells are the object's voxels, in its local frame).
bool deriveMicroRig(const class MicroField& field, MicroRig& out);

// The rig CACHE. Deriving a rig is a pass over the object's voxels, and it
// only has to happen ONCE: the result is saved next to the layer
// ("micro/alpaca.rig") and read back on every later load. The cache carries
// the layer's signature (its size and mtime), so a re-imported or edited
// layer invalidates it and the rig is derived again.
//
// The cache is a convenience, never a dependency: a missing or stale file
// just means the rig is derived in memory.
bool readMicroRigCache(const std::string& path, MicroRig& out,
                       uint64_t layerSize, uint64_t layerMtime);
bool writeMicroRigCache(const std::string& path, const MicroRig& rig,
                        uint64_t layerSize, uint64_t layerMtime);

// The rig OVERRIDE. An author-owned JSON file ("micro/alpaca.rig.json") that
// wins over the derived cache when its layer signature matches. The binary
// cache stays a convenience; the override is the authored truth.
bool readMicroRigOverride(const std::string& path, MicroRig& out,
                          uint64_t layerSize, uint64_t layerMtime);
bool writeMicroRigOverride(const std::string& path, const MicroRig& rig,
                           uint64_t layerSize, uint64_t layerMtime);

// Validate a rig: bone count <= 12, parent < index, no zero-length segments,
// known roles. Returns a list of human-readable issues (empty = clean).
std::vector<std::string> validateRig(const MicroRig& rig);

// Compose LOCAL bone rotations into rigid transforms along the parent chain.
//
// A skeleton is a CHAIN, not a bag of pivots: a bone's transform is its
// parent's composed with its own, `R_w = R_p * R_l` and
// `t_w = R_p * t_l + t_p`, where the local transform turns the bone about its
// own joint (`t_l = a - R_l * a`). The composition of two pivot rotations is
// not itself a pivot rotation, which is why the pair (R, t) is carried
// explicitly instead of "a pivot and an angle": the old per-bone independent
// rotation left the head turning about a joint the body had already moved,
// and the neck tore off the shoulders.
//
// `localRot` may be shorter than the bone list (a missing entry = identity).
// `rot`/`xform` are resized to the bone count. This is the ONLY implementation
// of the chain rule - the behaviour and the offline proof tool both call it.
void composeBoneChain(const MicroRig& rig,
                      const std::vector<glm::mat3>& localRot,
                      std::vector<glm::mat3>& rot,
                      std::vector<glm::vec3>& xform);

} // namespace vf::voxel
