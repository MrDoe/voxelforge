#pragma once
// micro_clip.hpp - a recorded animation clip for a MICRO object.
//
// A clip is a motion PRIMITIVE, not a puppeteer: it poses the skeleton (per-
// bone rotations plus a small cosmetic body offset) and declares the
// locomotion profile it was authored for (speed, stride). The actual
// displacement of the object is the locomotion code's job, so a clip can
// never walk the object through a wall - collision stays where it lives.
#ifndef VOXELFORGE_MICRO_CLIP_HPP
#define VOXELFORGE_MICRO_CLIP_HPP

#include "voxel/micro_rig.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vf::voxel {

enum class ClipEase : int { Step = 0, Linear = 1, Smooth = 2 };

struct BoneKey {
    float t = 0.f;
    glm::vec3 rotDeg { 0.f, 0.f, 0.f };
    ClipEase ease = ClipEase::Smooth;
};

struct BoneTrack {
    uint32_t boneId = 0;
    std::string name;
    std::vector<BoneKey> keys;
};

struct BodyKey {
    float t = 0.f;
    glm::vec3 offset { 0.f, 0.f, 0.f };
    float headingDeg = 0.f;
    ClipEase ease = ClipEase::Smooth;
};

enum class ClipClock : int { Distance = 0, Time = 1 };

struct MicroClip {
    std::string name;
    std::string description;
    std::string layer;

    float length = 1.f;
    float fps = 30.f;
    bool loop = true;

    ClipClock clock = ClipClock::Time;
    float speed = 0.20f;
    float stride = 0.90f;
    float blendIn = 0.20f;

    std::vector<BodyKey> body;
    std::vector<BoneTrack> tracks;

    bool empty() const { return tracks.empty() && body.empty(); }

    void sample(float t, const MicroRig& rig,
                std::vector<glm::mat3>& localRot,
                glm::vec3& bodyOffset, float& bodyHeadingDeg) const;

    std::vector<std::string> validate(const MicroRig& rig) const;
};

bool readMicroClip(const std::string& path, MicroClip& out);
bool writeMicroClip(const std::string& path, const MicroClip& clip);

bool parseMicroClip(const std::string& text, MicroClip& out);
std::string serializeMicroClip(const MicroClip& clip);

float wrapClipTime(float t, float length, bool loop);
glm::vec3 interpEuler(const BoneKey& a, const BoneKey& b, float t);
void interpBody(const BodyKey& a, const BodyKey& b, float t,
                glm::vec3& outOffset, float& outHeadingDeg);

} // namespace vf::voxel

#endif // VOXELFORGE_MICRO_CLIP_HPP