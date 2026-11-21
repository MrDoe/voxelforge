// Shared selector grammar for vf_mcp's cell verbs (delete_voxels, paint,
// describe, inspect). One parser + one matcher so every verb means the same
// thing by construction, and so a selection's bounds/count are reported the
// same way in dry_run and in the write itself.
//
// Header-only: vf_mcp, vf_tests and the app can all use it without a new link
// dependency. NO scenery facts live here — only geometry and record fields.
#pragma once

#include "worldfile.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vf::voxel::selector {

// Selector kinds. Several specs may be unioned by passing a vector.
struct Spec {
    enum class Kind { None, Box, Sphere, Component, Mat };

    Kind kind = Kind::None;
    std::array<int, 6> box = {};   // inclusive lattice cells: x0,y0,z0,x1,y1,z1
    std::array<int, 3> anchor = {};// lattice cell
    float radiusM = 0.f;        // metres, from the anchor CELL CENTRE
    int connectivity = 6;       // 6 or 26, component mode only
    int mat = -1;               // material filter, -1 = any

    bool valid() const { return kind != Kind::None; }

    std::string describe() const
    {
        std::string s;
        switch (kind) {
        case Kind::Box:
            s = "box[" + std::to_string(box[0]) + "," + std::to_string(box[1]) +
                "," + std::to_string(box[2]) + ".." + std::to_string(box[3]) +
                "," + std::to_string(box[4]) + "," + std::to_string(box[5]) + "]";
            break;
        case Kind::Sphere:
            s = "sphere(anchor=[" + std::to_string(anchor[0]) + "," +
                std::to_string(anchor[1]) + "," + std::to_string(anchor[2]) +
                "], r=" + std::to_string(radiusM) + "m)";
            break;
        case Kind::Component:
            s = "component(anchor=[" + std::to_string(anchor[0]) + "," +
                std::to_string(anchor[1]) + "," + std::to_string(anchor[2]) +
                "], conn=" + std::to_string(connectivity) + ")";
            break;
        case Kind::Mat:
            s = "mat(" + std::to_string(mat) + ")";
            break;
        default:
            s = "none";
            break;
        }
        if (mat >= 0)
            s += " & mat=" + std::to_string(mat);
        return s;
    }
};

inline uint32_t keyOf(int x, int y, int z)
{
    return (uint32_t(x) << 20) | (uint32_t(y) << 10) | uint32_t(z);
}
inline void unpackKey(uint32_t k, int& x, int& y, int& z)
{
    x = int(k >> 20);
    y = int((k >> 10) & 1023);
    z = int(k & 1023);
}

// The one world<->cell convention used by every editor path: cell centres are
// at -WORLD/2 + (index + 0.5)*VOXEL. Do NOT add a chunk origin here — lattice
// coords are absolute (see the overlay-forensics quirk).
inline std::array<float, 3> cellCentreWorld(const int c[3])
{
    const float half = 0.5f * WORLD;
    return { -half + (float(c[0]) + 0.5f) * VOXEL,
             -half + (float(c[1]) + 0.5f) * VOXEL,
             -half + (float(c[2]) + 0.5f) * VOXEL };
}

struct Selection {
    std::unordered_set<uint32_t> keys;
    int lo[3] = { 1 << 30, 1 << 30, 1 << 30 };
    int hi[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };

    size_t size() const { return keys.size(); }
    bool empty() const { return keys.empty(); }
    bool contains(int x, int y, int z) const
    {
        return keys.count(keyOf(x, y, z)) != 0;
    }
    void add(int x, int y, int z)
    {
        keys.insert(keyOf(x, y, z));
        lo[0] = std::min(lo[0], x); lo[1] = std::min(lo[1], y); lo[2] = std::min(lo[2], z);
        hi[0] = std::max(hi[0], x); hi[1] = std::max(hi[1], y); hi[2] = std::max(hi[2], z);
    }
    std::string summary() const
    {
        if (empty())
            return "empty selection";
        return "cells=" + std::to_string(keys.size()) + " bounds[" +
               std::to_string(lo[0]) + "," + std::to_string(lo[1]) + "," +
               std::to_string(lo[2]) + ".." + std::to_string(hi[0]) + "," +
               std::to_string(hi[1]) + "," + std::to_string(hi[2]) + "]";
    }
};

inline bool matMatches(const VoxelRecord& v, int filter)
{
    return filter < 0 || int(v.materialId) == filter;
}

// Resolve one spec against a layer's records. A component flood fill traverses
// ONLY cells matching the material filter — the old unmasked fill connected a
// tree to the ground through the terrain and deleted 13k cells by accident.
inline bool selectOne(const std::vector<VoxelRecord>& recs, const Spec& spec,
                      Selection& out, std::string& err)
{
    if (!spec.valid())
        return true;
    if (spec.connectivity != 6 && spec.connectivity != 26) {
        err = "connectivity must be 6 or 26";
        return false;
    }
    if (spec.kind == Spec::Kind::Box) {
        if (spec.box[0] > spec.box[3] || spec.box[1] > spec.box[4] ||
            spec.box[2] > spec.box[5]) {
            err = "selector box has min > max";
            return false;
        }
        for (const VoxelRecord& v : recs) {
            int x = v.x, y = v.y, z = v.z;
            if (!matMatches(v, spec.mat))
                continue;
            if (x < spec.box[0] || x > spec.box[3] || y < spec.box[1] ||
                y > spec.box[4] || z < spec.box[2] || z > spec.box[5])
                continue;
            out.add(x, y, z);
        }
        return true;
    }
    if (spec.kind == Spec::Kind::Sphere) {
        if (!(spec.radiusM > 0.f)) {
            err = "sphere selector needs radius > 0";
            return false;
        }
        const auto c = cellCentreWorld(spec.anchor.data());
        const float r2 = spec.radiusM * spec.radiusM;
        for (const VoxelRecord& v : recs) {
            if (!matMatches(v, spec.mat))
                continue;
            const int vc[3] = { v.x, v.y, v.z };
            const auto p = cellCentreWorld(vc);
            const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
            if (dx * dx + dy * dy + dz * dz <= r2)
                out.add(v.x, v.y, v.z);
        }
        return true;
    }
    if (spec.kind == Spec::Kind::Mat) {
        for (const VoxelRecord& v : recs)
            if (matMatches(v, spec.mat))
                out.add(v.x, v.y, v.z);
        return true;
    }

    // Component: seed must be a solid cell of THIS layer and match the mask.
    std::unordered_map<uint32_t, size_t> index;
    index.reserve(recs.size() * 2);
    for (size_t i = 0; i < recs.size(); ++i)
        index.emplace(keyOf(recs[i].x, recs[i].y, recs[i].z), i);
    const uint32_t seed = keyOf(spec.anchor[0], spec.anchor[1], spec.anchor[2]);
    auto it = index.find(seed);
    if (it == index.end() || !matMatches(recs[it->second], spec.mat)) {
        err = "no record of this layer at anchor [" +
              std::to_string(spec.anchor[0]) + "," + std::to_string(spec.anchor[1]) +
              "," + std::to_string(spec.anchor[2]) +
              "] matching the material filter";
        return false;
    }
    std::deque<uint32_t> q;
    q.push_back(seed);
    out.add(spec.anchor[0], spec.anchor[1], spec.anchor[2]);
    while (!q.empty()) {
        uint32_t k = q.front();
        q.pop_front();
        int x, y, z;
        unpackKey(k, x, y, z);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0)
                        continue;
                    if (spec.connectivity == 6 &&
                        std::abs(dx) + std::abs(dy) + std::abs(dz) != 1)
                        continue;
                    const int nx = x + dx, ny = y + dy, nz = z + dz;
                    if (nx < 0 || ny < 0 || nz < 0 || nx > 1023 || ny > 1023 || nz > 1023)
                        continue;
                    const uint32_t nk = keyOf(nx, ny, nz);
                    if (out.keys.count(nk))
                        continue;
                    auto jt = index.find(nk);
                    if (jt == index.end() || !matMatches(recs[jt->second], spec.mat))
                        continue;
                    out.add(nx, ny, nz);
                    q.push_back(nk);
                }
    }
    return true;
}

// Union of all specs. Material filters on individual specs are honoured; a
// bare `mat` arg is applied to every spec by the caller.
inline bool selectCells(const std::vector<VoxelRecord>& recs,
                        const std::vector<Spec>& specs, Selection& out,
                        std::string& err)
{
    if (specs.empty()) {
        err = "no selector given";
        return false;
    }
    for (const Spec& s : specs)
        if (!selectOne(recs, s, out, err))
            return false;
    if (out.empty()) {
        err = "no cells matched the selection";
        return false;
    }
    return true;
}

// Rebuild a layer with the selected cells removed, preserving record order.
inline std::vector<VoxelRecord> without(const std::vector<VoxelRecord>& recs,
                                        const Selection& sel, size_t& removed)
{
    std::vector<VoxelRecord> keep;
    keep.reserve(recs.size());
    removed = 0;
    for (const VoxelRecord& v : recs) {
        if (sel.contains(v.x, v.y, v.z)) {
            ++removed;
            continue;
        }
        keep.push_back(v);
    }
    return keep;
}

} // namespace vf::voxel::selector
