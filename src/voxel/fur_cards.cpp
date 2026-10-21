// fur_cards.cpp - generate transparent hair-clump cards from a micro field.
//
// The cards are a CPU-side product: they are generated once, when the micro
// object is loaded, and never touched again except by the live preview (a
// translation or a rotation, both of which the shader applies). That is the
// same bargain the surfel bake strikes, and for the same reason: a stamp or a
// drag must not cost a re-bake.
//
// Determinism is load-bearing. Two builds of the same object must produce the
// same cards, or the same scene would render differently from one run to the
// next (the same rule the micro emitter and the surfel bake follow), so
// nothing here may depend on iteration order, on a hash map's layout, or on
// the clock.
#include "voxel/fur_cards.hpp"

#include "voxel/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vf::voxel {
namespace {

// A deterministic hash -> [0,1). Same construction as the micro emitter's.
inline float hash01(int x, int y, int z, int slot)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u +
                 uint32_t(z) * 1274126177u + uint32_t(slot) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0x00FFFFFFu) / float(0x00FFFFFFu);
}

inline float hashSigned(int x, int y, int z, int slot)
{
    return hash01(x, y, z, slot) * 2.0f - 1.0f;
}

inline glm::vec3 safeNorm(glm::vec3 v, glm::vec3 fallback)
{
    const float l = glm::length(v);
    return l > 1e-6f ? v / l : fallback;
}

// The field's signed distance at a point in the LOCAL frame (the frame the
// cards live in). sampleSdf() takes the RECORDS frame, which is the local
// frame shifted by the field's own origin - see buildFurCards.
inline float sdfLocal(const MicroField& f, glm::vec3 pLocal)
{
    return f.sampleSdf(pLocal + f.origin);
}

// The outward normal at a point, from the trilinear field's gradient. The
// field grows outward, so the gradient points away from the surface.
//
// The field is quantised (a cell's value is a whole number of cells' depth),
// so a central difference over ONE cell is blocky; half a cell is enough to
// smooth it while staying inside the trilinear stencil's own neighbourhood.
glm::vec3 fieldNormal(const MicroField& f, glm::vec3 pLocal)
{
    const float e = 0.5f * f.voxel;
    const float gx = sdfLocal(f, pLocal + glm::vec3(e, 0, 0)) -
                     sdfLocal(f, pLocal - glm::vec3(e, 0, 0));
    const float gy = sdfLocal(f, pLocal + glm::vec3(0, e, 0)) -
                     sdfLocal(f, pLocal - glm::vec3(0, e, 0));
    const float gz = sdfLocal(f, pLocal + glm::vec3(0, 0, e)) -
                     sdfLocal(f, pLocal - glm::vec3(0, 0, e));
    glm::vec3 g(gx, gy, gz);
    if (glm::length(g) < 1e-7f)
        return glm::vec3(0.f, 1.f, 0.f);
    return glm::normalize(g);
}

// A cheap ambient occlusion at the root: how much of the neighbourhood
// around it is solid. Four taps in the tangent plane, one cell out.
float bakedAo(const MicroField& f, glm::vec3 root, glm::vec3 t1, glm::vec3 t2)
{
    const float r = 1.5f * f.voxel;
    int hits = 0;
    const glm::vec3 dirs[4] = { t1, -t1, t2, -t2 };
    for (const glm::vec3& d : dirs) {
        // A sample that lands INSIDE the solid (the field is negative there)
        // means the surface is concave at that direction: less sky reaches it.
        if (sdfLocal(f, root + d * r) < 0.f)
            ++hits;
    }
    return 1.0f - 0.25f * float(hits);
}

// The baked sun shadow: a binary sphere-trace of the field toward the sun.
//
// The march steps at a QUARTER of the object's own voxel, not at a fixed
// distance: the world bake's 0.1 m DDA is wider than a 5 mm object and steps
// straight over it. The origin is the root plus a hair along the normal
// (0.01 m), NOT the 0.3 m the world march uses - 0.3 m is above a 0.3 m tall
// object, and a shadow cast from there misses the object entirely.
float bakedSunShadow(const MicroField& f, glm::vec3 root, glm::vec3 n,
                     glm::vec3 sunDir, float maxDist)
{
    if (glm::dot(n, sunDir) <= 0.0f)
        return 0.0f;  // the face is turned away from the sun
    const glm::vec3 ro = root + n * 0.01f;
    const float step = 0.25f * f.voxel;
    for (float t = step; t < maxDist; t += step) {
        if (sdfLocal(f, ro + sunDir * t) < 0.f)
            return 0.0f;
    }
    return 1.0f;
}

}  // namespace

bool buildFurCards(const MicroField& field, const FurParams& params, FurObject& out,
                   std::string* err)
{
    out = FurObject {};
    if (field.size3() == 0 || !(field.voxel > 0.0f)) {
        if (err)
            *err = "empty field";
        return false;
    }
    if (params.perCell <= 0 || !(params.length > 0.f) || !(params.width > 0.f)) {
        if (err)
            *err = "degenerate fur parameters";
        return false;
    }

    std::vector<uint8_t> isFur(size_t(kPaletteN), 0);
    for (uint8_t m : params.materials) {
        if (m < kPaletteN)
            isFur[m] = 1;
    }

    const float vox = field.voxel;
    const glm::ivec3 dim = field.dim;
    const glm::vec3 sun = safeNorm(params.sunDir, glm::vec3(0.f, 1.f, 0.f));
    // The shadow march is bounded by the object's own size: nothing outside
    // the grid can occlude it.
    const float maxShadowDist = glm::length(glm::vec3(dim)) * vox;

    // ---- pass 1: the surface cells, their normals and their roots -------
    struct Root {
        glm::vec3 p;
        glm::vec3 n;
        glm::vec3 t1;
        glm::vec3 t2;
        glm::vec3 col;
        uint8_t mat = 0;
    };
    std::vector<Root> roots;
    roots.reserve(size_t(dim.x) * size_t(dim.y) / 8);

    for (int z = 0; z < dim.z; ++z) {
        for (int y = 0; y < dim.y; ++y) {
            for (int x = 0; x < dim.x; ++x) {
                const size_t i = field.idx(x, y, z);
                if (!field.solid[i])
                    continue;
                if (!isFur[field.mat[i]])
                    continue;
                // A SURFACE cell: at least one of the six neighbours is not
                // solid. The interior of the coat carries no fur - the cards
                // there would be buried and never seen.
                bool exposed = false;
                for (int k = 0; k < 6 && !exposed; ++k) {
                    const int nx = x + (k == 0 ? -1 : (k == 1 ? 1 : 0));
                    const int ny = y + (k == 2 ? -1 : (k == 3 ? 1 : 0));
                    const int nz = z + (k == 4 ? -1 : (k == 5 ? 1 : 0));
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= dim.x || ny >= dim.y ||
                        nz >= dim.z || !field.solid[field.idx(nx, ny, nz)])
                        exposed = true;
                }
                if (!exposed)
                    continue;

                const glm::vec3 c((float(x) + 0.5f) * vox, (float(y) + 0.5f) * vox,
                                  (float(z) + 0.5f) * vox);
                const glm::vec3 n = fieldNormal(field, c);
                // The root sits ON the surface: the field's value at the cell
                // centre is the (negative) distance to it, so stepping along
                // the normal by that distance lands on the zero crossing.
                const float d = sdfLocal(field, c);
                const float step = glm::clamp(-d, 0.0f, 1.5f * vox);
                const glm::vec3 up = std::fabs(n.y) < 0.99f ? glm::vec3(0.f, 1.f, 0.f)
                                                             : glm::vec3(1.f, 0.f, 0.f);
                Root r;
                r.p = c + n * step;
                r.n = n;
                r.t1 = safeNorm(glm::cross(n, up), glm::vec3(1.f, 0.f, 0.f));
                r.t2 = glm::cross(n, r.t1);
                r.col = glm::vec3(field.r[i], field.g[i], field.b[i]) / 255.0f;
                r.mat = field.mat[i];
                roots.push_back(r);
            }
        }
    }
    if (roots.empty()) {
        if (err)
            *err = "no fur material on the surface";
        return false;
    }

    // ---- pass 2: the cards ------------------------------------------------
    const glm::vec3 down(0.f, -1.f, 0.f);
    for (size_t ri = 0; ri < roots.size(); ++ri) {
        const Root& r = roots[ri];
        const int x = int(std::floor(r.p.x / vox));
        const int y = int(std::floor(r.p.y / vox));
        const int z = int(std::floor(r.p.z / vox));
        // The groom direction: DOWN the surface, so the fleece hangs. On a
        // surface that already faces down, the projection degenerates and the
        // hash picks a tangent instead.
        glm::vec3 groom = down - r.n * glm::dot(down, r.n);
        if (glm::length(groom) < 0.25f) {
            const float h = hash01(x, y, z, 77);
            groom = (h < 0.5f) ? r.t1 : r.t2;
        }
        groom = glm::normalize(groom);
        // The strand leaves the surface at an angle: a card that lies IN the
        // tangent plane would be invisible from the front, so the direction
        // is tilted out of the surface by 25..60 degrees.
        const float ao = bakedAo(field, r.p, r.t1, r.t2);
        const float sh = bakedSunShadow(field, r.p, r.n, sun, maxShadowDist);
        for (int k = 0; k < params.perCell; ++k) {
            const int slot = k * 7;
            // The root wanders inside the cell, so the coat is not a lattice
            // of dots.
            const float j1 = hashSigned(x, y, z, slot + 1);
            const float j2 = hashSigned(x, y, z, slot + 2);
            const glm::vec3 root = r.p + (r.t1 * j1 + r.t2 * j2) * (0.42f * vox);
            // The groom direction, spun around the normal by a hashed angle
            // and tilted out of the surface by a hashed amount. The tilt is
            // SHALLOW: a steep strand reads as a spike sticking out of the
            // body, and wool lies close to it.
            const float spin = hash01(x, y, z, slot + 3) * 6.2831853f;
            const float tilt = glm::radians(
                params.tiltMin +
                (params.tiltMax - params.tiltMin) * hash01(x, y, z, slot + 4));
            // The groom direction, SPUN inside the surface's own tangent
            // frame.
            //
            // The obvious spelling - groom*cos(spin) + t2*sin(spin) - is NOT
            // a rotation unless the groom happens to lie along t1, and the
            // groom is the projection of "down", so it generally does not.
            // The length then varies with the spin, and at the spin where the
            // two terms cancel the direction is pure numerical noise: the
            // card degenerated into a SPOKE pointing straight out of the
            // surface (measured: the strand's angle to the normal ranged
            // from 8.6 to 89.8 degrees instead of the intended 12..38).
            //
            // Expressing the groom in the tangent frame and rotating THAT
            // keeps the length at one for every spin.
            const float g1 = glm::dot(groom, r.t1);
            const float g2 = glm::dot(groom, r.t2);
            const float cs = std::cos(spin), sn = std::sin(spin);
            const glm::vec3 spun = r.t1 * (g1 * cs - g2 * sn) +
                                   r.t2 * (g1 * sn + g2 * cs);
            const glm::vec3 dir = glm::normalize(spun * std::cos(tilt) +
                                                 r.n * std::sin(tilt));
            FurCard c;
            const float len = params.length * (0.60f + 0.80f * hash01(x, y, z, slot + 5));
            c.rootLen = glm::vec4(root, len);
            c.dirWidth = glm::vec4(dir, params.width * (0.70f + 0.60f * hash01(x, y, z, slot + 6)));
            c.nrmAlpha = glm::vec4(r.n, params.alpha * (0.55f + 0.90f * hash01(x, y, z, slot + 7)));
            // The twist about the strand, and the CURL: the centre line's
            // amplitude, its cycles over the strand's length and its phase.
            // The amplitude scales with the length, so a short clump curls
            // less in metres and a long one curls more.
            c.tint = glm::vec4(r.col, hashSigned(x, y, z, slot + 8) * 0.9f);
            c.shade = glm::vec4(ao, sh, float(r.mat), 0.f);
            c.curl = glm::vec4(len * params.curlAmp *
                                   (0.55f + 0.90f * hash01(x, y, z, slot + 9)),
                               params.curlCycles *
                                   (0.65f + 0.70f * hash01(x, y, z, slot + 10)),
                               hash01(x, y, z, slot + 11) * 6.2831853f, 0.f);
            out.cards.push_back(c);
        }
    }
    if (out.cards.empty()) {
        if (err)
            *err = "no cards";
        return false;
    }

    // ---- pass 3: the bucket grid ------------------------------------------
    // The grid covers every card's bounding box, so a ray that clips a card
    // always visits a cell that holds it.
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());
    for (const FurCard& c : out.cards) {
        const glm::vec3 root = glm::vec3(c.rootLen);
        const glm::vec3 tip = root + glm::vec3(c.dirWidth) * c.rootLen.w;
        const glm::vec3 pad(c.dirWidth.w * 0.5f + c.curl.x);
        lo = glm::min(lo, glm::min(root, tip) - pad);
        hi = glm::max(hi, glm::max(root, tip) + pad);
    }
    const float cs = params.cellSize;
    out.grid.cell = glm::vec3(cs);
    out.grid.origin = lo;
    out.grid.dim = glm::max(glm::ivec3(1), glm::ivec3(glm::ceil((hi - lo) / cs)));

    const size_t cells = size_t(out.grid.dim.x) * size_t(out.grid.dim.y) *
                         size_t(out.grid.dim.z);
    std::vector<uint32_t> counts(cells + 1, 0);
    // The cell range a card touches.
    auto cardRange = [&](const FurCard& c, glm::ivec3& c0, glm::ivec3& c1) {
        const glm::vec3 root = glm::vec3(c.rootLen);
        const glm::vec3 tip = root + glm::vec3(c.dirWidth) * c.rootLen.w;
        // The pad covers the half width AND the curl: the centre line wanders
        // off the straight root-to-tip line by up to the curl amplitude, and a
        // card filed in the wrong cell is a card the DDA never finds.
        const glm::vec3 pad(c.dirWidth.w * 0.5f + c.curl.x);
        const glm::vec3 a = glm::min(root, tip) - pad;
        const glm::vec3 b = glm::max(root, tip) + pad;
        c0 = glm::clamp(glm::ivec3(glm::floor((a - out.grid.origin) / cs)),
                        glm::ivec3(0), out.grid.dim - glm::ivec3(1));
        c1 = glm::clamp(glm::ivec3(glm::floor((b - out.grid.origin) / cs)),
                        glm::ivec3(0), out.grid.dim - glm::ivec3(1));
    };
    for (const FurCard& c : out.cards) {
        glm::ivec3 c0, c1;
        cardRange(c, c0, c1);
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x)
                    ++counts[size_t(z) * size_t(out.grid.dim.y) * size_t(out.grid.dim.x) +
                             size_t(y) * size_t(out.grid.dim.x) + size_t(x) + 1];
    }
    out.cellStart.assign(cells + 1, 0);
    for (size_t i = 0; i < cells; ++i)
        out.cellStart[i + 1] = out.cellStart[i] + counts[i + 1];
    out.cellItems.assign(out.cellStart[cells], 0);
    std::vector<uint32_t> cursor(out.cellStart.begin(), out.cellStart.end() - 1);
    for (size_t ci = 0; ci < out.cards.size(); ++ci) {
        glm::ivec3 c0, c1;
        cardRange(out.cards[ci], c0, c1);
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x) {
                    const size_t cell = size_t(z) * size_t(out.grid.dim.y) *
                                           size_t(out.grid.dim.x) +
                                       size_t(y) * size_t(out.grid.dim.x) + size_t(x);
                    out.cellItems[cursor[cell]++] = uint32_t(ci);
                }
    }
    return true;
}

}  // namespace vf::voxel
