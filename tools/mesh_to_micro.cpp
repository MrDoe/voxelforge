// mesh_to_micro.cpp — author a MICRO-voxel layer from a real mesh (STL/OBJ).
//
// The repo had two authoring paths but neither made a *micro* mesh layer:
//   * vf_microvox  - procedural lathe / alpaca only (no mesh input);
//   * vf_mesh2vox  - world lattice (0.1 m), which the micro loader refuses.
// This tool fills the gap: it voxelizes a downloaded mesh on the object's OWN
// fine lattice, writes a micro .vxw, and registers a `role: micro` layer that
// carries meshSource/meshUp so the app draws it from the MESH (raster-owned),
// with the voxels serving as the shading/pick/shadow source.
//
//   mesh_to_micro <file.stl|file.obj> --out NAME [options]
//     --fit M         uniform-scale longest AABB axis to M metres (default: use
//                     the file's own units as metres)
//     --scale S       multiply vertices by S (mutually exclusive with --fit)
//     --mat N         palette material id for the emitted cells (default 20)
//     --voxel V       micro cell size in metres (default 0.005)
//     --up y|z        axis the FILE was authored in (default y; z = DCC export)
//     --at X,Y,Z      world position of the object's min corner
//     --ground X,Z    place min corner on the heightmap at X,Z
//     --append        register the layer in the active scenery's world.json
//     --dry-run       report the plan without writing anything
//
// Provenance: the mesh itself is staged under assets/models/ by the CC0 fetch
// script; meshSource is written relative to the shared library.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "voxel/common.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_mesh.hpp"
#include "voxel/scenery.hpp"
#include "voxel/worldfile.hpp"

namespace {
struct Options {
    std::string meshPath;
    std::string outName;
    float fit = -1.f;
    float scale = 1.f;
    bool scaled = false;
    uint8_t mat = 20;
    float voxel = 0.005f;
    vf::voxel::UpAxis up = vf::voxel::UpAxis::Y;
    glm::vec3 at { 0.f };
    bool haveAnchor = false;
    bool append = false;
    bool dryRun = false;
};

bool take(const std::string& a, int& i, int argc, char** argv,
          const char* name, std::string& dst)
{
    if (a != name)
        return false;
    if (i + 1 >= argc) {
        std::printf("mesh_to_micro: %s needs a value\n", name);
        std::exit(2);
    }
    dst = argv[++i];
    return true;
}

// Write a normalized ASCII STL of the (already scaled/rebased) triangles.
// The voxels are scaled to --fit metres, so the MESH the app rasterizes must
// carry the same scale - pointing meshSource at the raw download would draw an
// object ~5x too big whose vertices miss every solid cell (100% appearance
// fallback). This side-car lives beside the .vxw in the scenery's micro/ dir.
bool writeStl(const std::string& path,
              const std::vector<vf::voxel::MeshTri>& tris)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f << "solid micro_mesh\n";
    for (const auto& t : tris) {
        const glm::vec3 e1 = t.v[1] - t.v[0], e2 = t.v[2] - t.v[0];
        glm::vec3 n = glm::cross(e1, e2);
        const float len = glm::length(n);
        n = len > 1e-12f ? n / len : glm::vec3(0.f, 1.f, 0.f);
        f << "  facet normal " << n.x << ' ' << n.y << ' ' << n.z << '\n';
        f << "    outer loop\n";
        for (int k = 0; k < 3; ++k)
            f << "      vertex " << t.v[k].x << ' ' << t.v[k].y << ' '
              << t.v[k].z << '\n';
        f << "    endloop\n  endfacet\n";
    }
    f << "endsolid micro_mesh\n";
    return bool(f);
}
} // namespace

int main(int argc, char** argv)
{
    Options o;
    if (argc < 2) {
        std::printf("usage: mesh_to_micro <file.stl|file.obj> --out NAME "
                    "[--fit M | --scale S] [--mat N] [--voxel V] [--up y|z] "
                    "[--at X,Y,Z | --ground X,Z] [--append] [--dry-run]\n");
        return 2;
    }
    o.meshPath = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        std::string v;
        if (take(a, i, argc, argv, "--out", v))
            o.outName = v;
        else if (take(a, i, argc, argv, "--fit", v)) {
            o.fit = float(std::atof(v.c_str()));
            if (!(o.fit > 0.f)) {
                std::printf("mesh_to_micro: --fit must be > 0\n");
                return 2;
            }
        } else if (take(a, i, argc, argv, "--scale", v)) {
            o.scale = float(std::atof(v.c_str()));
            o.scaled = true;
            if (!(o.scale > 0.f)) {
                std::printf("mesh_to_micro: --scale must be > 0\n");
                return 2;
            }
        } else if (take(a, i, argc, argv, "--mat", v)) {
            const int m = std::atoi(v.c_str());
            if (m < 0 || m >= int(vf::voxel::kPaletteN)) {
                std::printf("mesh_to_micro: --mat must be 0..%d\n",
                            int(vf::voxel::kPaletteN) - 1);
                return 2;
            }
            o.mat = uint8_t(m);
        } else if (take(a, i, argc, argv, "--voxel", v)) {
            o.voxel = float(std::atof(v.c_str()));
            if (!(o.voxel > 0.f)) {
                std::printf("mesh_to_micro: --voxel must be > 0\n");
                return 2;
            }
        } else if (take(a, i, argc, argv, "--up", v)) {
            if (v == "z" || v == "Z")
                o.up = vf::voxel::UpAxis::Z;
            else if (v != "y" && v != "Y") {
                std::printf("mesh_to_micro: --up expects y or z\n");
                return 2;
            }
        } else if (take(a, i, argc, argv, "--at", v)) {
            if (std::sscanf(v.c_str(), " %f , %f , %f", &o.at.x, &o.at.y,
                            &o.at.z) != 3) {
                std::printf("mesh_to_micro: --at needs X,Y,Z\n");
                return 2;
            }
            o.haveAnchor = true;
        } else if (take(a, i, argc, argv, "--ground", v)) {
            float gx = 0.f, gz = 0.f;
            if (std::sscanf(v.c_str(), " %f , %f", &gx, &gz) != 2) {
                std::printf("mesh_to_micro: --ground needs X,Z\n");
                return 2;
            }
            o.at.x = gx;
            o.at.z = gz;
            o.at.y = vf::voxel::sharedHeightmap().sample(gx, gz);
            o.haveAnchor = true;
        } else if (a == "--append")
            o.append = true;
        else if (a == "--dry-run")
            o.dryRun = true;
        else {
            std::printf("mesh_to_micro: unknown option '%s'\n", a.c_str());
            return 2;
        }
    }
    if (o.outName.empty()) {
        std::printf("mesh_to_micro: --out NAME is required\n");
        return 2;
    }
    if (o.fit > 0.f && o.scaled) {
        std::printf("mesh_to_micro: --fit and --scale are mutually exclusive\n");
        return 2;
    }

    std::vector<vf::voxel::MeshTri> tris;
    std::string err;
    if (!vf::voxel::readMeshFile(o.meshPath, tris,
                                 /*defaultMat*/ int(o.mat), err)) {
        std::printf("mesh_to_micro: %s\n", err.c_str());
        return 1;
    }
    if (tris.empty()) {
        std::printf("mesh_to_micro: no triangles in %s\n", o.meshPath.c_str());
        return 1;
    }
    // Convert the FILE's frame into the object (Y-up) frame BEFORE measuring
    // the bounds - the same conversion loadMicroMesh applies for meshUp.
    if (o.up == vf::voxel::UpAxis::Z)
        for (auto& t : tris)
            for (int k = 0; k < 3; ++k)
                t.v[k] = vf::voxel::upAxisToY(t.v[k], o.up);

    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& t : tris)
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], t.v[k][a]);
                hi[a] = std::max(hi[a], t.v[k][a]);
            }
    const glm::vec3 extent = hi - lo;
    float s = o.scale;
    if (o.fit > 0.f) {
        const float longest = std::max(extent.x, std::max(extent.y, extent.z));
        if (!(longest > 1e-9f)) {
            std::printf("mesh_to_micro: degenerate mesh bounds\n");
            return 1;
        }
        s = o.fit / longest;
    }
    // Scale about the min corner and rebase it to the lattice origin.
    for (auto& t : tris)
        for (int k = 0; k < 3; ++k)
            t.v[k] = (t.v[k] - lo) * s;
    const glm::vec3 dim3 = extent * s;

    const int M = vf::voxel::kMicroMarginCells;
    const glm::ivec3 gridDim(
        int(std::ceil(dim3.x / o.voxel)) + 2 * M,
        int(std::ceil(dim3.y / o.voxel)) + 2 * M,
        int(std::ceil(dim3.z / o.voxel)) + 2 * M);
    const glm::vec3 gridOrigin(-float(M) * o.voxel);

    vf::voxel::MicroMeshGrid vg;
    if (!vf::voxel::voxelizeMicroMesh(tris, gridDim, o.voxel, gridOrigin, vg,
                                      &err)) {
        std::printf("mesh_to_micro: voxelizer: %s\n", err.c_str());
        return 1;
    }
    // Open / multi-component meshes (e.g. the compass, 10 components) do not
    // enclose volume, so the parity fill leaves some surface cells empty and
    // the app's per-vertex appearance lookup misses them (flat-grey patches).
    // Seed a solid cell at EVERY mesh vertex's lattice cell so each vertex has
    // a rest solid within 0 cells. This only thickens the shading source; the
    // rasterized silhouette still comes from the mesh.
    {
        auto cellOf = [&](float p, int axis) {
            return int(glm::floor((p - gridOrigin[axis]) / o.voxel));
        };
        size_t seeded = 0;
        for (const auto& t : tris)
            for (int k = 0; k < 3; ++k) {
                int x = std::clamp(cellOf(t.v[k].x, 0), 0, gridDim.x - 1);
                int y = std::clamp(cellOf(t.v[k].y, 1), 0, gridDim.y - 1);
                int z = std::clamp(cellOf(t.v[k].z, 2), 0, gridDim.z - 1);
                uint8_t& s = vg.solid[size_t(vg.idx(x, y, z))];
                if (!s) { s = 1; ++seeded; }
            }
        if (seeded)
            std::printf("mesh_to_micro: seeded %zu vertex cells for open "
                        "surfaces\n", seeded);
    }
    const glm::vec3& col = vf::voxel::kPalette[std::min<int>(
        o.mat, int(vf::voxel::kPaletteN) - 1)];
    const glm::vec2 rr = vf::voxel::kMaterialReflection[std::min<int>(
        o.mat, int(vf::voxel::kPaletteN) - 1)];

    // Emit a record for every solid cell. buildMicroField re-derives the grid
    // tightly around the record extents and adds its own air margin, so these
    // coordinates are the object's own lattice (margin already subtracted).
    std::vector<vf::voxel::VoxelRecord> recs;
    recs.reserve(vg.solidCount());
    for (int z = 0; z < gridDim.z; ++z)
        for (int y = 0; y < gridDim.y; ++y)
            for (int x = 0; x < gridDim.x; ++x) {
                if (!vg.solid[size_t(vg.idx(x, y, z))])
                    continue;
                recs.push_back(vf::voxel::makeVoxelRecord(
                    x - M, y - M, z - M, o.mat, int(col.r * 255.f),
                    int(col.g * 255.f), int(col.b * 255.f), int(rr.x), int(rr.y)));
            }
    if (recs.empty()) {
        std::printf("mesh_to_micro: voxelizer produced no solid cells\n");
        return 1;
    }

    vf::voxel::WorldFileMeta meta;
    meta.voxelSize = o.voxel;
    const int maxAxis = std::max(gridDim.x, std::max(gridDim.y, gridDim.z));
    meta.worldSize = float(maxAxis) * o.voxel;
    meta.waterLevel = vf::voxel::WATER_LEVEL;
    meta.gridN = uint32_t(maxAxis);
    meta.brickN = 8;

    const std::filesystem::path dir(vf::voxel::scenery::root() + "/micro");
    const std::string file = o.outName + ".vxw";
    if (o.dryRun) {
        std::printf("mesh_to_micro: would write %s (%zu records, %.1f KB), "
                    "grid %dx%dx%d at %.4f m, placed at (%.2f, %.2f, %.2f)\n",
                    (dir / file).string().c_str(), recs.size(),
                    recs.size() * 16.0 / 1024.0, gridDim.x, gridDim.y, gridDim.z,
                    o.voxel, o.at.x, o.at.y, o.at.z);
        return 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    vf::voxel::WorldFileData data;
    data.meta = meta;
    data.voxels = recs;
    const std::string path = (dir / file).string();
    if (!vf::voxel::worldfile::write(path, data)) {
        std::printf("mesh_to_micro: failed to write %s\n", path.c_str());
        return 1;
    }
    std::printf("mesh_to_micro: wrote %zu voxels to %s (%.4f m lattice)\n",
                recs.size(), path.c_str(), o.voxel);

    // The app rasterizes the MESH, so it must carry the SAME scale/rebase the
    // voxels got. Write a normalized STL side-car beside the .vxw and point
    // meshSource there (scenery::resolveShared tries the scenery dir first).
    // The original download stays in assets/models/ as the tracked source.
    const std::string stlName = o.outName + ".stl";
    const std::string stlPath = (dir / stlName).string();
    if (!writeStl(stlPath, tris)) {
        std::printf("mesh_to_micro: failed to write %s\n", stlPath.c_str());
        return 1;
    }
    const std::string meshSource = "micro/" + stlName;

    if (o.append) {
        const std::string manifest = vf::voxel::scenery::root() + "/world.json";
        std::vector<vf::voxel::worldfile::WorldLayer> layers;
        if (!vf::voxel::worldfile::loadManifest(manifest, layers)) {
            std::printf("mesh_to_micro: cannot read %s - file written, not "
                        "registered\n", manifest.c_str());
            return 0;
        }
        bool present = false;
        for (auto& l : layers)
            if (l.file == "micro/" + file) {
                l.role = "micro";
                l.enabled = true;
                l.meshSource = meshSource;
                l.meshUp = "y"; // the normalized side-car is always Y-up (converted above)
                present = true;
            }
        if (!present) {
            vf::voxel::worldfile::WorldLayer l;
            l.file = "micro/" + file;
            l.role = "micro";
            l.name = o.outName;
            l.enabled = true;
            l.pos[0] = o.at.x;
            l.pos[1] = o.at.y;
            l.pos[2] = o.at.z;
            l.meshSource = meshSource;
            l.meshUp = "y"; // the normalized side-car is always Y-up (converted above)
            layers.push_back(l);
        }
        if (vf::voxel::worldfile::writeManifest(manifest, layers))
            std::printf("mesh_to_micro: registered 'micro/%s' (meshSource %s) "
                        "in world.json\n", file.c_str(), meshSource.c_str());
        else
            std::printf("mesh_to_micro: file written but manifest append "
                        "failed\n");
    } else {
        std::printf("mesh_to_micro: add with --append, or list in world.json as "
                    "{\"file\":\"micro/%s\",\"role\":\"micro\","
                    "\"meshSource\":\"%s\"}\n", file.c_str(), meshSource.c_str());
    }
    return 0;
}
