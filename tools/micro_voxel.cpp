// micro_voxel - author a HIGH-DETAIL micro-voxel object on its OWN fine
// lattice and register it as a separate SVO world.
//
// The world lattice is fixed at 0.1 m, so a 30 cm vase cannot express features
// finer than 10 cm there. This tool writes a layer on its own fine lattice
// (default 5 mm) which the app loads into a SEPARATE SVO world and renders as a
// GPU overlay - the two are never merged.
//
//   micro_voxel --vase [--voxel 0.005] [--ground X,Z | --at X,Y,Z]
//                [--out NAME] [--mat N] [--profile vase|urn|alpaca] [--append]
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

// ---------------------------------------------------------------------------
// An ALPACA. Not a lathe: an animal is a union of primitives with a FUR
// displacement, so this evaluates an SDF over the grid instead. Only the SIGN
// matters (the app derives its own distance field), so the primitives may be
// approximate without any visual cost.
// ---------------------------------------------------------------------------

// Deterministic 3D value noise: the fleece needs clumps, not a smooth blob.
float hash3(glm::ivec3 p)
{
    uint32_t h = uint32_t(p.x) * 374761393u + uint32_t(p.y) * 668265263u +
                 uint32_t(p.z) * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0x00FFFFFFu) / float(0x00FFFFFFu);
}

float vnoise3(glm::vec3 p)
{
    const glm::ivec3 i = glm::ivec3(glm::floor(p));
    const glm::vec3 f = p - glm::vec3(i);
    const glm::vec3 w = f * f * (3.0f - 2.0f * f);
    float acc = 0.0f;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                acc += hash3(i + glm::ivec3(dx, dy, dz)) *
                       (dx ? w.x : 1.0f - w.x) * (dy ? w.y : 1.0f - w.y) *
                       (dz ? w.z : 1.0f - w.z);
    return acc;
}

// Three octaves at 5 cm / 2 cm / 1 cm periods. At an 8 mm voxel the finest is
// just over a cell, so the fleece reads as clumps rather than as noise.
float furNoise(glm::vec3 p)
{
    return 0.60f * vnoise3(p * 20.0f) + 0.28f * vnoise3(p * 48.0f) +
           0.12f * vnoise3(p * 96.0f);
}

// The STAPLE: the fleece, as fur rather than as a lump.
//
// furNoise() alone is a smooth 5 cm wave, and at an 8 mm voxel that reads as a
// wavy BODY - measured: the render showed a smooth stepped form with a photo
// pattern on it, "no fur visible except for the texture". Two things make it
// read as fleece:
//  * the strands HANG, so the noise is stretched along Y (a 7 cm vertical
//    period against a 3 cm horizontal one) - the tufts become vertical locks;
//  * the finest octave is deliberately AT the lattice pitch, so the surface
//    breaks into one-cell tufts and the silhouette goes ragged. Anything
//    smoother than the voxel is averaged away by the voxelisation.
float stapleNoise(glm::vec3 p)
{
    const glm::vec3 s(p.x, p.y * 0.42f, p.z);
    return 0.46f * vnoise3(s * 30.0f) + 0.32f * vnoise3(s * 68.0f) +
           0.22f * vnoise3(s * 140.0f);
}

float sdEllipsoid(glm::vec3 p, glm::vec3 r)
{
    const float k0 = glm::length(p / r);
    const float k1 = glm::length(p / (r * r));
    return k1 > 1e-9f ? k0 * (k0 - 1.0f) / k1 : -std::min(r.x, std::min(r.y, r.z));
}

// A capsule whose radius varies along its axis. Not an exact SDF, which is
// fine: only the sign is used.
float sdTaperedCapsule(glm::vec3 p, glm::vec3 a, glm::vec3 b, float ra, float rb)
{
    const glm::vec3 pa = p - a, ba = b - a;
    const float h = glm::clamp(glm::dot(pa, ba) / glm::dot(ba, ba), 0.0f, 1.0f);
    return glm::length(pa - ba * h) - mixf(ra, rb, h);
}

// A short vertical cylinder: the hooves. Only the sign matters.
float sdCylinderY(glm::vec3 p, glm::vec3 c, float r, float halfH)
{
    const glm::vec2 d = glm::vec2(glm::length(glm::vec2(p.x - c.x, p.z - c.z)) - r,
                                  std::abs(p.y - c.y) - halfH);
    return std::min(std::max(d.x, d.y), 0.0f) + glm::length(glm::max(d, 0.0f));
}

// The fleece palette: a Suri alpaca is a warm cream, with a tan muzzle, dark
// eyes and near-black hooves. Per-record colours, so the silhouette AND the
// face read at close range.
struct AlpacaLook {
    // Colours AND materials. A material is the unit of texture binding, and a
    // TEXTURED material ignores a record's colour - so the fleece needs its own
    // material (21, which a fur texture can be bound to without touching the
    // plaster vase), while the muzzle/eyes/hooves ride existing materials so
    // they do not turn furry. With no texture bound, the colours below show.
    glm::vec3 fleece { 0.94f, 0.90f, 0.82f };
    glm::vec3 muzzle { 0.80f, 0.70f, 0.58f };
    glm::vec3 eye { 0.10f, 0.09f, 0.08f };
    glm::vec3 hoof { 0.24f, 0.22f, 0.20f };
    uint8_t fleeceMat = 21; // fleece
    uint8_t muzzleMat = 19; // thatch - a dry tan nose
    uint8_t eyeMat = 4;     // rock - reads dark at 2 cells
    uint8_t hoofMat = 4;
};

std::vector<VoxelRecord> makeAlpaca(const Options& o, int& nx, int& ny, int& nz)
{
    const float vox = o.voxel;
    const float s = o.scale; // the whole animal scales about its feet
    // Bounds in metres, y up from the hooves. The grid is the AABB of the
    // primitives below plus a cell of slack, so no primitive can write a
    // negative index (which wraps in the uint16 record - see makeLathe).
    const glm::vec3 lo(-0.22f * s, 0.0f, -0.50f * s);
    const glm::vec3 hi(0.22f * s, 1.34f * s, 0.62f * s);
    nx = int(std::ceil((hi.x - lo.x) / vox)) + 2;
    ny = int(std::ceil((hi.y - lo.y) / vox)) + 2;
    nz = int(std::ceil((hi.z - lo.z) / vox)) + 2;

    const AlpacaLook look;
    const glm::vec3 furAmp(0.0f); // (unused: the amplitude is per-region below)
    (void)furAmp;
    std::vector<VoxelRecord> out;

    // Unit-scale geometry: metres with the animal ~1.3 m tall.
    for (int iz = 0; iz < nz; ++iz)
        for (int iy = 0; iy < ny; ++iy)
            for (int ix = 0; ix < nx; ++ix) {
                const glm::vec3 p = lo + glm::vec3(float(ix) + 0.5f, float(iy) + 0.5f,
                                                   float(iz) + 0.5f) * vox;
                const glm::vec3 q = p / s;

                // --- the primitives, nearest-region wins -------------------
                // The body is a fat ellipsoid; the neck lifts the head clear of
                // it; four tapered legs stand the animal on the ground.
                float d = sdEllipsoid(q - glm::vec3(0.0f, 0.62f, 0.02f),
                                      glm::vec3(0.175f, 0.195f, 0.33f));
                glm::vec3 col = look.fleece;
                uint8_t mat = look.fleeceMat;
                bool fleece = true;
                // Staple length per region: the back and neck carry the long
                // fleece, the ears are too thin to grow 4 cm of fur without
                // being swallowed by it, and the legs keep a short coat.
                float furLen = 0.055f;

                auto take = [&](float dd, glm::vec3 c, bool fur, uint8_t m,
                                float len) {
                    if (dd < d) {
                        d = dd;
                        col = c;
                        mat = m;
                        fleece = fur;
                        furLen = len;
                    }
                };

                take(sdTaperedCapsule(q, glm::vec3(0.0f, 0.70f, 0.26f),
                                      glm::vec3(0.0f, 1.02f, 0.34f), 0.105f, 0.075f),
                     look.fleece, true, look.fleeceMat, 0.050f);
                take(sdEllipsoid(q - glm::vec3(0.0f, 1.09f, 0.38f),
                                 glm::vec3(0.078f, 0.090f, 0.125f)),
                     look.fleece, true, look.fleeceMat, 0.040f);
                take(sdEllipsoid(q - glm::vec3(0.0f, 1.045f, 0.50f),
                                 glm::vec3(0.052f, 0.055f, 0.072f)),
                     look.muzzle, false, look.muzzleMat, 0.0f);
                take(sdTaperedCapsule(q, glm::vec3(0.0f, 0.70f, -0.32f),
                                      glm::vec3(0.0f, 0.82f, -0.40f), 0.050f, 0.032f),
                     look.fleece, true, look.fleeceMat, 0.045f);
                for (int side = -1; side <= 1; side += 2) {
                    const float sx = float(side);
                    // Ears: tall and slightly splayed, the alpaca's signature.
                    take(sdTaperedCapsule(q, glm::vec3(0.045f * sx, 1.15f, 0.35f),
                                          glm::vec3(0.078f * sx, 1.30f, 0.32f), 0.030f,
                                          0.013f),
                         look.fleece, true, look.fleeceMat, 0.008f);
                    // Eyes.
                    take(glm::length(q - glm::vec3(0.062f * sx, 1.115f, 0.415f)) - 0.017f,
                         look.eye, false, look.eyeMat, 0.0f);
                    // Legs: front pair shorter than the back (they are), each
                    // with a hoof.
                    take(sdTaperedCapsule(q, glm::vec3(0.083f * sx, 0.50f, 0.235f),
                                          glm::vec3(0.092f * sx, 0.06f, 0.245f), 0.055f,
                                          0.030f),
                         look.fleece, true, look.fleeceMat, 0.026f);
                    take(sdTaperedCapsule(q, glm::vec3(0.090f * sx, 0.52f, -0.235f),
                                          glm::vec3(0.100f * sx, 0.06f, -0.250f), 0.065f,
                                          0.032f),
                         look.fleece, true, look.fleeceMat, 0.028f);
                    take(sdCylinderY(q, glm::vec3(0.092f * sx, 0.025f, 0.245f), 0.034f,
                                     0.028f),
                         look.hoof, false, look.hoofMat, 0.0f);
                    take(sdCylinderY(q, glm::vec3(0.100f * sx, 0.025f, -0.250f), 0.036f,
                                     0.028f),
                         look.hoof, false, look.hoofMat, 0.0f);
                }

                // --- the fleece -------------------------------------------
                // Only near the surface, and only on the fleece regions: the
                // eyes, muzzle and hooves stay crisp.
                if (fleece && d < furLen + 0.015f) {
                    // Clumps carry the silhouette, the staple gives it the
                    // ragged edge; biased outward so the coat reads as grown
                    // fleece rather than as a thinned body.
                    const float n = 0.45f * furNoise(q) + 0.55f * stapleNoise(q);
                    d -= (n - 0.40f) * furLen;
                }
                if (d > 0.0f)
                    continue;

                VoxelRecord v;
                v.x = uint16_t(ix);
                v.y = uint16_t(iy);
                v.z = uint16_t(iz);
                v.a = 255;
                v.r = uint8_t(glm::clamp(col.r * 255.0f, 0.0f, 255.0f));
                v.g = uint8_t(glm::clamp(col.g * 255.0f, 0.0f, 255.0f));
                v.b = uint8_t(glm::clamp(col.b * 255.0f, 0.0f, 255.0f));
                v.materialId = mat;
                const glm::vec2 rr = kMaterialReflection[std::min<int>(mat, kPaletteN - 1)];
                v.reflectivity = uint8_t(rr.x);
                v.roughness = uint8_t(rr.y);
                out.push_back(v);
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
                    "[--append]\n"
                    "       micro_voxel --alpaca [--voxel 0.008] [--at X,Y,Z] "
                    "[--out NAME] [--append]\n");
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
        } else if (a == "--alpaca") {
            o.procedural = true;
            o.profile = "alpaca";
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
    std::vector<VoxelRecord> recs =
        o.profile == "alpaca" ? makeAlpaca(o, nx, ny, nz) : makeLathe(o, nx, ny, nz);
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