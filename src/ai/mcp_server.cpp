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
#include "voxel/common.hpp"
#include "voxel/layered_world.hpp"
#include "voxel/editable_world.hpp"
#include "voxel/heightmap.hpp"
#include "voxel/mesh_import.hpp"
#include "voxel/mesh_voxel.hpp"
#include "voxel/worldfile.hpp"
#include "voxel/micro_field.hpp"
#include "voxel/micro_rig.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits.h>

using namespace vf;

static const char* kMatNames[22] = {
    "grass dark", "grass light", "soil", "sand", "rock", "light rock", "wood",
    "roof shingles", "foliage", "lava", "ember", "glow cyan", "glow green",
    "glow purple", "glow blue", "white-hot", "snow", "bark", "moss", "thatch",
    "plaster", "fleece"};

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

// raw id token (number or string) or "" when absent (notification)
static std::string jsonGetId(const std::string& line)
{
    size_t p = line.find("\"id\"");
    if (p == std::string::npos)
        return "";
    size_t c = line.find(':', p);
    if (c == std::string::npos)
        return "";
    size_t s = c + 1;
    while (s < line.size() && isspace((unsigned char)line[s]))
        ++s;
    size_t e = s;
    if (s < line.size() && line[s] == '"') {
        ++e;
        while (e < line.size() && line[e] != '"')
            ++e;
        if (e < line.size())
            ++e;
    } else {
        while (e < line.size() && (isdigit((unsigned char)line[e]) || line[e] == '-' ||
                                   line[e] == '+' || line[e] == '.' ||
                                   tolower(line[e]) == 'e'))
            ++e;
    }
    return line.substr(s, e - s);
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
    voxel::EditableWorld editable{ VOXELFORGE_ASSET_DIR };

    void init() { editable.load(); }

    ToolResult listLayers()
    {
        std::vector<voxel::worldfile::WorldLayer> layers;
        std::ostringstream o;
        if (!voxel::worldfile::loadManifest(std::string(VOXELFORGE_ASSET_DIR) + "/world.json",
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
            std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
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

    voxel::LayeredWorld& world()
    {
        static voxel::LayeredWorld w;
        static bool init = false;
        if (!init) {
            w.load(std::string(VOXELFORGE_ASSET_DIR) + "/world.json");
            init = true;
        }
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
                     "rough?},...]",
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
                         "voxels:[{x,y,z,mat?,r?,g?,b?,refl?,rough?},...]",
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
                std::string(VOXELFORGE_ASSET_DIR) + "/" + safe + ".vxw", d))
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
              << ",\"rough\":" << int(v.roughness) << "}\n";
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

    std::string manifestPath() const { return std::string(VOXELFORGE_ASSET_DIR) + "/world.json"; }

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
            if (voxel::worldfile::read(std::string(VOXELFORGE_ASSET_DIR) + "/" + l.file, d))
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
        if (!voxel::worldfile::read(std::string(VOXELFORGE_ASSET_DIR) + "/" + file, d))
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
        std::ifstream in(std::string(VOXELFORGE_ASSET_DIR) + "/" + srcFile, std::ios::binary);
        if (!in)
            return { "cannot read source layer: " + srcFile, true };
        std::ofstream out(std::string(VOXELFORGE_ASSET_DIR) + "/" + dstFile, std::ios::binary);
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
        std::string oldPath = std::string(VOXELFORGE_ASSET_DIR) + "/" + oldFile;
        std::string newPath = std::string(VOXELFORGE_ASSET_DIR) + "/" + newFile;
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
        std::vector<voxel::worldfile::TextureBinding> bindings;
        voxel::worldfile::loadTextureManifest(manifestPath(), bindings);
        // remove existing binding for this mat
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
            bindings.push_back(b);
        }
        if (!voxel::worldfile::writeTextureManifest(manifestPath(), bindings))
            return { "failed to write textures", true };
        return { "texture binding updated for material " + std::to_string(mat) +
                     (file.empty() ? " (palette fallback)" : " <- " + file) +
                     "; the running app hot-reloads within ~1 s",
                 false };
    }

    // ----- reload cached world ----------------------------------------------
    void reloadWorld() { world().load(manifestPath()); }

    ToolResult reloadWorldTool(const std::string& /*args*/)
    {
        reloadWorld();
        return { "world reloaded from " + manifestPath(), false };
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
        if (out.empty())
            out = "/tmp/opencode/mcp_shot.ppm";

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
        if (!voxel::worldfile::read(std::string(VOXELFORGE_ASSET_DIR) + "/" + file, d))
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
        std::string fullPath = std::string(VOXELFORGE_ASSET_DIR) + "/" + file;
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
          "atlas layer 1..255 overriding the material's slot).",
          obj(anchorSchema + "," + cellSchema), false);
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
    entry("fur_stats", "Dump fur card counts per micro object (from .opencode/fur_stats.json)", obj(""), true);
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
                                      "delete_object", "import_mesh",
                                      "get_layer", "set_layer_transform", "set_layer_behaviour",
                                      "list_materials", "list_textures", "list_models",
                                      "list_micro", "world_info", "layer_stats",
                                      "duplicate_layer", "rename_layer", "delete_layer",
                                      "list_lights", "add_light", "remove_light",
                                      "set_texture", "inspect_layer", "probe_region",
                                      "reload_world", "render_shot", "inspect_rig",
                                      "set_pose", "fur_stats" };
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
    if (canonical == "list_layers")
        return srv.listLayers();
    if (canonical == "enable_layer")
        return srv.enableLayer(args);
    if (canonical == "probe")
        return srv.probe(args);
    if (canonical == "ground")
        return srv.ground(args);
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
    if (canonical == "inspect_rig")
        return srv.inspectRig(args);
    if (canonical == "set_pose")
        return srv.setPose(args);
    if (canonical == "fur_stats")
        return srv.furStats(args);
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
