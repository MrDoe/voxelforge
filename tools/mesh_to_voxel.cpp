// mesh_to_voxel - convert a triangle mesh (STL or OBJ) into a voxelforge
// object layer.
//
// Voxelize a mesh onto the VOXEL = 0.1 m lattice and write it as a named
// .vxw object layer, the same path vf_mcp write_object uses: the running app
// picks the new layer up through its world.json mtime poll within ~0.5 s.
//
// The conversion emits the FULL SOLID VOLUME, not just the surface shell:
// the loader flood-fills an object component to a solid, but that needs a
// watertight shell, and a rasterised 1-voxel shell is not (a voxelised
// cylinder comes back as a hollow tube). Solid fill removes the failure mode
// entirely. --shell restores the thin-shell variant for watertight meshes
// where the file size matters more than the risk.
//
// Usage:
//   mesh_to_voxel <file.stl|file.obj> --out NAME [options]
//
//   --scale S      model units per metre (STL from CAD is usually mm: 0.001)
//   --fit M        instead of --scale, uniform-scale so the longest AABB
//                  side becomes M metres
//   --rot-y DEG    rotate about the vertical (Y) axis
//   --swap-yz      treat the model as Z-up (swap Y and Z before anything else)
//   --flip         reverse the winding (import an inside-out mesh)
//   --ground X,Z   place the object's bottom-center on the terrain here
//   --at X,Y,Z     bottom-center at this world point (metres)
//   --cell X,Y,Z   bottom-center at this lattice cell (0..1023)
//   --mat N        palette id 0..16 for the whole object (default 4 rock)
//   --shell        emit the 1-voxel shell only (see above)
//   --dry-run      report stats, write no file
//
// Example (a 200 mm STL statue beside the hamlet hall):
//   mesh_to_voxel statue.stl --out statue --scale 0.001 --ground 8,12
//
// Explicit tool like vf_trees: never wired into the default build, tests, or
// the world bake target. Run it when you want a converted asset.

#include "voxel/common.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/worldfile.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {

struct Options {
    std::string inPath;
    std::string outName = "mesh_object";
    MeshImportOptions import;
    bool solid = true; // --shell clears it
    bool haveAnchor = false;
    glm::ivec3 anchor = { 0, 0, 0 };
    bool dryRun = false;
};

bool parseArgs(int argc, char** argv, Options& o, std::string& err)
{
    err.clear();
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto take = [&](const char* name, std::string& dst) -> bool {
            if (a != name)
                return false;
            if (i + 1 >= argc) {
                err = std::string(name) + " needs a value";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (take("--out", v))
            o.outName = v;
        else if (take("--scale", v)) {
            o.import.scale = float(std::atof(v.c_str()));
            if (o.import.scale <= 0.f) {
                err = "--scale must be > 0";
                return false;
            }
        } else if (take("--fit", v)) {
            o.import.fitMeters = float(std::atof(v.c_str()));
            o.import.hasFit = true;
            if (o.import.fitMeters <= 0.f || o.import.fitMeters > 90.f) {
                err = "--fit must be in (0, 90] metres";
                return false;
            }
        } else if (take("--rot-y", v))
            o.import.rotY = float(std::atof(v.c_str()));
        else if (take("--ground", v)) {
            float gx, gz;
            if (std::sscanf(v.c_str(), " %f , %f", &gx, &gz) != 2) {
                err = "--ground needs X,Z";
                return false;
            }
            const float H = sharedHeightmap().sample(gx, gz);
            o.anchor = { int(std::floor((gx + 0.5f * WORLD) / VOXEL)),
                         int(std::floor((H + 0.5f * WORLD) / VOXEL)),
                         int(std::floor((gz + 0.5f * WORLD) / VOXEL)) };
            o.haveAnchor = true;
        } else if (take("--at", v)) {
            float ax, ay, az;
            if (std::sscanf(v.c_str(), " %f , %f , %f", &ax, &ay, &az) != 3) {
                err = "--at needs X,Y,Z";
                return false;
            }
            o.anchor = { int(std::floor((ax + 0.5f * WORLD) / VOXEL)),
                         int(std::floor((ay + 0.5f * WORLD) / VOXEL)),
                         int(std::floor((az + 0.5f * WORLD) / VOXEL)) };
            o.haveAnchor = true;
        } else if (take("--cell", v)) {
            int cx, cy, cz;
            if (std::sscanf(v.c_str(), " %d , %d , %d", &cx, &cy, &cz) != 3) {
                err = "--cell needs X,Y,Z";
                return false;
            }
            o.anchor = { cx, cy, cz };
            o.haveAnchor = true;
        } else if (take("--mat", v)) {
            o.import.mat = std::atoi(v.c_str());
            if (o.import.mat < 0 || o.import.mat >= kPaletteN) {
                err = "--mat must be a palette id 0.." + std::to_string(kPaletteN - 1);
                return false;
            }
        } else if (a == "--shell")
            o.solid = false;
        else if (a == "--dry-run")
            o.dryRun = true;
        else if (a == "--swap-yz")
            o.import.swapYz = true;
        else if (a == "--flip")
            o.import.flip = true;
        else if (a.rfind("-", 0) == 0) {
            err = "unknown option: " + a;
            return false;
        } else
            pos.push_back(a);
    }
    if (pos.empty()) {
        err = "no input file";
        return false;
    }
    o.inPath = pos.front();
    if (!o.haveAnchor) {
        // default: beside the hamlet hall, out of the live-edit keep-out box
        const float gx = 12.f, gz = 12.f;
        const float H = sharedHeightmap().sample(gx, gz);
        o.anchor = { int(std::floor((gx + 0.5f * WORLD) / VOXEL)),
                     int(std::floor((H + 0.5f * WORLD) / VOXEL)),
                     int(std::floor((gz + 0.5f * WORLD) / VOXEL)) };
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, err)) {
        std::printf("mesh_to_voxel: %s\n", err.c_str());
        std::printf("usage: mesh_to_voxel <file.stl|file.obj> --out NAME "
                    "[--scale S | --fit M] [--rot-y DEG] [--swap-yz] [--flip] "
                    "[--ground X,Z | --at X,Y,Z | --cell X,Y,Z] [--mat N] "
                    "[--shell] [--dry-run]\n");
        return 2;
    }

    std::vector<VoxelRecord> recs;
    MeshImportStats stats;
    if (!convertMeshToRecords(o.inPath, o.import, o.solid, o.anchor,
                              recs, stats, err)) {
        std::fprintf(stderr, "mesh_to_voxel: %s\n", err.c_str());
        return 1;
    }
    std::printf("  %s: %zu triangles, AABB %.2f x %.2f x %.2f m\n",
                o.inPath.c_str(), stats.triangles, stats.extent.x, stats.extent.y,
                stats.extent.z);
    std::printf("  grid %dx%dx%d cells; shell %zu, interior %zu -> %zu solid voxels "
                "(~%.1f KB layer)\n",
                stats.nx, stats.ny, stats.nz, stats.shellVoxels,
                stats.interiorVoxels, stats.shellVoxels + stats.interiorVoxels,
                (stats.shellVoxels + stats.interiorVoxels) * 16.0 / 1024.0);
    if (o.solid && stats.leak) {
        std::printf("  WARNING: no interior volume - the mesh is not watertight\n"
                    "  at this size (the exterior fill leaked through a hole\n"
                    "  larger than one voxel). Only the shell is emitted, so the\n"
                    "  object may render hollow. Repair the mesh, or re-run with\n"
                    "  --fit to make it bigger.\n");
    }

    if (o.dryRun) {
        std::printf("dry run: no file written (would place bottom-center at cell "
                    "[%d %d %d] = world (%.1f, %.1f, %.1f))\n",
                    o.anchor.x, o.anchor.y, o.anchor.z,
                    -0.5f * WORLD + (o.anchor.x + 0.5f) * VOXEL,
                    -0.5f * WORLD + (o.anchor.y + 0.5f) * VOXEL,
                    -0.5f * WORLD + (o.anchor.z + 0.5f) * VOXEL);
        return 0;
    }

    if (stats.clamped > 0)
        std::printf("  note: %d cells fell outside the 0..1023 lattice and were "
                    "dropped (move the anchor or scale down)\n",
                    stats.clamped);
    if (recs.empty()) {
        std::fprintf(stderr, "mesh_to_voxel: no records survived placement\n");
        return 1;
    }

    EditableWorld ew(std::string(VOXELFORGE_ASSET_DIR), "mesh_scratch.vxw",
                     "mesh_scratch", "object");
    if (!ew.writeObjectLayer(o.outName, recs)) {
        std::fprintf(stderr, "mesh_to_voxel: failed to write layer '%s'\n",
                     o.outName.c_str());
        return 1;
    }
    std::printf("mesh_to_voxel: wrote %zu voxels to assets/%s.vxw (layer '%s'; "
                "the running app hot-reloads within ~0.5 s)\n",
                recs.size(), o.outName.c_str(), o.outName.c_str());
    return 0;
}
