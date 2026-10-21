// micro_rig.cpp - deriving a skeleton from an object's own voxels, and
// caching the result.
#include "voxel/micro_rig.hpp"

#include "voxel/micro_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace vf::voxel {

namespace {

// The distance from a point to a SEGMENT (not to its line): a leg is a
// segment, and its top is where the joint is.
float distToSegment(glm::vec3 p, glm::vec3 a, glm::vec3 b)
{
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    float t = len2 > 1e-12f ? glm::dot(p - a, ab) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    return glm::length(p - (a + ab * t));
}

// The cache's magic and version. The version is bumped if the layout
// changes; a reader that does not know it refuses the file (and the rig is
// derived again) rather than misreading it.
constexpr char kCacheMagic[8] = { 'V', 'F', 'R', 'I', 'G', 'C', 0, 1 };

} // namespace

int MicroRig::boneAt(glm::vec3 p) const
{
    int best = -1;
    float bestD = 1e30f;
    for (size_t i = 0; i < bones.size(); ++i) {
        const float d = distToSegment(p, bones[i].a, bones[i].b);
        if (d < bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

bool deriveMicroRig(const MicroField& field, MicroRig& out)
{
    out = MicroRig();
    if (field.size3() == 0)
        return false;

    // --- the solid cells -------------------------------------------------
    glm::vec3 lo(1e30f), hi(-1e30f);
    std::vector<glm::vec3> pts;
    pts.reserve(4096);
    for (int z = 0; z < field.dim.z; ++z)
        for (int y = 0; y < field.dim.y; ++y)
            for (int x = 0; x < field.dim.x; ++x) {
                const size_t i = field.idx(x, y, z);
                if (!field.solid[i])
                    continue;
                // The cell's CENTRE, in the object's own frame.
                const glm::vec3 p = field.origin +
                                    (glm::vec3(float(x), float(y), float(z)) +
                                     0.5f) * field.voxel;
                pts.push_back(p);
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
    if (pts.size() < 16)
        return false;

    out.center = 0.5f * (lo + hi);
    const glm::vec3 extent = hi - lo;

    // --- the frame ------------------------------------------------------
    // The objects are authored Y-up (the world is), so the up axis is known.
    //
    // The SPINE is the longer of the two HORIZONTAL axes - and it must not
    // be "the longest axis": an animal is usually TALLER than it is long
    // (the alpaca is 1.3 m tall and 1.1 m long), so the longest axis is the
    // vertical one and the whole derivation finds no legs at all (measured:
    // 3 bones, 0 legs).
    const int upAxis = 1;
    out.up = glm::vec3(0.f, 1.f, 0.f);
    const int axis = (extent.x >= extent.z) ? 0 : 2;
    out.forward = axis == 0 ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
    out.length = extent[axis];

    // --- the core: the middle band of the spine -------------------------
    // The core is the body's mass: the middle half of the spine's extent.
    float tMin = 1e30f, tMax = -1e30f;
    for (const glm::vec3& p : pts) {
        const float t = p[axis];
        tMin = std::min(tMin, t);
        tMax = std::max(tMax, t);
    }
    const float coreA = tMin + 0.22f * (tMax - tMin);
    const float coreB = tMin + 0.78f * (tMax - tMin);

    glm::vec3 coreLo(1e30f), coreHi(-1e30f);
    int coreCount = 0;
    // The heights of the band's cells, so the BELLY can be found by bulk.
    std::vector<float> bandHeights;
    bandHeights.reserve(pts.size());
    for (const glm::vec3& p : pts) {
        if (p[axis] < coreA || p[axis] > coreB)
            continue;
        coreLo = glm::min(coreLo, p);
        coreHi = glm::max(coreHi, p);
        bandHeights.push_back(p[upAxis]);
        ++coreCount;
    }
    if (coreCount < 8)
        return false;

    // The BELLY - where the body starts and the legs begin. It cannot be the
    // band's minimum height: the band runs along the spine, and a quadruped's
    // front legs stand INSIDE that band, so the minimum is the hooves and
    // nothing is "below the core" at all (measured: 3 bones, 0 legs).
    // The belly is a PERCENTILE instead: the legs are a small minority of
    // the band, so the 15th percentile of its heights cuts them off and
    // leaves the body's underside.
    std::sort(bandHeights.begin(), bandHeights.end());
    const float belly = bandHeights[size_t(double(bandHeights.size()) * 0.15)];
    coreLo[upAxis] = belly;

    // --- the legs: the vertical clusters BELOW the core ------------------
    // A column (in the two axes that are not the spine) that has cells below
    // the core and is thin is a limb. The columns are grouped by their
    // position in the plane, so a leg that sways is still one leg.
    // The columns are in the HORIZONTAL plane: a leg hangs DOWN, so its cells
    // share a position in x/z and differ only in height.
    const int a0 = (upAxis + 1) % 3, a1 = (upAxis + 2) % 3;
    const float coreBottom = coreLo[upAxis];
    struct Cluster {
        glm::vec2 centre { 0.f };
        int count = 0;
        glm::vec3 top { 0.f };
        glm::vec3 bottom { 0.f };
    };
    // A coarse grid over the plane: the cell size is a fraction of the
    // object's extent, so the same code finds the legs of a cat and of a
    // horse.
    const float cell = std::max(1e-3f, 0.12f * std::min(extent[a0], extent[a1]));
    std::map<std::pair<int, int>, Cluster> clusters;
    for (const glm::vec3& p : pts) {
        if (p[upAxis] >= coreBottom)
            continue; // not below the core
        const int cx = int(std::floor((p[a0] - lo[a0]) / cell));
        const int cy = int(std::floor((p[a1] - lo[a1]) / cell));
        Cluster& c = clusters[{ cx, cy }];
        c.centre += glm::vec2(p[a0], p[a1]);
        c.count += 1;
        if (c.count == 1) {
            c.top = p;
            c.bottom = p;
        } else {
            c.top = glm::max(c.top, p);
            c.bottom = glm::min(c.bottom, p);
        }
    }
    // Merge the neighbouring columns: a leg is usually two or three cells
    // wide, and the grid above splits it.
    struct Leg {
        glm::vec2 centre { 0.f };
        int count = 0;
        glm::vec3 top { 0.f };
        glm::vec3 bottom { 0.f };
    };
    std::vector<Leg> legs;
    for (const auto& kv : clusters) {
        const Cluster& c = kv.second;
        if (c.count < 4)
            continue; // a speck, not a leg
        const glm::vec2 centre = c.centre / float(c.count);
        bool merged = false;
        for (Leg& l : legs) {
            if (glm::length(l.centre - centre) < 1.6f * cell) {
                l.centre = (l.centre * float(l.count) + centre * float(c.count)) /
                           float(l.count + c.count);
                l.count += c.count;
                l.top = glm::max(l.top, c.top);
                l.bottom = glm::min(l.bottom, c.bottom);
                merged = true;
                break;
            }
        }
        if (!merged)
            legs.push_back({ centre, c.count, c.top, c.bottom });
    }
    // A leg has to be a leg: it hangs down and it is not a speck.
    legs.erase(std::remove_if(legs.begin(), legs.end(),
                              [&](const Leg& l) {
                                  const float drop = l.top[upAxis] - l.bottom[upAxis];
                                  return drop < 0.10f;
                              }),
               legs.end());

    // --- the head and the tail: the mass beyond the core ----------------
    glm::vec3 headLo(1e30f), headHi(-1e30f), tailLo(1e30f), tailHi(-1e30f);
    int headCount = 0, tailCount = 0;
    for (const glm::vec3& p : pts) {
        if (p[axis] > coreB) {
            headLo = glm::min(headLo, p);
            headHi = glm::max(headHi, p);
            ++headCount;
        } else if (p[axis] < coreA) {
            tailLo = glm::min(tailLo, p);
            tailHi = glm::max(tailHi, p);
            ++tailCount;
        }
    }

    // --- assemble the bones ---------------------------------------------
    // The spine: the core's own segment, along the spine's axis.
    {
        MicroBone b;
        b.role = BoneRole::Spine;
        glm::vec3 a = out.center, c = out.center;
        a[axis] = coreLo[axis];
        c[axis] = coreHi[axis];
        b.a = a;
        b.b = c;
        out.bones.push_back(b);
    }
    // The legs, the head and the tail hang off the SPINE, so a bow of the
    // body carries them with it (the chain is what keeps a limb attached
    // to the shoulder it belongs to).
    const int spineBone = 0;
    // The legs: their segment runs from the core's bottom (the joint) down
    // to the foot, so the seam with the body is INSIDE the core and never
    // shows on the silhouette.
    out.legCount = int(legs.size());
    for (size_t i = 0; i < legs.size(); ++i) {
        MicroBone b;
        b.role = BoneRole::Leg;
        b.legIndex = int(i);
        b.parent = spineBone;
        b.a = legs[i].top;
        b.a[upAxis] = coreBottom;
        b.b = legs[i].bottom;
        out.bones.push_back(b);
    }
    // The head: the front extension, from the core's front to the tip.
    if (headCount >= 4) {
        MicroBone b;
        b.role = BoneRole::Head;
        b.parent = spineBone;
        b.a = out.center;
        b.a[axis] = coreHi[axis];
        b.b = 0.5f * (headLo + headHi);
        b.b[axis] = headHi[axis];
        out.bones.push_back(b);
    }
    // The tail: the back extension.
    if (tailCount >= 4) {
        MicroBone b;
        b.role = BoneRole::Tail;
        b.parent = spineBone;
        b.a = out.center;
        b.a[axis] = coreLo[axis];
        b.b = 0.5f * (tailLo + tailHi);
        b.b[axis] = tailLo[axis];
        out.bones.push_back(b);
    }
    return out.valid();
}

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------

bool readMicroRigCache(const std::string& path, MicroRig& out, uint64_t layerSize,
                        uint64_t layerMtime)
{
    out = MicroRig();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[8] = {};
    uint64_t size = 0, mtime = 0;
    uint32_t bones = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 &&
              std::memcmp(magic, kCacheMagic, 8) == 0;
    if (ok)
        ok = std::fread(&size, 8, 1, f) == 1;
    if (ok)
        ok = std::fread(&mtime, 8, 1, f) == 1;
    if (ok)
        ok = std::fread(&bones, 4, 1, f) == 1;
    // A stale cache (the layer changed) is not an error - it just means the
    // rig has to be derived again.
    if (ok && (size != layerSize || mtime != layerMtime || bones == 0 || bones > 64))
        ok = false;
    if (ok) {
        out.bones.resize(bones);
        std::vector<float> raw(size_t(bones) * 8);
        ok = std::fread(raw.data(), 4, raw.size(), f) == raw.size();
        if (ok) {
            for (uint32_t i = 0; i < bones; ++i) {
                out.bones[i].a = glm::vec3(raw[i * 8 + 0], raw[i * 8 + 1], raw[i * 8 + 2]);
                out.bones[i].b = glm::vec3(raw[i * 8 + 3], raw[i * 8 + 4], raw[i * 8 + 5]);
                out.bones[i].role = BoneRole(raw[i * 8 + 6]);
                out.bones[i].legIndex = int(raw[i * 8 + 7]);
            }
        }
    }
    if (ok) {
        std::vector<float> frame(9);
        ok = std::fread(frame.data(), 4, frame.size(), f) == frame.size();
        if (ok) {
            out.center = glm::vec3(frame[0], frame[1], frame[2]);
            out.up = glm::vec3(frame[3], frame[4], frame[5]);
            out.forward = glm::vec3(frame[6], frame[7], frame[8]);
            out.length = 0.f;
            out.legCount = 0;
            for (const auto& b : out.bones) {
                if (b.role == BoneRole::Leg)
                    ++out.legCount;
                out.length = std::max(out.length, glm::length(b.b - b.a));
            }
        }
    }
    std::fclose(f);
    if (!ok)
        out = MicroRig();
    return ok;
}

bool writeMicroRigCache(const std::string& path, const MicroRig& rig, uint64_t layerSize,
                        uint64_t layerMtime)
{
    if (!rig.valid())
        return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(kCacheMagic, 1, 8, f) == 8;
    if (ok)
        ok = std::fwrite(&layerSize, 8, 1, f) == 1;
    if (ok)
        ok = std::fwrite(&layerMtime, 8, 1, f) == 1;
    const uint32_t bones = uint32_t(rig.bones.size());
    if (ok)
        ok = std::fwrite(&bones, 4, 1, f) == 1;
    std::vector<float> raw(size_t(bones) * 8, 0.f);
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        raw[i * 8 + 0] = rig.bones[i].a.x;
        raw[i * 8 + 1] = rig.bones[i].a.y;
        raw[i * 8 + 2] = rig.bones[i].a.z;
        raw[i * 8 + 3] = rig.bones[i].b.x;
        raw[i * 8 + 4] = rig.bones[i].b.y;
        raw[i * 8 + 5] = rig.bones[i].b.z;
        raw[i * 8 + 6] = float(int(rig.bones[i].role));
        raw[i * 8 + 7] = float(rig.bones[i].legIndex);
    }
    if (ok)
        ok = std::fwrite(raw.data(), 4, raw.size(), f) == raw.size();
    const float frame[9] = { rig.center.x, rig.center.y, rig.center.z,
                             rig.up.x,     rig.up.y,     rig.up.z,
                             rig.forward.x, rig.forward.y, rig.forward.z };
    if (ok)
        ok = std::fwrite(frame, 4, 9, f) == 9;
    std::fclose(f);
    return ok;
}

} // namespace vf::voxel
