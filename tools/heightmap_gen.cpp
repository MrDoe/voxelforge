// Offline terrain asset generator: hills + meandering river valley.
// Writes the 16-bit heightmap PNG (stored deflate - no dependencies), then
// sweeps the layer family (record-only .vxw files) + manifest from it. There
// is no merged cache: the app synthesizes its SVO directly from these layers.
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace vf::voxel;

namespace {

// ---- deterministic noise (mirrors common.hpp style) ------------------------
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

// ---- river ------------------------------------------------------------------
// A fresh, more strongly meandering channel with a wider, deeper bed.
float riverZ(float x)
{
    return 11.f * std::sin(x * 0.045f) + 6.f * std::sin(x * 0.121f + 1.7f) +
           2.5f * std::sin(x * 0.307f + 0.5f);
}
float riverW(float x) { return 3.2f + 1.0f * std::sin(x * 0.093f + 0.6f); }

float terrainHeightAt(float x, float z)
{
    float t = std::fabs(z - riverZ(x)) / riverW(x);

    // Fresh valley landform (rewritten): rolling foothill swell + ridged
    // fells + far mountain rampart, all flanking a clear carved channel.
    float landSwell = fbm2(x * 0.0085f + 11.3f, z * 0.0085f - 4.1f) * 6.0f;
    float bankRamp = smoothstepf(0.55f, 3.0f, t);
    float farRamp = smoothstepf(4.0f, 15.0f, t);
    float amp = (9.5f * bankRamp + 5.5f * farRamp) + landSwell * bankRamp;
    float ridge = 1.f - std::fabs(fbm2(x * 0.021f, z * 0.021f));
    float hills = ridge * ridge * ridge * 0.72f + fbm2(x * 0.06f, z * 0.06f) * 0.27f +
                  fbm2(x * 0.19f, z * 0.19f) * 0.15f;
    float floorH = glm::max(WATER_LEVEL + 0.65f + hills * amp, WATER_LEVEL + 0.40f);

    // channel bowl down to a wider, deeper level bed so the water plane stays
    // consistent and the river carves a clear valley.
    float bowl = 1.f - smoothstepf(0.34f, 1.05f, t);
    float bed = WATER_LEVEL - 2.7f + fbm2(x * 0.23f, z * 0.23f) * 0.14f;
    // riffle steps upstream (reference whitewater): three transverse gravel
    // bars raise the bed toward the surface where mid-stream boulders sit.
    {
        float riffle = 0.f;
        riffle += std::exp(-std::pow((x + 6.0f) / 1.6f, 2.0f));
        riffle += 0.8f * std::exp(-std::pow((x + 11.0f) / 1.9f, 2.0f));
        riffle += 0.7f * std::exp(-std::pow((x + 16.5f) / 2.1f, 2.0f));
        float inChannel = 1.f - smoothstepf(0.15f, 0.75f, t);
        bed += riffle * inChannel * 1.15f;
    }
    float h = glm::mix(floorH, bed, bowl);

    // distant mountain rampart (reference backdrop): radial rise with sharp
    // ridged peaks, kept out of the river corridor so the valley drains.
    {
        float r = std::sqrt(x * x + z * z);
        float mtnMask = smoothstepf(26.0f, 48.0f, r);
        if (mtnMask > 0.001f) {
            float mr = 1.f - std::fabs(fbm2(x * 0.016f + 3.7f, z * 0.016f - 1.2f));
            mr *= mr;
            float mtnH = (8.0f + 9.0f * mr + 3.0f * fbm2(x * 0.05f, z * 0.05f)) * mtnMask;
            mtnH *= smoothstepf(0.8f, 3.5f, t); // valley stays open
            float mtnTop = WATER_LEVEL + 0.65f + mtnH;
            h = glm::max(h, glm::mix(h, mtnTop, mtnMask));
        }
    }

    // micro relief: hummocks + tussocks on land only (never in the channel).
    {
        float landMask = smoothstepf(0.6f, 1.5f, t) * (1.f - bowl);
        if (landMask > 0.001f) {
            h += (fbm2(x * 0.85f, z * 0.85f) * 0.10f +
                  fbm2(x * 2.3f + 7.0f, z * 2.3f - 3.0f) * 0.04f) *
                 landMask;
        }
    }

    // building pad: flatten ground under the riverside house (covers the
    // cabin + porch + woodpile, ~5 m; feather to 7.2 m). Trees & rocks hug
    // the natural ground via sampled bases - no pads needed.
    auto pad = [&](glm::vec2 c, float r0, float r1) {
        float dd = glm::length(glm::vec2(x, z) - c);
        h = glm::mix(kPadY, h, smoothstepf(r0, r1, dd));
    };
    pad(kHousePos, 5.0f, 7.2f);
    // pond cove: a still inlet bay reaching toward the porch steps so the
    // waterfront (dock + canoe + pebble shore) sits steps from the cabin,
    // like the reference. Dug after the pad so it cuts through the apron.
    {
        float pdx = (x - 3.6f) / 2.6f, pdz = (z - 6.6f) / 2.0f;
        float pd = std::sqrt(pdx * pdx + pdz * pdz);
        float pondBed = WATER_LEVEL - 1.9f + fbm2(x * 0.23f, z * 0.23f) * 0.14f;
        h = glm::mix(pondBed, h, smoothstepf(0.55f, 1.0f, pd));
    }
    return glm::clamp(h, kHmMinMeters + 0.05f, kHmMaxMeters - 0.05f);
}

// ---- minimal PNG writer (16-bit gray, zlib stored blocks) --------------------
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
    // CRC over type + data
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
    ihdr[8] = 16;  // bit depth
    ihdr[9] = 0;   // grayscale
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    writeChunk(f, "IHDR", ihdr, 13);

    // raw scanlines: filter byte 0 + big-endian samples
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

    // zlib stream with stored deflate blocks
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
    const char* out = argc > 1 ? argv[1] : "assets/heightmap.png";
    const char* worldOut = argc > 2 ? argv[2] : "assets/world.vxw";
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

    // ---- build the full voxel world from the freshly generated heightmap ----
    vf::voxel::HeightMap hm;
    if (!hm.loadFromFile(out))
        return 1;
    vf::voxel::setSharedHeightmap(&hm);

    // lattice-height grid: the runtime renders terrain from these columns, so
    // materials must be classified against THIS geometry (smoothed, 10 cm
    // steps) rather than the raw 5 cm PNG - otherwise slopes that look grassy
    // in-engine get baked as rock.
    const int LAT = int(vf::voxel::WORLD / vf::voxel::VOXEL);
    std::vector<float> latH(size_t(LAT) * LAT);
    for (int iz = 0; iz < LAT; ++iz)
        for (int ix = 0; ix < LAT; ++ix)
            latH[size_t(iz) * LAT + size_t(ix)] =
                hm.sample(-0.5f * vf::voxel::WORLD + (ix + 0.5f) * vf::voxel::VOXEL,
                          -0.5f * vf::voxel::WORLD + (iz + 0.5f) * vf::voxel::VOXEL);
    auto latSlope = [&](float wx, float wz) {
        int ix = std::clamp(int((wx + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL), 5, LAT - 6);
        int iz = std::clamp(int((wz + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL), 5, LAT - 6);
        float gx = (latH[size_t(iz) * LAT + size_t(ix + 5)] -
                    latH[size_t(iz) * LAT + size_t(ix - 5)]) /
                   (10.0f * vf::voxel::VOXEL);
        float gz = (latH[size_t(iz + 5) * LAT + size_t(ix)] -
                    latH[size_t(iz - 5) * LAT + size_t(ix)]) /
                   (10.0f * vf::voxel::VOXEL);
        return std::sqrt(gx * gx + gz * gz);
    };
    auto latMat = [&](float wx, float wz, float H) {
        float wd = vf::voxel::WATER_LEVEL - H;
        float n = vf::voxel::fbm2(wx * 0.35f, wz * 0.35f);
        return vf::voxel::materialFromBands(wd, latSlope(wx, wz), n);
    };

    // pre-existing AI/user edits join the layer family so they are part of
    // the world like any other geometry (also registered into scene truth
    // above for probes)
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

    // ---- layered world output -------------------------------------------------
    // The world is described by a family of record-only .vxw layers plus a
    // JSON manifest. No merged cache is produced: the renderer synthesizes
    // its SVO from these layers directly.
    vf::voxel::WorldFileData data;
    data.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };

    struct LayerOut {
        std::string file, role, name;
        glm::vec3 pos { 0.f, 0.f, 0.f };
        std::vector<vf::voxel::VoxelRecord> voxels;
    };
    std::vector<LayerOut> layers;

    constexpr float kBAND = 0.20f;
    const int N = int(WORLD / VOXEL);
    auto record = [&](vf::voxel::VoxelRecord v, glm::vec3 p, uint8_t mat) {
        const glm::vec3& c = kPalette[std::min(int(mat),16)];
        const glm::vec2& rr = kMaterialReflection[std::min(int(mat),16)];
        v.r = uint8_t(c.r * 255.0f);
        v.g = uint8_t(c.g * 255.0f);
        v.b = uint8_t(c.b * 255.0f);
        v.a = 255;
        v.reflectivity = uint8_t(rr.x);
        v.roughness = uint8_t(rr.y);
        v.materialId = mat;
        (void)p;
        return v;
    };
    // (Object layers are runtime-authored now: hamlet_*.vxw via vf_mcp;
    // the baker only emits the terrain shell + preserved ai_edits.)

    // -- landscape layer: terrain-only shell over the whole valley -------------
    auto& landL = layers.emplace_back();
    landL.file = "landscape.vxw";
    landL.role = "landscape";
    landL.name = "landscape";
    for (int iz = 0; iz < N; ++iz)
        for (int ix = 0; ix < N; ++ix) {
            float wx = -0.5f * WORLD + (ix + 0.5f) * VOXEL;
            float wz = -0.5f * WORLD + (iz + 0.5f) * VOXEL;
            float H = sharedHeightmap().sample(wx, wz);
            uint8_t tm = latMat(wx, wz, H);
            int yc = int((H + 0.5f * WORLD) / VOXEL);
            for (int iy = glm::max(yc - 3, 0); iy <= glm::min(yc + 3, N - 1); ++iy) {
                float wy = -0.5f * WORLD + (iy + 0.5f) * VOXEL;
                if (std::fabs(wy - H) > kBAND) continue;
                vf::voxel::VoxelRecord v;
                v.x = uint16_t(ix);
                v.y = uint16_t(iy);
                v.z = uint16_t(iz);
                landL.voxels.push_back(record(v, glm::vec3(wx, wy, wz), tm));
            }
        }

    // -- ai_edits.vxw: user/AI edits are the highest-priority layer ------------
    // Already registered into scene truth above; here they join the layer
    // family (first = highest dedupe priority).
    if (!aiRecords.empty()) {
        LayerOut& aiL = *layers.insert(layers.begin(), LayerOut{});
        aiL.file = "ai_edits.vxw";
        aiL.role = "object";
        aiL.name = "ai_edits";
        aiL.voxels = aiRecords;
        std::printf("  merged %s: %zu records (highest priority)\n",
                    aiL.file.c_str(), aiL.voxels.size());
    }

    // -- write layer files + manifest ------------------------------------------
    std::string dir = worldOut;
    {
        size_t slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    }
    std::vector<vf::voxel::worldfile::WorldLayer> manifestLayers;
    manifestLayers.reserve(layers.size());
    size_t totalRecords = 0;
    for (const LayerOut& L : layers) {
        // The bake now owns only the terrain shell (+ preserved ai_edits);
        // runtime-authored layers (vf_mcp write_object/add_*: hamlet_*.vxw)
        // are appended below so a regen never drops the authored scene.
        vf::voxel::worldfile::WorldLayer wl;
        wl.file = L.file;
        wl.role = L.role;
        wl.name = L.name;
        wl.pos[0] = L.pos.x;
        wl.pos[1] = L.pos.y;
        wl.pos[2] = L.pos.z;
        wl.rotDeg = 0.f;
        wl.enabled = true;
        wl.listed = true;
        // NEVER write ai_edits.vxw back: the packer only consumes it. Writing
        // would stomp edits appended while this bake was running.
        if (L.file == "ai_edits.vxw") {
            totalRecords += L.voxels.size();
            manifestLayers.push_back(std::move(wl));
            continue;
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
    // preserve runtime-authored layers: append entries for .vxw files that
    // this bake does not own (present in the current manifest + on disk), so
    // re-running the baker keeps the authored object scene
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

    std::printf("layers written to %s: %zu total records\n", dir.c_str(),
                totalRecords);
    for (const LayerOut& L : layers)
        std::printf("  layer %-14s %8zu records (%s)\n", L.file.c_str(),
                    L.voxels.size(), L.role.c_str());
    std::printf("manifest %sworld.json: %zu layers, %zu total records\n", dir.c_str(),
                manifestLayers.size(), totalRecords);
    return 0;
}
