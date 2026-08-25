#include "voxel/picking.hpp"
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace vf::voxel {

glm::vec3 voxelCenter(glm::ivec3 c) {
    return glm::vec3(-0.5f * WORLD + (float(c.x) + 0.5f) * VOXEL,
                     -0.5f * WORLD + (float(c.y) + 0.5f) * VOXEL,
                     -0.5f * WORLD + (float(c.z) + 0.5f) * VOXEL);
}
glm::ivec3 worldToVoxel(glm::vec3 p) {
    return glm::ivec3(int(std::floor((p.x + 0.5f * WORLD) / VOXEL)),
                      int(std::floor((p.y + 0.5f * WORLD) / VOXEL)),
                      int(std::floor((p.z + 0.5f * WORLD) / VOXEL)));
}
glm::vec3 anchorBottomCenter(glm::ivec3 selectedVoxel) {
    // User spec: selected voxel IS the center-bottom voxel of the new object.
    // So anchor = center of that cell. Callers place object with y = anchor.y + localY.
    return voxelCenter(selectedVoxel);
}

glm::vec3 screenRayDir(double mx, double my, int fbW, int fbH,
                       float tanHalfFov, float aspect,
                       glm::vec3 fwd, glm::vec3 right, glm::vec3 up) {
    // NDC [-1,1], y flipped (GLFW top-left origin)
    float xNdc = float(2.0 * mx / double(fbW) - 1.0);
    float yNdc = float(1.0 - 2.0 * my / double(fbH));
    float xCam = xNdc * tanHalfFov * aspect;
    float yCam = yNdc * tanHalfFov;
    glm::vec3 d = glm::normalize(fwd + xCam * right + yCam * up);
    return d;
}

PickHit rayPick(glm::vec3 ro, glm::vec3 rd, float tMax, int maxSteps) {
    PickHit out;
    if (!sharedHeightmap().loaded()) return out;
    float t = 0.f;
    float lastD = 1e9f;
    for (int i = 0; i < maxSteps && t < tMax; ++i) {
        glm::vec3 p = ro + rd * t;
        // quick world bounds exit: if we go far outside, keep marching but check
        auto s = scene(p);
        float d = s.d;
        // hit threshold: within 2cm of surface (VOXEL*0.2)
        if (std::abs(d) < 0.02f) {
            out.hit = true;
            out.pos = p;
            out.dist = t;
            out.mat = s.mat;
            out.voxel = worldToVoxel(p);
            // normal: central diff, branch by terrain vs object like splat pass
            // If near object surface, use object SDF gradient; else terrain gradient
            const HeightMap& hm = sharedHeightmap();
            out.terrainHeight = hm.sample(p.x, p.z);
            float dObj = std::min({houseAt(p).d, treesAt(p).d, rocksAt(p).d, bushesAt(p).d, fenceAt(p).d, alpacaAt(p).d});
            float dTerrain = p.y - out.terrainHeight;
            if (dObj + 0.01f < dTerrain) {
                float e = 0.03f;
                auto od = [&](glm::vec3 q){ return std::min({houseAt(q).d, treesAt(q).d, rocksAt(q).d, bushesAt(q).d, fenceAt(q).d, alpacaAt(q).d}); };
                glm::vec3 n(od(p+glm::vec3(e,0,0))-od(p-glm::vec3(e,0,0)),
                            od(p+glm::vec3(0,e,0))-od(p-glm::vec3(0,e,0)),
                            od(p+glm::vec3(0,0,e))-od(p-glm::vec3(0,0,e)));
                if (glm::length(n) > 1e-6f) out.normal = glm::normalize(n); else out.normal = glm::vec3(0,1,0);
            } else {
                glm::vec2 g = hm.gradient(p.x, p.z);
                out.normal = glm::normalize(glm::vec3(-g.x, 1.f, -g.y));
            }
            // snap voxel to surface: step back slightly along normal then quantize
            // ensures the picked voxel is the solid one just below surface, not the air above
            glm::vec3 solidP = p - out.normal * (VOXEL * 0.25f);
            auto ss = scene(solidP);
            if (ss.d < 0.05f) { // likely inside solid, use that cell
                out.voxel = worldToVoxel(solidP);
                out.pos = voxelCenter(out.voxel);
            } else {
                // fallback: use hit point quantized directly
                out.voxel = worldToVoxel(p - rd * 0.05f);
            }
            return out;
        }
        // step: conservative
        float step = std::abs(d) * 0.85f;
        step = glm::clamp(step, 0.02f, 2.0f);
        // avoid sticking: detect oscillation
        if (std::abs(d - lastD) < 0.001f) step = glm::max(step, 0.05f);
        lastD = d;
        t += step;
        // if far below terrain and going away, break early?
    }
    return out;
}

} // namespace vf::voxel
