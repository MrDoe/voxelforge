// isle_opendoor — delete records inside world-space boxes from a layer file.
//
// Doors cannot be articulated yet (the user directive: "keep all doors
// open"), so an open door is authored by REMOVING the door-panel cells a
// shell-mode mesh import baked into the doorway plane. Same standalone
// compile pattern as isle_gen (reuses worldfile read/write so the CRC and
// any v2 sections stay valid).
//
// A --remap from,to additionally rewrites materialId, keeping the record's
// own RGB: mesh Kd colours that paletteNearest mapped to an EMISSIVE palette
// entry (mat 10 ember) would glow the whole roof and exhaust the derived
// light budget; remapping to an untextured mat keeps the colour, kills the
// glow and frees the slots for the beacons that should own them.
//
// usage: isle_opendoor <layer.vxw> [--box x0,y0,z0,x1,y1,z1 ...]
//                       [--remap from,to ...] [--stats]
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace vf::voxel;

namespace {
struct Box { float x0, y0, z0, x1, y1, z1; };
bool parse(const char* s, Box& b)
{
    return std::sscanf(s, "%f,%f,%f,%f,%f,%f", &b.x0, &b.y0, &b.z0, &b.x1,
                       &b.y1, &b.z1) == 6;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: isle_opendoor <layer.vxw> [--box x0,y0,z0,x1,y1,z1 "
                     "...] [--remap from,to ...]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::vector<Box> boxes;
    std::vector<std::pair<int, int>> remaps;
    bool stats = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stats") == 0) {
            stats = true;
        } else if (std::strcmp(argv[i], "--box") == 0 && i + 1 < argc) {
            Box b;
            if (!parse(argv[++i], b)) {
                std::fprintf(stderr, "bad box: %s\n", argv[i]);
                return 2;
            }
            boxes.push_back(b);
        } else if (std::strcmp(argv[i], "--remap") == 0 && i + 1 < argc) {
            int from = -1, to = -1;
            if (std::sscanf(argv[++i], "%d,%d", &from, &to) != 2 || from < 0 ||
                to < 0) {
                std::fprintf(stderr, "bad remap: %s\n", argv[i]);
                return 2;
            }
            remaps.emplace_back(from, to);
        } else {
            std::fprintf(stderr, "expected --box/--remap at %d\n", i);
            return 2;
        }
    }
    if (boxes.empty() && remaps.empty() && !stats) {
        std::fprintf(stderr, "nothing to do (no --box/--remap/--stats)\n");
        return 2;
    }
    WorldFileData d;
    if (!worldfile::read(path, d)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    if (stats) {
        std::map<int, size_t> hist;
        for (const auto& r : d.voxels)
            ++hist[int(r.materialId)];
        std::printf("stats %s: %zu records\n", path.c_str(), d.voxels.size());
        for (const auto& [m, n] : hist)
            std::printf("  mat %2d: %zu\n", m, n);
        if (boxes.empty() && remaps.empty())
            return 0; // read-only invocation: do not rewrite the file
    }
    auto wc = [](uint16_t c) { return -0.5f * WORLD + (float(c) + 0.5f) * VOXEL; };
    const size_t before = d.voxels.size();
    std::vector<VoxelRecord> keep;
    keep.reserve(before);
    for (const auto& r : d.voxels) {
        const float x = wc(r.x), y = wc(r.y), z = wc(r.z);
        bool cut = false;
        for (const auto& b : boxes)
            if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 &&
                z >= b.z0 && z <= b.z1) {
                cut = true;
                break;
            }
        if (!cut)
            keep.push_back(r);
    }
    d.voxels = std::move(keep);
    size_t remapped = 0;
    for (const auto& m : remaps) {
        size_t k = 0;
        for (auto& r : d.voxels)
            if (int(r.materialId) == m.first) {
                r.materialId = uint8_t(m.second);
                ++k;
            }
        remapped += k;
        std::printf("  remap %d->%d: %zu records\n", m.first, m.second, k);
    }
    if (!worldfile::write(path, d)) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    std::printf("opendoor %s: removed %zu of %zu records, remapped %zu\n",
                path.c_str(), before - d.voxels.size(), before, remapped);
    return 0;
}
