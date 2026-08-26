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
float riverZ(float x) { return 14.f * std::sin(x * 0.042f) + 5.f * std::sin(x * 0.113f + 1.7f); }
float riverW(float x) { return 2.6f + 0.8f * std::sin(x * 0.087f + 0.6f); }

float terrainHeightAt(float x, float z)
{
    float t = std::fabs(z - riverZ(x)) / riverW(x);

    // rolling ridged hills rising away from the river
    float bankRamp = smoothstepf(0.55f, 2.8f, t);
    float farRamp = smoothstepf(4.0f, 14.0f, t);
    float amp = 7.5f * bankRamp + 4.5f * farRamp;
    float ridge = 1.f - std::fabs(fbm2(x * 0.021f, z * 0.021f));
    float hills = ridge * ridge * ridge * 0.62f + fbm2(x * 0.06f, z * 0.06f) * 0.25f +
                  fbm2(x * 0.19f, z * 0.19f) * 0.13f;
    float floorH = glm::max(WATER_LEVEL + 0.55f + hills * amp, WATER_LEVEL + 0.35f);

    // channel bowl down to a level bed so the water plane stays consistent
    float bowl = 1.f - smoothstepf(0.30f, 0.95f, t);
    float bed = WATER_LEVEL - 2.1f + fbm2(x * 0.23f, z * 0.23f) * 0.12f;
    float h = glm::mix(floorH, bed, bowl);

    // building pad: flatten ground under the riverside house
    // (trees & rocks hug the natural ground via sampled bases - no pads needed)
    auto pad = [&](glm::vec2 c, float r0, float r1) {
        float dd = glm::length(glm::vec2(x, z) - c);
        h = glm::mix(kPadY, h, smoothstepf(r0, r1, dd));
    };
    pad(kHousePos, 4.6f, 7.5f);
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
    auto lattice = [&](int ix, int iy, int iz) {
        return glm::vec3(-0.5f * WORLD + (ix + 0.5f) * VOXEL,
                         -0.5f * WORLD + (iy + 0.5f) * VOXEL,
                         -0.5f * WORLD + (iz + 0.5f) * VOXEL);
    };
    auto record = [&](vf::voxel::VoxelRecord v, glm::vec3 p, uint8_t mat) {
        const glm::vec3& c = kPalette[mat];
        const glm::vec2& rr = kMaterialReflection[mat];
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
    // sweep an axis-aligned box of lattice cells, keeping the |d| <= kBAND shell
    auto sweep = [&](LayerOut& L, auto&& objFn, int ix0, int ix1, int iy0, int iy1,
                     int iz0, int iz1) {
        for (int iz = glm::max(iz0, 0); iz <= glm::min(iz1, N - 1); ++iz)
            for (int iy = glm::max(iy0, 0); iy <= glm::min(iy1, N - 1); ++iy)
                for (int ix = glm::max(ix0, 0); ix <= glm::min(ix1, N - 1); ++ix) {
                    glm::vec3 p = lattice(ix, iy, iz);
                    vf::voxel::ObjHit s = objFn(p);
                    if (std::fabs(s.d) > kBAND)
                        continue;
                    vf::voxel::VoxelRecord v;
                    v.x = uint16_t(ix);
                    v.y = uint16_t(iy);
                    v.z = uint16_t(iz);
                    L.voxels.push_back(record(v, p, s.mat));
                }
    };
    auto cellOf = [&](float w) { return int((w + 0.5f * WORLD) / VOXEL); };

    // -- object layers (evaluated alone so records attribute cleanly) ----------
    auto& houseL = layers.emplace_back();
    houseL.file = "house.vxw";
    houseL.role = "object";
    houseL.name = "house";
    houseL.pos = { kHousePos.x, kPadY, kHousePos.y };
    sweep(houseL, [](glm::vec3 p) { return vf::voxel::houseAt(p); },
          cellOf(kHousePos.x - 5.f), cellOf(kHousePos.x + 5.f),
          cellOf(kPadY - 1.f), cellOf(kPadY + 5.f),
          cellOf(kHousePos.y - 5.f), cellOf(kHousePos.y + 5.f));

    for (size_t t = 0; t < kTreeSpots.size(); ++t) {
        auto& L = layers.emplace_back();
        L.file = "tree" + std::to_string(t + 1) + ".vxw";
        L.role = "object";
        L.name = "tree" + std::to_string(t + 1);
        L.pos = { kTreeSpots[t].x, 0.f, kTreeSpots[t].y };
        const HeightMap& hm = sharedHeightmap();
        float gr = hm.sample(kTreeSpots[t].x, kTreeSpots[t].y);
        sweep(L,
              [&, t](glm::vec3 p) {
                  return vf::voxel::treeAt(p, kTreeSpots[t], gr);
              },
              cellOf(kTreeSpots[t].x - 2.3f), cellOf(kTreeSpots[t].x + 2.3f),
              cellOf(gr - 0.5f), cellOf(gr + 8.6f),
              cellOf(kTreeSpots[t].y - 2.3f), cellOf(kTreeSpots[t].y + 2.3f));
    }

    for (size_t r = 0; r < kRockSpots.size(); ++r) {
        auto& L = layers.emplace_back();
        L.file = "rock" + std::to_string(r + 1) + ".vxw";
        L.role = "object";
        L.name = "rock" + std::to_string(r + 1);
        L.pos = { kRockSpots[r].x, 0.f, kRockSpots[r].y };
        const HeightMap& hm = sharedHeightmap();
        glm::vec2 s = kRockSpots[r];
        float rad = kRockRadii[r];
        sweep(L,
              [&, r](glm::vec3 p) {
                  // half-buried boulder, mirroring rocksAt
                  float dx = p.x - s.x, dz = p.z - s.y;
                  if (dx * dx + dz * dz > (rad + 0.4f) * (rad + 0.4f))
                      return vf::voxel::ObjHit { 1e9f, 4u };
                  float cy = hm.sample(s.x, s.y) + rad * 0.30f;
                  float d = glm::length(glm::vec3(dx, p.y - cy, dz)) - rad;
                  return vf::voxel::ObjHit { d, uint8_t(r == 1 ? 5 : 4) };
              },
              cellOf(s.x - rad - 0.6f), cellOf(s.x + rad + 0.6f),
              cellOf(hm.sample(s.x, s.y) - rad), cellOf(hm.sample(s.x, s.y) + 2.f * rad),
              cellOf(s.y - rad - 0.6f), cellOf(s.y + rad + 0.6f));
    }

    auto& alpacaL = layers.emplace_back();
    alpacaL.file = "alpaca.vxw";
    alpacaL.role = "object";
    alpacaL.name = "alpaca";
    alpacaL.pos = { kAlpacaSpot.x, 0.f, kAlpacaSpot.y };
    {
        const HeightMap& hm = sharedHeightmap();
        float ga = hm.sample(kAlpacaSpot.x, kAlpacaSpot.y);
        alpacaL.pos.y = ga;
        sweep(alpacaL, [](glm::vec3 p) { return vf::voxel::alpacaAt(p); },
              cellOf(kAlpacaSpot.x - 1.25f), cellOf(kAlpacaSpot.x + 1.25f),
              cellOf(ga - 0.3f), cellOf(ga + 1.7f),
              cellOf(kAlpacaSpot.y - 1.25f), cellOf(kAlpacaSpot.y + 1.25f));
    }

    auto& fenceL = layers.emplace_back();
    fenceL.file = "fence1.vxw";
    fenceL.role = "object";
    fenceL.name = "fence1";
    fenceL.pos = { 0.5f * (kPaddockMin.x + kPaddockMax.x), 0.f,
                   0.5f * (kPaddockMin.y + kPaddockMax.y) };
    sweep(fenceL, [](glm::vec3 p) { return vf::voxel::fenceAt(p); },
          cellOf(kPaddockMin.x - 0.5f), cellOf(kPaddockMax.x + 0.5f),
          cellOf(-1.5f), cellOf(3.5f),
          cellOf(kPaddockMin.y - 0.5f), cellOf(kPaddockMax.y + 0.5f));

    auto& bushL = layers.emplace_back();
    bushL.file = "bushes.vxw";
    bushL.role = "scatter";
    bushL.name = "bushes";
    {
        // iterate bush CELLS directly (not the voxel lattice): mirrors the
        // hash placement in bushesAt so only real bushes get swept
        const HeightMap& hm = sharedHeightmap();
        const float cell = kBushCell;
        int nc = int(WORLD / cell) + 1;
        for (int cz = -1; cz <= nc; ++cz)
            for (int cx = -1; cx <= nc; ++cx) {
                float hCell = vf::voxel::hash2(float(cx) * 19.1f, float(cz) * 37.7f);
                if (hCell < 0.62f) continue;
                float jx = vf::voxel::hash2(float(cx) * 7.3f, float(cz) * 11.1f);
                float jz = vf::voxel::hash2(float(cx) * 13.7f, float(cz) * 17.3f);
                float hr = vf::voxel::hash2(float(cx) * 23.1f, float(cz) * 29.7f);
                glm::vec2 bc((cx + jx) * cell, (cz + jz) * cell);
                if (fabs(bc.x) > 0.5f * WORLD || fabs(bc.y) > 0.5f * WORLD) continue;
                float H = hm.sample(bc.x, bc.y);
                uint8_t mat = latMat(bc.x, bc.y, H);
                if (mat != 0 && mat != 1) continue;
                if (glm::length(hm.gradient(bc.x, bc.y)) > 0.9f) continue;
                float r = 0.35f + hr * 0.45f;
                glm::vec3 c(bc.x, H + r * 0.55f, bc.y);
                sweep(bushL,
                      [&](glm::vec3 p) {
                          return vf::voxel::ObjHit { glm::length(p - c) - r, mat };
                      },
                      cellOf(c.x - r - 0.3f), cellOf(c.x + r + 0.3f),
                      cellOf(c.y - r - 0.3f), cellOf(c.y + r + 1.2f),
                      cellOf(c.z - r - 0.3f), cellOf(c.z + r + 0.3f));
            }
    }

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
        // default manifest: terrain-only world. Content layers ship disabled
        // and are opted into via the GUI / enable_layer; ai_edits is enabled
        // exactly when it carries records. It is also first in the vector,
        // i.e. highest merge priority.
        vf::voxel::worldfile::WorldLayer wl;
        wl.file = L.file;
        wl.role = L.role;
        wl.name = L.name;
        wl.pos[0] = L.pos.x;
        wl.pos[1] = L.pos.y;
        wl.pos[2] = L.pos.z;
        wl.rotDeg = 0.f;
        wl.enabled = L.role == "landscape" || L.file == "ai_edits.vxw";
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
