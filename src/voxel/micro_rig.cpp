// micro_rig.cpp - deriving a skeleton from an object's own voxels, and
// caching the result.
#include "voxel/micro_rig.hpp"

#include "voxel/micro_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

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
// Version 2 carries each bone's PARENT. A version 1 cache is rejected and
// the rig derived again: parents are part of the rig, and a rig read
// back without them is a skeleton whose bones move independently - which
// tears the neck away from the shoulders when the spine bows.
constexpr char kCacheMagic[8] = { 'V', 'F', 'R', 'I', 'G', 'C', 0, 5 };

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

    // ...and it REACHES THE GROUND. A drop test alone also accepts a fleece
    // lock: the alpaca's coarse coat hangs below the belly, so two tufts were
    // read as extra limbs and the rig came back with 9 bones / 6 legs instead
    // of 7 / 4 - the phantom pair then swung about the belly on every step.
    // The object's own bottom is the ground reference (the hooves sit on it),
    // so the test stays generic: a model whose limbs do not touch its base
    // simply has no legs to animate.
    float groundY = 1e30f;
    for (const glm::vec3& p : pts)
        groundY = std::min(groundY, p[upAxis]);
    const float legReach = groundY + std::max(2.0f * cell, 0.06f * extent[upAxis]);
    legs.erase(std::remove_if(legs.begin(), legs.end(),
                              [&](const Leg& l) {
                                  return l.bottom[upAxis] > legReach;
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
    // The head: the front extension, from the core's front to the tip. It is
    // a CHAIN of links, NOT one bone. A single bone turns the whole neck about
    // its base: the bend is one hard crease at the shoulder, the neck stays a
    // rigid pole above it, and the outside of the bend opens a wedge - the
    // head sweeps an arc while the neck reads as a hinged prop. Split into
    // three, each joint turns a third of the total: the neck CURVES, every
    // crease is a third as deep, and the tip still turns by the full angle
    // (the links compose). The count is fixed and the geometry comes from the
    // object's own voxels, so this stays generic.
    if (headCount >= 4) {
        const int kNeckLinks = 3;
        glm::vec3 a = out.center;
        a[axis] = coreHi[axis];
        glm::vec3 b = 0.5f * (headLo + headHi);
        b[axis] = headHi[axis];
        int parent = spineBone;
        for (int link = 0; link < kNeckLinks; ++link) {
            MicroBone nb;
            const float t0 = float(link) / float(kNeckLinks);
            const float t1 = float(link + 1) / float(kNeckLinks);
            // The LAST link is the head itself; the ones below are the neck.
            nb.role = (link + 1 == kNeckLinks) ? BoneRole::Head : BoneRole::Neck;
            nb.parent = parent;
            nb.a = glm::mix(a, b, t0);
            nb.b = glm::mix(a, b, t1);
            out.bones.push_back(nb);
            parent = int(out.bones.size()) - 1;
        }
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

    // --- assign stable ids + names ------------------------------------
    // Ids are index+1 so they survive a re-derive of the same layer; the
    // override JSON preserves them across insert/delete/reparent. Names are
    // human-facing only (the policy reads ids).
    for (size_t i = 0; i < out.bones.size(); ++i) {
        out.bones[i].id = (uint32_t)(i + 1);
        const auto& b = out.bones[i];
        switch (b.role) {
            case BoneRole::Spine: out.bones[i].name = "spine"; break;
            case BoneRole::Head:  out.bones[i].name = "head";  break;
            case BoneRole::Tail:  out.bones[i].name = "tail";  break;
            case BoneRole::Neck:  out.bones[i].name = "neck" + std::to_string(b.parent); break;
            case BoneRole::Leg:   out.bones[i].name = "leg" + std::to_string(b.legIndex); break;
        }
    }

    // --- per-bone capsule radius ---------------------------------------
    // The radius is the median distance of the SURFACE cells this bone owns
    // (nearest bone by segment distance) to the bone's segment. A per-bone
    // (not uniform) radius is what makes the shader's capsule-subtraction
    // ownership test reduce walk seam drift; a uniform constant made flips
    // worse. Computed here so the cache carries it.
    if (!out.bones.empty()) {
        std::vector<std::vector<float>> dists(out.bones.size());
        for (const glm::vec3& p : pts) {
            // surface cell: a solid cell with at least one non-solid neighbour
            // (approximated: every solid cell; the median is robust to interior
            // cells, which sit close to their bone anyway).
            int best = -1;
            float bestD = 1e30f;
            for (size_t i = 0; i < out.bones.size(); ++i) {
                const float d = distToSegment(p, out.bones[i].a, out.bones[i].b);
                if (d < bestD) {
                    bestD = d;
                    best = int(i);
                }
            }
            if (best >= 0)
                dists[size_t(best)].push_back(bestD);
        }
        for (size_t i = 0; i < out.bones.size(); ++i) {
            if (dists[i].empty()) {
                out.bones[i].radius = 0.f;
                continue;
            }
            std::sort(dists[i].begin(), dists[i].end());
            out.bones[i].radius = dists[i][dists[i].size() / 2];
            // Clamp to a sane range so a bone with a stray far cell does not
            // claim the whole object.
            out.bones[i].radius = std::clamp(out.bones[i].radius, 0.f, 0.15f);
        }
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
        std::vector<float> raw(size_t(bones) * 10);
        ok = std::fread(raw.data(), 4, raw.size(), f) == raw.size();
        if (ok) {
            for (uint32_t i = 0; i < bones; ++i) {
                out.bones[i].a = glm::vec3(raw[i * 10 + 0], raw[i * 10 + 1], raw[i * 10 + 2]);
                out.bones[i].b = glm::vec3(raw[i * 10 + 3], raw[i * 10 + 4], raw[i * 10 + 5]);
                out.bones[i].role = BoneRole(raw[i * 10 + 6]);
                out.bones[i].legIndex = int(raw[i * 10 + 7]);
                const int parent = int(raw[i * 10 + 8]);
                // A parent outside the bone list is a corrupt cache, not a
                // root: the chain walk would read a bone that is not there.
                if (parent < -1 || parent >= int(bones))
                    ok = false;
                out.bones[i].parent = parent;
                out.bones[i].radius = raw[i * 10 + 9];
                out.bones[i].id = i + 1;
                switch (out.bones[i].role) {
                    case BoneRole::Spine: out.bones[i].name = "spine"; break;
                    case BoneRole::Head:  out.bones[i].name = "head";  break;
                    case BoneRole::Tail:  out.bones[i].name = "tail";  break;
                    case BoneRole::Neck:  out.bones[i].name = "neck";  break;
                    case BoneRole::Leg:   out.bones[i].name = "leg" + std::to_string(out.bones[i].legIndex); break;
                }
                // The ORDER is part of the format: a bone's parent must come
                // BEFORE it, because the chain is composed in one forward
                // pass - animateBones reads the parent's already-composed
                // transform. A cache that breaks that hands back a rig which
                // tears at the seam with every number looking plausible.
                if (parent >= int(i))
                    ok = false;
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
    std::vector<float> raw(size_t(bones) * 10, 0.f);
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        raw[i * 10 + 0] = rig.bones[i].a.x;
        raw[i * 10 + 1] = rig.bones[i].a.y;
        raw[i * 10 + 2] = rig.bones[i].a.z;
        raw[i * 10 + 3] = rig.bones[i].b.x;
        raw[i * 10 + 4] = rig.bones[i].b.y;
        raw[i * 10 + 5] = rig.bones[i].b.z;
        raw[i * 10 + 6] = float(int(rig.bones[i].role));
        raw[i * 10 + 7] = float(rig.bones[i].legIndex);
        // The PARENT is part of the rig: a cache that drops it hands back a
        // parentless skeleton on the next run, and the neck tears again.
        raw[i * 10 + 8] = float(rig.bones[i].parent);
        // Per-bone capsule radius: median distance of owned surface cells.
        raw[i * 10 + 9] = rig.bones[i].radius;
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

// ---- rig override (JSON) ---------------------------------------------------
namespace {

struct RigJson {
    const std::string& s;
    size_t i = 0;
    void skipWs() { while (i < s.size() && std::isspace(unsigned(s[i]))) ++i; }
    bool eat(char c) { skipWs(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool peek(char c) { skipWs(); return i < s.size() && s[i] == c; }
    bool str(std::string& out) {
        skipWs();
        if (i >= s.size() || s[i] != '"') return false;
        ++i; out.clear();
        while (i < s.size() && s[i] != '"') { if (s[i] == '\\' && i+1 < s.size()) ++i; out.push_back(s[i++]); }
        return i < s.size() && s[i++] == '"';
    }
    bool num(float& v) {
        skipWs();
        char* end = nullptr; const char* begin = s.c_str() + i;
        v = std::strtof(begin, &end);
        if (end == begin) return false;
        i += size_t(end - begin);
        return true;
    }
    bool numU64(uint64_t& v) {
        skipWs();
        char* end = nullptr; const char* begin = s.c_str() + i;
        unsigned long long x = std::strtoull(begin, &end, 10);
        if (end == begin) return false;
        i += size_t(end - begin);
        v = x;
        return true;
    }
    bool skipValue() {
        skipWs();
        if (i >= s.size()) return false;
        if (s[i] == '"') { std::string t; return str(t); }
        if (s[i] == '[' || s[i] == '{') {
            char open = s[i], close = s[i] == '[' ? ']' : '}';
            int d = 0;
            while (i < s.size()) {
                char c = s[i++];
                if (c == open) ++d; else if (c == close) { if (--d == 0) return true; }
                else if (c == '"') { while (i < s.size() && s[i] != '"') { if (s[i]=='\\' && i+1<s.size()) ++i; ++i; } if (i < s.size()) ++i; }
            }
            return false;
        }
        char* end = nullptr; const char* begin = s.c_str() + i;
        std::strtof(begin, &end);
        if (end != begin) { i += size_t(end - begin); return true; }
        return false;
    }
    bool numVec3(glm::vec3& v) {
        if (!eat('[')) return false;
        for (int c = 0; c < 3; ++c) { if (!num(v[c])) return false; if (c < 2) eat(','); }
        return eat(']');
    }
};

BoneRole parseRole(const std::string& s) {
    if (s == "spine") return BoneRole::Spine;
    if (s == "leg") return BoneRole::Leg;
    if (s == "head") return BoneRole::Head;
    if (s == "tail") return BoneRole::Tail;
    if (s == "neck") return BoneRole::Neck;
    return BoneRole::Spine;
}

const char* roleName(BoneRole r) {
    switch (r) {
        case BoneRole::Spine: return "spine";
        case BoneRole::Leg:   return "leg";
        case BoneRole::Head:  return "head";
        case BoneRole::Tail:  return "tail";
        case BoneRole::Neck:  return "neck";
    }
    return "spine";
}

} // namespace

bool readMicroRigOverride(const std::string& path, MicroRig& out,
                          uint64_t layerSize, uint64_t layerMtime)
{
    out = MicroRig();
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string text = buf.str();

    RigJson j { text };
    if (!j.eat('{')) return false;

    uint64_t size = 0, mtime = 0;
    bool hasSize = false, hasMtime = false, hasFrame = false;
    std::string k;
    while (j.str(k)) {
        if (k == "layerSize") { if (!j.numU64(size)) return false; hasSize = true; }
        else if (k == "layerMtime") { if (!j.numU64(mtime)) return false; hasMtime = true; }
        else if (k == "legCount") { float v = 0; if (!j.num(v)) return false; out.legCount = int(v); }
        else if (k == "frame" && j.eat('[')) {
            // [center, up, forward]: the gait's frame (and the MCP's report)
            // reads these, so an override that dropped them silently changed
            // which axis the legs swing about.
            glm::vec3 c, u, f;
            if (!j.numVec3(c)) return false;
            j.eat(',');
            if (!j.numVec3(u)) return false;
            j.eat(',');
            if (!j.numVec3(f)) return false;
            j.eat(']');
            out.center = c;
            out.up = u;
            out.forward = f;
            hasFrame = true;
        }
        else if (k == "bones" && j.eat('[')) {
            while (j.eat('{') || j.peek('{')) {
                if (!j.peek('{')) break;
                j.eat('{');
                MicroBone b;
                std::string bk;
                while (j.str(bk)) {
                    if (bk == "id") { float v = 0; if (!j.num(v)) return false; b.id = (uint32_t)v; }
                    else if (bk == "name") { if (!j.str(b.name)) return false; }
                    else if (bk == "role") { std::string v; if (!j.str(v)) return false; b.role = parseRole(v); }
                    else if (bk == "parent") { float v = 0; if (!j.num(v)) return false; b.parent = (int)v; }
                    else if (bk == "leg") { float v = 0; if (!j.num(v)) return false; b.legIndex = (int)v; }
                    else if (bk == "radius") { if (!j.num(b.radius)) return false; }
                    else if (bk == "a") { if (!j.numVec3(b.a)) return false; }
                    else if (bk == "b") { if (!j.numVec3(b.b)) return false; }
                    else { if (!j.skipValue()) return false; }
                    if (!j.eat(',')) break;
                }
                j.eat('}');
                out.bones.push_back(b);
                if (!j.eat(',')) break;
            }
            j.eat(']');
        } else {
            j.skipValue();
        }
        if (!j.eat(',')) break;
    }
    j.eat('}');

    // signature check
    if (hasSize && size != layerSize) return false;
    if (hasMtime && mtime != layerMtime) return false;
    if (out.bones.empty()) return false;

    // A v1 override carried no frame. Keep the struct's defaults (the
    // sensible up/forward axes) rather than inventing a centre; the caller
    // logs the mismatch so it is not mistaken for a derived frame.
    (void)hasFrame;

    // assign ids if missing
    for (size_t i = 0; i < out.bones.size(); ++i) {
        if (out.bones[i].id == 0) out.bones[i].id = (uint32_t)(i + 1);
        if (out.bones[i].name.empty()) {
            switch (out.bones[i].role) {
                case BoneRole::Spine: out.bones[i].name = "spine"; break;
                case BoneRole::Head:  out.bones[i].name = "head";  break;
                case BoneRole::Tail:  out.bones[i].name = "tail";  break;
                case BoneRole::Neck:  out.bones[i].name = "neck";  break;
                case BoneRole::Leg:   out.bones[i].name = "leg" + std::to_string(out.bones[i].legIndex); break;
            }
        }
    }
    return true;
}

bool writeMicroRigOverride(const std::string& path, const MicroRig& rig,
                           uint64_t layerSize, uint64_t layerMtime)
{
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "{\n  \"format\": \"vf-rig/1\",\n");
    std::fprintf(f, "  \"layerSize\": %llu,\n", (unsigned long long)layerSize);
    std::fprintf(f, "  \"layerMtime\": %llu,\n", (unsigned long long)layerMtime);
    // The gait reads the frame (up/forward decide which way the legs swing
    // and where the head reaches); dropping it made an overridden rig animate
    // about the wrong axes.
    std::fprintf(f, "  \"legCount\": %d,\n", rig.legCount);
    std::fprintf(f, "  \"frame\": [[%.4f, %.4f, %.4f], [%.4f, %.4f, %.4f], "
                 "[%.4f, %.4f, %.4f]],\n",
                 rig.center.x, rig.center.y, rig.center.z,
                 rig.up.x, rig.up.y, rig.up.z,
                 rig.forward.x, rig.forward.y, rig.forward.z);
    std::fprintf(f, "  \"bones\": [\n");
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        const MicroBone& b = rig.bones[i];
        std::fprintf(f, "    { \"id\": %u, \"name\": \"%s\", \"role\": \"%s\", "
                     "\"parent\": %d, \"leg\": %d, \"radius\": %.4f, "
                     "\"a\": [%.4f, %.4f, %.4f], \"b\": [%.4f, %.4f, %.4f] }%s\n",
                     b.id, b.name.c_str(), roleName(b.role), b.parent, b.legIndex,
                     b.radius, b.a.x, b.a.y, b.a.z, b.b.x, b.b.y, b.b.z,
                     i + 1 < rig.bones.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> validateRig(const MicroRig& rig)
{
    std::vector<std::string> issues;
    if (rig.bones.empty()) { issues.push_back("rig has no bones"); return issues; }
    if (rig.bones.size() > 12) issues.push_back("bone count " + std::to_string(rig.bones.size()) + " exceeds shader cap of 12");
    for (size_t i = 0; i < rig.bones.size(); ++i) {
        const auto& b = rig.bones[i];
        if (b.parent >= (int)i && b.parent != -1) issues.push_back("bone " + std::to_string(i) + " has parent >= index (must come before)");
        if (b.parent < -1 || b.parent >= (int)rig.bones.size()) issues.push_back("bone " + std::to_string(i) + " has invalid parent");
        const float len = glm::length(b.b - b.a);
        if (len < 1e-4f) issues.push_back("bone " + std::to_string(i) + " has zero-length segment");
        // role validity is implicit (enum)
    }
    // check for duplicate ids
    for (size_t i = 0; i < rig.bones.size(); ++i)
        for (size_t j = i + 1; j < rig.bones.size(); ++j)
            if (rig.bones[i].id != 0 && rig.bones[i].id == rig.bones[j].id)
                issues.push_back("duplicate bone id " + std::to_string(rig.bones[i].id));
    return issues;
}

void composeBoneChain(const MicroRig& rig,
                      const std::vector<glm::mat3>& localRot,
                      std::vector<glm::mat3>& rot,
                      std::vector<glm::vec3>& xform)
{
    const size_t n = rig.bones.size();
    rot.assign(n, glm::mat3(1.f));
    xform.assign(n, glm::vec3(0.f));
    for (size_t i = 0; i < n; ++i) {
        const MicroBone& b = rig.bones[i];
        const glm::mat3 R = (i < localRot.size()) ? localRot[i] : glm::mat3(1.f);
        // The local transform turns the bone about its OWN joint, so the
        // point `a` is a fixed point: R*a + t = a.
        const glm::vec3 t = b.a - R * b.a;
        const int parent = b.parent;
        if (parent >= 0 && size_t(parent) < n) {
            const glm::mat3 Rp = rot[size_t(parent)];
            const glm::vec3 tp = xform[size_t(parent)];
            rot[i] = Rp * R;
            xform[i] = Rp * t + tp;
        } else {
            rot[i] = R;
            xform[i] = t;
        }
    }
}

} // namespace vf::voxel
