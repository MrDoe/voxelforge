#include "voxel/picking.hpp"
#include "voxel/common.hpp"
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

// central-difference normal of the quantised field; the offset must span
// neighbouring cells (>= 1 cell) or every tap lands in the same cell and the
// gradient collapses to zero
glm::vec3 fieldNormal(const VoxelField& field, glm::vec3 p) {
    const float e = VOXEL * 1.5f;
    glm::vec3 n(field.sampleWorld(p + glm::vec3(e, 0, 0)).d -
                    field.sampleWorld(p - glm::vec3(e, 0, 0)).d,
                field.sampleWorld(p + glm::vec3(0, e, 0)).d -
                    field.sampleWorld(p - glm::vec3(0, e, 0)).d,
                field.sampleWorld(p + glm::vec3(0, 0, e)).d -
                    field.sampleWorld(p - glm::vec3(0, 0, e)).d);
    return glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 1, 0);
}

PickHit rayPick(const VoxelField& field, glm::vec3 ro, glm::vec3 rd, float tMax,
                int maxSteps) {
    PickHit out;
    if (!field.valid())
        return out;
    rd = glm::normalize(rd);
    // The records-derived field is quantised to VOXEL cells and evaluated at
    // cell centres, so |d| never settles inside a thin hit window: it jumps
    // straight from positive to negative across a surface. March until the
    // sign flips, then bisect the crossing between the last outside sample
    // and the first inside one.
    float prevT = 0.f;
    float lastD = field.sampleWorld(ro).d;
    if (lastD <= 0.f)
        return out; // camera inside geometry - nothing sensible to pick
    float t = 0.f;
    for (int i = 0; i < maxSteps && t < tMax; ++i) {
        glm::vec3 p = ro + rd * t;
        float d = field.sampleWorld(p).d;
        if (d <= 0.f) {
            // surface crossing in (prevT, t]: bisect to locate it
            float lo = prevT, hi = t;
            for (int k = 0; k < 20; ++k) {
                float mid = 0.5f * (lo + hi);
                if (field.sampleWorld(ro + rd * mid).d <= 0.f)
                    hi = mid;
                else
                    lo = mid;
            }
            glm::vec3 hp = ro + rd * hi; // just inside the surface
            out.hit = true;
            out.pos = hp;
            out.dist = hi;
            out.normal = fieldNormal(field, hp);
            // snap to the first solid cell along the ray: quantise points
            // nudged further inside until the sampled cell is actually solid
            glm::vec3 probeP = hp;
            glm::ivec3 v = worldToVoxel(probeP);
            for (int k = 0; k < 4 && field.sampleWorld(voxelCenter(v)).d > 0.f;
                 ++k) {
                probeP += rd * (VOXEL * 0.5f);
                v = worldToVoxel(probeP);
            }
            out.voxel = v;
            out.mat = field.sampleWorld(hp).mat;
            int ix = out.voxel.x, iz = out.voxel.z;
            out.terrainHeight = field.terrainColumn(ix, iz)
                                    ? field.terrainTopY(ix, iz)
                                    : -1e9f;
            return out;
        }
        prevT = t;
        float step = std::abs(d) * 0.85f;
        step = glm::clamp(step, 0.05f, 2.0f);
        // near-surface steps must stay sub-voxel so thin shells (fences,
        // walls) cannot be leapt over in one go
        if (std::abs(d) < 3.f * VOXEL)
            step = glm::min(step, VOXEL);
        if (std::abs(d - lastD) < 0.001f)
            step = glm::max(step, 0.06f); // stalled on a quantised plateau
        lastD = d;
        t += step;
    }
    return out;
}

} // namespace vf::voxel
