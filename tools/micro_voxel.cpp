// micro_voxel - author a HIGH-DETAIL micro-voxel object on its OWN fine
// lattice and register it as a separate SVO world.
//
// The world lattice is fixed at 0.1 m, so a 30 cm vase cannot express features
// finer than 10 cm there. This tool writes a layer on its own fine lattice
// (default 5 mm) which the app loads into a SEPARATE SVO world and renders as a
// GPU overlay - the two are never merged.
//
//   micro_voxel --vase [--voxel 0.005] [--ground X,Z | --at X,Y,Z]
//                [--out NAME] [--mat N] [--profile vase|urn] [--append]
//   micro_voxel <file.stl|file.obj> --out NAME [--voxel 0.005] ...
//
// It writes assets/micro/<out>.vxw with a meta whose voxelSize is the fine one,
// and (with --append) adds a "role":"micro" entry to assets/world.json so the
// running app picks it up within ~0.5 s. Without --append the file is written
// and the manifest is left alone.
//
// Explicit tool, like vf_trees / vf_mesh2vox: never wired into the default
// build, tests or the world bake target.
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/worldfile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {

inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

struct Options {
    std::string inPath;
    std::string outName = "micro_vase";
    std::string profile = "vase";
    bool procedural = false;
    float voxel = 0.005f;
    uint8_t mat = 20; // plaster white - reads well against grass and stone
    float scale = 1.0f;
    glm::vec3 at { 0.f };
    bool haveAnchor = false;
    bool dryRun = false;
    bool append = false;
};

// A lathe about Y: radius(t) from a profile, sampled at `steps` heights.
// Emits a SOLID volume (not a 1-cell shell), because the app's SDF bake and the
// tracer both read better from a closed volume - a rasterised shell at this
// resolution leaks. The tracer handles either, but solid is what a vase is.
std::vector<VoxelRecord> makeLathe(const Options& o, int& nx, int& ny, int& nz)
{
    // 30 cm tall, ~7.3 cm max diameter at 5 mm -> 60 x ~15 x ~15 cells.
    const float H = 0.30f * o.scale;
    const float R = 0.0365f * o.scale; // max radius
    const int steps = std::max(4, int(std::lround(H / o.voxel)));
    const float vox = o.voxel;

    auto radiusAt = [&](float t) -> float { // t in [0,1] bottom->top
        if (o.profile == "urn") {
            // foot -> swelling belly -> narrow waist -> flared lip
            return R * (0.55f + 0.45f * std::sin(t * 3.14159f)) *
                   (1.0f - 0.35f * t) + R * 0.28f * smoothstepf(0.86f, 1.0f, t);
        }
        // vase: foot, belly, shoulder, flared lip
        float r = R * 0.42f;                                       // foot
        r = mixf(r, R, smoothstepf(0.02f, 0.34f, t));               // belly
        r = mixf(r, R * 0.58f, smoothstepf(0.42f, 0.68f, t));       // waist
        r = mixf(r, R * 0.34f, smoothstepf(0.70f, 0.86f, t));       // shoulder
        r = mixf(r, R * 0.50f, smoothstepf(0.88f, 1.00f, t));       // lip
        return r;
    };

    // Grid the lathe needs. The half-width must cover the MAXIMUM radius in voxels
// (radiusAt can reach R exactly, and ceil() rounds up), plus a cell of margin.
// Sizing it from round(R/vox) instead lets the widest rings write a negative
// index, which wraps in the uint16 record and hands the loader a ~65 k cell
// axis - the load-time budget check rejects that, but it is a silent-looking
// failure here. Round UP and add the margin.
const int rVox = int(std::ceil(R / vox)) + 1;
    nx = nz = 2 * rVox + 1;
    ny = steps;
    const int cx = nx / 2, cz = nz / 2;

    std::vector<VoxelRecord> out;
    for (int iy = 0; iy < steps; ++iy) {
        const float t = (float(iy) + 0.5f) / float(steps);
        const float r = radiusAt(t);
        // Radius in VOXELS to compare against the integer ix/iz offsets. Mixing
        // the two units here (comparing an index against a metre radius) keeps
        // only the axis cell, which is exactly the "60 records for 60 layers"
        // signature the dry run printed before this was fixed.
        const int ir = int(std::ceil(r / vox));
        const float irf = float(ir);
        for (int iz = -ir; iz <= ir; ++iz)
            for (int ix = -ir; ix <= ir; ++ix) {
                if (float(ix) * float(ix) + float(iz) * float(iz) > irf * irf)
                    continue;
                VoxelRecord v;
                v.x = uint16_t(cx + ix);
                v.y = uint16_t(iy);
                v.z = uint16_t(cz + iz);
                v.a = 255;
                // subtle vertical shading so the form reads at close range
                const float shade = 0.82f + 0.18f * t;
                const glm::vec3 c = kPalette[std::min<int>(o.mat, kPaletteN - 1)];
                v.r = uint8_t(glm::clamp(c.r * shade * 255.f, 0.f, 255.f));
                v.g = uint8_t(glm::clamp(c.g * shade * 255.f, 0.f, 255.f));
                v.b = uint8_t(glm::clamp(c.b * shade * 255.f, 0.f, 255.f));
                v.materialId = o.mat;
                const glm::vec2 rr = kMaterialReflection[std::min<int>(o.mat, kPaletteN - 1)];
                v.reflectivity = uint8_t(rr.x);
                v.roughness = uint8_t(rr.y);
                out.push_back(v);
            }
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::string err;
    if (argc < 2) {
        std::printf("usage: micro_voxel --vase [--profile vase|urn] [--voxel 0.005] "
                    "[--mat N] [--scale S]\n"
                    "       micro_voxel [--ground X,Z | --at X,Y,Z] [--out NAME] "
                    "[--append]\n");
        return 2;
    }
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto take = [&](const char* name, std::string& dst) -> bool {
            if (a != name)
                return false;
            if (i + 1 >= argc) {
                err = std::string(name) + " needs a value";
                return true;
            }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (take("--out", v)) {
            o.outName = v;
        } else if (a == "--vase") {
            o.procedural = true;
        } else if (take("--profile", v)) {
            o.profile = v;
        } else if (take("--voxel", v)) {
            o.voxel = float(std::atof(v.c_str()));
            if (!(o.voxel > 0.f)) {
                err = "--voxel must be > 0";
                break;
            }
        } else if (take("--scale", v)) {
            o.scale = float(std::atof(v.c_str()));
        } else if (take("--mat", v)) {
            const int m = std::atoi(v.c_str());
            if (m < 0 || m >= kPaletteN) {
                err = "--mat must be 0.." + std::to_string(kPaletteN - 1);
                break;
            }
            o.mat = uint8_t(m);
        } else if (take("--ground", v)) {
            float gx = 0.f, gz = 0.f;
            if (std::sscanf(v.c_str(), " %f , %f", &gx, &gz) != 2) {
                err = "--ground needs X,Z";
                break;
            }
            o.at.x = gx;
            o.at.z = gz;
            o.at.y = sharedHeightmap().sample(gx, gz);
            o.haveAnchor = true;
        } else if (take("--at", v)) {
            if (std::sscanf(v.c_str(), " %f , %f , %f", &o.at.x, &o.at.y, &o.at.z) != 3) {
                err = "--at needs X,Y,Z";
                break;
            }
            o.haveAnchor = true;
        } else if (a == "--append") {
            o.append = true;
        } else if (a == "--dry-run") {
            o.dryRun = true;
        } else if (!a.empty() && a[0] == '-') {
            err = "unknown option: " + a;
            break;
        } else {
            pos.push_back(a);
        }
    }
    if (!err.empty()) {
        std::printf("micro_voxel: %s\n", err.c_str());
        return 2;
    }
    if (!o.procedural) {
        err = "mesh input needs the mesh voxelizer; use --vase for the procedural "
              "profile";
        std::printf("micro_voxel: %s\n", err.c_str());
        return 2;
    }
    if (!o.haveAnchor) {
        // default beside the hamlet hall, out of the live-edit keep-out box
        o.at.x = 12.0f;
        o.at.z = 12.0f;
        o.at.y = sharedHeightmap().sample(o.at.x, o.at.z);
    }

    int nx = 0, ny = 0, nz = 0;
    std::vector<VoxelRecord> recs = makeLathe(o, nx, ny, nz);
    std::printf("micro_voxel: profile '%s' -> %zu voxels, grid %dx%dx%d, "
                "voxel %.4f m (%.1f cm)\n",
                o.profile.c_str(), recs.size(), nx, ny, nz, o.voxel, o.voxel * 100.f);
    if (recs.empty()) {
        std::printf("micro_voxel: no records\n");
        return 1;
    }

    // meta: the layer's OWN lattice. worldSize is derived from the grid so the
    // layer round-trips through worldfile::read untouched; the app places it at
    // --at/--ground, never at the world lattice origin.
    WorldFileMeta meta;
    meta.voxelSize = o.voxel;
    meta.worldSize = float(nx) * o.voxel; // square extent covers the lathe
    meta.waterLevel = WATER_LEVEL;
    meta.gridN = uint32_t(std::max(nx, std::max(ny, nz)));
    meta.brickN = 8;

    const std::filesystem::path dir(std::string(VOXELFORGE_ASSET_DIR) + "/micro");
    const std::string file = o.outName + ".vxw";

    if (o.dryRun) {
        std::printf("dry run: would write assets/micro/%s (%zu records, %.1f KB) "
                    "placed at (%.2f, %.2f, %.2f)\n",
                    file.c_str(), recs.size(), recs.size() * 16.0 / 1024.0,
                    o.at.x, o.at.y, o.at.z);
        return 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    WorldFileData data;
    data.meta = meta;
    data.voxels = recs;
    const std::string path = (dir / file).string();
    if (!worldfile::write(path, data)) {
        std::printf("micro_voxel: failed to write %s\n", path.c_str());
        return 1;
    }
    std::printf("micro_voxel: wrote %zu voxels to assets/micro/%s\n", recs.size(),
                file.c_str());

    if (o.append) {
        const std::string manifest = std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
        std::vector<vf::voxel::worldfile::WorldLayer> layers;
        if (!worldfile::loadManifest(manifest, layers)) {
            std::printf("micro_voxel: cannot read %s - file written, not appended\n",
                        manifest.c_str());
            return 0;
        }
        bool present = false;
        for (auto& l : layers)
            if (l.file == "micro/" + file) {
                l.role = "micro";
                l.enabled = true;
                present = true;
            }
        if (!present) {
            vf::voxel::worldfile::WorldLayer l;
            l.file = "micro/" + file;
            l.role = "micro";
            l.name = o.outName;
            l.enabled = true;
            // pos = world position of the object's local min corner.
            l.pos[0] = o.at.x;
            l.pos[1] = o.at.y;
            l.pos[2] = o.at.z;
            layers.push_back(l);
        }
        if (worldfile::writeManifest(manifest, layers))
            std::printf("micro_voxel: registered 'micro/%s' in world.json (the "
                        "running app reloads it within ~0.5 s)\n",
                        file.c_str());
        else
            std::printf("micro_voxel: file written but manifest append failed\n");
    } else {
        std::printf("micro_voxel: add it with --append, or list it in world.json "
                    "as {\"file\":\"micro/%s\",\"role\":\"micro\",\"pos\":[x,y,z]}\n",
                    file.c_str());
    }
    return 0;
}