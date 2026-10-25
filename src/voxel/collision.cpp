// collision.cpp - sphere sweeps and walker footprints. See collision.hpp.
#include "voxel/collision.hpp"

#include <algorithm>
#include <cmath>

namespace vf::voxel {

namespace {

// The 26 directions around a sphere, each normalised so the sample lies on the
// sphere's surface. Building them once keeps the per-frame cost to 27 predicate
// calls rather than 26 normalisations.
struct Dirs {
    glm::vec3 v[26];
    Dirs()
    {
        int n = 0;
        for (int axis = 0; axis < 3; ++axis)
            for (int sign = -1; sign <= 1; sign += 2) {
                glm::vec3 d(0.0f);
                d[axis] = float(sign);
                v[n++] = d;
            }
        // 12 edges
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b)
                for (int sa = -1; sa <= 1; sa += 2)
                    for (int sb = -1; sb <= 1; sb += 2) {
                        glm::vec3 d(0.0f);
                        d[a] = float(sa);
                        d[b] = float(sb);
                        v[n++] = glm::normalize(d);
                    }
        // 8 corners
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    v[n++] = glm::normalize(glm::vec3(float(sx), float(sy),
                                                      float(sz)));
        // 6 + 12 + 8 = 26
    }
};

const Dirs& dirs()
{
    static const Dirs d;
    return d;
}

} // namespace

bool sphereBlocked(const SolidTest& solid, glm::vec3 centre, float radius)
{
    if (!solid)
        return false;
    if (solid(centre))
        return true;
    const float r = std::max(radius, 0.0f);
    if (r <= 0.0f)
        return false;
    for (const glm::vec3& d : dirs().v)
        if (solid(centre + d * r))
            return true;
    return false;
}

SphereMove moveSphere(const SolidTest& solid, glm::vec3 from, glm::vec3 delta,
                      float radius)
{
    SphereMove out;
    out.pos = from;
    if (!solid)
    {
        out.pos = from + delta;
        return out;
    }
    const float len = glm::length(delta);
    if (len <= 1e-7f)
        return out;

    // Already inside: honour the requested move so an embedded camera can
    // escape. A blocked start must never become a permanent trap.
    if (sphereBlocked(solid, from, radius))
    {
        out.pos = from + delta;
        out.hit = true;
        return out;
    }

    // Half a radius per substep: a full-diameter step could straddle a
    // one-cell wall, and the substeps are the only thing that stops a fast
    // camera passing through it.
    const float stepLen = std::max(radius * 0.5f, 0.02f);
    const int steps = std::clamp(int(std::ceil(len / stepLen)), 1, 32);
    const glm::vec3 step = delta / float(steps);

    glm::vec3 pos = from;
    for (int i = 0; i < steps; ++i)
    {
        const glm::vec3 cand = pos + step;
        if (!sphereBlocked(solid, cand, radius))
        {
            pos = cand;
            continue;
        }
        out.hit = true;
        // Slide: try each axis on its own, largest component first, always
        // from the position reached by the previous axis. That is what makes
        // walking into a wall at an angle glide along it instead of stopping.
        int order[3] = { 0, 1, 2 };
        std::sort(order, order + 3, [&](int a, int b) {
            return std::fabs(step[a]) > std::fabs(step[b]);
        });
        glm::vec3 slid = pos;
        bool moved = false;
        for (int k = 0; k < 3; ++k)
        {
            const int a = order[k];
            if (std::fabs(step[a]) < 1e-7f)
                continue;
            glm::vec3 t = slid;
            t[a] += step[a];
            if (!sphereBlocked(solid, t, radius))
            {
                slid = t;
                moved = true;
            }
        }
        if (!moved)
        {
            out.stopped = true;
            break;
        }
        pos = slid;
    }
    out.pos = pos;
    return out;
}

bool cylinderBlocked(const SolidTest& solid, glm::vec3 centre, float radius,
                     float y0, float y1, int sides, int layers)
{
    if (!solid)
        return false;
    sides = std::max(sides, 4);
    layers = std::max(layers, 1);
    const float lo = std::min(y0, y1);
    const float hi = std::max(y0, y1);
    const float r = std::max(radius, 0.0f);
    for (int l = 0; l < layers; ++l)
    {
        const float t = layers == 1 ? 0.0f : float(l) / float(layers - 1);
        const float y = glm::mix(lo, hi, t);
        if (solid(glm::vec3(centre.x, y, centre.z)))
            return true;
        for (int s = 0; s < sides; ++s)
        {
            const float a = 6.28318530718f * float(s) / float(sides);
            if (solid(glm::vec3(centre.x + std::cos(a) * r, y,
                                centre.z + std::sin(a) * r)))
                return true;
        }
    }
    return false;
}

} // namespace vf::voxel
