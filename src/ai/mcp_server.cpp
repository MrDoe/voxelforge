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
#include "voxel/worldfile.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace vf;

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

    // bottom-center lattice anchor from "anchor":[x,y,z] or "ground":[x,z]
    bool resolveAnchor(const std::string& args, glm::ivec3& out, std::string& err)
    {
        std::vector<int> anc;
        if (ai::jsonGetIntArray(args, "anchor", anc, 3)) {
            out = { anc[0], anc[1], anc[2] };
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
                                                  c.g, c.b, c.refl, c.rough));
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
                                                    c.b, c.refl, c.rough));
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
          "anchor/ground. Each cell: {dx,dy,dz,mat?,r?,g?,b?,refl?,rough?} "
          "(omit r,g,b to use the material palette).",
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
          "absolute voxels:[{x,y,z,mat?,r?,g?,b?,refl?,rough?}]. Registers + "
          "enables in world.json so the app hot-reloads. Modify by read_object "
          "-> edit -> write_object.",
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
    entry("clear_edits", "Remove all session edits", obj(""), true);
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
                                      "delete_object" };
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
