// vf_slice: ASCII cross-sections of the records-derived VoxelField.
//
// The fast feedback loop for layer-by-layer voxel object authoring - no GPU,
// no window (samples the baked field; assets/world.json + layers must exist,
// see heightmap_gen). Includes ai_edits, so AI-placed objects show up.
//
// Usage:
//   vf_slice --axis x|y|z --center X Y Z --span S [--res N] [--band B]
//
// axis is the plane normal through --center. Columns ascend along u, rows
// descend along v (screen-like). Solid cells print their material glyph,
// empty cells within --band of the surface print '+', deep air prints ' '.
#include "voxel/layered_world.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vf::voxel;

int main(int argc, char** argv)
{
    char axis = 'y';
    float center[3] = { 0.f, 0.f, 0.f };
    float span = 12.f;
    int res = 61;
    float band = 0.15f;

    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need = [&](int k) -> bool {
            if (i + k >= argc) {
                std::fprintf(stderr, "missing value after %s\n", argv[i]);
                std::exit(2);
            }
            return true;
        };
        if (s == "--axis" && need(1)) {
            axis = argv[++i][0];
        } else if (s == "--center" && need(3)) {
            center[0] = float(std::atof(argv[++i]));
            center[1] = float(std::atof(argv[++i]));
            center[2] = float(std::atof(argv[++i]));
        } else if (s == "--span" && need(1)) {
            span = float(std::atof(argv[++i]));
        } else if (s == "--res" && need(1)) {
            res = std::atoi(argv[++i]);
        } else if (s == "--band" && need(1)) {
            band = float(std::atof(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: vf_slice --axis x|y|z --center X Y Z --span S"
                         " [--res N] [--band B]\n");
            return 2;
        }
    }
    if ((axis != 'x' && axis != 'y' && axis != 'z') || span <= 0.f ||
        res < 8 || res > 240) {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    LayeredWorld lw;
    if (!lw.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json")) {
        std::fprintf(stderr,
                     "assets/world.json missing/unreadable - run: ninja -C build world\n");
        return 1;
    }
    const VoxelField& field = lw.field();

    const float step = span / float(res - 1);
    const float lo = -span * 0.5f;
    const char* glyphs = ".,:s#%wRf";
    const char* uName = axis == 'y' ? "x" : (axis == 'x' ? "z" : "x");
    const char* vName = axis == 'y' ? "z" : "y";

    std::printf("plane %c=%.2f  span %.1f m  res %d  (cols: %s asc, rows: %s desc)\n",
                axis, center[axis == 'x' ? 0 : (axis == 'y' ? 1 : 2)], span, res, uName,
                vName);
    std::printf("legend: .=grass ,=grass-l :=soil s=sand #=rock %%=rock-l w=wood "
                "R=roof f=foliage | '+' = < %.0f cm from surface\n",
                band * 100.f);

    for (int row = 0; row < res; ++row) {
        // row 0 prints the top: v descends with increasing row
        float v = lo + float((res - 1) - row) * step;
        std::string line;
        int last = -1;
        for (int col = 0; col < res; ++col) {
            float u = lo + float(col) * step;
            glm::vec3 p(0.f);
            if (axis == 'y')
                p = glm::vec3(center[0] + u, center[1], center[2] + v);
            else if (axis == 'x')
                p = glm::vec3(center[0], center[1] + v, center[2] + u);
            else
                p = glm::vec3(center[0] + u, center[1] + v, center[2]);

            VoxelField::Sample s = field.sampleWorld(p);
            char ch = ' ';
            if (s.d < 0.f)
                ch = s.mat < 9 ? glyphs[s.mat] : '?';
            else if (s.d < band)
                ch = '+';
            line.push_back(ch);
            if (ch != ' ')
                last = col;
        }
        line.resize(size_t(last + 1));
        std::printf("%s\n", line.c_str());
    }
    return 0;
}
