#define GLM_ENABLE_EXPERIMENTAL
#include "voxel/worldfile.hpp"
#include "voxel/common.hpp"
#include <glm/gtx/quaternion.hpp>
#include <spdlog/spdlog.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_set>

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

namespace {

void putRecords(std::vector<uint8_t>& payload, const std::vector<VoxelRecord>& voxels)
{
    putU64(payload, voxels.size());
    payload.reserve(payload.size() + voxels.size() * 16);
    for (const VoxelRecord& v : voxels) {
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
}

void putLegacySvo(std::vector<uint8_t>& payload, const WorldFileData& d)
{
    auto words = [&](const auto& v) {
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(v.data()),
                       reinterpret_cast<const uint8_t*>(v.data() + v.size()));
    };
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
}

bool readRecords(const uint8_t* p, size_t n, std::vector<VoxelRecord>& out)
{
    Reader r { p, n };
    uint64_t count = 0;
    if (!r.pod(count) || count > n / sizeof(VoxelRecord))
        return false;
    out.resize(size_t(count));
    for (VoxelRecord& v : out) {
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
        v.reserved = tail[1]; // per-cell texture override (phase 2)
    }
    return true;
}

bool readLegacySvo(Reader& r, WorldFileData& out)
{
    auto vec = [&](std::vector<uint32_t>& v) {
        uint64_t count = 0;
        if (!r.pod(count) || count > uint64_t(r.n - r.off) / 4)
            return false;
        v.resize(size_t(count));
        return r.get(v.data(), size_t(count) * 4);
    };
    uint64_t gridCount = 0;
    if (!r.pod(gridCount) || gridCount > uint64_t(r.n - r.off) / 4)
        return false;
    out.chunkGrid.resize(size_t(gridCount));
    if (!r.get(out.chunkGrid.data(), size_t(gridCount) * 4))
        return false;
    return vec(out.childBase) && vec(out.payload) && vec(out.handles) &&
           vec(out.bricks);
}

} // namespace

bool write(const std::string& path, const WorldFileData& d)
{
    std::vector<uint8_t> payload;
    const bool v2 = !d.sections.empty();
    if (!v2) {
        putLegacySvo(payload, d);
        putRecords(payload, d.voxels);
    } else {
        // tagged sections: legacy SVO + records + caller extras (e.g. the
        // ChunkStore live-edit overlay)
        putU32(payload, uint32_t(d.sections.size()) + 2);
        std::vector<uint8_t> sec;
        putLegacySvo(sec, d);
        putU32(payload, worldfile::kSectionLegacySvo);
        putU64(payload, sec.size());
        payload.insert(payload.end(), sec.begin(), sec.end());
        sec.clear();
        putRecords(sec, d.voxels);
        putU32(payload, worldfile::kSectionRecords);
        putU64(payload, sec.size());
        payload.insert(payload.end(), sec.begin(), sec.end());
        for (const Section& s : d.sections) {
            putU32(payload, s.type);
            putU64(payload, s.data.size());
            payload.insert(payload.end(), s.data.begin(), s.data.end());
        }
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
    u32at(4, v2 ? worldfile::kVersion2 : worldfile::kVersion);
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

    Reader r { buf.data(), buf.size() };
    char magic[4];
    uint32_t version = 0;
    if (!r.get(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !r.pod(version) ||
        (version != kVersion && version != kVersion2)) {
        spdlog::error("worldfile: '{}' is not a VXW v1/v2 file", path);
        return false;
    }
    Reader h { buf.data(), kHeaderBytes, 8 };
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

    if (version == kVersion) {
        if (!readLegacySvo(r, out))
            return false;
        uint64_t voxCount = 0;
        if (!r.pod(voxCount) || voxCount > uint64_t(r.n - r.off) / sizeof(VoxelRecord))
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
            v.reserved = tail[1]; // per-cell texture override (phase 2)
        }
        return true;
    }

    // ---- v2: tagged-section payload --------------------------------------
    uint32_t sectionCount = 0;
    if (!r.pod(sectionCount) || sectionCount > 256)
        return false;
    for (uint32_t s = 0; s < sectionCount; ++s) {
        uint32_t type = 0;
        uint64_t len = 0;
        if (!r.pod(type) || !r.pod(len) || len > uint64_t(r.n - r.off))
            return false;
        const uint8_t* base = r.p + r.off;
        if (type == kSectionRecords) {
            if (!readRecords(base, size_t(len), out.voxels))
                return false;
        } else if (type == kSectionLegacySvo) {
            Reader sr { base, size_t(len) };
            if (!readLegacySvo(sr, out))
                return false;
        } else {
            // opaque (e.g. the ChunkStore overlay): keep verbatim so a
            // read-modify-write round trip preserves it
            out.sections.push_back(
                { type, std::vector<uint8_t>(base, base + size_t(len)) });
        }
        r.off += size_t(len);
    }
    return true;
}

// --- manifest (assets/world.json) -------------------------------------------
// Minimal JSON reader: just enough for the fixed-shape world manifest.
// Supported values: objects, arrays, strings, numbers, true/false. Comments
// (// to end of line) are tolerated so humans can annotate the file.
namespace {

struct Json {
    const std::string& s;
    size_t i = 0;

    void skipWs()
    {
        while (i < s.size()) {
            if (std::isspace(unsigned(s[i]))) {
                ++i;
            } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else {
                break;
            }
        }
    }
    bool eat(char c)
    {
        skipWs();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool peek(char c)
    {
        skipWs();
        return i < s.size() && s[i] == c;
    }
    bool str(std::string& out)
    {
        skipWs();
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size())
                ++i; // naive escape: take next char literally
            out.push_back(s[i++]);
        }
        return i < s.size() && s[i++] == '"';
    }
    bool num(float& v)
    {
        skipWs();
        char* end = nullptr;
        const char* begin = s.c_str() + i;
        v = std::strtof(begin, &end);
        if (end == begin)
            return false;
        i += size_t(end - begin);
        return true;
    }
    bool boolean(bool& v)
    {
        skipWs();
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            v = true;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            v = false;
            return true;
        }
        return false;
    }
    // consume any value (string / number / bool / array / object) without
    // interpreting it - keeps unknown keys from derailing the parse
    bool skipValue()
    {
        skipWs();
        if (i >= s.size())
            return false;
        char c = s[i];
        std::string sinkStr;
        float sinkNum = 0.f;
        bool sinkBool = false;
        if (c == '"')
            return str(sinkStr);
        if (c == 't' || c == 'f')
            return boolean(sinkBool);
        if (c == '[' || c == '{') {
            char open = c, close = c == '[' ? ']' : '}';
            int depth = 0;
            while (i < s.size()) {
                char d = s[i++];
                if (d == open)
                    ++depth;
                else if (d == close && --depth == 0)
                    return true;
            }
            return false;
        }
        return num(sinkNum);
    }
};

} // namespace

bool loadManifest(const std::string& path, std::vector<WorldLayer>& out)
{
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    Json j{ text };
    if (!j.eat('{'))
        return false;
    // top-level: { "layers": [ ... ] } - other keys ignored
    bool foundLayers = false;
    while (!j.peek('}') && j.i < text.size()) {
        std::string key;
        if (!j.str(key))
            return false;
        if (!j.eat(':'))
            return false;
        if (key == "layers" && j.eat('[')) {
            foundLayers = true;
            while (!j.peek(']') && j.i < text.size()) {
                WorldLayer layer;
                if (!j.eat('{'))
                    return false;
                while (!j.peek('}') && j.i < text.size()) {
                    std::string k;
                    if (!j.str(k) || !j.eat(':'))
                        return false;
                    if (k == "file")
                        j.str(layer.file);
                    else if (k == "role")
                        j.str(layer.role);
                    else if (k == "name")
                        j.str(layer.name);
                    else if (k == "rot")
                        j.num(layer.rotDeg);
                    else if (k == "rotX")
                        j.num(layer.rotX);
                    else if (k == "rotZ")
                        j.num(layer.rotZ);
                    else if (k == "enabled")
                        j.boolean(layer.enabled);
                    else if (k == "pos") {
                        if (!j.eat('['))
                            return false;
                        for (int c = 0; c < 3; ++c) {
                            if (!j.num(layer.pos[c]))
                                return false;
                            if (c < 2)
                                j.eat(','); // separators between components
                        }
                        if (!j.eat(']'))
                            return false;
                    } else {
                        j.skipValue(); // unsupported key - skip robustly
                    }
                    if (!j.eat(','))
                        break;
                }
                j.eat('}');
                if (!layer.file.empty())
                    out.push_back(std::move(layer));
                if (!j.eat(','))
                    break;
            }
            j.eat(']');
        } else {
            j.skipValue(); // unsupported top-level key
        }
        if (!j.eat(','))
            break;
    }
    j.eat('}');
    return foundLayers && !out.empty();
}

// Optional "textures" table from the same manifest (see loadTextureManifest).
// Shares the Json helper above; unknown keys are skipped so the table can grow.
bool loadTextureManifest(const std::string& path, std::vector<TextureBinding>& out)
{
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    Json j{ text };
    if (!j.eat('{'))
        return false;
    while (!j.peek('}') && j.i < text.size()) {
        std::string key;
        if (!j.str(key) || !j.eat(':'))
            return false;
        if (key == "textures" && j.eat('[')) {
            while (!j.peek(']') && j.i < text.size()) {
                TextureBinding b;
                if (!j.eat('{'))
                    return false;
                while (!j.peek('}') && j.i < text.size()) {
                    std::string k;
                    if (!j.str(k) || !j.eat(':'))
                        return false;
                    if (k == "file")
                        j.str(b.file);
                    else if (k == "mat") {
                        float v = -1.f;
                        j.num(v);
                        b.mat = int(v);
                    } else if (k == "scale") {
                        j.num(b.scale);
                    } else {
                        j.skipValue();
                    }
                    if (!j.eat(','))
                        break;
                }
                j.eat('}');
                if (!b.file.empty() && b.mat >= 0 && b.mat < int(kPaletteN))
                    out.push_back(std::move(b));
                if (!j.eat(','))
                    break;
            }
            j.eat(']');
        } else {
            j.skipValue();
        }
        if (!j.eat(','))
            break;
    }
    return true;
}

// One top-level "key": <value> pair of a manifest, value text verbatim.
// Scanning is balanced-brace aware, so nested objects/arrays (and the odd
// comment or trailing comma inside them) survive a rewrite untouched.
struct TopPair {
    std::string key;
    std::string value;
};

static void scanTopLevel(const std::string& text, std::vector<TopPair>& out)
{
    size_t i = 0;
    while (i < text.size() && text[i] != '{')
        ++i;
    if (i == text.size())
        return;
    ++i; // past the top-level '{'
    for (;;) {
        while (i < text.size() && text[i] != '"' && text[i] != '}')
            ++i;
        if (i >= text.size() || text[i] == '}')
            return;
        std::string key;
        size_t k0 = i + 1;
        for (size_t j = k0; j < text.size(); ++j) {
            if (text[j] == '\\') {
                ++j;
                continue;
            }
            if (text[j] == '"') {
                key = text.substr(k0, j - k0);
                i = j + 1;
                break;
            }
        }
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        if (i >= text.size() || text[i] != ':')
            return; // malformed; stop scanning
        ++i;
        size_t v0 = i;
        int depth = 0;
        while (i < text.size()) {
            char c = text[i];
            if (c == '"') {
                ++i;
                while (i < text.size()) {
                    if (text[i] == '\\')
                        ++i;
                    else if (text[i] == '"')
                        break;
                    ++i;
                }
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0)
                    break; // the manifest's closing brace
                --depth;
                if (depth == 0) {
                    ++i;
                    break;
                }
            } else if (depth == 0 && c == ',') {
                break;
            }
            ++i;
        }
        std::string raw = text.substr(v0, i - v0);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' ||
                                raw.back() == '\n' || raw.back() == '\r'))
            raw.pop_back();
        if (!key.empty())
            out.push_back({ key, raw });
    }
}

static std::string readFileText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// Re-emit every scanned pair verbatim except the ones the caller replaces.
// `first` tracks whether the caller has already written a member, so the
// separators stay valid whichever keys are skipped.
static void emitPreserved(std::FILE* f, const std::vector<TopPair>& pairs,
                          const char* skipA, const char* skipB, bool& first)
{
    for (const TopPair& p : pairs) {
        if (p.key == skipA || (skipB && p.key == skipB))
            continue;
        std::fprintf(f, "%s\n  \"%s\":%s", first ? "" : ",",
                     p.key.c_str(), p.value.c_str());
        first = false;
    }
}

bool writeTextureManifest(const std::string& path,
                          const std::vector<TextureBinding>& textures)
{
    std::vector<TopPair> pairs;
    scanTopLevel(readFileText(path), pairs);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "{");
    bool first = true;
    emitPreserved(f, pairs, "textures", nullptr, first);
    const char* sep = first ? "" : ",";
    if (textures.empty()) {
        std::fprintf(f, "%s\n  \"textures\": []\n}\n", sep);
    } else {
        std::fprintf(f, "%s\n  \"textures\": [\n", sep);
        for (size_t i = 0; i < textures.size(); ++i) {
            const TextureBinding& b = textures[i];
            std::fprintf(f,
                         "    { \"file\": \"%s\", \"mat\": %d, \"scale\": %.2f }%s\n",
                         b.file.c_str(), b.mat, b.scale,
                         i + 1 < textures.size() ? "," : "");
        }
        std::fprintf(f, "  ]\n}\n");
    }
    return std::fclose(f) == 0;
}

bool writeManifest(const std::string& path, const std::vector<WorldLayer>& layers)
{
    const std::string prev = readFileText(path);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "{\n  \"version\": 1,\n  \"layers\": [\n");
    for (size_t i = 0; i < layers.size(); ++i) {
        const WorldLayer& l = layers[i];
        std::fprintf(f,
                     "    { \"file\": \"%s\", \"role\": \"%s\", \"name\": \"%s\", "
                     "\"pos\": [%.2f, %.2f, %.2f], \"rot\": %.1f, \"rotX\": %.1f, "
                     "\"rotZ\": %.1f, \"enabled\": %s }%s\n",
                     l.file.c_str(), l.role.c_str(), l.name.c_str(), l.pos[0], l.pos[1],
                     l.pos[2], l.rotDeg, l.rotX, l.rotZ,
                     l.enabled ? "true" : "false",
                     i + 1 < layers.size() ? "," : "");
    }
    std::fprintf(f, "  ]");
    if (!prev.empty()) {
        std::vector<TopPair> pairs;
        scanTopLevel(prev, pairs);
        bool first = false; // version + layers are already written
        emitPreserved(f, pairs, "version", "layers", first);
    }
    std::fprintf(f, "\n}\n");
    return std::fclose(f) == 0;
}

bool readLayered(const std::string& manifestPath, const WorldFileMeta& expected,
                 std::vector<VoxelRecord>& out)
{
    std::vector<WorldLayer> layers;
    if (!loadManifest(manifestPath, layers))
        return false;
    std::string dir = manifestPath;
    size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);

    out.clear();
    std::unordered_set<uint32_t> claimed; // x<<20 | y<<10 | z of first claimant
    for (const WorldLayer& l : layers) {
        if (l.role == "packed")
            continue; // merged cache - renderers read the live layers instead
        if (!l.enabled)
            continue; // GUI-excluded: file stays on disk, out of merges
        WorldFileData data;
        if (!read(dir + l.file, data))
            return false;
        if (data.meta.worldSize != expected.worldSize ||
            data.meta.voxelSize != expected.voxelSize ||
            data.meta.gridN != expected.gridN) {
            spdlog::warn("worldfile: layer '{}' meta mismatch (rejected)", l.file);
            return false;
        }
        const std::vector<VoxelRecord>* vox = &data.voxels;
        std::vector<VoxelRecord> placed;
        if ((l.role == "object" || l.role == "scatter") &&
            (l.pos[0] != 0.f || l.pos[1] != 0.f || l.pos[2] != 0.f ||
             l.rotDeg != 0.f || l.rotX != 0.f || l.rotZ != 0.f)) {
            transformRecords(data.voxels, expected,
                             glm::vec3(l.pos[0], l.pos[1], l.pos[2]),
                             l.rotDeg, l.rotX, l.rotZ, placed);
            vox = &placed;
        }
        for (const VoxelRecord& v : *vox) {
            uint32_t key = (uint32_t(v.x) << 20) | (uint32_t(v.y) << 10) | uint32_t(v.z);
            if (claimed.insert(key).second)
                out.push_back(v);
        }
    }
    return true;
}

glm::mat3 placementRotation(float yawDeg, float pitchDeg, float rollDeg)
{
    return glm::mat3(glm::rotate(glm::mat4(1.f), glm::radians(yawDeg),
                                 glm::vec3(0.f, 1.f, 0.f))) *
           glm::mat3(glm::rotate(glm::mat4(1.f), glm::radians(pitchDeg),
                                 glm::vec3(1.f, 0.f, 0.f))) *
           glm::mat3(glm::rotate(glm::mat4(1.f), glm::radians(rollDeg),
                                 glm::vec3(0.f, 0.f, 1.f)));
}

glm::vec3 placementEuler(const glm::mat3& rotation)
{
    // Inverse of placementRotation() for R = Ry(yaw) Rx(pitch) Rz(roll).
    // GLM's mat3 indexing is column-major: [column][row].
    const float sp = std::clamp(-rotation[2][1], -1.0f, 1.0f);
    const float pitch = std::asin(sp);
    const float cp = std::cos(pitch);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (std::abs(cp) > 1e-5f) {
        roll = std::atan2(rotation[0][1], rotation[1][1]);
        yaw = std::atan2(rotation[2][0], rotation[2][2]);
    } else {
        // At the gimbal-lock pole choose roll = 0 and fold the remaining
        // rotation into yaw; any equivalent Euler triple is valid there.
        roll = 0.0f;
        yaw = std::atan2(-rotation[2][0], rotation[0][0]);
    }
    return glm::degrees(glm::vec3(yaw, pitch, roll));
}

glm::mat3 rotatePlacementLocal(const glm::mat3& current,
                               PlacementAxis axis, float degrees)
{
    const glm::vec3 v = axis == PlacementAxis::X   ? glm::vec3(1, 0, 0)
                      : axis == PlacementAxis::Y   ? glm::vec3(0, 1, 0)
                                                   : glm::vec3(0, 0, 1);
    const glm::mat3 local = glm::mat3(glm::rotate(
        glm::mat4(1.0f), glm::radians(degrees), v));
    return current * local;
}

glm::mat3 relativePlacementRotation(float oldYawDeg, float oldPitchDeg,
                                    float oldRollDeg,
                                    float newYawDeg, float newPitchDeg,
                                    float newRollDeg)
{
    const glm::mat3 oldR = placementRotation(oldYawDeg, oldPitchDeg, oldRollDeg);
    const glm::mat3 newR = placementRotation(newYawDeg, newPitchDeg, newRollDeg);
    return newR * glm::transpose(oldR);
}

bool recordBottomCenter(const std::vector<VoxelRecord>& src,
                        const WorldFileMeta& meta, glm::vec3& out)
{
    if (src.empty())
        return false;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const VoxelRecord& v : src) {
        const glm::vec3 p = v.position(meta);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    out = glm::vec3(0.5f * (lo.x + hi.x), lo.y, 0.5f * (lo.z + hi.z));
    return true;
}

void transformRecords(const std::vector<VoxelRecord>& src,
                      const WorldFileMeta& meta,
                      const glm::vec3& offset,
                      float rotDeg, float rotX, float rotZ,
                      std::vector<VoxelRecord>& out)
{
    out.clear();
    if (src.empty())
        return;
    const float vox = meta.voxelSize;
    const float half = 0.5f * meta.worldSize;
    // the lattice extent: gridN is the CHUNK grid, the record lattice spans
    // worldSize/voxelSize cells (e.g. 1024 at VOXEL=0.1 over WORLD=102.4)
    const int grid = int(std::lround(meta.worldSize / meta.voxelSize));
    const auto toWorld = [&](const VoxelRecord& v) -> glm::vec3 {
        return { -half + (float(v.x) + 0.5f) * vox,
                 -half + (float(v.y) + 0.5f) * vox,
                 -half + (float(v.z) + 0.5f) * vox };
    };
    const auto toCell = [&](const glm::vec3& p) -> glm::ivec3 {
        return { int(std::floor((p.x + half) / vox)),
                 int(std::floor((p.y + half) / vox)),
                 int(std::floor((p.z + half) / vox)) };
    };
    const auto inBounds = [&](const glm::ivec3& c) -> bool {
        return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < grid && c.y < grid && c.z < grid;
    };
    const auto keyOf = [](uint16_t x, uint16_t y, uint16_t z) -> uint32_t {
        return (uint32_t(x) << 20) | (uint32_t(y) << 10) | uint32_t(z);
    };
    const bool noRot = rotDeg == 0.f && rotX == 0.f && rotZ == 0.f;
    const bool noOff = offset.x == 0.f && offset.y == 0.f && offset.z == 0.f;
    if (noRot && noOff) {
        out = src; // identity: byte-identical to the unplaced merge
        return;
    }
    if (noRot) {
        // pure translation: the offset is an exact cell delta (toCell/toWorld
        // are exact inverses on a record's centre)
        const glm::ivec3 d = toCell(toWorld(src.front()) + offset) -
                             glm::ivec3(src.front().x, src.front().y, src.front().z);
        out.reserve(src.size());
        for (const VoxelRecord& v : src) {
            const glm::ivec3 c(int(v.x) + d.x, int(v.y) + d.y, int(v.z) + d.z);
            if (!inBounds(c))
                continue;
            VoxelRecord t = v;
            t.x = uint16_t(c.x);
            t.y = uint16_t(c.y);
            t.z = uint16_t(c.z);
            out.push_back(t);
        }
        return;
    }
    // rotation: destination-driven inverse map over the rotated AABB.
    // Every destination cell queries its pre-image, so the rotated shell
    // covers its whole footprint - holes can only come from the source
    // itself - and a 90-degree step (pivot at a cell centre or edge) maps
    // cell centres to cell centres exactly.
    std::unordered_map<uint32_t, const VoxelRecord*> srcCell;
    srcCell.reserve(src.size() * 2);
    for (const VoxelRecord& v : src)
        srcCell.emplace(keyOf(v.x, v.y, v.z), &v);
    glm::vec3 pivot;
    if (!recordBottomCenter(src, meta, pivot))
        return;
    // Composite Euler rotation about that canonical pivot. The destination-
    // driven loop queries each cell through R^-1, so the rotated shell covers
    // its whole footprint (holes can only come from the source itself).
    const glm::mat3 R = placementRotation(rotDeg, rotX, rotZ);
    const glm::mat3 Rinv = glm::transpose(R);
    const auto fwd = [&](const glm::vec3& p) -> glm::vec3 {
        return pivot + R * (p - pivot) + offset;
    };
    const auto inv = [&](const glm::vec3& q) -> glm::vec3 {
        return pivot + Rinv * (q - offset - pivot);
    };
    glm::vec3 dmn(1e9f), dmx(-1e9f);
    for (const VoxelRecord& v : src) {
        const glm::vec3 q = fwd(v.position(meta));
        dmn = glm::min(dmn, q);
        dmx = glm::max(dmx, q);
    }
    const glm::ivec3 c0 = toCell(dmn) - glm::ivec3(1);
    const glm::ivec3 c1 = toCell(dmx) + glm::ivec3(1);
    out.reserve(src.size());
    for (int y = std::max(0, c0.y); y <= std::min(grid - 1, c1.y); ++y)
        for (int z = std::max(0, c0.z); z <= std::min(grid - 1, c1.z); ++z)
            for (int x = std::max(0, c0.x); x <= std::min(grid - 1, c1.x); ++x) {
                VoxelRecord q;
                q.x = uint16_t(x);
                q.y = uint16_t(y);
                q.z = uint16_t(z);
                const glm::ivec3 s = toCell(inv(toWorld(q)));
                if (!inBounds(s))
                    continue;
                const auto it = srcCell.find(keyOf(uint16_t(s.x), uint16_t(s.y), uint16_t(s.z)));
                if (it == srcCell.end())
                    continue;
                VoxelRecord t = *it->second;
                t.x = uint16_t(x);
                t.y = uint16_t(y);
                t.z = uint16_t(z);
                out.push_back(t);
            }
}
} // namespace vf::voxel::worldfile
