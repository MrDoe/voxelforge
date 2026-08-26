#pragma once
#include <glm/glm.hpp>

namespace vf {

// Push-constant block for the SVO ray-march compute shader (128 bytes).
struct alignas(16) RaymarchPush {
    glm::vec4 camPos;
    glm::vec4 camRight;
    glm::vec4 camUp;
    glm::vec4 camFwd;
    glm::vec4 a; // tanHalfFov, aspect, extentX, extentY
    glm::vec4 b; // worldSize, voxelSize, gridN, frameIdx
    glm::vec4 sunDir; // normalized direction TOWARD the sun (xyz)
    glm::vec4 misc;   // x = animation time (seconds); rest unused
};
static_assert(sizeof(RaymarchPush) == 128);

} // namespace vf
