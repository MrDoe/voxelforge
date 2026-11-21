// vf_mcp: Model Context Protocol server exposing voxelforge world editing
// over stdio (newline-delimited JSON-RPC 2.0, per the MCP stdio transport).
//
// Lets any MCP client (opencode, Claude Desktop-style hosts, gemma bridges)
// inspect and modify the layered voxel world. All edits land in the
// ai_edits.vxw layer via EditableWorld, so they persist and join the manifest.
// The running voxelforge instance watches the layer files and hot-reloads its
// SVO within a second - no external repack step involved.
//
// Tools:
//   list_layers                       -> manifest layers + enabled state
//   enable_layer {layer, enabled}     -> rewrite manifest entry
//   probe {x,y,z}                     -> scene signed distance / material
//   ground {x,z}                      -> terrain height + suggested anchor cell
//   add_box {anchor,[size],[material]}
//   add_cylinder {anchor,radius,height,[material]}
//   add_ellipsoid {anchor,radius:[rx,ry,rz],[material]}
//   add_stamp {anchor,cells:[{dx,dy,dz,mat}...]}
//   clear_edits {}
// Anchors are bottom-center lattice cells [0..1023]^3; alternatively pass
// "ground":[x,z] to snap the anchor onto the terrain surface.
#include "ai/tools.hpp"
#include "ai/builders.hpp"
#include "voxel/selector.hpp"
#include "voxel/chunk_index.hpp"
#include "voxel/common.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/picking.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/scenery.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_rig.hpp"
#include "voxel/micro_clip.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <unistd.h>
#include <limits.h>

using namespace vf;

static const char* kMatNames[int(vf::voxel::kPaletteN)] = {
    "grass dark", "grass light", "soil", "sand", "rock", "light rock", "wood",
    "roof shingles", "foliage", "lava", "ember", "glow cyan", "glow green",
    "glow purple", "glow blue", "white-hot", "snow", "bark", "moss", "thatch",
    "plaster", "fleece", "wet soil"};

// --- tiny JSON writers -------------------------------------------------------
static std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                o += buf;
            } else {
                o += c;
            }
        }
    }
    return o;
}
static std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

// minimal readers on top of tools.hpp helpers
static std::string jsonGetString(const std::string& json, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return "";
    size_t c = json.find(':', p);
    if (c == std::string::npos)
        return "";
    size_t q = json.find('"', c);
    if (q == std::string::npos)
        return "";
    size_t e = q + 1;
    while (e < json.size()) {
        if (json[e] == '\\' && e + 1 < json.size()) {
            e += 2;
            continue;
        }
        if (json[e] == '"')
            break;
        ++e;
    }
    return json.substr(q + 1, e - q - 1);
}

// raw TOP-LEVEL id token (number or string) or "" when absent (notification).
// A plain find("\"id\"") matches a nested id inside params.arguments when a
// client serializes `params` before `id` (OpenCode does), echoes the wrong
// JSON-RPC id, and the client closes the connection on "unknown message ID".
static std::string jsonGetId(const std::string& line)
{
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inStr) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') {
            const bool topId = depth == 1 && line.compare(i, 4, "\"id\"") == 0;
            inStr = true;
            if (!topId) continue;
            size_t c = i + 4;
            while (c < line.size() && isspace((unsigned char)line[c])) ++c;
            if (c >= line.size() || line[c] != ':') continue;
            ++c;
            while (c < line.size() && isspace((unsigned char)line[c])) ++c;
            size_t s = c, e = s;
            if (s < line.size() && line[s] == '"') {
                ++e;
                while (e < line.size() && line[e] != '"') {
                    if (line[e] == '\\' && e + 1 < line.size()) ++e;
                    ++e;
                }
                if (e < line.size()) ++e;
            } else {
                while (e < line.size() && (isdigit((unsigned char)line[e]) ||
                       line[e] == '-' || line[e] == '+' || line[e] == '.' ||
                       tolower(line[e]) == 'e')) ++e;
            }
            return line.substr(s, e - s);
        }
        if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') --depth;
    }
    return "";
}

static bool jsonHasArray(const std::string& json, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t a = json.find('[', p);
    return a != std::string::npos;
}

// A fully-specified voxel cell. Colour/response fields default to -1 meaning
// "derive from material palette". Source coordinates may be either relative
// (dx,dy,dz, for add_voxels) or absolute (x,y,z, for write_object); `rel`
// records which form was supplied.
struct RawVox {
    int x = 0, y = 0, z = 0;
    bool rel = false;
    int r = -1, g = -1, b = -1;
    int mat = 6;
    int refl = -1, rough = -1;
    int tex = 0; // per-cell texture override (atlas layer; 0 = material slot)
};

// Parse cells:[{...}] / voxels:[{...}]. Each entry supplies dx/dy/dz OR x/y/z
// plus optional mat,r,g,b,refl,rough. Returns false when absent/empty.
static bool parseRawVoxels(const std::string& json, const char* key,
                           std::vector<RawVox>& out, size_t maxCells)
{
    out.clear();
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t a = json.find('[', p);
    if (a == std::string::npos)
        return false;
    size_t pos = a;
    while (out.size() < maxCells) {
        size_t obj = json.find('{', pos);
        if (obj == std::string::npos)
            break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos)
            break;
        std::string cell = json.substr(obj, end - obj + 1);
        auto grab = [&](const char* k) -> int {
            std::string pat = std::string("\"") + k + "\"";
            size_t q = cell.find(pat);
            if (q == std::string::npos)
                return 0;
            q = cell.find(':', q);
            if (q == std::string::npos)
                return 0;
            return std::atoi(cell.c_str() + q + 1);
        };
        RawVox c;
        bool hasDx = cell.find("\"dx\"") != std::string::npos;
        bool hasX = cell.find("\"x\"") != std::string::npos;
        if (hasDx) {
            c.rel = true;
            c.x = grab("dx"); c.y = grab("dy"); c.z = grab("dz");
        } else if (hasX) {
            c.rel = false;
            c.x = grab("x"); c.y = grab("y"); c.z = grab("z");
        } else {
            pos = end + 1;
            continue; // malformed cell lacking any coordinates
        }
        auto present = [&](const char* k) {
            return cell.find(std::string("\"") + k + "\"") != std::string::npos;
        };
        c.mat = present("mat") ? grab("mat") : 6;
        c.r = present("r") ? grab("r") : -1;
        c.g = present("g") ? grab("g") : -1;
        c.b = present("b") ? grab("b") : -1;
        c.tex = present("tex") ? grab("tex") : 0;
        c.refl = present("refl") ? grab("refl") : -1;
        c.rough = present("rough") ? grab("rough") : -1;
        out.push_back(c);
        pos = end + 1;
    }
    return !out.empty();
}

// Override colour/response on a generated record set (palette-derived by
// default) when the caller supplied explicit values.
static void applyAppearance(std::vector<voxel::VoxelRecord>& recs, int r, int g,
                            int b, int refl, int rough)
{
    auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    for (auto& v : recs) {
        if (r >= 0)
            v.r = uint8_t(cl(r));
        if (g >= 0)
            v.g = uint8_t(cl(g));
        if (b >= 0)
            v.b = uint8_t(cl(b));
        if (refl >= 0)
            v.reflectivity = uint8_t(cl(refl));
        if (rough >= 0)
            v.roughness = uint8_t(cl(rough));
    }
}

// A primitive within a composed object. `at` is a relative offset (in voxels)
// from the object's base anchor; type selects the rasterizer.
struct ShapeSpec {
    std::string type;
    int at[3] = { 0, 0, 0 };
    int size[3] = { 4, 4, 4 };       // box, in voxels
    float radii[3] = { 0.5f, 0.5f, 0.5f }; // ellipsoid, in meters
    float radius = 0.35f, height = 0.9f;   // cylinder, in meters
    int mat = 6;
    int rgb[3] = { -1, -1, -1 };
    int refl = -1, rough = -1;
};

// Parse shapes:[{type,at,size|radii|radius+height,mat?,rgb?,refl?,rough?},...]
static bool parseShapes(const std::string& json, std::vector<ShapeSpec>& out,
                        size_t maxShapes)
{
    out.clear();
    size_t p = json.find("\"shapes\"");
    if (p == std::string::npos)
        return false;
    size_t a = json.find('[', p);
    if (a == std::string::npos)
        return false;
    size_t pos = a;
    while (out.size() < maxShapes) {
        size_t obj = json.find('{', pos);
        if (obj == std::string::npos)
            break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos)
            break;
        std::string s = json.substr(obj, end - obj + 1);
        ShapeSpec sh;
        sh.type = jsonGetString(s, "type");
        std::vector<int> iv;
        if (ai::jsonGetIntArray(s, "at", iv, 3)) {
            sh.at[0] = iv[0];
            sh.at[1] = iv[1];
            sh.at[2] = iv[2];
        }
        if (ai::jsonGetIntArray(s, "size", iv, 3)) {
            sh.size[0] = iv[0];
            sh.size[1] = iv[1];
            sh.size[2] = iv[2];
        }
        if (ai::jsonGetIntArray(s, "rgb", iv, 3)) {
            sh.rgb[0] = iv[0];
            sh.rgb[1] = iv[1];
            sh.rgb[2] = iv[2];
        }
        std::vector<float> fv;
        if (ai::jsonGetFloatArray(s, "radii", fv, 3)) {
            sh.radii[0] = fv[0];
            sh.radii[1] = fv[1];
            sh.radii[2] = fv[2];
        }
        ai::jsonGetFloat(s, "radius", sh.radius);
        ai::jsonGetFloat(s, "height", sh.height);
        ai::jsonGetInt(s, "mat", sh.mat);
        ai::jsonGetInt(s, "refl", sh.refl);
        ai::jsonGetInt(s, "rough", sh.rough);
        out.push_back(sh);
        pos = end + 1;
    }
    return !out.empty();
}

// --- tool implementations ----------------------------------------------------
namespace {

// end offset (exclusive) of the JSON object opening at `open`, honoring
// string literals so braces inside values cannot derail the depth count
size_t jsonObjectEnd(const std::string& s, size_t open)
{
    int depth = 0;
    bool inStr = false;
    for (size_t e = open; e < s.size(); ++e) {
        char c = s[e];
        if (inStr) {
            if (c == '\\')
                ++e;
            else if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0)
            return e + 1;
    }
    return std::string::npos;
}

struct ToolResult {
    std::string text;
    bool isError = false;
};

struct Server {
    voxel::EditableWorld editable{ voxel::scenery::root() };
    std::string m_worldSig; // mtime+size of world.json + ai_edits.vxw

    void init() { editable.load(); }

    // A long-lived vf_mcp process must not answer probe/ground from a world
    // snapshot from before an agent turn or an UNDO. The signature changes
    // whenever the app/agent rewrites authored files; reload both caches.
    std::string worldSignature() const
    {
        std::ostringstream o;
        for (const char* f : { "world.json", "ai_edits.vxw" }) {
            const std::string p = voxel::scenery::root() + "/" + f;
            std::error_code ec;
            o << std::filesystem::last_write_time(p, ec).time_since_epoch().count()
              << ":" << std::filesystem::file_size(p, ec) << ";";
        }
        return o.str();
    }
    void refreshIfChanged()
    {
        const std::string sig = worldSignature();
        if (sig == m_worldSig)
            return;
        world().load(voxel::scenery::root() + "/world.json");
        editable.load();
        m_worldSig = sig;
    }

    ToolResult listLayers()
    {
        std::vector<voxel::worldfile::WorldLayer> layers;
        std::ostringstream o;
        if (!voxel::worldfile::loadManifest(voxel::scenery::root() + "/world.json",
                                     layers)) {
            return { "no world.json manifest found", true };
        }
        for (auto& l : layers) {
            if (l.role == "packed")
                continue;
            o << l.name << " (" << l.file << ", " << l.role << ", "
              << (l.enabled ? "enabled" : "disabled") << ")\n";
        }
        o << "ai_edits voxels this session: " << editable.size();
        return { o.str(), false };
    }

    ToolResult enableLayer(const std::string& args)
    {
        std::string layerName = jsonGetString(args, "layer");
        if (layerName.empty())
            layerName = jsonGetString(args, "name");
        int enabledInt = 1;
        ai::jsonGetInt(args, "enabled", enabledInt);
        std::vector<voxel::worldfile::WorldLayer> layers;
        const std::string manifestPath =
            voxel::scenery::root() + "/world.json";
        if (!voxel::worldfile::loadManifest(manifestPath, layers))
            return { "manifest unreadable", true };
        bool found = false;
        for (auto& l : layers) {
            if (l.name == layerName || l.file == layerName) {
                if (l.role == "landscape")
                    return { "landscape is always enabled", true };
                if (l.role == "packed")
                    return { "cannot toggle merged cache", true };
                l.enabled = enabledInt != 0;
                found = true;
                break;
            }
        }
        if (!found)
            return { "no such layer: " + layerName, true };
        if (!voxel::worldfile::writeManifest(manifestPath, layers))
            return { "failed to write manifest", true };
        return { "layer " + layerName + (enabledInt ? " enabled" : " disabled") +
                     "; the running app hot-reloads within ~1 s",
                 false };
    }

    // Accessor only: refreshIfChanged() owns the single load. Loading here too
    // made the first tool call of a fresh vf_mcp process build LayeredWorld
    // twice (static init + explicit reload), ~7 s instead of ~5 s.
    voxel::LayeredWorld& world()
    {
        static voxel::LayeredWorld w;
        return w;
    }

    ToolResult probe(const std::string& args)
    {
        float x = 0, y = 0, z = 0;
        ai::jsonGetFloat(args, "x", x);
        ai::jsonGetFloat(args, "y", y);
        ai::jsonGetFloat(args, "z", z);
        auto s = world().field().sampleWorld({ x, y, z });
        char buf[128];
        snprintf(buf, sizeof(buf), "d=%+.3f material=%d (%s)", s.d, int(s.mat),
                 s.d < 0 ? "solid" : "empty");
        return { buf, false };
    }

    ToolResult ground(const std::string& args)
    {
        float x = 0, z = 0;
        ai::jsonGetFloat(args, "x", x);
        ai::jsonGetFloat(args, "z", z);
        const voxel::VoxelField& f = world().field();
        float H = f.terrainHeightAtWorld(x, z);
        int cy = int((H + 0.5f * voxel::WORLD) / voxel::VOXEL);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "terrain %.2f m at (%.1f, %.1f); anchor cell [%d %d %d]",
                 H, x, z, int((x + 0.5f * voxel::WORLD) / voxel::VOXEL), cy,
                 int((z + 0.5f * voxel::WORLD) / voxel::VOXEL));
        return { buf, false };
    }

    // One-call surface map over an x/z rectangle. Each column reports the top
    // solid world Y and material, sampled through the SAME field probe uses,
    // so live store edits / the overlay are reflected. This replaces dozens
    // of probe() round-trips when mapping terrain (a cone, a crater, a shore).
    ToolResult surfaceScan(const std::string& args)
    {
        float x0 = -5.f, z0 = -5.f, x1 = 5.f, z1 = 5.f;
        std::vector<float> box;
        if (ai::jsonGetFloatArray(args, "box", box, 4) && box.size() == 4) {
            x0 = box[0]; z0 = box[1]; x1 = box[2]; z1 = box[3];
        } else {
            std::vector<float> mn, mx;
            if (ai::jsonGetFloatArray(args, "min", mn, 2) && mn.size() == 2 &&
                ai::jsonGetFloatArray(args, "max", mx, 2) && mx.size() == 2) {
                x0 = mn[0]; z0 = mn[1]; x1 = mx[0]; z1 = mx[1];
            } else {
                ai::jsonGetFloat(args, "x0", x0);
                ai::jsonGetFloat(args, "z0", z0);
                ai::jsonGetFloat(args, "x1", x1);
                ai::jsonGetFloat(args, "z1", z1);
            }
        }
        if (x0 > x1) std::swap(x0, x1);
        if (z0 > z1) std::swap(z0, z1);
        float step = 0.5f;
        ai::jsonGetFloat(args, "step", step);
        step = std::clamp(step, 0.1f, 5.f);
        const int nx = int(std::floor((x1 - x0) / step)) + 1;
        const int nz = int(std::floor((z1 - z0) / step)) + 1;
        if (nx < 1 || nz < 1 || size_t(nx) * size_t(nz) > 2500)
            return { "surface_scan grid too large (max 2500 columns); raise "
                     "\"step\" or shrink the box", true };

        char b[64];
        std::ostringstream o;
        o << "surface_scan x[" << x0 << ".." << x1 << "] z[" << z0 << ".." << z1
          << "] step=" << step << "m grid=" << nx << "x" << nz << "\nx:";
        for (int ix = 0; ix < nx; ++ix) {
            snprintf(b, sizeof(b), "%7.1f", x0 + ix * step);
            o << b;
        }
        o << "\n";
        std::vector<int> hist(voxel::kPaletteN, 0);
        float lo = 1e9f, hi = -1e9f;
        for (int iz = 0; iz < nz; ++iz) {
            snprintf(b, sizeof(b), "z=%5.1f:", z0 + iz * step);
            o << b;
            for (int ix = 0; ix < nx; ++ix) {
                const float x = x0 + ix * step;
                const float z = z0 + iz * step;
                float top = -1e9f;
                int mat = -1;
                for (float y = 30.f; y >= -5.f; y -= 0.1f) {
                    auto s = world().field().sampleWorld({ x, y, z });
                    if (s.d < 0) { top = y; mat = int(s.mat); break; }
                }
                if (mat >= 0 && mat < voxel::kPaletteN) {
                    hist[mat]++;
                    lo = std::min(lo, top);
                    hi = std::max(hi, top);
                }
                if (top < -1e8f) snprintf(b, sizeof(b), "      --");
                else snprintf(b, sizeof(b), "%5.1f/%-2d", top, mat);
                o << b;
            }
            o << "\n";
        }
        o << "surface height " << lo << ".." << hi << " m; materials:";
        for (size_t i = 0; i < hist.size(); ++i)
            if (hist[i]) o << " " << i << ":" << hist[i];
        return { o.str(), false };
    }

    // bottom-center lattice anchor from "anchor":[x,y,z] (cells),
    // "at":[x,y,z] (world metres) or "ground":[x,z]
    bool resolveAnchor(const std::string& args, glm::ivec3& out, std::string& err)
    {
        std::vector<int> anc;
        if (ai::jsonGetIntArray(args, "anchor", anc, 3)) {
            out = { anc[0], anc[1], anc[2] };
        } else if (jsonHasArray(args, "at")) {
            std::vector<float> w;
            if (!ai::jsonGetFloatArray(args, "at", w, 3)) {
                err = "at must be [x,y,z] world metres";
                return false;
            }
            for (int a = 0; a < 3; ++a)
                out[a] = int(std::floor((w[a] + 0.5f * voxel::WORLD) / voxel::VOXEL));
        } else if (jsonHasArray(args, "ground")) {
            std::vector<float> g;
            if (!ai::jsonGetFloatArray(args, "ground", g, 2)) {
                err = "ground must be [x,z]";
                return false;
            }
            float H = world().field().terrainHeightAtWorld(g[0], g[1]);
            out = { int((g[0] + 0.5f * voxel::WORLD) / voxel::VOXEL),
                    int((H + 0.5f * voxel::WORLD) / voxel::VOXEL),
                    int((g[1] + 0.5f * voxel::WORLD) / voxel::VOXEL) };
        } else {
            err = "provide \"anchor\":[x,y,z] (lattice cells) or \"ground\":[x,z]";
            return false;
        }
        out.x = std::clamp(out.x, 0, 1023);
        out.y = std::clamp(out.y, 0, 1023);
        out.z = std::clamp(out.z, 0, 1023);
        return true;
    }

    ToolResult addShape(const std::string& tool, const std::string& args)
    {
        glm::ivec3 anchor;
        std::string err;
        if (!resolveAnchor(args, anchor, err))
            return { err, true };

        int mat = 6;
        ai::jsonGetInt(args, "material", mat) || ai::jsonGetInt(args, "mat", mat);
        mat = std::clamp(mat, 0, 15);

        std::vector<voxel::VoxelRecord> recs;
        // optional explicit colour/response (overrides the palette)
        std::vector<int> rgb;
        int refl = -1, rough = -1;
        if (ai::jsonGetIntArray(args, "rgb", rgb, 3))
            ; // collected below
        ai::jsonGetInt(args, "refl", refl);
        ai::jsonGetInt(args, "rough", rough);
        auto finishAppearance = [&]() {
            if (!rgb.empty() || refl >= 0 || rough >= 0) {
                int r = rgb.size() > 0 ? rgb[0] : -1;
                int g = rgb.size() > 1 ? rgb[1] : -1;
                int b = rgb.size() > 2 ? rgb[2] : -1;
                applyAppearance(recs, r, g, b, refl, rough);
            }
        };

        if (tool == "add_box") {
            std::vector<int> sz;
            if (!ai::jsonGetIntArray(args, "size", sz, 3)) {
                // accept meters ("size_m") as a courtesy
                std::vector<float> sm;
                if (ai::jsonGetFloatArray(args, "size_m", sm, 3)) {
                    sz = { int(std::lround(sm[0] / voxel::VOXEL)),
                            int(std::lround(sm[1] / voxel::VOXEL)),
                            int(std::lround(sm[2] / voxel::VOXEL)) };
                } else {
                    sz = { 4, 4, 4 }; // conservative default prop
                }
            }
            glm::ivec3 s = glm::clamp(glm::ivec3(sz[0], sz[1], sz[2]),
                                       glm::ivec3(1), glm::ivec3(32));
            recs = editable.makeBox(anchor, s, uint8_t(mat));
        } else if (tool == "add_cylinder") {
            float r = 0.35f, h = 0.9f;
            ai::jsonGetFloat(args, "radius", r);
            ai::jsonGetFloat(args, "height", h);
            recs = editable.makeCylinderY(anchor, r, h, uint8_t(mat));
        } else if (tool == "add_ellipsoid") {
            std::vector<float> rad{ 0.5f, 0.5f, 0.5f };
            ai::jsonGetFloatArray(args, "radius", rad, 3);
            if (rad.size() != 3)
                rad = { 0.5f, 0.5f, 0.5f };
            recs = editable.makeEllipsoid(
                anchor, { rad[0], rad[1], rad[2] }, uint8_t(mat));
        } else if (tool == "add_stamp") {
            std::vector<ai::StampCellLite> cells;
            if (!ai::parseStampCells(args, cells))
                return { "add_stamp needs cells:[{dx,dy,dz,mat},...]", true };
            if (cells.size() > 1000)
                cells.resize(1000);
            std::vector<voxel::StampCell> scs;
            for (auto& c : cells)
                scs.push_back({ int8_t(c.dx), int8_t(c.dy), int8_t(c.dz),
                                uint8_t(std::clamp(c.mat, 0, 15)) });
            recs = editable.makeStamp(anchor, scs);
        } else {
            return { "unknown add-tool", true };
        }

        finishAppearance();
        size_t added = editable.append(recs);
        if (added == 0)
            return { "nothing added (all cells overlap existing edits)", true };
        char buf[224];
        snprintf(buf, sizeof(buf), "%zu voxels added at anchor [%d %d %d]; "
                                    "the running app hot-reloads within ~1 s "
                                    "(visible in every render mode)",
                 added, anchor.x, anchor.y, anchor.z);
        return { buf, false };
    }

    // Arbitrary shape from explicit voxel cells (relative to anchor). Each cell
    // may override colour/response; otherwise the material palette is used.
    ToolResult addVoxels(const std::string& args)
    {
        glm::ivec3 anchor;
        std::string err;
        if (!resolveAnchor(args, anchor, err))
            return { err, true };
        std::vector<RawVox> cells;
        if (!parseRawVoxels(args, "cells", cells, 20000))
            return { "add_voxels needs cells:[{dx,dy,dz,mat?,r?,g?,b?,refl?,"
                     "rough?,tex?},...]",
                     true };
        std::vector<voxel::VoxelRecord> recs;
        recs.reserve(cells.size());
        for (auto& c : cells) {
            int ax = anchor.x + c.x, ay = anchor.y + c.y, az = anchor.z + c.z;
            if (ax < 0 || ay < 0 || az < 0 || ax >= 1024 || ay >= 1024 || az >= 1024)
                continue;
            recs.push_back(voxel::makeVoxelRecord(ax, ay, az, uint8_t(c.mat), c.r,
                                                  c.g, c.b, c.refl, c.rough,
                                                  c.tex));
        }
        // Optional \"name\"/\"layer\": write to a standalone named object layer
        // instead of ai_edits. This makes every add addressable and removable
        // (delete_voxels/undo_call work on named layers) - the old behaviour
        // of dumping everything into ai_edits left no surgical way back.
        std::string layerName = jsonGetString(args, "name");
        if (layerName.empty()) layerName = jsonGetString(args, "layer");
        if (!layerName.empty()) {
            if (!editable.writeObjectLayer(layerName, recs))
                return { "add_voxels: failed to write object layer \"" + layerName +
                         "\" (illegal name or manifest error)", true };
            char buf2[256];
            snprintf(buf2, sizeof(buf2),
                     "%zu voxels added to named object layer \"%s\" (.vxw "
                     "registered + enabled); the running app hot-reloads "
                     "within ~1 s",
                     recs.size(), layerName.c_str());
            return { buf2, false };
        }

        size_t added = editable.append(recs);
        if (added == 0)
            return { "nothing added (cells empty or all out of bounds)", true };
        char buf[224];
        snprintf(buf, sizeof(buf), "%zu voxels added to ai_edits at anchor "
                                    "[%d %d %d]; the running app hot-reloads "
                                    "within ~1 s",
                 added, anchor.x, anchor.y, anchor.z);
        return { buf, false };
    }

    // Create or overwrite a standalone named object layer from absolute voxels.
    ToolResult writeObject(const std::string& args)
    {
        std::string name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "layer");
        if (name.empty())
            return { "write_object needs a \"name\"", true };
        std::vector<voxel::VoxelRecord> recs;

        // Composed-object path: a list of primitives relative to one base
        // anchor (the same anchor/ground convention as the add_* tools). This
        // is the easy way to build a creature: one call, many primitives.
        std::vector<ShapeSpec> shapes;
        if (parseShapes(args, shapes, 512)) {
            glm::ivec3 base;
            std::string err;
            if (!resolveAnchor(args, base, err))
                return { err, true };
            for (auto& sh : shapes) {
                glm::ivec3 sa = glm::clamp(
                    glm::ivec3(base.x + sh.at[0], base.y + sh.at[1],
                                base.z + sh.at[2]),
                    glm::ivec3(0), glm::ivec3(1023));
                int m = std::clamp(sh.mat, 0, 15);
                std::vector<voxel::VoxelRecord> part;
                if (sh.type == "box") {
                    glm::ivec3 s = glm::clamp(
                        glm::ivec3(sh.size[0], sh.size[1], sh.size[2]),
                        glm::ivec3(1), glm::ivec3(64));
                    part = editable.makeBox(sa, s, uint8_t(m));
                } else if (sh.type == "ellipsoid") {
                    part = editable.makeEllipsoid(
                        sa, glm::vec3(sh.radii[0], sh.radii[1], sh.radii[2]),
                        uint8_t(m));
                } else if (sh.type == "cylinder") {
                    part = editable.makeCylinderY(sa, sh.radius, sh.height,
                                                  uint8_t(m));
                }
                // unknown type -> skipped
                if (sh.rgb[0] >= 0 || sh.refl >= 0 || sh.rough >= 0)
                    applyAppearance(part, sh.rgb[0], sh.rgb[1], sh.rgb[2],
                                    sh.refl, sh.rough);
                recs.insert(recs.end(), part.begin(), part.end());
            }
            if (recs.empty())
                return { "shapes produced no voxels (check types/sizes)", true };
        } else {
            // Raw-voxel path: explicit absolute cells.
            std::vector<RawVox> cells;
            if (!parseRawVoxels(args, "voxels", cells, 200000))
                return { "write_object needs shapes:[...] or "
                         "voxels:[{x,y,z,mat?,r?,g?,b?,refl?,rough?,tex?},...]",
                         true };
            recs.reserve(cells.size());
            for (auto& c : cells) {
                if (c.rel)
                    return { "write_object voxels must use absolute x,y,z (not "
                             "dx,dy,dz)",
                             true };
                if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= 1024 || c.y >= 1024 ||
                    c.z >= 1024)
                    continue;
                recs.push_back(voxel::makeVoxelRecord(c.x, c.y, c.z,
                                                    uint8_t(c.mat), c.r, c.g,
                                                    c.b, c.refl, c.rough,
                                                    c.tex));
            }
            if (recs.empty())
                return { "write_object: no valid voxels", true };
        }

        if (!editable.writeObjectLayer(name, recs))
            return { "write_object failed (illegal name or manifest error)",
                     true };
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "wrote %zu voxels to object layer \"%s\" (.vxw registered + "
                 "enabled); the running app hot-reloads within ~1 s",
                 recs.size(), name.c_str());
        return { buf, false };
    }

    // Dump an existing object layer's voxels so an AI can read-modify-write it.
    ToolResult readObject(const std::string& args)
    {
        std::string name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "layer");
        if (name.empty())
            return { "read_object needs a \"name\"", true };
        std::string safe = voxel::EditableWorld::sanitizeLayerName(name);
        if (safe.empty())
            return { "illegal layer name", true };
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(
                voxel::scenery::root() + "/" + safe + ".vxw", d))
            return { "cannot read layer (missing or corrupt .vxw)", true };
        std::ostringstream o;
        // AABB + physical size: a CPU-side sanity check (no GPU needed) so an
        // agent can confirm an object's proportions before rendering.
        if (!d.voxels.empty()) {
            int mn[3] = { 1 << 30, 1 << 30, 1 << 30 };
            int mx[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
            for (const auto& v : d.voxels) {
                int c[3] = { v.x, v.y, v.z };
                for (int a = 0; a < 3; ++a) {
                    mn[a] = std::min(mn[a], c[a]);
                    mx[a] = std::max(mx[a], c[a]);
                }
            }
            int dim[3] = { mx[0] - mn[0] + 1, mx[1] - mn[1] + 1, mx[2] - mn[2] + 1 };
            o << "object \"" << safe << "\": " << d.voxels.size() << " voxels, "
              << "bounds x[" << mn[0] << ".." << mx[0] << "] y[" << mn[1] << ".."
              << mx[1] << "] z[" << mn[2] << ".." << mx[2] << "], size "
              << dim[0] << "x" << dim[1] << "x" << dim[2] << " voxels (~"
              << (dim[0] * 0.1f) << "m x " << (dim[1] * 0.1f) << "m x "
              << (dim[2] * 0.1f) << "m)\n";
        } else {
            o << "object \"" << safe << "\": 0 voxels\n";
        }
        // Cap the listing so the transport stays small; the AI can re-query a
        // sub-region by asking for specific cells if it needs them all.
        size_t shown = std::min<size_t>(d.voxels.size(), 2000);
        for (size_t i = 0; i < shown; ++i) {
            const auto& v = d.voxels[i];
            o << "{\"x\":" << v.x << ",\"y\":" << v.y << ",\"z\":" << v.z
              << ",\"mat\":" << int(v.materialId) << ",\"r\":" << int(v.r)
              << ",\"g\":" << int(v.g) << ",\"b\":" << int(v.b)
              << ",\"refl\":" << int(v.reflectivity)
              << ",\"rough\":" << int(v.roughness)
              << ",\"tex\":" << int(v.reserved) << "}\n"; // per-cell atlas slot
        }
        if (d.voxels.size() > shown)
            o << "... (" << (d.voxels.size() - shown) << " more)\n";
        return { o.str(), false };
    }

    ToolResult deleteObject(const std::string& args)
    {
        std::string name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "layer");
        if (name.empty())
            return { "delete_object needs a \"name\"", true };
        if (!editable.deleteObjectLayer(name))
            return { "cannot delete layer (absent, protected, or illegal name)",
                     true };
        return { "deleted object layer \"" + name +
                     "\"; the running app hot-reloads within ~1 s",
                 false };
    }

    // Studio parity for micro animations: author a bone key in a .anim clip
    // using the SAME quantisation/seeding rule as studioKeyAt and sampleTrackEuler.
    ToolResult clipKey(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "clip_key needs a \"layer\"", true };
        int boneId = -1;
        ai::jsonGetInt(args, "bone", boneId);
        if (boneId < 0) return { "clip_key needs \"bone\" (id >= 0)", true };
        float t = 0.f;
        if (!ai::jsonGetFloat(args, "time", t) || t < 0.f)
            return { "clip_key needs \"time\" >= 0", true };
        std::vector<float> rot;
        if (!ai::jsonGetFloatArray(args, "rot", rot, 3))
            return { "clip_key needs \"rot\":[rx,ry,rz] in degrees", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        voxel::worldfile::WorldLayer* layer = nullptr;
        for (auto& l : layers)
            if (l.name == name || l.file == name) { layer = &l; break; }
        if (!layer || layer->role != "micro")
            return { "clip_key needs a role:\"micro\" layer", true };
        std::string rel = jsonGetString(args, "clip");
        if (rel.empty()) {
            const size_t slash = layer->file.find_last_of('/');
            const std::string base = slash == std::string::npos
                ? layer->file.substr(0, layer->file.find_last_of('.'))
                : layer->file.substr(slash + 1, layer->file.find_last_of('.') - slash - 1);
            rel = (slash == std::string::npos ? std::string("micro/")
                                              : layer->file.substr(0, slash + 1)) +
                  base + ".anim";
        }
        const std::string path = voxel::scenery::root() + "/" + rel;
        voxel::MicroClip clip;
        if (!voxel::readMicroClip(path, clip)) {
            clip = voxel::MicroClip();
            clip.name = "studio";
            clip.description = "authored through vf_mcp clip_key";
            clip.layer = layer->file;
            clip.length = std::max(2.f, t + 0.5f);
            clip.fps = 30.f;
            clip.loop = true;
            clip.clock = voxel::ClipClock::Time;
            clip.speed = 0.20f;
            clip.stride = 0.90f;
            clip.blendIn = 0.20f;
        }
        clip.length = std::max(clip.length, t + 0.5f);
        voxel::BoneTrack* track = nullptr;
        for (auto& tr : clip.tracks)
            if (tr.boneId == uint32_t(boneId)) { track = &tr; break; }
        if (!track) {
            clip.tracks.push_back({});
            track = &clip.tracks.back();
            track->boneId = uint32_t(boneId);
        }
        const float step = clip.fps > 1e-3f ? 1.f / clip.fps : 1.f / 30.f;
        t = std::round(t / step) * step;
        voxel::BoneKey* key = nullptr;
        for (auto& k : track->keys)
            if (std::abs(k.t - t) < 0.5f * step) { key = &k; break; }
        if (!key) {
            voxel::BoneKey k;
            k.t = t;
            k.rotDeg = voxel::sampleTrackEuler(track->keys, t); // seed from the blend
            k.ease = voxel::ClipEase::Smooth;
            track->keys.push_back(k);
            std::sort(track->keys.begin(), track->keys.end(),
                      [](const voxel::BoneKey& a, const voxel::BoneKey& b) { return a.t < b.t; });
            for (auto& kk : track->keys)
                if (std::abs(kk.t - t) < 0.5f * step) { key = &kk; break; }
        }
        key->rotDeg = glm::vec3(rot[0], rot[1], rot[2]);
        std::string ease = jsonGetString(args, "ease");
        if (ease == "step") key->ease = voxel::ClipEase::Step;
        else if (ease == "linear") key->ease = voxel::ClipEase::Linear;
        else key->ease = voxel::ClipEase::Smooth;
        if (!voxel::writeMicroClip(path, clip))
            return { "cannot write clip: " + path, true };
        bool listed = false;
        for (const auto& c : layer->clips) if (c == rel) listed = true;
        if (!listed) {
            layer->clips.push_back(rel);
            if (!voxel::worldfile::writeManifest(manifestPath(), layers))
                return { "clip written but manifest update failed", true };
        }
        reloadWorld();
        return { "clip_key " + rel + ": bone " + std::to_string(boneId) +
                 " t=" + std::to_string(t) + " rot=(" + std::to_string(rot[0]) + "," +
                 std::to_string(rot[1]) + "," + std::to_string(rot[2]) + "), " +
                 std::to_string(track->keys.size()) + " key(s); the app hot-reloads",
                 false };
    }

    // Verification primitive: compare two headless --shot PPMs. The engine's
    // own 1.25 % TAA noise floor is the interpretation threshold; callers pair
    // this with render_shot before/after a write (and pin micro poses first).
    ToolResult diffShots(const std::string& args)
    {
        const std::string a = jsonGetString(args, "a");
        const std::string b = jsonGetString(args, "b");
        if (a.empty() || b.empty())
            return { "diff_shots needs \"a\" and \"b\" (.ppm paths)", true };
        int threshold = 12;
        ai::jsonGetInt(args, "threshold", threshold);
        auto readPpm = [](const std::string& p, int& w, int& h,
                          std::vector<uint8_t>& rgb) -> bool {
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            std::string magic;
            f >> magic >> w >> h;
            int maxv = 0;
            f >> maxv;
            if (magic != "P6" || maxv != 255 || w <= 0 || h <= 0) return false;
            f.get(); // one whitespace after maxval
            rgb.resize(size_t(w) * size_t(h) * 3);
            return bool(f.read(reinterpret_cast<char*>(rgb.data()), rgb.size()));
        };
        int wa = 0, ha = 0, wb = 0, hb = 0;
        std::vector<uint8_t> ca, cb;
        if (!readPpm(a, wa, ha, ca)) return { "cannot read PPM: " + a, true };
        if (!readPpm(b, wb, hb, cb)) return { "cannot read PPM: " + b, true };
        if (wa != wb || ha != hb)
            return { "shot dimensions differ", true };
        size_t changed = 0;
        double sum = 0.0;
        for (size_t i = 0; i < ca.size(); i += 3) {
            int d = std::max({ std::abs(int(ca[i]) - int(cb[i])),
                               std::abs(int(ca[i + 1]) - int(cb[i + 1])),
                               std::abs(int(ca[i + 2]) - int(cb[i + 2])) });
            if (d > threshold) ++changed;
            sum += std::abs((int(ca[i]) + int(ca[i + 1]) + int(ca[i + 2])) / 3.0 -
                            (int(cb[i]) + int(cb[i + 1]) + int(cb[i + 2])) / 3.0);
        }
        const size_t px = size_t(wa) * size_t(ha);
        std::ostringstream o;
        o << "diff " << a << " vs " << b << ": " << wa << "x" << ha
          << " px=" << px << " changed=" << changed
          << " (" << (100.0 * double(changed) / double(px)) << " %)"
          << " meanAbsLuma=" << (sum / double(px))
          << " threshold=" << threshold
          << " (engine TAA noise floor ~1.25 %)";
        return { o.str(), false };
    }

    // ----- shared v2 helpers ------------------------------------------------
    // A cell may be given as a lattice triple or as world metres. Normalising
    // here kills the -51.2 m offset trap: everything downstream uses cells.
    bool parseCellArg(const std::string& args, int cell[3], std::string& err) const
    {
        std::vector<int> c;
        if (ai::jsonGetIntArray(args, "cell", c, 3) ||
            ai::jsonGetIntArray(args, "at", c, 3) ||
            ai::jsonGetIntArray(args, "anchor", c, 3)) {
            for (int i = 0; i < 3; ++i) cell[i] = c[i];
            return true;
        }
        float x = 0.f, y = 0.f, z = 0.f;
        if (ai::jsonGetFloat(args, "x", x) && ai::jsonGetFloat(args, "y", y) &&
            ai::jsonGetFloat(args, "z", z)) {
            const glm::ivec3 v = voxel::worldToVoxel({ x, y, z });
            for (int i = 0; i < 3; ++i) cell[i] = v[i];
            return true;
        }
        err = "needs cell:[x,y,z] (lattice) or x/y/z (metres)";
        return false;
    }

    // The FILTER is `from` only. `mat` is the TARGET colour in paint /
    // recolor_layer — treating it as a filter selects zero cells and reads
    // like a silent no-op (measured 2026-11-20).
    int materialFilter(const std::string& args) const
    {
        int mat = -1;
        ai::jsonGetInt(args, "from", mat);
        return mat;
    }

    // Parse the shared selector grammar. `mat` applies to every spec unless a
    // spec carries its own filter (none do yet, so this is simple).
    bool parseSelector(const std::string& args, int matFilter,
                       std::vector<voxel::selector::Spec>& specs,
                       std::string& err) const
    {
        using Spec = voxel::selector::Spec;
        std::vector<int> box, lo, hi, anc;
        if (ai::jsonGetIntArray(args, "box", box, 6)) {
            Spec s; s.kind = Spec::Kind::Box; s.mat = matFilter;
            for (int i = 0; i < 6; ++i) s.box[i] = box[i];
            specs.push_back(s);
        } else if (ai::jsonGetIntArray(args, "min", lo, 3) &&
                   ai::jsonGetIntArray(args, "max", hi, 3)) {
            Spec s; s.kind = Spec::Kind::Box; s.mat = matFilter;
            for (int i = 0; i < 3; ++i) { s.box[i] = lo[i]; s.box[i + 3] = hi[i]; }
            specs.push_back(s);
        }
        const bool hasAnchor = ai::jsonGetIntArray(args, "anchor", anc, 3) ||
                               ai::jsonGetIntArray(args, "cell", anc, 3);
        float radius = -1.f;
        ai::jsonGetFloat(args, "radius", radius);
        int component = 0, conn = 6;
        ai::jsonGetInt(args, "component", component);
        ai::jsonGetInt(args, "connectivity", conn);
        if (hasAnchor && radius > 0.f) {
            Spec s; s.kind = Spec::Kind::Sphere; s.mat = matFilter;
            for (int i = 0; i < 3; ++i) s.anchor[i] = anc[i];
            s.radiusM = radius;
            specs.push_back(s);
        }
        if (hasAnchor && component) {
            Spec s; s.kind = Spec::Kind::Component; s.mat = matFilter;
            for (int i = 0; i < 3; ++i) s.anchor[i] = anc[i];
            s.connectivity = conn;
            specs.push_back(s);
        }
        if (!hasAnchor && (radius > 0.f || component)) {
            err = "anchor:[x,y,z] is required with radius or component";
            return false;
        }
        if (specs.empty() && matFilter >= 0) {
            Spec s; s.kind = Spec::Kind::Mat; s.mat = matFilter;
            specs.push_back(s);
        }
        if (specs.empty()) {
            err = "needs box:[x0,y0,z0,x1,y1,z1] (or min/max), anchor+radius, "
                  "anchor+component, or mat";
            return false;
        }
        return true;
    }

    // Read the persisted store overlay WITHOUT applying it. Only the chunk
    // list and edit AABBs are needed to answer "is this cell store-edited?".
    struct OverlayInfo {
        bool present = false;
        size_t bytes = 0;
        std::vector<std::pair<int, std::array<int, 6>>> chunks;
    };
    OverlayInfo readOverlayInfo() const
    {
        OverlayInfo info;
        std::string path = voxel::scenery::root() + "/runtime_edits.vxw";
        if (const char* p = getenv("VF_OVERLAY_PATH"); p && *p) path = p;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return info;
        info.present = true;
        info.bytes = std::filesystem::file_size(path, ec);
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(path, d))
            return info;
        for (const auto& s : d.sections) {
            if (s.type != voxel::worldfile::kSectionStoreChunks)
                continue;
            const std::vector<uint8_t>& b = s.data;
            size_t off = 0;
            auto rd32 = [&](uint32_t& v) -> bool {
                if (off + 4 > b.size()) return false;
                std::memcpy(&v, b.data() + off, 4); off += 4; return true;
            };
            uint32_t schema = 0, count = 0;
            if (!rd32(schema) || schema != 3 || !rd32(count))
                break;
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t ci = 0, nbox = 0, nbrick = 0;
                if (!rd32(ci)) break;
                off += 4;                       // state byte + 3 pad
                std::array<int, 6> bounds{};
                if (off + 24 > b.size()) break;
                std::memcpy(bounds.data(), b.data() + off, 24); off += 24;
                if (!rd32(nbox) || !rd32(nbrick)) break;
                off += size_t(nbox) * 10;
                off += size_t(nbrick) * (4 + 1024 * 4);
                if (off > b.size()) break;
                info.chunks.push_back({ int(ci), bounds });
            }
            break;
        }
        return info;
    }

    bool cellStoreEdited(const OverlayInfo& info, const int cell[3]) const
    {
        const int ci = voxel::chunkIndexOfCell(cell[0], cell[1], cell[2]);
        for (const auto& kv : info.chunks) {
            if (kv.first != ci)
                continue;
            const auto& b = kv.second;
            return cell[0] >= b[0] && cell[0] <= b[3] && cell[1] >= b[1] &&
                   cell[1] <= b[4] && cell[2] >= b[2] && cell[2] <= b[5];
        }
        return false;
    }

    // One call answering "what IS here, and which lane should edit it?".
    ToolResult describeCell(const std::string& args)
    {
        int cell[3] = {};
        std::string err;
        if (!parseCellArg(args, cell, err))
            return { err, true };
        const auto wp = voxel::selector::cellCentreWorld(cell);
        const auto smp = world().field().sampleWorld({ wp[0], wp[1], wp[2] });
        const std::string owner =
            smp.layer ? world().layerFile(smp.layer) : std::string();

        std::ostringstream o;
        o << "cell=[" << cell[0] << "," << cell[1] << "," << cell[2] << "] world=["
          << wp[0] << "," << wp[1] << "," << wp[2] << "]m\n"
          << "  material=" << int(smp.mat) << " ("
          << (smp.mat < int(voxel::kPaletteN) ? kMatNames[smp.mat] : "?") << ")\n"
          << "  object=" << (smp.obj ? "true" : "false")
          << " layerId=" << int(smp.layer)
          << " owner=" << (owner.empty() ? "(terrain / unowned)" : owner) << "\n"
          << "  terrainHeight=" << world().field().terrainHeightAtWorld(wp[0], wp[2])
          << "\n";

        const OverlayInfo ov = readOverlayInfo();
        const bool edited = cellStoreEdited(ov, cell);
        o << "  overlay=" << (ov.present ? ("present (" + std::to_string(ov.chunks.size()) +
                                             " edit chunks, " + std::to_string(ov.bytes) + " B)") : "none")
          << " storeEdited=" << (edited ? "true" : "false") << "\n";

        // Bounded material-masked flood over the merged field: component
        // bounds + histogram, the census a selector edit would operate on.
        std::unordered_set<uint32_t> seen;
        std::deque<std::array<int, 3>> q;
        q.push_back({ cell[0], cell[1], cell[2] });
        seen.insert(voxel::selector::keyOf(cell[0], cell[1], cell[2]));
        int lo[3] = { cell[0], cell[1], cell[2] };
        int hi[3] = { cell[0], cell[1], cell[2] };
        std::array<int, int(voxel::kPaletteN)> hist{};
        static const int dir[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
        ++hist[std::min(int(smp.mat), int(voxel::kPaletteN) - 1)];
        while (!q.empty() && seen.size() < 20000) {
            const auto c = q.front(); q.pop_front();
            for (int d = 0; d < 6; ++d) {
                const int n[3] = { c[0] + dir[d][0], c[1] + dir[d][1], c[2] + dir[d][2] };
                if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] > 1023 || n[1] > 1023 || n[2] > 1023)
                    continue;
                const uint32_t k = voxel::selector::keyOf(n[0], n[1], n[2]);
                if (seen.count(k)) continue;
                const auto pp = voxel::selector::cellCentreWorld(n);
                const auto s2 = world().field().sampleWorld({ pp[0], pp[1], pp[2] });
                if (s2.d > 0.f) continue;
                seen.insert(k);
                ++hist[std::min(int(s2.mat), int(voxel::kPaletteN) - 1)];
                q.push_back({ n[0], n[1], n[2] });
                for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], n[a]); hi[a] = std::max(hi[a], n[a]); }
            }
        }
        o << "  component=" << seen.size() << " cells bounds[" << lo[0] << ".." << hi[0]
          << "][" << lo[1] << ".." << hi[1] << "][" << lo[2] << ".." << hi[2] << "]\n"
          << "  histogram:";
        for (int m = 0; m < int(voxel::kPaletteN); ++m)
            if (hist[m]) o << " " << m << "=" << hist[m];
        o << "\n  lane=" << (edited ? "store (already edited live)" :
                              (owner.empty() ? "store (terrain: stamp)" :
                               "either: stamp=reversible, selector=baked/permanent")) << "\n";
        return { o.str(), false };
    }

    // Region-scoped recolor using the shared selector. Keeps recolor_layer as
    // the whole-layer sugar; this is the surgical, dry-run-able version.
    ToolResult paintCells(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "paint needs a \"layer\"", true };
        int toMat = -1, filter = materialFilter(args);
        ai::jsonGetInt(args, "mat", toMat) || ai::jsonGetInt(args, "material", toMat);
        std::vector<voxel::selector::Spec> specs;
        std::string err;
        if (!parseSelector(args, filter, specs, err))
            return { err, true };
        int r = -1, g = -1, b = -1, refl = -1, rough = -1, tex = -1;
        ai::jsonGetInt(args, "r", r); ai::jsonGetInt(args, "g", g);
        ai::jsonGetInt(args, "b", b);
        ai::jsonGetInt(args, "refl", refl); ai::jsonGetInt(args, "rough", rough);
        ai::jsonGetInt(args, "tex", tex);
        if (toMat < 0 && r < 0 && g < 0 && b < 0 && refl < 0 && rough < 0 && tex < 0)
            return { "paint needs at least one of mat/r/g/b/refl/rough/tex", true };
        if (toMat >= int(voxel::kPaletteN))
            return { "mat must be 0.." + std::to_string(voxel::kPaletteN - 1), true };

        std::string file = name;
        bool protectedLayer = false;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) {
                    file = l.file;
                    if (l.role == "landscape" || l.role == "packed") protectedLayer = true;
                    break;
                }
        if (file.find(".vxw") == std::string::npos) file += ".vxw";
        if (protectedLayer) return { "refusing to paint protected layer " + file, true };
        if (file == "ai_edits.vxw")
            return { "paint ai_edits via stamp(mat/...) in the store lane", true };
        const std::string path = voxel::scenery::root() + "/" + file;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(path, d))
            return { "cannot read layer file: " + file, true };
        voxel::selector::Selection sel;
        if (!voxel::selector::selectCells(d.voxels, specs, sel, err))
            return { err, true };
        std::array<int, int(voxel::kPaletteN)> before{}, after{};
        for (const auto& v : d.voxels)
            before[std::min(int(v.materialId), int(voxel::kPaletteN) - 1)]++;
        int touched = 0;
        for (auto& v : d.voxels) {
            if (!sel.contains(v.x, v.y, v.z)) continue;
            ++touched;
            if (toMat >= 0) v.materialId = uint8_t(toMat);
            if (r >= 0) v.r = uint8_t(std::clamp(r, 0, 255));
            if (g >= 0) v.g = uint8_t(std::clamp(g, 0, 255));
            if (b >= 0) v.b = uint8_t(std::clamp(b, 0, 255));
            if (refl >= 0) v.reflectivity = uint8_t(std::clamp(refl, 0, 255));
            if (rough >= 0) v.roughness = uint8_t(std::clamp(rough, 0, 255));
            if (tex >= 0) v.reserved = uint8_t(std::clamp(tex, 0, 255));
        }
        after.fill(0);
        for (const auto& v : d.voxels)
            after[std::min(int(v.materialId), int(voxel::kPaletteN) - 1)]++;
        std::ostringstream o;
        for (size_t i = 0; i < specs.size(); ++i)
            o << (i ? " + " : "") << specs[i].describe();
        o << "\n  " << sel.summary() << " -> recoloured " << touched << " cells\n";
        for (int m = 0; m < int(voxel::kPaletteN); ++m)
            if (before[m] || after[m])
                o << "  mat " << m << ": " << before[m] << " -> " << after[m] << "\n";
        int dry = 0; ai::jsonGetInt(args, "dry_run", dry);
        if (dry) return { o.str() + "dry run, nothing written", false };
        const std::string tmp = path + ".tmp";
        std::error_code ec;
        if (!voxel::worldfile::write(tmp, d)) {
            std::remove(tmp.c_str());
            return { "failed to write " + tmp, true };
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) { std::remove(tmp.c_str()); return { "failed to replace " + file, true }; }
        reloadWorld();
        return { o.str() + "the running app hot-reloads within ~1 s", false };
    }

    // Two-call transform flow. commit:false answers "where would it land?" by
    // running the SAME worldfile::transformRecords the app uses at load, and
    // reports the resulting AABB without writing anything. commit:true writes
    // the manifest (the panel's Apply equivalent) and the app hot-reloads.
    ToolResult transformLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "transform needs a \"layer\"", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        voxel::worldfile::WorldLayer* target = nullptr;
        for (auto& l : layers)
            if (l.name == name || l.file == name) { target = &l; break; }
        if (!target) return { "no such layer: " + name, true };
        // Collect the requested delta. Both commits use the same convention as
        // set_layer_transform: pos is a metres offset, angles are degrees.
        std::vector<float> pos;
        float yaw = target->rotDeg, pitch = target->rotX, roll = target->rotZ;
        bool havePos = ai::jsonGetFloatArray(args, "pos", pos, 3);
        ai::jsonGetFloat(args, "yaw", yaw) || ai::jsonGetFloat(args, "rot", yaw);
        ai::jsonGetFloat(args, "pitch", pitch) || ai::jsonGetFloat(args, "rotX", pitch);
        ai::jsonGetFloat(args, "roll", roll) || ai::jsonGetFloat(args, "rotZ", roll);
        if (havePos && pos.size() != 3)
            return { "pos must be [x,y,z] metres", true };
        const glm::vec3 off = havePos ? glm::vec3(pos[0], pos[1], pos[2])
                                      : glm::vec3(target->pos[0], target->pos[1], target->pos[2]);
        int commit = 0;
        ai::jsonGetInt(args, "commit", commit);
        std::ostringstream o;
        o << "layer " << name << " pos=[" << off.x << "," << off.y << "," << off.z
          << "] yaw/pitch/roll=" << yaw << "/" << pitch << "/" << roll << "\n";
        if (commit) {
            const std::string resultMsg = setLayerTransform(args).text;
            return { o.str() + "committed: " + resultMsg, false };
        }
        // Preview: apply the exact load-time placement to the layer's records.
        const std::string file = target->file.find(".vxw") == std::string::npos
            ? target->file + ".vxw" : target->file;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(voxel::scenery::root() + "/" + file, d))
            return { "cannot read layer file for preview: " + file, true };
        std::vector<voxel::VoxelRecord> out;
        voxel::worldfile::transformRecords(d.voxels, d.meta, off, yaw, pitch, roll, out);
        if (out.empty())
            return { "transform preview produced no cells", true };
        int lo[3] = { 1 << 30, 1 << 30, 1 << 30 };
        int hi[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
        for (const auto& v : out) {
            lo[0] = std::min(lo[0], int(v.x)); lo[1] = std::min(lo[1], int(v.y));
            lo[2] = std::min(lo[2], int(v.z));
            hi[0] = std::max(hi[0], int(v.x)); hi[1] = std::max(hi[1], int(v.y));
            hi[2] = std::max(hi[2], int(v.z));
        }
        const auto wp0 = voxel::selector::cellCentreWorld(lo);
        const auto wp1 = voxel::selector::cellCentreWorld(hi);
        o << "PREVIEW only (no manifest write): " << out.size() << " cells, "
          << "bounds [" << lo[0] << ".." << hi[0] << "][" << lo[1] << ".." << hi[1]
          << "][" << lo[2] << ".." << hi[2] << "] world ["
          << wp0[0] << "," << wp0[1] << "," << wp0[2] << "] .. ["
          << wp1[0] << "," << wp1[1] << "," << wp1[2] << "]\n"
          << "call again with commit:true to write world.json";
        return { o.str(), false };
    }

    // The per-layer Import button, exposed: translate a layer's records so its
    // bottom-centre lands on the anchor and append only that copy to ai_edits.
    // Layer files hold absolute lattice coords, so enabling a layer shows it
    // where it was baked — import is the ONLY runtime placement path.
    ToolResult placeCopy(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "place_copy needs a \"layer\"", true };
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) { file = l.file; break; }
        if (file.find(".vxw") == std::string::npos) file += ".vxw";
        if (file == "ai_edits.vxw" || file == "landscape.vxw")
            return { "place_copy refuses " + file + " (live/derived layer)", true };
        glm::ivec3 anchor;
        std::string err;
        if (!resolveAnchor(args, anchor, err))
            return { err, true };
        const size_t added = editable.importLayer(
            voxel::scenery::root() + "/" + file, anchor);
        if (added == 0)
            return { "place_copy: nothing added (missing layer, bad meta, or "
                     "all cells out of bounds)", true };
        reloadWorld();
        return { "place_copy '" + file + "': " + std::to_string(added) +
                 " cells appended to ai_edits at anchor [" + std::to_string(anchor.x) +
                 "," + std::to_string(anchor.y) + "," + std::to_string(anchor.z) +
                 "]; the app hot-reloads within ~1 s", false };
    }

    // ----- intent builders: intent -> cells, generated on the server ---------
    ToolResult buildPath(const std::string& args)
    {
        std::vector<float> a, b;
        if (!ai::jsonGetFloatArray(args, "from", a, 2) ||
            !ai::jsonGetFloatArray(args, "to", b, 2))
            return { "add_path needs from:[x,z] and to:[x,z] in metres", true };
        float width = 0.4f, step = 0.2f;
        ai::jsonGetFloat(args, "width", width);
        ai::jsonGetFloat(args, "step", step);
        int mat = 3;
        ai::jsonGetInt(args, "mat", mat) || ai::jsonGetInt(args, "material", mat);
        if (width <= 0.f || width > 12.f || step <= 0.f || step > 5.f ||
            mat < 0 || mat >= int(voxel::kPaletteN))
            return { "add_path: width 0..12, step 0..5, mat valid", true };
        int allow = 0;
        ai::jsonGetInt(args, "allow_keepout", allow);
        const float mx = 0.5f * (a[0] + b[0]), mz = 0.5f * (a[1] + b[1]);
        if (!allow && (ai::builders::inKeepOut(a[0], a[1]) ||
                       ai::builders::inKeepOut(b[0], b[1]) ||
                       ai::builders::inKeepOut(mx, mz)))
            return { "add_path crosses the live_edit keep-out (world x -3..3, "
                     "z -1..6); pass allow_keepout:true only if you know the "
                     "carve-water test depends on that region", true };
        const float dx = b[0] - a[0], dz = b[1] - a[1];
        const float len = std::sqrt(dx * dx + dz * dz);
        const float inv = len > 1e-5f ? 1.f / len : 0.f;
        const float px = -dz * inv, pz = dx * inv; // lateral unit vector
        const int lanes = std::max(1, int(std::ceil(width / voxel::VOXEL)));
        const auto height = [this](float x, float z) {
            return world().field().terrainHeightAtWorld(x, z);
        };
        std::vector<voxel::VoxelRecord> recs;
        for (int lane = 0; lane < lanes; ++lane) {
            const float off = (float(lane) - 0.5f * float(lanes - 1)) * voxel::VOXEL;
            auto recsLane = ai::builders::path(
                { a[0] + px * off, a[1] + pz * off },
                { b[0] + px * off, b[1] + pz * off }, step, mat, height);
            recs.insert(recs.end(), recsLane.begin(), recsLane.end());
        }
        if (recs.empty())
            return { "add_path produced no cells", true };
        std::string name = jsonGetString(args, "name");
        if (name.empty()) name = "mcp_path_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (!editable.writeObjectLayer(name, recs))
            return { "add_path: failed to write layer (illegal name?)", true };
        return { "add_path '" + name + "': " + std::to_string(recs.size()) +
                 " cells over " + std::to_string(int(std::ceil(len / step)) + 1) +
                 " samples, ground sampled per cell; the app hot-reloads", false };
    }

    ToolResult buildTree(const std::string& args)
    {
        glm::ivec3 anchor;
        std::string err;
        if (!resolveAnchor(args, anchor, err))
            return { err, true };
        float height = 2.5f, canopy = 1.1f;
        ai::jsonGetFloat(args, "height", height);
        ai::jsonGetFloat(args, "canopy_r", canopy);
        unsigned int seed = 1;
        { int s = 1; ai::jsonGetInt(args, "seed", s); seed = unsigned(s); }
        if (height < 0.3f || height > 12.f || canopy < 0.2f || canopy > 6.f)
            return { "add_tree: height 0.3..12, canopy_r 0.2..6", true };
        auto recs = ai::builders::tree(anchor, height, canopy, seed);
        std::string name = jsonGetString(args, "name");
        if (name.empty()) name = "mcp_tree_" + std::to_string(seed);
        if (!editable.writeObjectLayer(name, recs))
            return { "add_tree: failed to write layer", true };
        return { "add_tree '" + name + "': " + std::to_string(recs.size()) +
                 " cells (seed " + std::to_string(seed) + "); deterministic; "
                 "the app hot-reloads", false };
    }

    // ----- intent queue: the only bridge into the App's live store lane ------
    std::string intentsPath() const
    {
        return voxel::scenery::root() + "/mcp_intents.jsonl";
    }
    std::string resultsPath() const
    {
        return voxel::scenery::root() + "/mcp_results.jsonl";
    }
    std::string nextIntentId()
    {
        static unsigned long long counter = 0;
        return "mcp" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            "_" + std::to_string(++counter);
    }
    ToolResult postIntent(const char* tool, const std::string& args)
    {
        const std::string id = nextIntentId();
        std::ofstream f(intentsPath(), std::ios::app);
        if (!f) return { "cannot open intent queue: " + intentsPath(), true };
        f << "{\"id\":" << jstr(id) << ",\"tool\":" << jstr(tool)
          << ",\"args\":" << (args.empty() ? std::string("{}") : args) << "}\n";
        f.flush();
        return { std::string("queued ") + tool + " id=" + id +
                 "; call stamp_result / commit_result for the engine report", false };
    }
    ToolResult readResult(const char* tool, const std::string& args)
    {
        std::string id = jsonGetString(args, "id");
        std::ifstream f(resultsPath());
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("\"id\":\"" + id + "\"") == std::string::npos &&
                line.find("\"id\": \"" + id + "\"") == std::string::npos)
                continue;
            return { line, false };
        }
        return { std::string(tool) + " " + id + ": pending or no result", true };
    }

    ToolResult stampTool(const std::string& args)
    {
        int cell[3] = {};
        std::string err;
        // A screen click is resolved by the App through pickAtWindowPos; no
        // cell is needed on this side, but one is still accepted (and remains
        // the deterministic form used by the gate).
        std::vector<float> scr;
        if (!ai::jsonGetFloatArray(args, "screen", scr, 2) &&
            !ai::jsonGetFloatArray(args, "pixel", scr, 2) &&
            !parseCellArg(args, cell, err))
            return { err, true };
        std::string mode = jsonGetString(args, "mode");
        if (mode.empty()) mode = jsonGetString(args, "brush");
        static const char* kModes[] = { "carve", "add", "delete", "paint", "smooth" };
        bool ok = false;
        for (const char* m : kModes) if (mode == m) ok = true;
        if (!ok)
            return { "stamp needs mode: carve|add|delete|paint|smooth", true };
        float diameter = 2.f, depth = 1.5f, strength = 0.65f;
        ai::jsonGetFloat(args, "diameter", diameter);
        ai::jsonGetFloat(args, "depth", depth);
        ai::jsonGetFloat(args, "strength", strength);
        int mat = -1; ai::jsonGetInt(args, "mat", mat);
        if (diameter < 0.1f || diameter > 32.f || depth < 0.1f || depth > 12.f ||
            strength < 0.f || strength > 1.f || mat >= int(voxel::kPaletteN))
            return { "stamp: diameter 0.1..32, depth 0.1..12, strength 0..1, mat 0.."
                     + std::to_string(voxel::kPaletteN - 1), true };
        return postIntent("stamp", args);
    }

    // View/edit intents. Validation lives here (so an agent gets an instant
    // argument error); the actual pick, edit and commit live in the App, which
    // owns the camera, the ChunkStore and the undo stack. "screen" is a click
    // in the VIEW at logical window pixels - pickAtWindowPos runs the same
    // screen->ray + store/micro competition the mouse uses.
    ToolResult viewTool(const std::string& tool, const std::string& args)
    {
        auto boolArg = [&](const char* key, bool fallback) {
            const std::string pat = std::string("\"") + key + "\"";
            size_t p = args.find(pat);
            if (p == std::string::npos) return fallback;
            p = args.find(':', p + pat.size());
            if (p == std::string::npos) return fallback;
            ++p;
            while (p < args.size() && std::isspace(static_cast<unsigned char>(args[p]))) ++p;
            if (args.compare(p, 4, "true") == 0) return true;
            if (args.compare(p, 5, "false") == 0) return false;
            return fallback;
        };

        if (tool == "undo" || tool == "clear")
            return postIntent(tool.c_str(), "{}");
        if (tool == "box") {
            std::vector<int> box;
            const bool clear = boolArg("clear", false);
            if (!clear && !ai::jsonGetIntArray(args, "box", box, 6))
                return { "view_box needs box:[x0,y0,z0,x1,y1,z1] or clear:true", true };
            return postIntent("box", args);
        }

        std::vector<float> scr;
        const bool hasScreen = ai::jsonGetFloatArray(args, "screen", scr, 2) ||
                               ai::jsonGetFloatArray(args, "pixel", scr, 2);
        int cell[3] = {};
        std::string err;
        const bool hasCell = parseCellArg(args, cell, err);
        if (!hasScreen && !hasCell)
            return { tool + " needs screen:[x,y] (a click in the view), "
                            "cell:[x,y,z], or x/y/z in metres", true };
        if (hasScreen && (std::abs(scr[0]) > 100000.f || std::abs(scr[1]) > 100000.f))
            return { "screen coordinates are logical window pixels; got "
                     + std::to_string(scr[0]) + "," + std::to_string(scr[1]), true };

        if (tool == "click") {
            std::string mode = jsonGetString(args, "mode");
            if (mode.empty()) mode = jsonGetString(args, "brush");
            static const char* kModes[] = { "carve", "add", "delete", "paint",
                                            "smooth", "rotate", "move", "select" };
            bool ok = false;
            for (const char* m : kModes) if (mode == m) ok = true;
            if (!ok)
                return { "view_click needs mode: carve|add|delete|paint|smooth|"
                         "rotate|move|select", true };
            float diameter = 2.f, depth = 1.5f, strength = 0.65f;
            ai::jsonGetFloat(args, "diameter", diameter);
            ai::jsonGetFloat(args, "depth", depth);
            ai::jsonGetFloat(args, "strength", strength);
            int mat = -1;
            ai::jsonGetInt(args, "mat", mat);
            if (diameter < 0.1f || diameter > 32.f || depth < 0.1f || depth > 12.f ||
                strength < 0.f || strength > 1.f || mat >= int(voxel::kPaletteN))
                return { "view_click: diameter 0.1..32, depth 0.1..12, "
                         "strength 0..1, mat 0.." +
                         std::to_string(voxel::kPaletteN - 1), true };
            if (mode == "rotate") {
                float y = 0.f, p = 0.f, r = 0.f;
                ai::jsonGetFloat(args, "yaw", y) || ai::jsonGetFloat(args, "rot", y);
                ai::jsonGetFloat(args, "pitch", p) || ai::jsonGetFloat(args, "rotX", p);
                ai::jsonGetFloat(args, "roll", r) || ai::jsonGetFloat(args, "rotZ", r);
                if (std::abs(y) > 3600.f || std::abs(p) > 3600.f ||
                    std::abs(r) > 3600.f)
                    return { "view_click rotate: yaw/pitch/roll within +-3600 deg", true };
            }
            if (mode == "move") {
                float dx = 0.f, dy = 0.f, dz = 0.f;
                ai::jsonGetFloat(args, "dx", dx);
                ai::jsonGetFloat(args, "dy", dy);
                ai::jsonGetFloat(args, "dz", dz);
                if (std::abs(dx) > 100.f || std::abs(dy) > 100.f || std::abs(dz) > 100.f)
                    return { "view_click move: dx/dy/dz within +-100 m", true };
            }
        }
        return postIntent(tool.c_str(), args);
    }

    ToolResult snapshotLayer(const std::string& args, bool restore)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "needs a \"layer\"", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        std::string file = name;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) { file = l.file; break; }
        if (file.find(".vxw") == std::string::npos) file += ".vxw";
        if (file == "ai_edits.vxw" || file == "landscape.vxw")
            return { "snapshot/restore of " + file + " is refused (live/derived layer)", true };
        const std::string path = voxel::scenery::root() + "/" + file;
        std::error_code ec;
        if (!restore) {
            if (!std::filesystem::exists(path, ec)) return { "no such layer file: " + file, true };
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string bak = path + ".snap-" + std::to_string(now) + ".bak";
            std::filesystem::copy_file(path, bak,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return { "snapshot copy failed: " + ec.message(), true };
            return { "snapshot " + bak + " (" + std::to_string(std::filesystem::file_size(bak, ec)) +
                     " B). Preserved outside /tmp.", false };
        }
        std::string src = jsonGetString(args, "file");
        if (src.empty()) src = jsonGetString(args, "snapshot");
        if (src.empty()) return { "restore_snapshot needs \"file\" (the .bak path)", true };
        if (!std::filesystem::exists(src, ec)) return { "no such snapshot: " + src, true };
        const std::string tmp = path + ".snaprestore.tmp";
        std::filesystem::copy_file(src, tmp,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return { "restore copy failed: " + ec.message(), true };
        std::filesystem::rename(tmp, path, ec);
        if (ec) { std::remove(tmp.c_str()); return { "restore rename failed: " + ec.message(), true }; }
        reloadWorld();
        return { "restored " + file + " from " + src + "; the app hot-reloads within ~1 s", false };
    }

    // Surgically remove CELLS from an existing layer, leaving the rest of the
    // layer (and its manifest entry/placement) intact. This is the missing
    // counterpart to delete_object (whole layer) and clear_edits (ai_edits
    // only); it is how you take ONE tree out of a 150k-cell forest layer.
    //
    // Selectors (unioned if several are given):
    //   box:[x0,y0,z0,x1,y1,z1]   inclusive lattice cells
    //   min:[x,y,z], max:[x,y,z]  same, spelled as two corners
    //   anchor:[x,y,z] + radius:<metres>       sphere around the cell centre
    //   anchor:[x,y,z] + component:true        the whole connected component
    //   connectivity:26            optional (default 6) for component mode
    //   mat / from / material:<id> optional material filter
    //   dry_run:true               report the selection without writing
    ToolResult deleteVoxels(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) return { "delete_voxels needs a \"layer\"", true };

        std::string file = name;
        bool protectedLayer = false;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) {
                    file = l.file;
                    if (l.role == "landscape" || l.role == "packed")
                        protectedLayer = true;
                    break;
                }
        if (file.find(".vxw") == std::string::npos) file += ".vxw";
        if (protectedLayer)
            return { "refusing to carve protected layer " + file +
                     " (landscape/packed)", true };
        if (file == "ai_edits.vxw")
            return { "carve ai_edits with the in-app brush/Undo instead "
                     "(the app owns that layer's live state)", true };

        const std::string path = voxel::scenery::root() + "/" + file;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(path, d))
            return { "cannot read layer file: " + file, true };
        const size_t total = d.voxels.size();
        if (total == 0)
            return { file + ": layer is empty", true };

        // The shared selector grammar is the single implementation of box /
        // sphere / material-masked component used by EVERY cell verb.
        int matFilter = materialFilter(args);
        ai::jsonGetInt(args, "mat", matFilter) ||
            ai::jsonGetInt(args, "material", matFilter);
        int dryFlag = 0;
        ai::jsonGetInt(args, "dry_run", dryFlag);
        std::vector<int> box, anc;
        const bool hasBox = ai::jsonGetIntArray(args, "box", box, 6);
        const bool hasMinMax = ai::jsonGetIntArray(args, "min", box, 3) &&
                               ai::jsonGetIntArray(args, "max", anc, 3);
        const bool hasAnchor = ai::jsonGetIntArray(args, "anchor", anc, 3);
        if (!hasBox && !hasMinMax && !hasAnchor)
            return { "delete_voxels needs box:[x0,y0,z0,x1,y1,z1] (or min/max), "
                     "or anchor + radius (metres) / component:true", true };
        std::vector<voxel::selector::Spec> specs;
        std::string selErr;
        if (!parseSelector(args, matFilter, specs, selErr))
            return { selErr, true };
        voxel::selector::Selection sel;
        if (!voxel::selector::selectCells(d.voxels, specs, sel, selErr))
            return { selErr.empty() ? file + ": no cells matched the selection" : selErr, true };
        const size_t removeCount = sel.size();
        if (removeCount >= total)
            return { "selection covers every cell of " + file +
                     "; use delete_object to remove the whole layer", true };

        std::vector<voxel::VoxelRecord> keep;
        keep.reserve(total - removeCount);
        for (const auto& v : d.voxels)
            if (!sel.contains(v.x, v.y, v.z))
                keep.push_back(v);
        const int* mn = sel.lo;
        const int* mx = sel.hi;
        std::ostringstream o;
        o << (dryFlag ? "would carve " : "carved ") << removeCount << " of "
          << total << " cells from " << file << " (bounds ["
          << mn[0] << ".." << mx[0] << "] [" << mn[1] << ".." << mx[1]
          << "] [" << mn[2] << ".." << mx[2] << "]), " << keep.size()
          << " remain";
        if (dryFlag)
            return { o.str() + "; dry run, nothing written", false };
        d.voxels = std::move(keep);

        // Same atomic temp+rename contract as writeManifest / OverlayWriter:
        // never truncate a tracked .vxw in place (a crash would leave a
        // corrupt layer and no way back).
        const std::string tmp = path + ".tmp";
        std::error_code ec;
        if (!voxel::worldfile::write(tmp, d)) {
            std::remove(tmp.c_str());
            return { "failed to write " + tmp + "; original layer untouched", true };
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::remove(tmp.c_str());
            return { "failed to replace " + file + "; original layer untouched", true };
        }
        // This server caches a LayeredWorld; worldSignature() watches only
        // world.json / ai_edits.vxw, so an object-layer carve needs an
        // explicit reload before probe/ground answer from the new content.
        reloadWorld();
        o << "; the running app hot-reloads within ~1 s. Not undoable - "
             "keep a backup of the .vxw before carving.";
        return { o.str(), false };
    }

    // Cell-level carve of the ai_edits live layer. delete_voxels refuses
    // ai_edits because the running app owns that file's live state; this tool
    // is the explicit, snapshot-backed escape hatch for removing a bad add
    // (e.g. a flat lava disk) surgically instead of clear_edits (nuke all).
    // Requires confirm:true. Writes atomically via temp+rename and leaves a
    // labelled .bak beside the file so undo_call/restore_snapshot can revert.
    ToolResult deleteEdits(const std::string& args)
    {
        int confirmed = 0;
        ai::jsonGetInt(args, "confirm", confirmed);
        if (!confirmed)
            return { "delete_edits requires confirm:true (it carves the live "
                     "ai_edits layer); use dry_run:true first to see the "
                     "selection", true };

        const std::string path = voxel::scenery::root() + "/" +
                                 voxel::EditableWorld::kFileName;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(path, d))
            return { "cannot read ai_edits.vxw", true };
        const size_t total = d.voxels.size();
        if (total == 0)
            return { "ai_edits.vxw is empty", true };

        int matFilter = materialFilter(args);
        ai::jsonGetInt(args, "mat", matFilter) ||
            ai::jsonGetInt(args, "material", matFilter);
        int dryFlag = 0;
        ai::jsonGetInt(args, "dry_run", dryFlag);
        std::vector<int> box, anc;
        const bool hasBox = ai::jsonGetIntArray(args, "box", box, 6);
        const bool hasMinMax = ai::jsonGetIntArray(args, "min", box, 3) &&
                               ai::jsonGetIntArray(args, "max", anc, 3);
        const bool hasAnchor = ai::jsonGetIntArray(args, "anchor", anc, 3);
        if (!hasBox && !hasMinMax && !hasAnchor)
            return { "delete_edits needs box:[x0,y0,z0,x1,y1,z1] (or min/max) "
                     "or anchor + radius/component", true };
        std::vector<voxel::selector::Spec> specs;
        std::string selErr;
        if (!parseSelector(args, matFilter, specs, selErr))
            return { selErr, true };
        voxel::selector::Selection sel;
        if (!voxel::selector::selectCells(d.voxels, specs, sel, selErr))
            return { selErr.empty() ? "ai_edits: no cells matched" : selErr, true };
        const size_t removeCount = sel.size();
        if (removeCount >= total)
            return { "selection covers every cell of ai_edits; use "
                     "clear_edits to drop the whole layer", true };

        std::vector<voxel::VoxelRecord> keep;
        keep.reserve(total - removeCount);
        for (const auto& v : d.voxels)
            if (!sel.contains(v.x, v.y, v.z)) keep.push_back(v);
        const int* mn = sel.lo;
        const int* mx = sel.hi;
        std::ostringstream o;
        o << (dryFlag ? "would carve " : "carved ") << removeCount << " of "
          << total << " ai_edits cells (bounds [" << mn[0] << ".." << mx[0]
          << "] [" << mn[1] << ".." << mx[1] << "] [" << mn[2] << ".." << mx[2]
          << "]), " << keep.size() << " remain";
        if (dryFlag)
            return { o.str() + "; dry run, nothing written", false };

        std::error_code ec;
        const auto now = std::chrono::system_clock::now().time_since_epoch().count();
        const std::string bak = path + ".snap-" + std::to_string(now) + ".bak";
        std::filesystem::copy_file(path, bak,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return { "snapshot copy failed: " + ec.message(), true };

        d.voxels = std::move(keep);
        const std::string tmp = path + ".tmp";
        if (!voxel::worldfile::write(tmp, d)) {
            std::remove(tmp.c_str());
            return { "failed to write ai_edits tmp; original untouched", true };
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::remove(tmp.c_str());
            return { "failed to replace ai_edits; original untouched", true };
        }
        // The in-memory EditableWorld cache must be reloaded too, otherwise a
        // later add_* would save the old records back and resurrect the cells.
        editable.load();
        reloadWorld();
        o << "; backup " << bak << "; the app hot-reloads within ~1 s. "
             "Revert with restore_snapshot(file:\"" << bak << "\").";
        return { o.str(), false };
    }

    // Convert an STL/OBJ mesh file into a voxel object layer. Same pipeline as
    // the offline mesh_to_voxel tool: solid voxelization on the 0.1 m lattice,
    // written as a named layer the app hot-reloads.
    ToolResult importMesh(const std::string& args)
    {
        std::string file = jsonGetString(args, "file");
        if (file.empty())
            file = jsonGetString(args, "path");
        if (file.empty())
            return { "import_mesh needs a \"file\" (.stl or .obj)", true };
        std::string name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "layer");
        if (name.empty())
            return { "import_mesh needs a \"name\" for the object layer", true };
        glm::ivec3 anchor;
        std::string err;
        if (!resolveAnchor(args, anchor, err))
            return { err, true };

        voxel::MeshImportOptions imp;
        float scale = 0.f, fit = 0.f, rotY = 0.f;
        if (ai::jsonGetFloat(args, "scale", scale) && scale > 0.f) {
            imp.scale = scale;
        } else if (ai::jsonGetFloat(args, "fit", fit) && fit > 0.f) {
            imp.fitMeters = glm::clamp(fit, 0.1f, 90.f);
            imp.hasFit = true;
        }
        if (ai::jsonGetFloat(args, "rotY", rotY))
            imp.rotY = rotY;
        if (ai::jsonGetFloat(args, "rot_y", rotY))
            imp.rotY = rotY;
        int sw = 0, fl = 0, sh = 0;
        ai::jsonGetInt(args, "swapYz", sw) || ai::jsonGetInt(args, "swap_yz", sw);
        ai::jsonGetInt(args, "flip", fl);
        ai::jsonGetInt(args, "shell", sh);
        imp.swapYz = sw != 0;
        imp.flip = fl != 0;
        ai::jsonGetInt(args, "material", imp.mat) ||
            ai::jsonGetInt(args, "mat", imp.mat);
        imp.mat = std::clamp(imp.mat, 0, int(vf::voxel::kPaletteN) - 1);

        std::vector<voxel::VoxelRecord> recs;
        voxel::MeshImportStats stats;
        if (!voxel::convertMeshToRecords(file, imp, sh == 0, anchor,
                                         recs, stats, err))
            return { "mesh conversion failed: " + err, true };

        if (!editable.writeObjectLayer(name, recs))
            return { "import_mesh failed (illegal name or manifest error)",
                     true };
        std::ostringstream o;
        o << "imported " << file << " -> layer \"" << name << "\": " << recs.size()
          << " voxels, size " << stats.nx << "x" << stats.ny << "x" << stats.nz
          << " cells (~" << stats.extent.x << "m x " << stats.extent.y << "m x "
          << stats.extent.z << "m) at anchor [" << anchor.x << " " << anchor.y
          << " " << anchor.z << "]";
        if (stats.clamped > 0)
            o << "; " << stats.clamped << " cells were out of bounds and dropped";
        if (sh == 0 && stats.leak)
            o << "\nWARNING: no interior volume (the mesh is not watertight at "
                 "this scale) - only the shell was written, so it may render "
                 "hollow. Repair the mesh or increase --fit.";
        o << "; the running app hot-reloads within ~1 s";
        return { o.str(), false };
    }

    // ----- query helpers -----------------------------------------------------
    // Find a layer entry by name or file; returns index or -1.
    int findLayerIndex(const std::string& nameOrFile)
    {
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return -1;
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].name == nameOrFile || layers[i].file == nameOrFile)
                return int(i);
        return -1;
    }

    std::string manifestPath() const { return voxel::scenery::root() + "/world.json"; }

    ToolResult getLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            return { "get_layer needs a \"layer\" (name or file)", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        for (auto& l : layers) {
            if (l.name != name && l.file != name)
                continue;
            std::ostringstream o;
            o << "file=" << l.file << " role=" << l.role << " name=" << l.name
              << " enabled=" << (l.enabled ? "true" : "false")
              << " listed=" << (l.listed ? "true" : "false")
              << " pos=[" << l.pos[0] << "," << l.pos[1] << "," << l.pos[2] << "]"
              << " rot=" << l.rotDeg << " rotX=" << l.rotX << " rotZ=" << l.rotZ
              << " scale=" << l.scale;
            if (!l.behaviour.empty())
                o << " behaviour=" << l.behaviour;
            // voxel count if readable
            voxel::WorldFileData d;
            if (voxel::worldfile::read(voxel::scenery::root() + "/" + l.file, d))
                o << " voxels=" << d.voxels.size();
            return { o.str(), false };
        }
        return { "no such layer: " + name, true };
    }

    ToolResult setLayerTransform(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            return { "set_layer_transform needs a \"layer\"", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        bool found = false;
        for (auto& l : layers) {
            if (l.name != name && l.file != name)
                continue;
            std::vector<float> pos;
            if (ai::jsonGetFloatArray(args, "pos", pos, 3))
                if (pos.size() == 3) {
                    l.pos[0] = pos[0];
                    l.pos[1] = pos[1];
                    l.pos[2] = pos[2];
                }
            float v = 0.f;
            if (ai::jsonGetFloat(args, "rot", v))
                l.rotDeg = v;
            if (ai::jsonGetFloat(args, "rotX", v))
                l.rotX = v;
            if (ai::jsonGetFloat(args, "rotZ", v))
                l.rotZ = v;
            if (ai::jsonGetFloat(args, "scale", v) && v > 0.f)
                l.scale = v;
            // also accept yaw/pitch/roll aliases
            if (ai::jsonGetFloat(args, "yaw", v))
                l.rotDeg = v;
            if (ai::jsonGetFloat(args, "pitch", v))
                l.rotX = v;
            if (ai::jsonGetFloat(args, "roll", v))
                l.rotZ = v;
            found = true;
            break;
        }
        if (!found)
            return { "no such layer: " + name, true };
        if (!voxel::worldfile::writeManifest(manifestPath(), layers))
            return { "failed to write manifest", true };
        return { "transform updated for " + name + "; the running app hot-reloads within ~1 s",
                 false };
    }

    ToolResult setLayerBehaviour(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            return { "set_layer_behaviour needs a \"layer\"", true };
        std::string behaviour = jsonGetString(args, "behaviour");
        if (behaviour.empty())
            behaviour = jsonGetString(args, "behavior");
        std::string command = jsonGetString(args, "command");
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        const bool hasBehaviour = args.find("\"behaviour\"") != std::string::npos ||
                                 args.find("\"behavior\"") != std::string::npos;
        const bool hasCommand = args.find("\"command\"") != std::string::npos;
        bool found = false;
        for (auto& l : layers) {
            if (l.name != name && l.file != name)
                continue;
            if (hasBehaviour)
                l.behaviour = behaviour;
            if (hasCommand)
                l.command = command;
            found = true;
            break;
        }
        if (!found)
            return { "no such layer: " + name, true };
        if (!voxel::worldfile::writeManifest(manifestPath(), layers))
            return { "failed to write manifest", true };
        std::string msg = "updated " + name;
        if (hasBehaviour)
            msg += " behaviour='" + behaviour + "'";
        if (hasCommand)
            msg += " command='" + command + "'";
        msg += "; the running app hot-reloads within ~1 s";
        return { msg, false };
    }

    ToolResult listMaterials()
    {
        std::ostringstream o;
        for (int i = 0; i < voxel::kPaletteN; ++i) {
            const glm::vec3& c = voxel::kPalette[i];
            const glm::vec2& rr = voxel::kMaterialReflection[i];
            const glm::vec3& em = voxel::kEmissive[i];
            o << i << " " << kMatNames[i]
              << " rgb(" << int(c.r * 255) << "," << int(c.g * 255) << ","
              << int(c.b * 255) << ") refl=" << int(rr.x) << " rough=" << int(rr.y);
            if (em.x > 0 || em.y > 0 || em.z > 0)
                o << " emissive=" << em.x << "," << em.y << "," << em.z;
            o << "\\n";
        }
        return { o.str(), false };
    }

    ToolResult listTextures()
    {
        std::ostringstream o;
        std::string dir = std::string(VOXELFORGE_ASSET_DIR) + "/textures";
        std::error_code ec;
        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file())
                continue;
            std::string ext = e.path().extension().string();
            for (char& c : ext)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
                continue;
            files.push_back(e.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        for (auto& f : files)
            o << f << "\\n";
        o << files.size() << " texture files";
        // also list current bindings
        std::vector<voxel::worldfile::TextureBinding> bindings;
        if (voxel::worldfile::loadTextureManifest(manifestPath(), bindings)) {
            o << "\\n--- bindings ---\\n";
            for (auto& b : bindings)
                o << "mat " << b.mat << " <- " << b.file << " scale=" << b.scale
                  << (b.emissive ? " emissive" : "") << "\\n";
        }
        return { o.str(), false };
    }

    ToolResult listModels()
    {
        std::ostringstream o;
        std::string dir = std::string(VOXELFORGE_ASSET_DIR) + "/models";
        std::error_code ec;
        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file())
                continue;
            std::string ext = e.path().extension().string();
            for (char& c : ext)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".stl" && ext != ".obj")
                continue;
            files.push_back(e.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        for (auto& f : files)
            o << f << "\\n";
        o << files.size() << " model files";
        return { o.str(), false };
    }

    ToolResult listMicro()
    {
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        std::ostringstream o;
        for (auto& l : layers) {
            if (l.role != "micro")
                continue;
            o << l.name << " (" << l.file << ", " << (l.enabled ? "enabled" : "disabled")
              << ") pos=[" << l.pos[0] << "," << l.pos[1] << "," << l.pos[2] << "]"
              << " rot=" << l.rotDeg << " rotX=" << l.rotX << " rotZ=" << l.rotZ;
            if (!l.behaviour.empty())
                o << " behaviour=" << l.behaviour;
            o << "\\n";
        }
        return { o.str(), false };
    }

    ToolResult worldInfo()
    {
        std::ostringstream o;
        o << "worldSize=" << voxel::WORLD << " voxelSize=" << voxel::VOXEL
          << " waterLevel=" << voxel::WATER_LEVEL
          << " gridSize=" << voxel::GRID_N << " brickSize=" << voxel::BRICK_N
          << " latticeN=1024 materials=" << voxel::kPaletteN
          << " maxLights=" << voxel::kMaxLights;
        return { o.str(), false };
    }

    ToolResult layerStats(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            return { "layer_stats needs a \"layer\" (name or file)", true };
        // resolve file
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers)) {
            for (auto& l : layers)
                if (l.name == name || l.file == name) {
                    file = l.file;
                    break;
                }
        }
        if (file.find(".vxw") == std::string::npos)
            file += ".vxw";
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(voxel::scenery::root() + "/" + file, d))
            return { "cannot read layer file: " + file, true };
        if (d.voxels.empty())
            return { file + ": 0 voxels", false };
        int mn[3] = { 1 << 30, 1 << 30, 1 << 30 };
        int mx[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
        for (const auto& v : d.voxels)
            for (int a = 0; a < 3; ++a) {
                int c = a == 0 ? v.x : (a == 1 ? v.y : v.z);
                mn[a] = std::min(mn[a], c);
                mx[a] = std::max(mx[a], c);
            }
        std::ostringstream o;
        o << file << ": " << d.voxels.size() << " voxels, bounds x[" << mn[0]
          << ".." << mx[0] << "] y[" << mn[1] << ".." << mx[1] << "] z["
          << mn[2] << ".." << mx[2] << "], size " << (mx[0] - mn[0] + 1) << "x"
          << (mx[1] - mn[1] + 1) << "x" << (mx[2] - mn[2] + 1) << " voxels (~"
          << ((mx[0] - mn[0] + 1) * 0.1f) << "m x "
          << ((mx[1] - mn[1] + 1) * 0.1f) << "m x "
          << ((mx[2] - mn[2] + 1) * 0.1f) << "m)";
        return { o.str(), false };
    }

    ToolResult duplicateLayer(const std::string& args)
    {
        std::string src = jsonGetString(args, "source");
        if (src.empty())
            src = jsonGetString(args, "from");
        if (src.empty())
            src = jsonGetString(args, "layer");
        std::string newName = jsonGetString(args, "name");
        if (newName.empty())
            newName = jsonGetString(args, "to");
        if (src.empty() || newName.empty())
            return { "duplicate_layer needs \"source\" and \"name\"", true };
        std::string safe = voxel::EditableWorld::sanitizeLayerName(newName);
        if (safe.empty())
            return { "illegal target name", true };
        // resolve source file
        std::string srcFile = src;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == src || l.file == src) {
                    srcFile = l.file;
                    break;
                }
        if (srcFile.find(".vxw") == std::string::npos)
            srcFile += ".vxw";
        std::string dstFile = safe + ".vxw";
        std::ifstream in(voxel::scenery::root() + "/" + srcFile, std::ios::binary);
        if (!in)
            return { "cannot read source layer: " + srcFile, true };
        std::ofstream out(voxel::scenery::root() + "/" + dstFile, std::ios::binary);
        if (!out)
            return { "cannot write target layer", true };
        out << in.rdbuf();
        in.close();
        out.close();
        // register in manifest
        voxel::worldfile::WorldLayer nl;
        nl.file = dstFile;
        nl.name = safe;
        nl.role = "object";
        nl.enabled = false;
        nl.listed = true;
        nl.pos[0] = nl.pos[1] = nl.pos[2] = 0.f;
        nl.rotDeg = 0.f;
        layers.clear();
        voxel::worldfile::loadManifest(manifestPath(), layers);
        // avoid duplicate file entries
        bool exists = false;
        for (auto& l : layers)
            if (l.file == dstFile) {
                exists = true;
                break;
            }
        if (!exists) {
            size_t insertAt = 0;
            for (size_t i = 0; i < layers.size(); ++i)
                if (layers[i].role == "landscape") {
                    insertAt = i;
                    break;
                }
            layers.insert(layers.begin() + insertAt, nl);
            if (!voxel::worldfile::writeManifest(manifestPath(), layers))
                return { "copied file but manifest update failed", true };
        }
        return { "duplicated " + srcFile + " -> " + dstFile + "; (disabled by default)",
                 false };
    }

    ToolResult renameLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "from");
        std::string newName = jsonGetString(args, "new_name");
        if (newName.empty())
            newName = jsonGetString(args, "to");
        if (name.empty() || newName.empty())
            return { "rename_layer needs \"layer\" and \"new_name\"", true };
        std::string safe = voxel::EditableWorld::sanitizeLayerName(newName);
        if (safe.empty())
            return { "illegal new name", true };
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        int idx = -1;
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].name == name || layers[i].file == name) {
                idx = int(i);
                break;
            }
        if (idx < 0)
            return { "no such layer: " + name, true };
        std::string oldFile = layers[idx].file;
        std::string newFile = safe + ".vxw";
        // rename file on disk if present
        std::string oldPath = voxel::scenery::root() + "/" + oldFile;
        std::string newPath = voxel::scenery::root() + "/" + newFile;
        std::error_code ec;
        if (std::filesystem::exists(oldPath, ec))
            std::filesystem::rename(oldPath, newPath, ec);
        layers[idx].file = newFile;
        layers[idx].name = safe;
        if (!voxel::worldfile::writeManifest(manifestPath(), layers))
            return { "renamed file but manifest update failed", true };
        return { "renamed " + oldFile + " -> " + newFile, false };
    }

    ToolResult deleteLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            return { "delete_layer needs a \"layer\"", true };
        return editable.deleteObjectLayer(name) ? ToolResult{ "deleted layer " + name, false }
                                                : ToolResult{ "cannot delete (absent/protected/illegal)", true };
    }

    ToolResult listLights()
    {
        std::vector<voxel::worldfile::LightSource> lights;
        voxel::worldfile::loadLightManifest(manifestPath(), lights);
        std::ostringstream o;
        for (size_t i = 0; i < lights.size(); ++i) {
            const auto& l = lights[i];
            o << i << ": pos=[" << l.pos.x << "," << l.pos.y << "," << l.pos.z
              << "] color=[" << l.color.x << "," << l.color.y << "," << l.color.z
              << "] radius=" << l.radius << " intensity=" << l.intensity;
            if (!l.follow.empty())
                o << " follow=" << l.follow;
            o << "\\n";
        }
        o << lights.size() << " lights";
        return { o.str(), false };
    }

    ToolResult addLight(const std::string& args)
    {
        std::vector<float> pos, col;
        if (!ai::jsonGetFloatArray(args, "pos", pos, 3))
            return { "add_light needs pos:[x,y,z]", true };
        if (pos.size() != 3)
            return { "pos must be [x,y,z]", true };
        voxel::worldfile::LightSource l;
        l.pos = { pos[0], pos[1], pos[2] };
        if (ai::jsonGetFloatArray(args, "color", col, 3) && col.size() == 3)
            l.color = { col[0], col[1], col[2] };
        else if (ai::jsonGetFloatArray(args, "colour", col, 3) && col.size() == 3)
            l.color = { col[0], col[1], col[2] };
        float v = 0.f;
        if (ai::jsonGetFloat(args, "radius", v) && v > 0.f)
            l.radius = v;
        else
            l.radius = 8.f;
        if (ai::jsonGetFloat(args, "intensity", v) && v > 0.f)
            l.intensity = v;
        else
            l.intensity = 1.5f;
        std::string follow = jsonGetString(args, "follow");
        if (!follow.empty())
            l.follow = follow;
        // load existing + append
        std::vector<voxel::worldfile::LightSource> lights;
        voxel::worldfile::loadLightManifest(manifestPath(), lights);
        lights.push_back(l);
        if (!voxel::worldfile::writeLightManifest(manifestPath(), lights))
            return { "failed to write lights", true };
        return { "added light at [" + std::to_string(pos[0]) + "," +
                     std::to_string(pos[1]) + "," + std::to_string(pos[2]) +
                     "]; running app hot-reloads within ~1 s",
                 false };
    }

    ToolResult removeLight(const std::string& args)
    {
        int idx = -1;
        ai::jsonGetInt(args, "index", idx);
        if (idx < 0)
            ai::jsonGetInt(args, "id", idx);
        std::vector<voxel::worldfile::LightSource> lights;
        if (!voxel::worldfile::loadLightManifest(manifestPath(), lights))
            return { "no lights / manifest unreadable", true };
        if (idx < 0 || idx >= int(lights.size()))
            return { "invalid light index", true };
        lights.erase(lights.begin() + idx);
        if (!voxel::worldfile::writeLightManifest(manifestPath(), lights))
            return { "failed to write lights", true };
        return { "removed light " + std::to_string(idx), false };
    }

    ToolResult setTextureBindingTool(const std::string& args)
    {
        int mat = -1;
        ai::jsonGetInt(args, "mat", mat) || ai::jsonGetInt(args, "material", mat);
        if (mat < 0 || mat >= voxel::kPaletteN)
            return { "set_texture needs a valid \"mat\" (0.." + std::to_string(voxel::kPaletteN - 1) + ")",
                     true };
        std::string file = jsonGetString(args, "file");
        float scale = 0.f;
        ai::jsonGetFloat(args, "scale", scale);
        int em = -1;
        ai::jsonGetInt(args, "emissive", em);
        float es = 0.f;
        ai::jsonGetFloat(args, "emissiveScale", es);
        std::string card = jsonGetString(args, "card");
        std::vector<voxel::worldfile::TextureBinding> bindings;
        voxel::worldfile::loadTextureManifest(manifestPath(), bindings);
        // Carry the replaced binding's card over: editing one field (e.g.
        // scale) must never silently drop its foliage card. Explicit
        // "card": "" clears it.
        std::string oldCard;
        for (const auto& b : bindings)
            if (b.mat == mat) oldCard = b.card;
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                     [&](const voxel::worldfile::TextureBinding& b) {
                                         return b.mat == mat;
                                     }),
                       bindings.end());
        if (!file.empty()) {
            voxel::worldfile::TextureBinding b;
            b.file = file;
            b.mat = mat;
            b.scale = scale > 0.02f ? scale : 0.5f;
            b.emissive = em > 0;
            b.emissiveScale = es > 0.f ? es : 1.f;
            b.card = args.find("\"card\"") != std::string::npos ? card : oldCard;
            bindings.push_back(b);
        }
        if (!voxel::worldfile::writeTextureManifest(manifestPath(), bindings))
            return { "failed to write textures", true };
        return { "texture binding updated for material " + std::to_string(mat) +
                     (file.empty() ? " (palette fallback)" : " <- " + file) +
                     "; the running app hot-reloads within ~1 s",
                 false };
    }

    // ----- UI settings ("ui" manifest block: sidebar controls as data) ------
    // The running app hot-applies from the manifest (~1 s); patch only the
    // keys provided. See world/ui_settings.cpp in the app.
    ToolResult getUi()
    {
        std::ostringstream o;
        float e = 0.f, a = 0.f;
        if (voxel::worldfile::loadSunManifest(manifestPath(), e, a))
            o << "sun: elev " << e << " azim " << a << "\\n";
        voxel::worldfile::UiSettings u;
        if (voxel::worldfile::loadUiManifest(manifestPath(), u))
            o << "ui: " << voxel::worldfile::uiSettingsToJson(u) << "\\n";
        else
            o << "ui: (none - controls are at startup/env defaults)\\n";
        o << "keys: renderFlags backend taa volFog motionBlur dof linearFx "
            "autoExposure exposure tonemapLook ssaoStrength ssaoRadius "
            "ssaoBlur ssaoDebug splatRadius edgeBlend edgeShrink edgeFill "
            "edgeBridgeSize cornerFill lightBudget lightK camSpeed camPos "
            "camYaw camPitch furEnabled furPerCell furLength furWidth furCurl "
            "furCycles furTilt furSss brushMode brushWidth brushDepth "
            "brushMaterial smoothStrength falloffCurve smoothPreserveVolume "
            "uiFontScale scenery\\nrenderFlags bits: 0 AO 1 Shadows 2 Flora 3 Water "
            "5 SSR 6 SSAO 7 DetailNormals 8 Enclosure 9 TerrainInterp "
            "10 AutoExpose 11 Transmission 13 PhotoCards";
        return { o.str(), false };
    }

    ToolResult setUi(const std::string& args)
    {
        voxel::worldfile::UiSettings p;
        std::ostringstream applied;
        auto num = [&](const char* key, float& dst) {
            float v;
            if (ai::jsonGetFloat(args, key, v)) {
                dst = v;
                applied << " " << key << "=" << v;
            }
        };
        auto str = [&](const char* key, std::string& dst) {
            std::string v = jsonGetString(args, key);
            if (!v.empty()) { dst = v; applied << " " << key << "=" << v; }
        };
        num("renderFlags", p.renderFlags); str("backend", p.backend);
        num("taa", p.taa); num("volFog", p.volFog);
        num("motionBlur", p.motionBlur); num("dof", p.dof);
        num("linearFx", p.linearFx); num("autoExposure", p.autoExposure);
        num("exposure", p.exposure); num("tonemapLook", p.tonemapLook);
        num("ssaoStrength", p.ssaoStrength); num("ssaoRadius", p.ssaoRadius);
        num("ssaoBlur", p.ssaoBlur); num("ssaoDebug", p.ssaoDebug);
        num("splatRadius", p.splatRadius); num("edgeBlend", p.edgeBlend);
        num("edgeShrink", p.edgeShrink); num("edgeFill", p.edgeFill);
        num("edgeBridgeSize", p.edgeBridgeSize); num("cornerFill", p.cornerFill);
        num("lightBudget", p.lightBudget); num("lightK", p.lightK);
        num("camSpeed", p.camSpeed);
        std::vector<float> cam;
        if (ai::jsonGetFloatArray(args, "camPos", cam, 3) && cam.size() == 3) {
            p.camX = cam[0]; p.camY = cam[1]; p.camZ = cam[2];
            applied << " camPos";
        }
        num("camX", p.camX); num("camY", p.camY); num("camZ", p.camZ);
        num("camYaw", p.camYaw); num("camPitch", p.camPitch);
        num("furEnabled", p.furEnabled); num("furPerCell", p.furPerCell);
        num("furLength", p.furLength); num("furWidth", p.furWidth);
        num("furCurl", p.furCurl); num("furCycles", p.furCycles);
        num("furTilt", p.furTilt); num("furSss", p.furSss);
        str("brushMode", p.brushMode); num("brushWidth", p.brushWidth);
        num("brushDepth", p.brushDepth); num("brushMaterial", p.brushMaterial);
        num("smoothStrength", p.smoothStrength);
        str("falloffCurve", p.falloffCurve);
        num("smoothPreserveVolume", p.smoothPreserveVolume);
        num("uiFontScale", p.uiFontScale);
        str("scenery", p.scenery);
        if (applied.str().empty())
            return { "set_ui: no known keys in arguments (see get_ui)", true };
        if (!voxel::worldfile::mergeUiManifest(manifestPath(), p))
            return { "failed to write the ui block", true };
        return {"ui" + applied.str() +
                    " written; the running app hot-applies within ~1 s", false};
    }

    ToolResult setSun(const std::string& args)
    {
        float elev, azim;
        if (!ai::jsonGetFloat(args, "elev", elev) ||
            !ai::jsonGetFloat(args, "azim", azim))
            return { "set_sun needs elev and azim (degrees, toward the sun; "
                     "day 34/238, night -30/96)", true };
        if (!std::isfinite(elev) || !std::isfinite(azim))
            return { "set_sun: elev/azim must be finite degrees", true };
        if (!voxel::worldfile::writeSunManifest(manifestPath(), elev, azim))
            return { "failed to write the sun block", true };
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "sun authored: elev %.2f azim %.2f; the running app "
                      "hot-applies (and rebakes shadows) within ~1 s", elev, azim);
        return { buf, false };
    }

    ToolResult listSceneries()
    {
        std::ostringstream o;
        const std::string cur = voxel::scenery::root();
        for (const auto& s : voxel::scenery::list())
            o << s << (cur.size() >= s.size() &&
                              cur.compare(cur.size() - s.size(), s.size(), s) == 0
                          ? "  (active)" : "") << "\\n";
        o << "active root: " << cur;
        return { o.str(), false };
    }

    ToolResult setScenery(const std::string& args)
    {
        std::string name = jsonGetString(args, "scenery");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty())
            return { "set_scenery needs \"scenery\" (folder name)", true };
        bool known = false;
        for (const auto& s : voxel::scenery::list())
            if (s == name) known = true;
        if (!known)
            return { "no scenery folder '" + name + "' under scenery/", true };
        voxel::worldfile::UiSettings p;
        p.scenery = name;
        if (!voxel::worldfile::mergeUiManifest(manifestPath(), p))
            return { "failed to write the ui block", true };
        return { "scenery switch to '" + name +
                    "' authored; the app switches between frames at its ~1 s poll", false };
    }

    // Alter an existing layer's voxels in place: mat swap (whole layer or
    // only cells matching `from`), colour/response, AND the per-cell atlas
    // override `tex` (0 clears it, 1..255 selects a bound atlas slot).
    ToolResult recolorLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty()) name = jsonGetString(args, "name");
        if (name.empty()) name = jsonGetString(args, "file");
        if (name.empty()) return { "recolor_layer needs a \"layer\"", true };
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) { file = l.file; break; }
        if (file.find(".vxw") == std::string::npos) file += ".vxw";
        if (file == "ai_edits.vxw")
            return { "recolor ai_edits via the chat/brush tools instead "
                     "(the app owns that layer's live state)", true };
        int toMat = -1, fromMat = -1, r = -1, g = -1, b = -1;
        int refl = -1, rough = -1, tex = -1;
        ai::jsonGetInt(args, "mat", toMat) || ai::jsonGetInt(args, "material", toMat);
        ai::jsonGetInt(args, "from", fromMat);
        ai::jsonGetInt(args, "r", r);
        ai::jsonGetInt(args, "g", g);
        ai::jsonGetInt(args, "b", b);
        ai::jsonGetInt(args, "refl", refl);
        ai::jsonGetInt(args, "rough", rough);
        ai::jsonGetInt(args, "tex", tex);
        if (toMat < 0 && fromMat < 0 && r < 0 && g < 0 && b < 0 && refl < 0 &&
            rough < 0 && tex < 0)
            return { "recolor_layer needs at least one of mat/from/r/g/b/"
                     "refl/rough/tex", true };
        if (toMat >= int(voxel::kPaletteN))
            return { "mat must be 0.." + std::to_string(voxel::kPaletteN - 1), true };
        const std::string path = voxel::scenery::root() + "/" + file;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(path, d))
            return { "cannot read layer file: " + file, true };
        int touched = 0;
        for (auto& v : d.voxels) {
            if (fromMat >= 0 && int(v.materialId) != fromMat) continue;
            if (toMat >= 0) v.materialId = uint8_t(toMat);
            if (r >= 0) v.r = uint8_t(std::clamp(r, 0, 255));
            if (g >= 0) v.g = uint8_t(std::clamp(g, 0, 255));
            if (b >= 0) v.b = uint8_t(std::clamp(b, 0, 255));
            if (refl >= 0) v.reflectivity = uint8_t(std::clamp(refl, 0, 255));
            if (rough >= 0) v.roughness = uint8_t(std::clamp(rough, 0, 255));
            // reserved byte is the per-cell atlas-slot override (Sample::tex
            // -> surfel tan_aspect.w); 0 = material's own slot
            if (tex >= 0) v.reserved = uint8_t(std::clamp(tex, 0, 255));
            ++touched;
        }
        if (touched == 0)
            return { file + ": no cells matched (from=" +
                         std::to_string(fromMat) + ")", true };
        if (!voxel::worldfile::write(path, d))
            return { "failed to rewrite " + file, true };
        std::ostringstream o;
        o << "recoloured " << touched << " cells in " << file;
        if (toMat >= 0) o << " -> material " << toMat;
        if (r >= 0 || g >= 0 || b >= 0)
            o << " rgb(" << r << "," << g << "," << b << ")";
        if (tex >= 0) o << " tex=" << std::clamp(tex, 0, 255)
                        << (tex == 0 ? " (cleared; material's own slot)" : "");
        o << "; the running app hot-reloads within ~1 s";
        return { o.str(), false };
    }

    // Change one authored light in place (add_light only appends).
    ToolResult updateLight(const std::string& args)
    {
        int idx = -1;
        ai::jsonGetInt(args, "index", idx);
        if (idx < 0) ai::jsonGetInt(args, "id", idx);
        std::vector<voxel::worldfile::LightSource> lights;
        if (!voxel::worldfile::loadLightManifest(manifestPath(), lights))
            return { "no lights / manifest unreadable", true };
        if (idx < 0 || idx >= int(lights.size()))
            return { "invalid light index (0.." +
                         std::to_string(lights.size() - 1) + ")", true };
        auto& l = lights[size_t(idx)];
        std::vector<float> v3;
        if (ai::jsonGetFloatArray(args, "pos", v3, 3) && v3.size() == 3)
            l.pos = { v3[0], v3[1], v3[2] };
        if ((ai::jsonGetFloatArray(args, "color", v3, 3) ||
             ai::jsonGetFloatArray(args, "colour", v3, 3)) && v3.size() == 3)
            l.color = { v3[0], v3[1], v3[2] };
        float f;
        if (ai::jsonGetFloat(args, "radius", f) && f > 0.f) l.radius = f;
        if (ai::jsonGetFloat(args, "intensity", f) && f > 0.f) l.intensity = f;
        std::string s = jsonGetString(args, "follow");
        if (args.find("\"follow\"") != std::string::npos) l.follow = s;
        if (!voxel::worldfile::writeLightManifest(manifestPath(), lights))
            return { "failed to write lights", true };
        std::ostringstream o;
        o << "light " << idx << " updated: pos=[" << l.pos.x << "," << l.pos.y
          << "," << l.pos.z << "] radius=" << l.radius
          << " intensity=" << l.intensity;
        if (!l.follow.empty()) o << " follow=" << l.follow;
        o << "; the running app hot-reloads within ~1 s";
        return { o.str(), false };
    }

    // ----- reload cached world ----------------------------------------------
    void reloadWorld() { world().load(manifestPath()); }

    ToolResult reloadWorldTool(const std::string& /*args*/)
    {
        reloadWorld();
        return { "world reloaded from " + manifestPath(), false };
    }

    // ----- LIVE screen capture ----------------------------------------------
    // Grab the running app's actual X window (HUD, sidebar and all) via
    // ImageMagick `import`. Unlike render_shot (a fresh headless process that
    // reads the baked files), this shows what the USER currently sees - the
    // only way to verify a hot-reload actually landed in the live view.
    ToolResult captureScreen(const std::string& args)
    {
        std::string out = jsonGetString(args, "out");
        if (out.empty()) out = jsonGetString(args, "path");
        if (out.empty()) {
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            out = "workspace/screenshots/live_" + std::to_string(now) + ".png";
            std::error_code dirEc;
            std::filesystem::create_directories("workspace/screenshots", dirEc);
        }
        // Find the main Voxelforge window. wmctrl prints "0xID  desktop  host  TITLE";
        // match a title that ends in "Voxelforge" but exclude the "AI panel" window,
        // terminals whose cwd/path merely contains the word, and browsers.
        std::string winId;
        {
            FILE* p = popen("wmctrl -l 2>/dev/null", "r");
            if (p) {
                char line[512];
                while (fgets(line, sizeof(line), p)) {
                    std::string s(line);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                    if (s.find("Voxelforge") == std::string::npos) continue;
                    if (s.find("AI panel") != std::string::npos ||
                        s.find("christoph") != std::string::npos ||
                        s.find("Firefox") != std::string::npos ||
                        s.find("Code") != std::string::npos) continue;
                    // title is the last whitespace-separated token run; require
                    // the line to end with "Voxelforge" so the terminal (ends in
                    // "voxelforge" path) and other titled windows are excluded.
                    if (s.size() < 10 || s.compare(s.size() - 10, 10, "Voxelforge") != 0)
                        continue;
                    size_t sp = s.find(' ');
                    if (sp == std::string::npos) continue;
                    winId = s.substr(0, sp);
                    break;
                }
                pclose(p);
            }
        }
        if (winId.empty())
            return { "capture_screen: no live Voxelforge window found (is the app "
                     "running on DISPLAY? wmctrl -l saw none titled 'Voxelforge')", true };

        std::ostringstream cmd;
        cmd << "import -window " << winId << " \"" << out << "\" 2>/tmp/opencode/mcp_capture.err";
        int rc = std::system(cmd.str().c_str());
        std::error_code ec;
        auto sz = std::filesystem::file_size(out, ec);
        if (rc != 0 || ec || sz == 0) {
            std::ifstream ef("/tmp/opencode/mcp_capture.err");
            std::string err((std::istreambuf_iterator<char>(ef)),
                            std::istreambuf_iterator<char>());
            return { "capture_screen failed for window " + winId + ": " +
                     err.substr(0, 300), true };
        }
        return { "captured LIVE app window " + winId + " -> " + out + " (" +
                 std::to_string(sz) + " bytes) - this is the user's actual view "
                 "(HUD + sidebar included)", false };
    }

    // ----- headless render shot ---------------------------------------------
    ToolResult renderShot(const std::string& args)
    {
        // locate the voxelforge binary next to this executable
        char self[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len == -1)
            return { "cannot resolve own executable path", true };
        self[len] = '\0';
        std::filesystem::path bin = std::filesystem::path(self).parent_path() / "voxelforge";
        if (!std::filesystem::exists(bin))
            return { "voxelforge binary not found next to vf_mcp (" + bin.string() + ")", true };

        std::string out = jsonGetString(args, "out");
        if (out.empty())
            out = jsonGetString(args, "path");
        if (out.empty()) {
            // Agent screenshots live in the local (gitignored) gallery so
            // the in-app AI panel can show them; unique per call to avoid
            // clobbering a previous turn's evidence.
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            out = "workspace/screenshots/shot_" + std::to_string(now) + ".ppm";
            std::error_code dirEc;
            std::filesystem::create_directories("workspace/screenshots", dirEc);
        }

        std::ostringstream cmd;
        cmd << bin.string() << " --shot " << out;
        int w = 0, h = 0;
        ai::jsonGetInt(args, "width", w);
        ai::jsonGetInt(args, "height", h);
        if (w > 0) cmd << " --width " << w;
        if (h > 0) cmd << " --height " << h;
        std::string mode = jsonGetString(args, "mode");
        if (!mode.empty()) cmd << " --mode " << mode;
        std::vector<float> cam;
        if (ai::jsonGetFloatArray(args, "cam", cam, 6) && cam.size() == 6) {
            std::ostringstream c;
            c << cam[0] << "," << cam[1] << "," << cam[2] << "," << cam[3] << ","
              << cam[4] << "," << cam[5];
            cmd << " --cam " << c.str();
        }
        float at = 0.f;
        if (ai::jsonGetFloat(args, "animtime", at))
            cmd << " --animtime " << at;
        std::vector<float> sun;
        if (ai::jsonGetFloatArray(args, "sun", sun, 2) && sun.size() == 2)
            cmd << " --sun " << sun[0] << " " << sun[1];
        // isolate overlay so a session's paints don't pollute the shot
        std::string full = "VF_NO_OVERLAY=1 VF_OVERLAY_PATH=/tmp/opencode/mcp_overlay.vxw " +
                           cmd.str() + " 2>/tmp/opencode/mcp_shot.err";
        int rc = std::system(full.c_str());
        std::ifstream ef("/tmp/opencode/mcp_shot.err");
        std::string err((std::istreambuf_iterator<char>(ef)),
                        std::istreambuf_iterator<char>());
        if (rc != 0)
            return { "render failed (rc=" + std::to_string(rc) + "): " + err.substr(0, 500),
                     true };
        // report output size
        std::error_code ec;
        auto sz = std::filesystem::file_size(out, ec);
        std::ostringstream o;
        o << "rendered " << out << " (" << (ec ? 0 : sz) << " bytes)";
        if (!err.empty())
            o << "\\nstderr: " << err.substr(0, 300);
        return { o.str(), false };
    }

    // ----- inspect layer (bbox + count + material histogram) -----------------
    ToolResult inspectLayer(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            name = jsonGetString(args, "path");
        if (name.empty())
            return { "inspect_layer needs a \"layer\"/\"path\"", true };
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) {
                    file = l.file;
                    break;
                }
        if (file.find(".vxw") == std::string::npos)
            file += ".vxw";
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(voxel::scenery::root() + "/" + file, d))
            return { "cannot read layer file: " + file, true };
        if (d.voxels.empty())
            return { file + ": 0 voxels", false };
        int mn[3] = { 1 << 30, 1 << 30, 1 << 30 };
        int mx[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
        std::vector<int> hist(voxel::kPaletteN, 0);
        for (const auto& v : d.voxels) {
            int c[3] = { v.x, v.y, v.z };
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], c[a]);
                mx[a] = std::max(mx[a], c[a]);
            }
            if (v.materialId < voxel::kPaletteN)
                hist[v.materialId]++;
        }
        std::ostringstream o;
        o << file << ": " << d.voxels.size() << " voxels, bounds x[" << mn[0]
          << ".." << mx[0] << "] y[" << mn[1] << ".." << mx[1] << "] z["
          << mn[2] << ".." << mx[2] << "], size " << (mx[0] - mn[0] + 1) << "x"
          << (mx[1] - mn[1] + 1) << "x" << (mx[2] - mn[2] + 1) << " voxels (~"
          << ((mx[0] - mn[0] + 1) * 0.1f) << "m x "
          << ((mx[1] - mn[1] + 1) * 0.1f) << "m x "
          << ((mx[2] - mn[2] + 1) * 0.1f) << "m)\\nmaterial histogram:";
        for (int i = 0; i < voxel::kPaletteN; ++i)
            if (hist[i] > 0)
                o << " " << i << ":" << hist[i];
        return { o.str(), false };
    }

    // ----- probe region: solid/air counts + material histogram ---------------
    ToolResult probeRegion(const std::string& args)
    {
        // accept lattice cell bounds [x0,y0,z0,x1,y1,z1] or world meters via
        // x0/y0/z0/x1/y1/z1 numbers (metres). Detect: if any value > 1024 treat
        // as... no, just accept cell coords when all ints. Simpler: accept
        // "min":[x,y,z], "max":[x,y,z] in lattice cells, defaulting to a
        // 10-cell cube around origin if absent.
        std::vector<int> mn, mx;
        ai::jsonGetIntArray(args, "min", mn, 3);
        ai::jsonGetIntArray(args, "max", mx, 3);
        // also accept x0..z1
        int x0 = 0, y0 = 0, z0 = 0, x1 = 9, y1 = 9, z1 = 9;
        if (mn.size() == 3 && mx.size() == 3) {
            x0 = mn[0]; y0 = mn[1]; z0 = mn[2];
            x1 = mx[0]; y1 = mx[1]; z1 = mx[2];
        } else {
            ai::jsonGetInt(args, "x0", x0);
            ai::jsonGetInt(args, "y0", y0);
            ai::jsonGetInt(args, "z0", z0);
            ai::jsonGetInt(args, "x1", x1);
            ai::jsonGetInt(args, "y1", y1);
            ai::jsonGetInt(args, "z1", z1);
        }
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
        x0 = std::clamp(x0, 0, 1023);
        y0 = std::clamp(y0, 0, 1023);
        z0 = std::clamp(z0, 0, 1023);
        x1 = std::clamp(x1, 0, 1023);
        y1 = std::clamp(y1, 0, 1023);
        z1 = std::clamp(z1, 0, 1023);
        long solid = 0, air = 0;
        std::vector<int> hist(voxel::kPaletteN, 0);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float wx = -0.5f * voxel::WORLD + (x + 0.5f) * voxel::VOXEL;
                    float wy = -0.5f * voxel::WORLD + (y + 0.5f) * voxel::VOXEL;
                    float wz = -0.5f * voxel::WORLD + (z + 0.5f) * voxel::VOXEL;
                    auto s = world().field().sampleWorld({ wx, wy, wz });
                    if (s.d < 0) {
                        ++solid;
                        if (s.mat < voxel::kPaletteN)
                            hist[s.mat]++;
                    } else {
                        ++air;
                    }
                }
        std::ostringstream o;
        o << "region [" << x0 << "," << y0 << "," << z0 << "]-[" << x1 << "," << y1
          << "," << z1 << "]: solid=" << solid << " air=" << air
          << " total=" << (solid + air) << "\\nmaterial histogram:";
        for (int i = 0; i < voxel::kPaletteN; ++i)
            if (hist[i] > 0)
                o << " " << i << ":" << hist[i];
        return { o.str(), false };
    }
    // ----- inspect rig (derive skeleton from a micro layer) -----------------
    ToolResult inspectRig(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            name = jsonGetString(args, "file");
        if (name.empty())
            return { "inspect_rig needs a \"layer\" (name or file)", true };
        // resolve file
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (voxel::worldfile::loadManifest(manifestPath(), layers))
            for (auto& l : layers)
                if (l.name == name || l.file == name) {
                    file = l.file;
                    break;
                }
        if (file.find(".vxw") == std::string::npos)
            file += ".vxw";
        // micro layers live under micro/ possibly; if not found try as-is
        std::string fullPath = voxel::scenery::root() + "/" + file;
        voxel::WorldFileData d;
        if (!voxel::worldfile::read(fullPath, d))
            return { "cannot read layer file: " + file, true };
        if (d.voxels.empty())
            return { file + ": empty", true };
        // build field + derive rig
        voxel::MicroField field;
        std::string err;
        if (!voxel::buildMicroField(d.voxels, d.meta, field, &err))
            return { "buildMicroField failed: " + err, true };
        voxel::MicroRig rig;
        if (!voxel::deriveMicroRig(field, rig))
            return { "deriveMicroRig failed", true };
        const char* roleNames[] = { "spine", "leg", "head", "tail", "neck" };
        std::ostringstream o;
        o << file << ": " << rig.bones.size() << " bones, " << rig.legCount
          << " legs, spine axis " << (rig.forward.x != 0 ? "X" : "Z")
          << ", length " << rig.length << " m\\n";
        for (size_t i = 0; i < rig.bones.size(); ++i) {
            const auto& b = rig.bones[i];
            const char* rn = (int(b.role) >= 0 && int(b.role) < 5)
                                 ? roleNames[int(b.role)]
                                 : "?";
            o << i << ": " << rn << " parent=" << b.parent
              << " a=(" << b.a.x << "," << b.a.y << "," << b.a.z << ")"
              << " b=(" << b.b.x << "," << b.b.y << "," << b.b.z << ")"
              << " len=" << glm::length(b.b - b.a) << "\\n";
        }
        return { o.str(), false };
    }

    ToolResult setPose(const std::string& args)
    {
        std::string name = jsonGetString(args, "layer");
        if (name.empty())
            name = jsonGetString(args, "name");
        if (name.empty())
            return { "set_pose needs a \"layer\"", true };
        std::vector<float> pose;
        // jsonGetFloatArray demands an exact count, so parse the array manually
        // for the variable-length pose.
        {
            std::string pat = "\"pose\"";
            size_t p = args.find(pat);
            if (p != std::string::npos) {
                size_t a = args.find('[', p);
                size_t b = args.find(']', a);
                if (a != std::string::npos && b != std::string::npos) {
                    std::string inside = args.substr(a + 1, b - a - 1);
                    std::string tok;
                    for (size_t i = 0; i <= inside.size(); ++i) {
                        if (i == inside.size() || inside[i] == ',') {
                            if (!tok.empty()) {
                                try {
                                    pose.push_back(std::stof(tok));
                                } catch (...) {
                                }
                            }
                            tok.clear();
                        } else if (!std::isspace((unsigned char)inside[i])) {
                            tok += inside[i];
                        }
                    }
                }
            }
        }
        if (pose.empty() || pose.size() % 4 != 0)
            return { "set_pose needs \"pose\":[bone,rx,ry,rz, ...] (degrees, flat)", true };
        // resolve file
        std::string file = name;
        std::vector<voxel::worldfile::WorldLayer> layers;
        if (!voxel::worldfile::loadManifest(manifestPath(), layers))
            return { "manifest unreadable", true };
        bool found = false;
        for (auto& l : layers) {
            if (l.name != name && l.file != name)
                continue;
            l.pose = pose;
            found = true;
            break;
        }
        if (!found)
            return { "no such layer: " + name, true };
        if (!voxel::worldfile::writeManifest(manifestPath(), layers))
            return { "failed to write manifest", true };
        return { "pose set on " + name + " (" + std::to_string(pose.size() / 4) +
                     " bones); the running app hot-reloads within ~1 s",
                 false };
    }

    ToolResult furStats(const std::string& /*args*/)
    {
        // Read .opencode/fur_stats.json (written by loadMicroObjects).
        std::string path = ".opencode/fur_stats.json";
        std::ifstream f(path);
        if (!f) {
            // try relative to asset dir parent
            path = std::string(VOXELFORGE_ASSET_DIR) + "/../.opencode/fur_stats.json";
            f.open(path);
        }
        if (!f)
            return { "fur_stats.json not found (run the app first)", true };
        std::ostringstream buf;
        buf << f.rdbuf();
        return { buf.str(), false };
    }
};

std::string toolSchemas()
{
    // hand-written inputSchema objects (JSON Schema subset)
    auto obj = [](const std::string& props) {
        return "{\"type\":\"object\",\"properties\":{" + props + "}}";
    };
    std::string t = R"( {"tools":[)";
    auto entry = [&](const char* name, const char* desc, const std::string& schema,
                     bool last) {
        t += std::string("{\"name\":") + jstr(name) + ",\"description\":" +
             jstr(desc) + ",\"inputSchema\":" + schema + "}" + (last ? "" : ",");
    };
    entry("list_layers", "List all voxelforge layers with enabled state", obj(""), false);
    entry("enable_layer", "Enable/disable a named layer",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + jstr("enabled") +
                  ":{\"type\":\"boolean\"}"),
          false);
    entry("probe", "Signed distance + material at a world point (meters)",
          obj(jstr("x") + ":{\"type\":\"number\"}," + jstr("y") +
                  ":{\"type\":\"number\"}," + jstr("z") + ":{\"type\":\"number\"}"),
          false);
    entry("ground", "Terrain height and suggested anchor cell at [x,z]",
          obj(jstr("x") + ":{\"type\":\"number\"}," + jstr("z") +
                  ":{\"type\":\"number\"}"),
          false);
    std::string anchorSchema = jstr("anchor") +
        ":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
        "\"ground\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
        "\"material\":{\"type\":\"integer\"}";
    entry("add_box", "Add a box; anchor=bottom-center lattice cell or ground=[x,z]; "
                     "size in voxels (10 = 1m)",
          obj(anchorSchema + "," + jstr("size") +
                  ":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}"),
          false);
    entry("add_cylinder",
          "Add a vertical cylinder (radius/height in meters); optional "
          "rgb:[r,g,b] / refl / rough override the palette colour/response.",
          obj(anchorSchema + ",\"radius\":{\"type\":\"number\"},"
              "\"height\":{\"type\":\"number\"}"), false);
    entry("add_ellipsoid",
          "Add an ellipsoid/boulder (radius meters); optional rgb:[r,g,b] / "
          "refl / rough override the palette colour/response.",
          obj(anchorSchema + ",\"radius\":{\"type\":\"array\","
              "\"items\":{\"type\":\"number\"}}"), false);
    entry("add_stamp", "Add literal 0.1m voxel art relative to anchor",
          obj(anchorSchema + ",\"cells\":{\"type\":\"array\"}"), false);
    std::string cellSchema =
        "\"cells\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}";
    entry("add_voxels",
          "Add an arbitrary voxel shape from explicit cells relative to the "
          "anchor/ground. Each cell: {dx,dy,dz,mat?,r?,g?,b?,refl?,rough?,tex?} "
          "(omit r,g,b to use the material palette; tex = optional texture "
          "atlas layer 1..255 overriding the material's slot). Optional "
          "name/layer writes to a standalone named object layer instead of "
          "ai_edits (addressable + removable).",
          obj(anchorSchema + "," + cellSchema +
                  ",\"name\":{\"type\":\"string\"},"
                  "\"layer\":{\"type\":\"string\"}"),
          false);
    std::string voxSchema =
        "\"voxels\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}";
    std::string shapeSchema =
        "\"shapes\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}";
    entry("write_object",
          "Create or overwrite a named standalone object layer (.vxw). Two "
          "ways: (1) composed of primitives via shapes:[{type:\"box|ellipsoid|"
          "cylinder\", at:[dx,dy,dz] relative to anchor/ground, size:[sx,sy,sz] "
          "voxels OR radii:[rx,ry,rz] m OR radius/height m, mat?, rgb?:[r,g,b], "
          "refl?,rough?},...] - the easy path for creatures/structures; (2) raw "
          "absolute voxels:[{x,y,z,mat?,r?,g?,b?,refl?,rough?,tex?}]. Registers "
          "+ enables in world.json so the app hot-reloads. Modify by "
          "read_object -> edit -> write_object.",
          obj(jstr("name") + ":{\"type\":\"string\"}," + anchorSchema + "," +
                  voxSchema + "," + shapeSchema),
          false);
    entry("read_object",
          "Dump an existing object layer's voxels (absolute x,y,z + material + "
          "colour/response) plus an AABB/size summary so proportions can be "
          "checked on the CPU before rendering. The read-modify-write loop for "
          "modifying an object.",
          obj(jstr("name") + ":{\"type\":\"string\"}"), false);
    entry("delete_object",
          "Remove a named object layer file + its manifest entry (never "
          "landscape/packed).",
          obj(jstr("name") + ":{\"type\":\"string\"}"), false);
    entry("delete_voxels",
          "Surgically delete CELLS from an existing layer (one tree, a region, "
          "or a connected component) without deleting the layer file. Select "
          "with box:[x0,y0,z0,x1,y1,z1] inclusive lattice cells (or min/max), "
          "or anchor:[x,y,z] + radius:<metres> / component:true. Optional "
          "mat/from/material filter and connectivity:26. dry_run:true reports "
          "the selection without writing. Refuses landscape/packed/ai_edits "
          "and a selection covering the whole layer; writes atomically but is "
          "NOT undoable.",
          obj(jstr("layer") + ":{\"type\":\"string\"},"
              "\"box\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"min\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"max\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"anchor\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"radius\":{\"type\":\"number\"},"
              "\"component\":{\"type\":\"boolean\"},"
              "\"connectivity\":{\"type\":\"integer\"},"
              "\"mat\":{\"type\":\"integer\"},\"from\":{\"type\":\"integer\"},"
              "\"dry_run\":{\"type\":\"boolean\"}"),
          false);
    entry("import_mesh",
          "Convert an STL or OBJ mesh file into a voxel object layer (solid "
          "fill on the 0.1m lattice; the app hot-reloads it). Args: "
          "\"file\" (path, .stl/.obj; resolved relative to CWD or assets/), "
          "\"name\" (layer to create/overwrite), \"anchor\":[x,y,z] lattice "
          "cells or \"ground\":[x,z] for the bottom-center, then options: "
          "\"scale\" (model units per metre; STL from CAD is usually 0.001) "
          "OR \"fit\" (uniform scale so the longest side = this many metres), "
          "\"rotY\" degrees, \"swapYz\" true for a Z-up model, \"flip\" true "
          "for reversed winding, \"shell\" true for a thin shell instead of "
          "solid fill, \"mat\" palette id 0..20 for STL / untextured faces. "
          "OBJ MTL Kd colours map to the nearest palette material.",
          obj(jstr("file") + ":{\"type\":\"string\"}," + jstr("name") +
                  ":{\"type\":\"string\"}," + anchorSchema +
                  ",\"mat\":{\"type\":\"integer\"},"
                  "\"scale\":{\"type\":\"number\"},"
                  "\"fit\":{\"type\":\"number\"},"
                  "\"rotY\":{\"type\":\"number\"},"
                  "\"swapYz\":{\"type\":\"boolean\"},"
                  "\"flip\":{\"type\":\"boolean\"},"
                  "\"shell\":{\"type\":\"boolean\"}"),
          false);
    entry("clear_edits", "Remove all session edits (clears ai_edits.vxw)", obj(""), false);
    entry("get_layer", "Detailed info about one layer (file, role, name, enabled, pos, rot, scale, behaviour, voxel count)",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("set_layer_transform",
          "Set a layer's placement: pos:[x,y,z] (metres), rot/rotX/rotZ (degrees, or yaw/pitch/roll), scale. Only provided fields change.",
          obj(jstr("layer") + ":{\"type\":\"string\"}," +
              jstr("pos") + ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"rot\":{\"type\":\"number\"},\"rotX\":{\"type\":\"number\"},"
              "\"rotZ\":{\"type\":\"number\"},\"scale\":{\"type\":\"number\"},"
              "\"yaw\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},"
              "\"roll\":{\"type\":\"number\"}"), false);
    entry("set_layer_behaviour",
          "Set behaviour (\"\"/\"grazer\") and/or a command (\"walk\"/\"run\"/\"graze\"/\"stop\"/\"auto\") on a micro layer",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + jstr("behaviour") + ":{\"type\":\"string\"}," +
              jstr("command") + ":{\"type\":\"string\"}"),
          false);
    entry("list_materials", "List the palette materials: id, name, rgb, reflectivity, roughness, emissive", obj(""), false);
    entry("list_textures", "List texture files in assets/textures and current material bindings", obj(""), false);
    entry("list_models", "List STL/OBJ mesh files in assets/models", obj(""), false);
    entry("list_micro", "List micro-voxel object layers (role micro) with placement", obj(""), false);
    entry("world_info", "World constants: size, voxel size, water level, grid, materials, max lights", obj(""), false);
    entry("layer_stats", "AABB + voxel count for a layer file",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("duplicate_layer", "Copy a layer .vxw to a new name (registered, disabled by default)",
          obj(jstr("source") + ":{\"type\":\"string\"}," + jstr("name") + ":{\"type\":\"string\"}"),
          false);
    entry("rename_layer", "Rename a layer (file + manifest entry)",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + jstr("new_name") + ":{\"type\":\"string\"}"),
          false);
    entry("delete_layer", "Delete a non-protected layer (file + manifest entry)",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("list_lights", "List authored point lights in world.json", obj(""), false);
    entry("add_light", "Add a point light (pos, color, radius, intensity, optional follow layer)",
          obj(jstr("pos") + ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"radius\":{\"type\":\"number\"},\"intensity\":{\"type\":\"number\"},"
              "\"follow\":{\"type\":\"string\"}"), false);
    entry("remove_light", "Remove a point light by index",
          obj(jstr("index") + ":{\"type\":\"integer\"}"), false);
    entry("set_texture", "Bind a texture file to a material (empty file = palette fallback)",
          obj(jstr("mat") + ":{\"type\":\"integer\"}," + jstr("file") + ":{\"type\":\"string\"},"
              "\"scale\":{\"type\":\"number\"},\"emissive\":{\"type\":\"integer\"},"
              "\"emissiveScale\":{\"type\":\"number\"}"), false);
    entry("inspect_layer", "Detailed layer inspection: bbox, voxel count, material histogram",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("probe_region", "Count solid/air cells + material histogram in a lattice box",
          obj(jstr("min") + ":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              + jstr("max") + ":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"x0\":{\"type\":\"integer\"},\"y0\":{\"type\":\"integer\"},"
              "\"z0\":{\"type\":\"integer\"},\"x1\":{\"type\":\"integer\"},"
              "\"y1\":{\"type\":\"integer\"},\"z1\":{\"type\":\"integer\"}"), false);
    entry("reload_world", "Reload the cached world field from world.json", obj(""), false);
    entry("render_shot",
          "Headless render: camera + resolution -> image path (PPM). Runs the voxelforge binary.",
          obj(jstr("out") + ":{\"type\":\"string\"}," +
              jstr("cam") + ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},"
              "\"mode\":{\"type\":\"string\"},\"animtime\":{\"type\":\"number\"},"
              "\"sun\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}}"), false);
    entry("inspect_rig", "Derive and report the skeleton (bones, roles, parents, segments) of a micro layer",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("set_pose", "Set a static rigged pose on a micro layer: pose:[bone,rx,ry,rz,...] (degrees, flat)",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + jstr("pose") +
              ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}}"), false);
    entry("fur_stats", "Dump fur card counts per micro object (from .opencode/fur_stats.json)", obj(""), false);
    entry("get_ui", "Read persisted UI settings (world.json ui block + authored sun)", obj(""), false);
    entry("set_ui",
          "Patch sidebar UI settings: any of renderFlags, backend, taa, volFog, motionBlur, dof, "
          "linearFx, autoExposure, exposure, tonemapLook, ssaoStrength, ssaoRadius, ssaoBlur, "
          "ssaoDebug, splatRadius, edgeBlend, edgeShrink, edgeFill, edgeBridgeSize, cornerFill, "
          "lightBudget, lightK, camSpeed, camPos, camYaw, camPitch, furEnabled, furPerCell, "
          "furLength, furWidth, furCurl, furCycles, furTilt, furSss, brushMode, brushWidth, "
          "brushDepth, brushMaterial, smoothStrength, falloffCurve, smoothPreserveVolume, scenery. "
          "Only provided keys change; the app hot-applies within ~1 s.",
          obj("\"renderFlags\":{\"type\":\"integer\"}," + jstr("backend") +
              ":{\"type\":\"string\"},"
              "\"taa\":{\"type\":\"integer\"},\"volFog\":{\"type\":\"integer\"},"
              "\"motionBlur\":{\"type\":\"integer\"},\"dof\":{\"type\":\"integer\"},"
              "\"linearFx\":{\"type\":\"integer\"},\"autoExposure\":{\"type\":\"integer\"},"
              "\"exposure\":{\"type\":\"number\"},\"tonemapLook\":{\"type\":\"integer\"},"
              "\"ssaoStrength\":{\"type\":\"number\"},\"ssaoRadius\":{\"type\":\"number\"},"
              "\"ssaoBlur\":{\"type\":\"integer\"},\"ssaoDebug\":{\"type\":\"integer\"},"
              "\"splatRadius\":{\"type\":\"number\"},\"edgeBlend\":{\"type\":\"number\"},"
              "\"edgeShrink\":{\"type\":\"number\"},\"edgeFill\":{\"type\":\"integer\"},"
              "\"edgeBridgeSize\":{\"type\":\"number\"},\"cornerFill\":{\"type\":\"integer\"},"
              "\"lightBudget\":{\"type\":\"integer\"},\"lightK\":{\"type\":\"integer\"},"
              "\"camSpeed\":{\"type\":\"number\"}," + jstr("camPos") +
              ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"camYaw\":{\"type\":\"number\"},\"camPitch\":{\"type\":\"number\"},"
              "\"furEnabled\":{\"type\":\"integer\"},\"furPerCell\":{\"type\":\"integer\"},"
              "\"furLength\":{\"type\":\"number\"},\"furWidth\":{\"type\":\"number\"},"
              "\"furCurl\":{\"type\":\"number\"},\"furCycles\":{\"type\":\"number\"},"
              "\"furTilt\":{\"type\":\"number\"},\"furSss\":{\"type\":\"number\"}," +
              jstr("brushMode") + ":{\"type\":\"string\"},"
              "\"brushWidth\":{\"type\":\"integer\"},\"brushDepth\":{\"type\":\"integer\"},"
              "\"brushMaterial\":{\"type\":\"integer\"},\"smoothStrength\":{\"type\":\"number\"}," +
              jstr("falloffCurve") + ":{\"type\":\"string\"},"
              "\"smoothPreserveVolume\":{\"type\":\"integer\"},"
              "\"uiFontScale\":{\"type\":\"number\"}," + jstr("scenery") +
              ":{\"type\":\"string\"}"),
          false);
    entry("set_sun", "Author the scene sun: elev/azim degrees toward the sun (day 34/238, night -30/96)",
          obj("\"elev\":{\"type\":\"number\"},\"azim\":{\"type\":\"number\"}"), false);
    entry("list_sceneries", "List scenery folders and mark the active one", obj(""), false);
    entry("set_scenery", "Switch the live app to another scenery",
          obj(jstr("scenery") + ":{\"type\":\"string\"}"), false);
    entry("recolor_layer",
          "Alter an existing layer's cells: mat (optional from-filter), r/g/b/refl/rough, and "
          "tex (per-cell atlas slot: 0 clears the override, 1..255 binds a slot). Rewrites the "
          ".vxw; the app hot-reloads.",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + jstr("mat") +
              ":{\"type\":\"integer\"},\"from\":{\"type\":\"integer\"},"
              "\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},"
              "\"b\":{\"type\":\"integer\"},\"refl\":{\"type\":\"integer\"},"
              "\"rough\":{\"type\":\"integer\"},\"tex\":{\"type\":\"integer\"}"), false);
    entry("update_light",
          "Change one authored light in place (index + any of pos, color, radius, intensity, follow)",
          obj(jstr("index") + ":{\"type\":\"integer\"}," + jstr("pos") +
              ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"color\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"radius\":{\"type\":\"number\"},\"intensity\":{\"type\":\"number\"}," +
              jstr("follow") + ":{\"type\":\"string\"}"), false);
    entry("describe",
          "One-call entry point: owner layer / terrain, material, store-edited flag, "
          "component bounds + histogram, and which edit lane to use. Accepts "
          "cell:[x,y,z] or x/y/z in metres.",
          obj(jstr("cell") + ":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
              "\"z\":{\"type\":\"number\"}"), false);
    entry("paint",
          "Region-scoped recolor using the shared selector (box / min+max / "
          "anchor+radius / anchor+component, optional mat filter). dry_run prints "
          "bounds + a before/after histogram. Never creates geometry.",
          obj(jstr("layer") + ":{\"type\":\"string\"},"
              "\"box\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"min\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"max\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"anchor\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"radius\":{\"type\":\"number\"},\"component\":{\"type\":\"boolean\"},"
              "\"connectivity\":{\"type\":\"integer\"},\"mat\":{\"type\":\"integer\"},"
              "\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},"
              "\"b\":{\"type\":\"integer\"},\"refl\":{\"type\":\"integer\"},"
              "\"rough\":{\"type\":\"integer\"},\"tex\":{\"type\":\"integer\"},"
              "\"dry_run\":{\"type\":\"boolean\"}"), false);
    entry("stamp",
          "Store-lane edit on terrain OR objects: mode carve|add|delete|paint|smooth "
          "at a cell, with diameter/depth/mat/strength. Posts an intent that the "
          "running app executes through the SAME code path as the GUI brush "
          "(undo+surfels+height/water+overlay for free). Returns an id; use "
          "stamp_result for the engine report.",
          obj(jstr("mode") + ":{\"type\":\"string\"},"
              "\"cell\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
              "\"z\":{\"type\":\"number\"},\"diameter\":{\"type\":\"number\"},"
              "\"depth\":{\"type\":\"number\"},\"mat\":{\"type\":\"integer\"},"
              "\"strength\":{\"type\":\"number\"},\"box_scope\":{\"type\":\"array\","
              "\"items\":{\"type\":\"integer\"}}"), false);
    entry("stamp_result", "Fetch the engine's instrument report for a queued stamp id.",
          obj(jstr("id") + ":{\"type\":\"string\"}"), false);
    // ---- view / edit tools: a click into the live view -------------------
    std::string viewTarget = jstr("screen") +
        ":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
        "\"cell\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
        "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"
        "\"z\":{\"type\":\"number\"}";
    entry("view_pick",
          "Click into the live VIEW at logical window pixels screen:[x,y] (or "
          "a cell/world point) and report what the app's real pick path hits: "
          "cell, world position, material, owning layer and distance. Use a "
          "screenshot from capture_screen/render_shot to choose the pixel. "
          "Returns an id; use edit_result for the report.",
          obj(viewTarget), false);
    entry("view_click",
          "Click into the live VIEW at screen:[x,y] (or cell/xyz) and use an "
          "edit tool directly, through the same paths the GUI brush and Apply "
          "buttons use. mode: carve|add|delete|paint|smooth (cell brushes: "
          "diameter/depth/strength/mat/box_scope), rotate (yaw/pitch/roll "
          "degrees) or move (dx/dy/dz metres) on the object under the click, "
          "select (activate that object), with commit:false to stage a "
          "preview instead of writing the manifest. Returns an id; use "
          "edit_result for the instrument report.",
          obj(viewTarget + "," + jstr("mode") + ":{\"type\":\"string\"},"
              "\"layer\":{\"type\":\"string\"},\"diameter\":{\"type\":\"number\"},"
              "\"depth\":{\"type\":\"number\"},\"strength\":{\"type\":\"number\"},"
              "\"mat\":{\"type\":\"integer\"},\"yaw\":{\"type\":\"number\"},"
              "\"pitch\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"},"
              "\"dx\":{\"type\":\"number\"},\"dy\":{\"type\":\"number\"},"
              "\"dz\":{\"type\":\"number\"},\"commit\":{\"type\":\"boolean\"},"
              "\"box_scope\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}"),
          false);
    entry("view_undo",
          "Undo the last live-edit stroke in the running app (the same stack "
          "as the sidebar's Undo / Ctrl+Z). Returns an id; use edit_result.",
          obj(""), false);
    entry("view_clear",
          "Clear ALL live edits in the running app, including its overlay file "
          "(the same irreversible action as 'Clear live edits'). Returns an id; "
          "use edit_result.",
          obj(""), false);
    entry("view_box",
          "Set or clear the selection box that scopes every cell brush in the "
          "running app. box:[x0,y0,z0,x1,y1,z1] inclusive lattice cells, or "
          "clear:true. Returns an id; use edit_result.",
          obj("\"box\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"clear\":{\"type\":\"boolean\"}"), false);
    entry("edit_result",
          "Fetch the running app's instrument report for any view_* intent id "
          "(also accepts stamp ids).",
          obj(jstr("id") + ":{\"type\":\"string\"}"), false);
    entry("snapshot",
          "Back up a tracked .vxw layer to a labelled .bak INSIDE the scenery "
          "(never /tmp). Every destructive baked write should be snapshot-backed.",
          obj(jstr("layer") + ":{\"type\":\"string\"}"), false);
    entry("restore_snapshot",
          "Restore a layer from a labelled snapshot via temp+rename; reloads the world.",
          obj(jstr("layer") + ":{\"type\":\"string\"},"
              "\"file\":{\"type\":\"string\"}"), false);
    entry("commit_overlay",
          "Converge the lanes: promote live store additions/paints into ai_edits.vxw "
          "(the authored live layer the baker preserves) and clear the runtime "
          "overlay. Pure deletions are refused — use delete_voxels on the baked "
          "layer instead. Returns an id; use stamp_result for the report.",
          obj(""), false);
    entry("add_path",
          "Server-generated path: samples the heightfield PER CELL across the "
          "width, so a cross-slope ridge lifts the right cells. from/to in "
          "metres; refuses the live_edit keep-out unless allow_keepout:true.",
          obj("\"from\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"to\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"width\":{\"type\":\"number\"},\"step\":{\"type\":\"number\"},"
              "\"mat\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"},"
              "\"allow_keepout\":{\"type\":\"boolean\"}"), false);
    entry("add_tree",
          "Deterministic seeded tree (trunk + clumped canopy) generated as a "
          "standalone authored layer. Same seed -> byte-identical cells.",
          obj(anchorSchema + ",\"height\":{\"type\":\"number\"},"
              "\"canopy_r\":{\"type\":\"number\"},\"seed\":{\"type\":\"integer\"},"
              "\"name\":{\"type\":\"string\"}"), false);
    entry("place_copy",
          "The per-layer Import button: translate a layer's records so its "
          "bottom-centre lands on the anchor and append only that copy to "
          "ai_edits (the runtime placement path).",
          obj(jstr("layer") + ":{\"type\":\"string\"}," + anchorSchema), false);
    entry("transform",
          "Two-call transform: commit:false previews the exact transformRecords "
          "placement and AABB without writing; commit:true writes world.json "
          "(the panel's Apply). pos in metres, yaw/pitch/roll in degrees.",
          obj(jstr("layer") + ":{\"type\":\"string\"},"
              "\"pos\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"yaw\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},"
              "\"roll\":{\"type\":\"number\"},\"commit\":{\"type\":\"boolean\"}"), false);
    entry("diff_shots",
          "Pixel-diff two headless --shot PPMs: changed-pixel count above a "
          "threshold (default 12) and mean absolute luma delta. The engine's "
          "TAA noise floor is ~1.25 %.",
          obj(jstr("a") + ":{\"type\":\"string\"}," + jstr("b") +
              ":{\"type\":\"string\"},\"threshold\":{\"type\":\"integer\"}"), false);
    entry("clip_key",
          "Author a bone key in a micro .anim clip (the studio's Add bone key "
          "+ ring drag, exposed). Time is quantised to fps; a new key is seeded "
          "from the existing blend via sampleTrackEuler. Creates and registers "
          "the clip if needed.",
          obj(jstr("layer") + ":{\"type\":\"string\"},"
              "\"bone\":{\"type\":\"integer\"},\"time\":{\"type\":\"number\"},"
              "\"rot\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"ease\":{\"type\":\"string\"},\"clip\":{\"type\":\"string\"}"), false);
    entry("surface_scan",
          "One-call surface map over an x/z rectangle: for each column the top "
          "solid world Y and material, sampled through the live field (so "
          "store edits/overlay are reflected). Replaces dozens of probe calls. "
          "Args: box:[x0,z0,x1,z1] metres (or min/max, or x0/z0/x1/z1), "
          "step:metres (default 0.5, grid capped at 2500 columns).",
          obj("\"box\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"min\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"max\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},"
              "\"x0\":{\"type\":\"number\"},\"z0\":{\"type\":\"number\"},"
              "\"x1\":{\"type\":\"number\"},\"z1\":{\"type\":\"number\"},"
              "\"step\":{\"type\":\"number\"}"), false);
    entry("delete_edits",
          "Snapshot-backed cell-level carve of the live ai_edits layer (the "
          "surgical undo for a bad add). Requires confirm:true; call with "
          "dry_run:true first. Same selector grammar as delete_voxels "
          "(box/min+max/anchor+radius/component, optional mat filter). Leaves "
          "a labelled .bak - revert with restore_snapshot(file:<bak>).",
          obj("\"confirm\":{\"type\":\"boolean\"},"
              "\"dry_run\":{\"type\":\"boolean\"},"
              "\"box\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"min\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"max\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"anchor\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
              "\"radius\":{\"type\":\"number\"},"
              "\"component\":{\"type\":\"boolean\"},"
              "\"mat\":{\"type\":\"integer\"}"), false);
    entry("help", "Dump the full vf_mcp tool catalogue with exact signatures. "
                  "Use instead of searching for tool names.",
          obj(""), false);
    entry("capture_screen",
          "Capture the LIVE running Voxelforge app window from X11 (HUD + "
          "sidebar included) to a PNG. Unlike render_shot (fresh headless "
          "process reading baked files), this shows the user's ACTUAL current "
          "view - use it to verify a hot-reload really landed. Returns the PNG "
          "path; read it with an image viewer. Args: out/path (default "
          "workspace/screenshots/live_<time>.png).",
          obj("\"out\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}"), true);
    t += "] } ";
    return t;
}

ToolResult callTool(Server& srv, const std::string& name, std::string args)
{
    // native tools pass through verbatim; anything else goes through the
    // fuzzy normalizer so LLM bridges can send invented names
    static const char* kNative[] = { "list_layers",   "enable_layer", "probe",
                                      "ground",        "clear_edits",  "add_box",
                                      "add_cylinder",  "add_ellipsoid", "add_stamp",
                                      "add_voxels",    "write_object", "read_object",
                                      "delete_object", "delete_voxels", "import_mesh",
                                      "get_layer", "set_layer_transform", "set_layer_behaviour",
                                      "list_materials", "list_textures", "list_models",
                                      "list_micro", "world_info", "layer_stats",
                                      "duplicate_layer", "rename_layer", "delete_layer",
                                      "list_lights", "add_light", "remove_light",
                                      "set_texture", "inspect_layer", "probe_region",
                                      "reload_world", "render_shot", "inspect_rig",
                                      "set_pose", "fur_stats",
                                      "get_ui", "set_ui", "set_sun",
                                      "list_sceneries", "set_scenery",
                                      "recolor_layer", "update_light",
                                      "describe", "paint", "stamp", "stamp_result",
                                      "snapshot", "restore_snapshot", "commit_overlay",
                                      "add_path", "add_tree", "place_copy", "transform",
                                      "diff_shots", "clip_key",
                                      "surface_scan", "delete_edits", "help",
                                      "capture_screen",
                                      "view_pick", "view_click", "view_undo",
                                      "view_clear", "view_box", "edit_result" };
    std::string canonical = name;
    bool native = false;
    for (const char* n : kNative)
        if (name == n) {
            native = true;
            break;
        }
    if (!native) {
        ai::NormalizedCall n = ai::normalizeToolCall(name, args);
        if (!n.ok)
            return { n.error, true };
        static const std::string prefix = "create_";
        canonical = n.name.compare(0, prefix.size(), prefix) == 0
                        ? "add_" + n.name.substr(prefix.size())
                        : n.name;
        args = n.argsJson; // normalized args may carry injected defaults
    }

    // Read-only tools never touch the layered world: stamp_result reads the
    // intent results file, commit_overlay posts an intent, diff_shots reads
    // PPMs. Rebuilding VoxelField + LayeredWorld for them made a cold
    // `stamp_result` take ~7 s and look like a frozen chat.
    if (canonical != "stamp_result" && canonical != "commit_overlay" &&
        canonical != "diff_shots" && canonical != "view_pick" &&
        canonical != "view_click" && canonical != "view_undo" &&
        canonical != "view_clear" && canonical != "view_box" &&
        canonical != "edit_result") {
        // The world is DATA on disk: re-adopt if the app or an undo changed it
        // since the previous tool call (this server is a long-lived process).
        srv.refreshIfChanged();
    }
    if (canonical == "list_layers")
        return srv.listLayers();
    if (canonical == "enable_layer")
        return srv.enableLayer(args);
    if (canonical == "probe")
        return srv.probe(args);
    if (canonical == "ground")
        return srv.ground(args);
    if (canonical == "surface_scan")
        return srv.surfaceScan(args);
    if (canonical == "help") {
        // Dump the full tool catalogue with exact signatures - removes the
        // discover-by-search overhead that cost several calls per session.
        return { toolSchemas(), false };
    }
    if (canonical == "clear_edits") {
        srv.editable.clear();
        return { "all AI edits cleared; the running app hot-reloads within ~1 s",
                 false };
    }
    if (canonical == "add_voxels")
        return srv.addVoxels(args);
    if (canonical == "write_object")
        return srv.writeObject(args);
    if (canonical == "read_object")
        return srv.readObject(args);
    if (canonical == "delete_object")
        return srv.deleteObject(args);
    if (canonical == "delete_voxels")
        return srv.deleteVoxels(args);
    if (canonical == "delete_edits")
        return srv.deleteEdits(args);
    if (canonical == "import_mesh")
        return srv.importMesh(args);
    if (canonical == "get_layer")
        return srv.getLayer(args);
    if (canonical == "set_layer_transform")
        return srv.setLayerTransform(args);
    if (canonical == "set_layer_behaviour")
        return srv.setLayerBehaviour(args);
    if (canonical == "list_materials")
        return srv.listMaterials();
    if (canonical == "list_textures")
        return srv.listTextures();
    if (canonical == "list_models")
        return srv.listModels();
    if (canonical == "list_micro")
        return srv.listMicro();
    if (canonical == "world_info")
        return srv.worldInfo();
    if (canonical == "layer_stats")
        return srv.layerStats(args);
    if (canonical == "duplicate_layer")
        return srv.duplicateLayer(args);
    if (canonical == "rename_layer")
        return srv.renameLayer(args);
    if (canonical == "delete_layer")
        return srv.deleteLayer(args);
    if (canonical == "list_lights")
        return srv.listLights();
    if (canonical == "add_light")
        return srv.addLight(args);
    if (canonical == "remove_light")
        return srv.removeLight(args);
    if (canonical == "set_texture")
        return srv.setTextureBindingTool(args);
    if (canonical == "inspect_layer")
        return srv.inspectLayer(args);
    if (canonical == "probe_region")
        return srv.probeRegion(args);
    if (canonical == "reload_world")
        return srv.reloadWorldTool(args);
    if (canonical == "render_shot")
        return srv.renderShot(args);
    if (canonical == "capture_screen")
        return srv.captureScreen(args);
    if (canonical == "inspect_rig")
        return srv.inspectRig(args);
    if (canonical == "set_pose")
        return srv.setPose(args);
    if (canonical == "fur_stats")
        return srv.furStats(args);
    if (canonical == "get_ui")
        return srv.getUi();
    if (canonical == "set_ui")
        return srv.setUi(args);
    if (canonical == "set_sun")
        return srv.setSun(args);
    if (canonical == "list_sceneries")
        return srv.listSceneries();
    if (canonical == "set_scenery")
        return srv.setScenery(args);
    if (canonical == "recolor_layer")
        return srv.recolorLayer(args);
    if (canonical == "update_light")
        return srv.updateLight(args);
    if (canonical == "describe")
        return srv.describeCell(args);
    if (canonical == "paint")
        return srv.paintCells(args);
    if (canonical == "stamp")
        return srv.stampTool(args);
    if (canonical == "stamp_result")
        return srv.readResult("stamp", args);
    if (canonical == "edit_result")
        return srv.readResult("edit", args);
    if (canonical == "view_pick")
        return srv.viewTool("pick", args);
    if (canonical == "view_click")
        return srv.viewTool("click", args);
    if (canonical == "view_undo")
        return srv.viewTool("undo", args);
    if (canonical == "view_clear")
        return srv.viewTool("clear", args);
    if (canonical == "view_box")
        return srv.viewTool("box", args);
    if (canonical == "snapshot")
        return srv.snapshotLayer(args, false);
    if (canonical == "restore_snapshot")
        return srv.snapshotLayer(args, true);
    if (canonical == "commit_overlay")
        return srv.postIntent("commit_overlay", args);
    if (canonical == "add_path")
        return srv.buildPath(args);
    if (canonical == "add_tree")
        return srv.buildTree(args);
    if (canonical == "place_copy")
        return srv.placeCopy(args);
    if (canonical == "transform")
        return srv.transformLayer(args);
    if (canonical == "diff_shots")
        return srv.diffShots(args);
    if (canonical == "clip_key")
        return srv.clipKey(args);
    if (canonical.rfind("add_", 0) == 0)
        return srv.addShape(canonical, args);
    return { "unknown tool: " + name, true };
}

} // namespace

int main()
{
    // CRITICAL: stdout is the MCP transport - all logging must go to stderr
    auto errLog = std::make_shared<spdlog::logger>(
        "mcp_stderr", std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>());
    spdlog::set_default_logger(errLog);
    spdlog::set_pattern("%v");
    Server srv;
    srv.init();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        std::string method = jsonGetString(line, "method");
        std::string id = jsonGetId(line);
        bool isNotification = id.empty();
        std::string resultPayload;

        if (method == "initialize") {
            resultPayload =
                "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                "\"serverInfo\":{\"name\":\"voxelforge\",\"version\":\"1.0\"}}";
        } else if (method == "notifications/initialized" ||
                   method.rfind("notifications/", 0) == 0) {
            continue; // notifications: no response
        } else if (method == "ping") {
            resultPayload = "{}";
        } else if (method == "tools/list") {
            resultPayload = toolSchemas();
            while (!resultPayload.empty() && resultPayload.back() == ' ')
                resultPayload.pop_back();
        } else if (method == "tools/call") {
            // params.name / params.arguments
            size_t pp = line.find("\"params\"");
            std::string params = pp == std::string::npos ? "{}" : line.substr(pp);
            std::string name = jsonGetString(params, "name");
            std::string arguments = "{}";
            size_t ap = params.find("\"arguments\"");
            if (ap != std::string::npos) {
                size_t ob = params.find('{', ap);
                if (ob != std::string::npos) {
                    size_t end = jsonObjectEnd(params, ob);
                    arguments = params.substr(ob, end == std::string::npos
                                                     ? std::string::npos
                                                     : end - ob);
                }
            }
            ToolResult tr = callTool(srv, name, arguments);
            resultPayload = "{\"content\":[{\"type\":\"text\"," + jstr("text") + ":" +
                            jstr(tr.text) + "}]";
            resultPayload += tr.isError ? ",\"isError\":true}" : "}";
        } else if (!method.empty()) {
            if (isNotification)
                continue;
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << id
                      << ",\"error\":{\"code\":-32601,\"message\":\"method not "
                         "found\"}}\n"
                      << std::flush;
            continue;
        } else {
            continue; // not JSON-RPC enough to answer
        }

        if (isNotification)
            continue;
        std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":"
                  << resultPayload << "}\n"
                  << std::flush;
    }
    return 0;
}
