#pragma once
// STL / OBJ mesh import + unit transform, shared by the GUI, the
// tools/mesh_to_voxel.cpp converter, and the vf_mcp `import_mesh` tool.
//
// readMeshFile() parses binary+ASCII STL and a Wavefront OBJ subset (v, vt,
// vn, f with quads/n-gons and relative indices, mtllib/usemtl with Kd
// colours). transformMesh() applies the import options and leaves the mesh in
// METRES with its AABB min at the origin and its base on y=0, ready for
// mesh_voxel.hpp's voxelizeMesh().
//
// Units are the caller's responsibility: STL from CAD is usually millimetres,
// so pass scale=0.001 or fitMeters to size it in metres.
#include "voxel/common.hpp"
#include "voxel/mesh_voxel.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vf::voxel {

struct MeshImportOptions {
    float scale = 1.0f; // model units -> metres
    bool hasFit = false;
    float fitMeters = 2.0f; // uniform scale so the longest AABB side = this
    float rotY = 0.0f;      // degrees about the vertical (Y) axis
    bool swapYz = false;    // Z-up import
    bool flip = false;      // reverse winding (inside-out mesh)
    int mat = 4;            // palette id for STL / untextured faces
};

inline bool readFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0)
        return false;
    f.seekg(0, std::ios::beg);
    out.resize(size_t(n));
    return bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

inline uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline float le32f(const uint8_t* p)
{
    uint32_t u = le32(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// Nearest kPalette entry by sRGB distance; the brick keeps `mat` in the same
// colour family so the surface response (refl/rough) matches the appearance.
inline uint8_t paletteNearest(uint8_t r, uint8_t g, uint8_t b)
{
    float best = 1e30f;
    uint8_t bestId = 4;
    for (int i = 0; i < kPaletteN; ++i) {
        const glm::vec3& c = kPalette[size_t(i)];
        const float dr = float(r) - c.r * 255.f;
        const float dg = float(g) - c.g * 255.f;
        const float db = float(b) - c.b * 255.f;
        const float d = dr * dr + dg * dg + db * db;
        if (d < best) {
            best = d;
            bestId = uint8_t(i);
        }
    }
    return bestId;
}

// Binary STL: 80 B header, u32 triangle count, 50 B per triangle (facet
// normal + 3 float32 vertices). ASCII STL lists "facet ... vertex x y z x3
// ... endfacet". Some BINARY files also start with "solid" in the header, so
// the declared-count size check decides the format.
inline bool readSTL(const std::vector<uint8_t>& buf, std::vector<MeshTri>& tris,
                    std::string& err)
{
    err.clear();
    if (buf.size() < 84) {
        err = "file too small to be an STL";
        return false;
    }
    const uint32_t count = le32(buf.data() + 80);
    if (size_t(84) + size_t(count) * 50 == buf.size()) {
        tris.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = buf.data() + 84 + size_t(i) * 50;
            MeshTri t;
            for (int k = 0; k < 3; ++k) // facet normal at p+0, verts at p+12
                t.v[k] = { le32f(p + 12 + k * 12), le32f(p + 12 + k * 12 + 4),
                           le32f(p + 12 + k * 12 + 8) };
            tris.push_back(t);
        }
        return true;
    }
    std::string s(buf.begin(), buf.end());
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::istringstream in(s);
    std::string tok;
    MeshTri cur;
    int vi = 0;
    bool inFacet = false;
    while (in >> tok) {
        if (tok == "facet") {
            inFacet = true;
            vi = 0;
        } else if (tok == "vertex" && inFacet) {
            float x, y, z;
            if (!(in >> x >> y >> z)) {
                err = "ASCII STL: vertex line missing coordinates";
                return false;
            }
            if (vi < 3)
                cur.v[vi++] = { x, y, z };
        } else if (tok == "endfacet" && inFacet) {
            if (vi == 3)
                tris.push_back(cur);
            inFacet = false;
        }
    }
    if (tris.empty()) {
        err = "STL parsed as ASCII but no facets found (size mismatch vs the "
              "binary header count; is the file truncated?)";
        return false;
    }
    return true;
}

inline bool readOBJ(const std::vector<uint8_t>& buf, std::vector<MeshTri>& tris,
                    int defaultMat, std::string& err,
                    const std::string& sourceDir = {})
{
    err.clear();
    std::istringstream in(std::string(buf.begin(), buf.end()));
    std::string line;
    std::vector<glm::vec3> verts;
    uint8_t curMat = uint8_t(std::clamp(defaultMat, 0, int(kPaletteN) - 1));
    std::string curMtlName;
    bool curHasColor = false;
    uint8_t curR = 0, curG = 0, curB = 0;
    struct Mtl {
        uint8_t mat;
        bool hasColor;
        uint8_t r, g, b;
    };
    std::vector<std::pair<std::string, Mtl>> mtls;
    auto findMtl = [&](const std::string& name) -> const Mtl* {
        for (const auto& m : mtls)
            if (m.first == name)
                return &m.second;
        return nullptr;
    };
    auto emitFace = [&](const std::vector<int>& idx) {
        if (idx.size() < 3)
            return;
        // A malformed face must not turn a GUI import into an out-of-bounds
        // read. Valid OBJ indices are 1-based positive or negative-relative;
        // both are resolved to [0, verts.size()) before triangulation.
        for (int index : idx)
            if (index < 0 || size_t(index) >= verts.size())
                return;
        for (size_t i = 1; i + 1 < idx.size(); ++i) { // fan triangulation
            MeshTri t;
            t.v[0] = verts[size_t(idx[0])];
            t.v[1] = verts[size_t(idx[i])];
            t.v[2] = verts[size_t(idx[i + 1])];
            t.mat = curMat;
            t.hasColor = curHasColor;
            t.r = curR;
            t.g = curG;
            t.b = curB;
            tris.push_back(t);
        }
    };
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.resize(hash);
        std::istringstream ls(line);
        std::string tag;
        if (!(ls >> tag))
            continue;
        if (tag == "v") {
            float x, y, z;
            if (!(ls >> x >> y >> z)) {
                err = "OBJ: bad vertex line";
                return false;
            }
            verts.push_back({ x, y, z });
        } else if (tag == "f") {
            std::vector<int> idx;
            std::string tok;
            while (ls >> tok) {
                const int n = int(verts.size());
                int v = 0, sgn = 1;
                size_t p = 0;
                if (p < tok.size() && tok[p] == '-') {
                    sgn = -1;
                    ++p;
                }
                while (p < tok.size() && std::isdigit((unsigned char)tok[p])) {
                    v = v * 10 + (tok[p] - '0');
                    ++p;
                }
                if (p == 0 || v == 0)
                    continue; // malformed token; texture/normal suffixes ignored
                v *= sgn;
                idx.push_back(v > 0 ? v - 1 : n + v); // 1-based / negative-relative
            }
            emitFace(idx);
        } else if (tag == "usemtl") {
            std::string name;
            ls >> name;
            curMtlName = name;
            if (const Mtl* m = findMtl(name)) {
                curMat = m->mat;
                curHasColor = m->hasColor;
                curR = m->r;
                curG = m->g;
                curB = m->b;
            } else {
                curMat = uint8_t(std::clamp(defaultMat, 0, int(kPaletteN) - 1));
                curHasColor = false;
            }
        } else if (tag == "mtllib") {
            // resolve relative to the OBJ, CWD, the asset dir, or its textures
            // subdir
            std::string name;
            if (!(ls >> name))
                continue;
            std::vector<std::string> dirs;
            if (!sourceDir.empty())
                dirs.push_back(sourceDir + "/");
            dirs.push_back("");
            dirs.push_back(std::string(VOXELFORGE_ASSET_DIR) + "/");
            dirs.push_back(std::string(VOXELFORGE_ASSET_DIR) + "/textures/");
            for (const std::string& d : dirs) {
                std::vector<uint8_t> mbuf;
                if (!readFileBytes(d + name, mbuf))
                    continue;
                std::istringstream mi(std::string(mbuf.begin(), mbuf.end()));
                std::string mline, mname;
                while (std::getline(mi, mline)) {
                    std::istringstream ml(mline);
                    std::string mtag;
                    if (!(ml >> mtag))
                        continue;
                    if (mtag == "newmtl") {
                        ml >> mname;
                    } else if (mtag == "Kd" && !mname.empty()) {
                        float r, g, b;
                        if (ml >> r >> g >> b) {
                            const int ir = std::clamp(int(std::lround(r * 255.f)), 0, 255);
                            const int ig = std::clamp(int(std::lround(g * 255.f)), 0, 255);
                            const int ib = std::clamp(int(std::lround(b * 255.f)), 0, 255);
                            Mtl m;
                            m.mat = paletteNearest(uint8_t(ir), uint8_t(ig), uint8_t(ib));
                            m.hasColor = true;
                            m.r = uint8_t(ir);
                            m.g = uint8_t(ig);
                            m.b = uint8_t(ib);
                            mtls.push_back({ mname, m });
                        }
                    }
                }
                // OBJ files commonly put `usemtl` before `mtllib`; apply the
                // now-loaded definition to the active face stream as well.
                if (const Mtl* active = findMtl(curMtlName)) {
                    curMat = active->mat;
                    curHasColor = active->hasColor;
                    curR = active->r;
                    curG = active->g;
                    curB = active->b;
                }
                break;
            }
        }
    }
    if (verts.empty()) {
        err = "OBJ has no vertices";
        return false;
    }
    if (tris.empty()) {
        err = "OBJ has no faces";
        return false;
    }
    return true;
}

// Dispatch by extension. STL gets a uniform material; OBJ keeps its MTL
// colours (falling back to defaultMat for untextured faces).
inline bool readMeshFile(const std::string& path, std::vector<MeshTri>& tris,
                        int defaultMat, std::string& err)
{
    tris.clear();
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) {
        err = "cannot read " + path;
        return false;
    }
    std::string lower = path;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool isObj = lower.size() > 4 &&
                       lower.compare(lower.size() - 4, 4, ".obj") == 0;
    const bool isStl = lower.size() > 4 &&
                       lower.compare(lower.size() - 4, 4, ".stl") == 0;
    if (isObj) {
        const std::string sourceDir =
            std::filesystem::path(path).parent_path().string();
        if (!readOBJ(buf, tris, defaultMat, err, sourceDir))
            return false;
    } else if (isStl) {
        if (!readSTL(buf, tris, err))
            return false;
        const uint8_t m = uint8_t(std::clamp(defaultMat, 0, int(kPaletteN) - 1));
        for (auto& t : tris)
            t.mat = m;
    } else {
        err = "unknown extension (need .stl or .obj): " + path;
        return false;
    }
    return true;
}

// Apply the import transform in place; extent receives the final AABB size in
// metres. After this the mesh sits with its min at the origin and its base on
// y=0, so the caller can place the bottom-center directly.
inline bool transformMesh(std::vector<MeshTri>& tris, const MeshImportOptions& o,
                          glm::vec3& extent, std::string& err)
{
    if (tris.empty()) {
        err = "mesh has no triangles";
        return false;
    }
    if (o.flip)
        for (auto& t : tris)
            std::swap(t.v[1], t.v[2]);

    if (o.swapYz)
        for (auto& t : tris)
            for (glm::vec3& p : t.v)
                p = { p.x, p.z, p.y };

    // rotate about the AABB centre so the object spins in place
    if (o.rotY != 0.f) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const auto& t : tris)
            for (const glm::vec3& p : t.v)
                for (int k = 0; k < 3; ++k) {
                    lo[k] = glm::min(lo[k], p[k]);
                    hi[k] = glm::max(hi[k], p[k]);
                }
        const glm::vec3 c = 0.5f * (lo + hi);
        const float rad = glm::radians(o.rotY);
        const float cs = std::cos(rad), sn = std::sin(rad);
        for (auto& t : tris)
            for (glm::vec3& p : t.v) {
                const glm::vec3 q = p - c;
                p = { c.x + cs * q.x + sn * q.z, p.y, c.z - sn * q.x + cs * q.z };
            }
    }

    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& t : tris)
        for (const glm::vec3& p : t.v)
            for (int k = 0; k < 3; ++k) {
                lo[k] = glm::min(lo[k], p[k]);
                hi[k] = glm::max(hi[k], p[k]);
            }
    float s = o.scale;
    if (o.hasFit) {
        const float longest = glm::max(hi.x - lo.x, glm::max(hi.y - lo.y, hi.z - lo.z));
        if (longest <= 1e-8f) {
            err = "mesh has zero extent";
            return false;
        }
        s = o.fitMeters / longest;
    }
    for (auto& t : tris)
        for (glm::vec3& p : t.v)
            p = (p - lo) * s;

    hi = glm::vec3(0.f);
    for (const auto& t : tris)
        for (const glm::vec3& p : t.v)
            for (int k = 0; k < 3; ++k)
                hi[k] = glm::max(hi[k], p[k]);
    extent = hi;
    return true;
}

// Result metadata shared by the CLI, MCP tool, and GUI importer.  Keeping the
// conversion in one helper prevents the three entry points from drifting on
// path resolution, fit semantics, or the solid/shell default.
struct MeshImportStats {
    size_t triangles = 0;
    size_t shellVoxels = 0;
    size_t interiorVoxels = 0;
    size_t solidVoxels = 0;
    int nx = 0, ny = 0, nz = 0;
    int clamped = 0;
    bool leak = false;
    glm::vec3 extent { 0.f };
};

// Resolve a user-facing mesh path.  The app is normally launched from the
// repository root, but a packaged build may be launched elsewhere; accept the
// literal path as well as paths relative to assets/ and assets/models/.
inline bool resolveMeshPath(const std::string& requested, std::string& resolved,
                            std::string& err)
{
    err.clear();
    resolved.clear();
    if (requested.empty()) {
        err = "mesh path is empty";
        return false;
    }

    namespace fs = std::filesystem;
    const fs::path assetDir(fs::path(VOXELFORGE_ASSET_DIR));
    std::vector<fs::path> candidates;
    const fs::path req(requested);
    if (req.is_absolute()) {
        candidates.push_back(req);
    } else {
        candidates.push_back(req);
        candidates.push_back(assetDir / req);
        candidates.push_back(assetDir / "models" / req);
        // Also accept a path written relative to the repository root when the
        // app's working directory is elsewhere.
        candidates.push_back(assetDir.parent_path() / req);
    }
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec)
            continue;
        resolved = candidate.string();
        return true;
    }
    err = "cannot read mesh: " + requested;
    return false;
}

// Parse, transform, voxelize, and place a mesh in one call.  `path` may be a
// literal path or one of the relative forms accepted by resolveMeshPath().
inline bool convertMeshToRecords(const std::string& path,
                                  const MeshImportOptions& options,
                                  bool solid,
                                  glm::ivec3 anchor,
                                  std::vector<VoxelRecord>& records,
                                  MeshImportStats& stats,
                                  std::string& err)
{
    records.clear();
    stats = MeshImportStats {};
    std::string resolved;
    if (!resolveMeshPath(path, resolved, err))
        return false;
    if (!options.hasFit && !(options.scale > 0.f)) {
        err = "mesh scale must be > 0 (or enable fit-to-size)";
        return false;
    }
    if (options.hasFit && (!(options.fitMeters > 0.f) || options.fitMeters > 90.f)) {
        err = "fit size must be in (0, 90] metres";
        return false;
    }

    std::vector<MeshTri> tris;
    if (!readMeshFile(resolved, tris, options.mat, err))
        return false;
    stats.triangles = tris.size();
    if (!transformMesh(tris, options, stats.extent, err))
        return false;

    MeshVoxelOptions voxelOptions;
    voxelOptions.solid = solid;
    VoxelizedMesh mesh;
    if (!voxelizeMesh(tris, mesh, voxelOptions, &err))
        return false;
    stats.nx = mesh.nx;
    stats.ny = mesh.ny;
    stats.nz = mesh.nz;
    stats.leak = mesh.leak;
    for (uint8_t cell : mesh.cell) {
        if (cell == 1)
            ++stats.shellVoxels;
        else if (cell == 2)
            ++stats.interiorVoxels;
    }
    int clamped = 0;
    meshToRecords(mesh, anchor, records, &clamped);
    stats.clamped = clamped;
    stats.solidVoxels = records.size();
    if (records.empty()) {
        err = clamped > 0
            ? "no mesh voxels survived placement (outside the 0..1023 lattice)"
            : "mesh voxelization produced no records";
        return false;
    }
    return true;
}

} // namespace vf::voxel
