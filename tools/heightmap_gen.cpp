// Offline terrain asset generator: hills + meandering river valley.
// Writes the 16-bit heightmap PNG (stored deflate - no dependencies), then
// builds the full chunked-SVO world from it and serializes assets/world.vxw
// (VXW v1: GPU buffers with RGBA/reflection voxels + surface voxel records).
#include "voxel/common.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/world.hpp"
#include "voxel/worldfile.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

    vf::voxel::World world;
    world.build();
    auto st = world.stats();
    std::printf("world built: nodes=%zu bricks=%zu activeChunks=%zu in %.2fs\n",
                st.nodes, st.bricks, st.activeChunks, double(st.buildSeconds));

    // explicit surface-band voxel records across the whole world
    vf::voxel::WorldFileData data;
    data.meta = { WORLD, VOXEL, WATER_LEVEL, uint32_t(GRID_N), 8 };
    const auto& g = world.gpu();
    data.chunkGrid = g.chunkGrid;
    data.childBase = g.childBase;
    data.payload = g.payload;
    data.handles = g.handles;
    data.bricks = g.bricks;

    constexpr float kBAND = 0.20f;
    const int N = int(WORLD / VOXEL);
    data.voxels.reserve(6u << 20);
    for (int iz = 0; iz < N; ++iz) {
        float wz = -0.5f * WORLD + (iz + 0.5f) * VOXEL;
        for (int ix = 0; ix < N; ++ix) {
            float wx = -0.5f * WORLD + (ix + 0.5f) * VOXEL;
            float H = terrainHeightAt(wx, wz);
            int yc = int((H + 0.5f * WORLD) / VOXEL);
            int y0 = glm::clamp(yc - 3, 0, N - 1), y1 = glm::clamp(yc + 3, 0, N - 1);
            for (int iy = y0; iy <= y1; ++iy) {
                float wy = -0.5f * WORLD + (iy + 0.5f) * VOXEL;
                if (std::fabs(wy - H) > kBAND)
                    continue;
                uint8_t mat = materialAt(wx, wz, H);
                const glm::vec3& c = kPalette[mat];
                const glm::vec2& rr = kMaterialReflection[mat];
                vf::voxel::VoxelRecord v;
                v.x = uint16_t(ix);
                v.y = uint16_t(iy);
                v.z = uint16_t(iz);
                v.r = uint8_t(c.r * 255.0f);
                v.g = uint8_t(c.g * 255.0f);
                v.b = uint8_t(c.b * 255.0f);
                v.a = 255;
                v.reflectivity = uint8_t(rr.x);
                v.roughness = uint8_t(rr.y);
                v.materialId = mat;
                data.voxels.push_back(v);
            }
        }
    }
    if (!vf::voxel::worldfile::write(worldOut, data)) {
        std::fprintf(stderr, "failed to write %s\n", worldOut);
        return 1;
    }
    std::printf("world %s: %zu voxel records, svo buffers %zu words\n", worldOut,
                data.voxels.size(), data.bricks.size() + data.handles.size());
    return 0;
}
