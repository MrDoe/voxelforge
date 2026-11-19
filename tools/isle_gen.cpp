// Offline terrain baker for the LighthouseIsle scenery: a TROPICAL sea
// island on the fixed world water plane (WATER_LEVEL = -0.9).
//
// v2 layout (mountain + caves request, 2026-11-19): a bigger island
// (radii 36 x 28 m) with a wide south beach into a shallow sand shelf, a
// dune line, a jungle meadow, an east headland with the flattened lighthouse
// pad, a WALK-IN CAVE mesa at the NE (authored as a shell-voxel layer by
// tools/isle_nature.py — this baker only flattens its footplate, because a
// cave air void must sit ABOVE the heightfield top: terrain below the column
// top is promoted solid and cannot be unbaked), a volcanic cone at the N
// (heightfield, rocky bands), a cenote (blue hole) east of the meadow, a
// tidal pool on the beach flat, a cottage pad SW and a gravel footpath.
//
// Water is the one world-wide plane, so every column below WATER_LEVEL simply
// reads as sea; the shelf stays shallow (< 0.55 m under the plane) near the
// beach so materialFromBands keeps it light sand (turquoise over the shelf).
//
// Writes the 16-bit heightmap PNG (stored deflate — same writer as
// heightmap_gen), then landscape.vxw + world.json PRESERVING ai_edits and
// foreign authored layers, exactly like the hamlet baker.
//
// Standalone tool: compile via scenery/LighthouseIsle/bake.sh (not wired
// into the default ninja build on purpose — author-time tool).
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/worldfile.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {

// ---- deterministic noise (same family as heightmap_gen) --------------------
float fractf(float x) { return x - std::floor(x); }
float hash2(float x, float y)
{
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453123f;
    return fractf(h);
}
float valueNoise2(float x, float y)
{
    float xi = std::floor(x), yi = std::floor(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3.f - 2.f * xf), v = yf * yf * (3.f - 2.f * yf);
    float a = hash2(xi, yi), b = hash2(xi + 1.f, yi);
    float c = hash2(xi, yi + 1.f), d = hash2(xi + 1.f, yi + 1.f);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v) * 2.f - 1.f;
}
float fbm2(float x, float y)
{
    return valueNoise2(x, y) * 0.6f + valueNoise2(x * 2.13f, y * 2.13f) * 0.28f +
           valueNoise2(x * 4.41f, y * 4.41f) * 0.12f;
}
inline float gauss(float u) { return std::exp(-u * u); }

// ---- authored layout (world metres; camera views the isle from open sea S) --
const float kRx = 36.f, kRz = 28.f;             // elliptical coast radii
const glm::vec2 kHeadland { 27.f, -5.f };       // rocky NE point
// v3 rule (user 2026-11-19): the landscape is generated terrain edited for
// detail ONLY — no building pads, no carved water features. These two are
// layout anchors consumed by the object layers (the lighthouse plinth
// bridges down to the natural knoll; the house footing lives in
// isle_interiors.py); nothing here flattens or carves for them.
const float kLighthousePadY = 5.60f;            // tower base reference level
const glm::vec2 kHousePad { -12.f, -6.f };      // cottage layout anchor
const glm::vec2 kVolcano { -6.f, 16.f };        // heightfield cone
// (a cave-mesa footplate lived here in v2.0; the dome read ugly and the user
// ordered it removed 2026-11-19 — the walk-in-cave CONTRACT is kept in
// .opencode/wiki/concepts/lighthouse-isle.md for a future cliff-hewn cave)
// footpath polyline (cottage lawn -> beach -> jetty root); gravel material
const glm::vec2 kPathA { -11.f, -7.5f }, kPathB { -2.f, -14.f },
              kPathC { 1.f, -19.f };

// normalised coast distance: ~1.0 at the shoreline, >1 at sea
float coastD(float x, float z)
{
    float e = std::sqrt((x * x) / (kRx * kRx) + (z * z) / (kRz * kRz));
    // wobble: two low-freq lobes + a finer nibble (~16% total)
    float wob = fbm2(x * 0.05f + 3.1f, z * 0.05f - 1.4f) * 0.15f +
                fbm2(x * 0.12f - 6.0f, z * 0.12f + 2.2f) * 0.05f;
    return e * (1.f + wob);
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    glm::vec2 ab = b - a;
    float t = glm::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.f, 1.f);
    return glm::length(p - (a + ab * t));
}
float pathDist(float x, float z)
{
    glm::vec2 p(x, z);
    return std::min(segDist(p, kPathA, kPathB), segDist(p, kPathB, kPathC));
}

float terrainHeightAt(float x, float z)
{
    float d = coastD(x, z);
    // south (-z) faces the open sea and carries the wide beach + dunes
    float southness = glm::clamp(-z / 30.f, 0.f, 1.f);

    // ---- sea bed: shallow sand shelf (turquoise), then a rock drop-off
    float bed = WATER_LEVEL - 0.55f - 4.1f * smoothstepf(1.00f, 1.30f, d) +
                fbm2(x * 0.09f, z * 0.09f) * 0.12f;

    // ---- land assembly
    float land = 0.30f * smoothstepf(1.16f, 0.80f, d);   // gentle beach ramp
    land += 4.1f * smoothstepf(0.74f, 0.16f, d) *
            (0.70f + 0.55f * fbm2(x * 0.045f, z * 0.045f));  // jungle hills
    // NE ridge sweeping to the headland
    land += 2.6f * gauss((x - 14.f) / 12.f) * gauss((z - 8.f) / 10.f);
    // the headland knoll under the lighthouse: an ORGANIC rocky rise (no
    // flat pad — v3 rule), 4-5 m with a wobbled crown; the tower's own
    // rock plinth in isle_lighthouse.vxw bridges the uneven top.
    {
        float hd = std::hypot(x - kHeadland.x, (z - kHeadland.y) * 1.25f);
        land += 4.6f * smoothstepf(8.5f, 2.2f, hd) *
                (0.82f + 0.30f * fbm2(x * 0.10f + 11.f, z * 0.10f));
    }
    // volcanic cone at the N (rocky; heightfield so its flanks slope-class)
    {
        float cd = std::sqrt((x - kVolcano.x) * (x - kVolcano.x) / (10.5f * 10.5f) +
                             (z - kVolcano.y) * (z - kVolcano.y) / (9.f * 9.f));
        float cone = 10.5f * smoothstepf(1.0f, 0.35f, cd);
        // small crater dent at the very top
        cone -= 1.6f * smoothstepf(0.28f, 0.10f, cd);
        cone += 0.7f * fbm2(x * 0.16f, z * 0.16f) * smoothstepf(0.9f, 0.4f, cd);
        land += std::max(cone, 0.f);
    }
    // dune line behind the south beach
    land += 0.34f * southness * gauss((d - 0.93f) / 0.06f) *
            (0.7f + 0.6f * fbm2(x * 0.3f, z * 0.3f));
    // micro-relief on land only
    float landMask = smoothstepf(0.72f, 1.04f, d);
    land += (fbm2(x * 0.8f, z * 0.8f) * 0.11f +
             fbm2(x * 2.2f + 7.f, z * 2.2f - 3.f) * 0.045f) *
            landMask;

    float h = glm::mix(bed, land, smoothstepf(1.045f, 0.88f, d));

    // ---- cenote (blue hole): REMOVED (v3) — a dish the plane fills is an
    // object baked into terrain, and the user rule separates the two.
    // ---- tidal pool: REMOVED (v3), same reason.

    // NO object-support pads (user rule 2026-11-19: landscape is terrain
    // generated + edited for detail, but every object/building is its own
    // layer file and brings its own foundation — the lighthouse plinth and
    // the house footing live in isle_nature.py / isle_interiors.py).
    // The cenote and tidal pool dishes went with the pads: a carved water
    // feature baked into the heightfield is a thing, not a terrain detail.

    return glm::clamp(h, kHmMinMeters + 0.05f, kHmMaxMeters - 0.05f);
}

// island material bands: sand shelf / beach / dune grass / rock / jungle /
// gravel path / lichen duff. Grass mats (0/1) are deliberate — applyFlora's
// blade micro layer only fires on materials <=1 (voxel small plants are
// banned by the 2026-11-19 directive; the dunes are pure shading + the
// upcoming photo cards).
uint8_t isleMat(float x, float z, float H, float slope)
{
    float wd = WATER_LEVEL - H;
    float n = fbm2(x * 0.35f, z * 0.35f);
    float southness = glm::clamp(-z / 30.f, 0.f, 1.f);

    // lighthouse rock platform (pad apron included)
    if (glm::length(glm::vec2(x, z) - kHeadland) < 5.2f && wd < -0.2f)
        return n > 0.0f ? uint8_t(5) : uint8_t(4);

    if (wd > 0.55f)
        return n > 0.15f ? uint8_t(5) : uint8_t(4); // deep bed: dark rock
    if (wd > -0.02f)
        return 3;                                  // waterline wet sand
    if (wd > -0.62f)                               // beach flat + swash
        return (n > 0.38f && southness < 0.25f) ? uint8_t(2) : uint8_t(3);
    if (slope > 0.85f)
        return n > 0.0f ? uint8_t(4) : uint8_t(5); // cliffs / cone flanks
    // dune band right behind the beach: dune grass (mats 0/1 fire the flora)
    if (southness > 0.25f && wd < -0.35f && wd > -1.20f &&
        glm::length(glm::vec2(x / kRx, z / kRz)) < 0.98f)
        return n > 0.20f ? uint8_t(1) : uint8_t(0);
    // gravel footpath (feathered so its edge is not a stamp). NEVER mat 15:
    // kEmissive keys on the CELL MATERIAL (9-15), so a mat-15 path glows
    // regardless of the gravel photo bound to slot 15 — mat 5 light rock
    // instead (pebble layers get the photo via tex override 15).
    if (wd < -0.65f && pathDist(x, z) < 1.05f + 0.35f * n)
        return 5;
    if (wd < -3.0f && slope < 0.55f && n > 0.44f)
        return 2;                                  // duff patches in the wood
    return n > 0.25f ? uint8_t(1) : uint8_t(0);    // jungle meadow two-tone
}

// ---- minimal PNG writer (16-bit gray, zlib stored deflate — copy of the
// hamlet baker's; heightmap.hpp/.cpp reads it back with stb). -----------------
uint32_t crcTable[256];
void initCrc()
{
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
uint32_t adler32(const uint8_t* d, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
void be32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
void writeChunk(std::FILE* f, const char* type, const uint8_t* data, uint32_t len)
{
    uint8_t hdr[8], crcB[4];
    be32(hdr, len);
    std::memcpy(hdr + 4, type, 4);
    std::fwrite(hdr, 1, 8, f);
    if (len)
        std::fwrite(data, 1, len, f);
    uint32_t state = ~0u;
    for (size_t i = 0; i < 4; ++i)
        state = crcTable[(state ^ reinterpret_cast<const uint8_t*>(type)[i]) & 255] ^
                (state >> 8);
    for (size_t i = 0; i < len; ++i)
        state = crcTable[(state ^ data[i]) & 255] ^ (state >> 8);
    be32(crcB, state ^ ~0u);
    std::fwrite(crcB, 1, 4, f);
}
bool writePng16(const char* path, const std::vector<uint16_t>& px, uint32_t w, uint32_t h)
{
    initCrc();
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    std::fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 16;
    ihdr[9] = 0;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    writeChunk(f, "IHDR", ihdr, 13);
    size_t rowBytes = size_t(1) + size_t(w) * 2;
    std::vector<uint8_t> raw(rowBytes * h);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* r = &raw[size_t(y) * rowBytes];
        r[0] = 0;
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t s = px[size_t(y) * w + x];
            r[1 + x * 2] = uint8_t(s >> 8);
            r[2 + x * 2] = uint8_t(s);
        }
    }
    size_t total = raw.size();
    size_t nBlocks = (total + 65534) / 65535;
    std::vector<uint8_t> z;
    z.reserve(total + 5 * nBlocks + 6);
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < total) {
        size_t len = std::min<size_t>(65535, total - off);
        bool last = off + len >= total;
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(len & 255));
        z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t((~len) & 255));
        z.push_back(uint8_t(((~len) >> 8) & 255));
        z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + len));
        off += len;
    }
    uint8_t adl[4];
    be32(adl, adler32(raw.data(), total));
    z.insert(z.end(), adl, adl + 4);
    writeChunk(f, "IDAT", z.data(), uint32_t(z.size()));
    writeChunk(f, "IEND", nullptr, 0);
    return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "scenery/LighthouseIsle/heightmap.png";
    std::string worldOut = argc > 2 ? argv[2] : "scenery/LighthouseIsle/world.vxw";
    std::vector<uint16_t> px(size_t(kHmSize) * kHmSize);
    float mn = 1e30f, mx = -1e30f;
    size_t water = 0;
    for (uint32_t y = 0; y < kHmSize; ++y) {
        for (uint32_t x = 0; x < kHmSize; ++x) {
            float wx = (float(x) / (kHmSize - 1) - 0.5f) * WORLD;
            float wz = (float(y) / (kHmSize - 1) - 0.5f) * WORLD;
            float hgt = terrainHeightAt(wx, wz);
            mn = glm::min(mn, hgt);
            mx = glm::max(mx, hgt);
            water += hgt < WATER_LEVEL ? 1 : 0;
            px[size_t(y) * kHmSize + x] =
                uint16_t(glm::clamp((hgt - kHmMinMeters) / (kHmMaxMeters - kHmMinMeters),
                                    0.0f, 1.0f) *
                         65535.0f);
        }
    }
    if (!writePng16(out, px, kHmSize, kHmSize)) {
        std::fprintf(stderr, "failed to write %s\n", out);
        return 1;
    }
    std::printf("heightmap %s: %ux%u, h in [%.2f, %.2f] m, water coverage %.1f%%\n",
                out, kHmSize, kHmSize, mn, mx, 100.0 * double(water) / double(px.size()));

    // ---- landscape shell from the freshly written PNG (identical pipeline
    // to the hamlet baker: classify against the LATTICE geometry)
    vf::voxel::HeightMap hm;
    if (!hm.loadFromFile(out))
        return 1;
    vf::voxel::setSharedHeightmap(&hm);

    const int LAT = int(WORLD / VOXEL);
    std::vector<float> latH(size_t(LAT) * LAT);
    for (int iz = 0; iz < LAT; ++iz)
        for (int ix = 0; ix < LAT; ++ix)
            latH[size_t(iz) * LAT + size_t(ix)] =
                hm.sample(-0.5f * WORLD + (ix + 0.5f) * VOXEL,
                          -0.5f * WORLD + (iz + 0.5f) * VOXEL);
    auto latSlope = [&](float wx, float wz) {
        int ix = std::clamp(int((wx + 0.5f * WORLD) / VOXEL), 5, LAT - 6);
        int iz = std::clamp(int((wz + 0.5f * WORLD) / VOXEL), 5, LAT - 6);
        float gx = (latH[size_t(iz) * LAT + size_t(ix + 5)] -
                    latH[size_t(iz) * LAT + size_t(ix - 5)]) /
                   (10.0f * VOXEL);
        float gz = (latH[size_t(iz + 5) * LAT + size_t(ix)] -
                    latH[size_t(iz - 5) * LAT + size_t(ix)]) /
                   (10.0f * VOXEL);
        return std::sqrt(gx * gx + gz * gz);
    };

    // preserve a previous ai_edits layer (chat/MCP edits survive a regen)
    std::vector<vf::voxel::VoxelRecord> aiRecords;
    {
        std::string adir = worldOut;
        size_t aslash = adir.find_last_of("/\\");
        adir = aslash == std::string::npos ? std::string() : adir.substr(0, aslash + 1);
        vf::voxel::WorldFileData ai;
        if (vf::voxel::worldfile::read(adir + "ai_edits.vxw", ai) &&
            !ai.voxels.empty() && ai.meta.worldSize == WORLD &&
            ai.meta.voxelSize == VOXEL) {
            aiRecords = std::move(ai.voxels);
            std::printf("  ai_edits: %zu records kept as highest-priority layer\n",
                        aiRecords.size());
        }
    }

    vf::voxel::WorldFileData data;
    data.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };

    struct LayerOut {
        std::string file, role, name;
        std::vector<vf::voxel::VoxelRecord> voxels;
    };
    std::vector<LayerOut> layers;

    auto record = [&](vf::voxel::VoxelRecord v, uint8_t mat) {
        const glm::vec3& c = kPalette[std::min(int(mat), int(kPaletteN) - 1)];
        const glm::vec2& rr = kMaterialReflection[std::min(int(mat), int(kPaletteN) - 1)];
        v.r = uint8_t(c.r * 255.0f);
        v.g = uint8_t(c.g * 255.0f);
        v.b = uint8_t(c.b * 255.0f);
        v.a = 255;
        v.reflectivity = uint8_t(rr.x);
        v.roughness = uint8_t(rr.y);
        v.materialId = mat;
        return v;
    };

    constexpr float kBAND = 0.20f;
    const int N = int(WORLD / VOXEL);
    auto& landL = layers.emplace_back();
    landL.file = "landscape.vxw";
    landL.role = "landscape";
    landL.name = "landscape";
    landL.voxels.reserve(size_t(N) * 4);
    for (int iz = 0; iz < N; ++iz)
        for (int ix = 0; ix < N; ++ix) {
            float wx = -0.5f * WORLD + (ix + 0.5f) * VOXEL;
            float wz = -0.5f * WORLD + (iz + 0.5f) * VOXEL;
            float H = sharedHeightmap().sample(wx, wz);
            uint8_t tm = isleMat(wx, wz, H, latSlope(wx, wz));
            int yc = int((H + 0.5f * WORLD) / VOXEL);
            for (int iy = glm::max(yc - 3, 0); iy <= glm::min(yc + 3, N - 1); ++iy) {
                float wy = -0.5f * WORLD + (iy + 0.5f) * VOXEL;
                if (std::fabs(wy - H) > kBAND) continue;
                vf::voxel::VoxelRecord v;
                v.x = uint16_t(ix);
                v.y = uint16_t(iy);
                v.z = uint16_t(iz);
                landL.voxels.push_back(record(v, tm));
            }
        }

    if (!aiRecords.empty()) {
        LayerOut& aiL = *layers.insert(layers.begin(), LayerOut{});
        aiL.file = "ai_edits.vxw";
        aiL.role = "object";
        aiL.name = "ai_edits";
        aiL.voxels = aiRecords;
    }

    std::string dir = worldOut;
    {
        size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    }
    std::vector<vf::voxel::worldfile::WorldLayer> manifestLayers;
    size_t totalRecords = 0;
    for (const LayerOut& L : layers) {
        vf::voxel::worldfile::WorldLayer wl;
        wl.file = L.file;
        wl.role = L.role;
        wl.name = L.name;
        wl.rotDeg = 0.f;
        wl.enabled = true;
        wl.listed = true;
        if (L.file == "ai_edits.vxw") {
            totalRecords += L.voxels.size();
            manifestLayers.push_back(std::move(wl));
            continue; // NEVER write ai_edits back — the packer only consumes it
        }
        vf::voxel::WorldFileData ld;
        ld.meta = data.meta;
        ld.voxels = L.voxels;
        if (!vf::voxel::worldfile::write(dir + L.file, ld)) {
            std::fprintf(stderr, "failed to write %s\n", (dir + L.file).c_str());
            return 1;
        }
        totalRecords += L.voxels.size();
        manifestLayers.push_back(std::move(wl));
    }
    // preserve runtime-authored layers (vf_mcp / mesh tools) across regens
    {
        std::vector<vf::voxel::worldfile::WorldLayer> prev;
        if (vf::voxel::worldfile::loadManifest(dir + "world.json", prev)) {
            for (auto& pl : prev) {
                if (pl.role == "packed")
                    continue;
                bool known = false;
                for (auto& ml : manifestLayers)
                    if (ml.file == pl.file) {
                        known = true;
                        break;
                    }
                if (known)
                    continue;
                std::FILE* f = std::fopen((dir + pl.file).c_str(), "rb");
                if (!f)
                    continue;
                std::fclose(f);
                manifestLayers.push_back(pl);
            }
        }
    }
    if (!vf::voxel::worldfile::writeManifest(dir + "world.json", manifestLayers)) {
        std::fprintf(stderr, "failed to write %sworld.json\n", dir.c_str());
        return 1;
    }
    std::printf("isle bake done: %s, %zu total records, %zu manifest layers\n",
                dir.c_str(), totalRecords, manifestLayers.size());
    return 0;
}
