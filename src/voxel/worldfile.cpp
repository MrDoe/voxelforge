#include "voxel/worldfile.hpp"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cstring>

namespace vf::voxel::worldfile {

namespace {

constexpr uint32_t kHeaderBytes = 64;

uint32_t crc32(const uint8_t* d, size_t n)
{
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ ~0u;
}

void putU32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}
void putU64(std::vector<uint8_t>& b, uint64_t v)
{
    putU32(b, uint32_t(v));
    putU32(b, uint32_t(v >> 32));
}
void putF32(std::vector<uint8_t>& b, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    putU32(b, u);
}

struct Reader {
    const uint8_t* p;
    size_t n, off = 0;
    bool get(void* dst, size_t bytes)
    {
        if (off + bytes > n)
            return false;
        std::memcpy(dst, p + off, bytes);
        off += bytes;
        return true;
    }
    template <typename T>
    bool pod(T& v)
    {
        return get(&v, sizeof(T));
    }
};

} // namespace

bool write(const std::string& path, const WorldFileData& d)
{
    std::vector<uint8_t> payload;
    auto words = [&](const auto& v) {
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(v.data()),
                       reinterpret_cast<const uint8_t*>(v.data() + v.size()));
    };
    // svo buffers in fixed order
    putU64(payload, d.chunkGrid.size());
    words(d.chunkGrid);
    putU64(payload, d.childBase.size());
    words(d.childBase);
    putU64(payload, d.payload.size());
    words(d.payload);
    putU64(payload, d.handles.size());
    words(d.handles);
    putU64(payload, d.bricks.size());
    words(d.bricks);
    // explicit voxel records
    putU64(payload, d.voxels.size());
    payload.reserve(payload.size() + d.voxels.size() * 16);
    for (const VoxelRecord& v : d.voxels) {
        payload.push_back(uint8_t(v.x));
        payload.push_back(uint8_t(v.x >> 8));
        payload.push_back(uint8_t(v.y));
        payload.push_back(uint8_t(v.y >> 8));
        payload.push_back(uint8_t(v.z));
        payload.push_back(uint8_t(v.z >> 8));
        payload.push_back(v.r);
        payload.push_back(v.g);
        payload.push_back(v.b);
        payload.push_back(v.a);
        payload.push_back(v.reflectivity);
        payload.push_back(v.roughness);
        payload.push_back(v.materialId);
        payload.push_back(v.reserved);
        payload.push_back(0);
        payload.push_back(0);
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        spdlog::error("worldfile: cannot write '{}'", path);
        return false;
    }
    uint8_t hdr[kHeaderBytes];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, kMagic, 4);
    auto u32at = [&](size_t o, uint32_t v) {
        hdr[o] = uint8_t(v);
        hdr[o + 1] = uint8_t(v >> 8);
        hdr[o + 2] = uint8_t(v >> 16);
        hdr[o + 3] = uint8_t(v >> 24);
    };
    u32at(4, kVersion);
    float fs[3] = { d.meta.worldSize, d.meta.voxelSize, d.meta.waterLevel };
    for (int i = 0; i < 3; ++i) {
        uint32_t u;
        std::memcpy(&u, &fs[i], 4);
        u32at(8 + 4 * i, u);
    }
    u32at(20, d.meta.gridN);
    u32at(24, d.meta.brickN);
    u32at(28, crc32(payload.data(), payload.size()));

    bool ok = std::fwrite(hdr, 1, kHeaderBytes, f) == kHeaderBytes &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok &= std::fclose(f) == 0;
    if (!ok)
        spdlog::error("worldfile: short write '{}'", path);
    return ok;
}

bool read(const std::string& path, WorldFileData& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long fileBytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileBytes < long(kHeaderBytes)) {
        std::fclose(f);
        return false;
    }
    const size_t total = size_t(fileBytes);
    std::vector<uint8_t> buf(total);
    bool rd = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!rd)
        return false;

    Reader r{ buf.data(), buf.size() };
    char magic[4];
    uint32_t version = 0;
    if (!r.get(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !r.pod(version) ||
        version != kVersion) {
        spdlog::error("worldfile: '{}' is not a VXW v1 file", path);
        return false;
    }
    Reader h{ buf.data(), kHeaderBytes, 8 };
    uint32_t fs[3];
    h.pod(fs[0]);
    h.pod(fs[1]);
    h.pod(fs[2]);
    std::memcpy(&out.meta.worldSize, &fs[0], 4);
    std::memcpy(&out.meta.voxelSize, &fs[1], 4);
    std::memcpy(&out.meta.waterLevel, &fs[2], 4);
    h.pod(out.meta.gridN);
    h.pod(out.meta.brickN);

    r.off = kHeaderBytes;
    uint32_t storedCrc = 0;
    std::memcpy(&storedCrc, buf.data() + 28, 4);
    if (crc32(buf.data() + kHeaderBytes, buf.size() - kHeaderBytes) != storedCrc) {
        spdlog::error("worldfile: '{}' failed CRC check", path);
        return false;
    }

    auto vec = [&](std::vector<uint32_t>& v) {
        uint64_t count = 0;
        if (!r.pod(count))
            return false;
        v.resize(size_t(count));
        return r.get(v.data(), size_t(count) * 4);
    };
    uint64_t gridCount = 0;
    if (!r.pod(gridCount)) {
        return false;
    }
    out.chunkGrid.resize(size_t(gridCount));
    if (!r.get(out.chunkGrid.data(), size_t(gridCount) * 4))
        return false;
    if (!(vec(out.childBase) && vec(out.payload) && vec(out.handles) && vec(out.bricks)))
        return false;
    uint64_t voxCount = 0;
    if (!r.pod(voxCount))
        return false;
    out.voxels.resize(size_t(voxCount));
    for (VoxelRecord& v : out.voxels) {
        uint16_t xyz[3];
        if (!r.get(xyz, 6))
            return false;
        v.x = xyz[0];
        v.y = xyz[1];
        v.z = xyz[2];
        if (!r.get(&v.r, 6))
            return false; // r g b a refl rough
        uint8_t tail[4];
        if (!r.get(tail, 4))
            return false; // mat reserved pad pad
        v.materialId = tail[0];
    }
    return true;
}

} // namespace vf::voxel::worldfile
