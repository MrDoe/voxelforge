#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace vf::voxel {

struct PickHit {
    bool hit = false;
    glm::vec3 pos {0.f};      // world hit point
    glm::vec3 normal {0.f,1.f,0.f};
    uint8_t mat = 0;
    float dist = 0.f;         // t along ray
    glm::ivec3 voxel {0};     // quantized lattice coord
    float terrainHeight = 0.f;
};

// Ray march using scene() CPU truth. ro = camera pos, rd = normalized dir.
PickHit rayPick(glm::vec3 ro, glm::vec3 rd, float tMax = 90.f, int maxSteps = 900);

// Screen → ray helper. mx,my in framebuffer px (0..W, 0..H, top-left origin).
// tanHalfFov = tan(fov*0.5), aspect = W/H
glm::vec3 screenRayDir(double mx, double my, int fbW, int fbH,
                       float tanHalfFov, float aspect,
                       glm::vec3 fwd, glm::vec3 right, glm::vec3 up);

glm::vec3 voxelCenter(glm::ivec3 c); // world pos of lattice cell center
glm::ivec3 worldToVoxel(glm::vec3 p);
glm::vec3 anchorBottomCenter(glm::ivec3 selectedVoxel); // same as voxelCenter, documented as bottom-center anchor

} // namespace vf::voxel
