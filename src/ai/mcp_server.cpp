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

// --- tool implementations ----------------------------------------------------
namespace {

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

    ToolResult probe(const std::string& args)
    {
        float x = 0, y = 0, z = 0;
        ai::jsonGetFloat(args, "x", x);
        ai::jsonGetFloat(args, "y", y);
        ai::jsonGetFloat(args, "z", z);
        auto s = voxel::scene({ x, y, z });
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
        const voxel::HeightMap& hm = voxel::sharedHeightmap();
        float H = hm.sample(x, z);
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
            const voxel::HeightMap& hm = voxel::sharedHeightmap();
            float H = hm.sample(g[0], g[1]);
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
        mat = std::clamp(mat, 0, 8);

        std::vector<voxel::VoxelRecord> recs;
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
                                uint8_t(std::clamp(c.mat, 0, 8)) });
            recs = editable.makeStamp(anchor, scs);
        } else {
            return { "unknown add-tool", true };
        }

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
    entry("add_cylinder", "Add a vertical cylinder (radius/height in meters)",
          obj(anchorSchema + ",\"radius\":{\"type\":\"number\"},"
              "\"height\":{\"type\":\"number\"}"), false);
    entry("add_ellipsoid", "Add an ellipsoid/boulder (radius meters)",
          obj(anchorSchema + ",\"radius\":{\"type\":\"array\","
              "\"items\":{\"type\":\"number\"}}"), false);
    entry("add_stamp", "Add literal 0.1m voxel art relative to anchor",
          obj(anchorSchema + ",\"cells\":{\"type\":\"array\"}"), false);
    entry("clear_edits", "Remove all session edits", obj(""), true);
    t += "] } ";
    return t;
}

ToolResult callTool(Server& srv, const std::string& name, std::string args)
{
    // native tools pass through verbatim; anything else goes through the
    // fuzzy normalizer so LLM bridges can send invented names
    static const char* kNative[] = { "list_layers",  "enable_layer", "probe",
                                     "ground",       "clear_edits",  "add_box",
                                     "add_cylinder", "add_ellipsoid", "add_stamp" };
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
            resultPayload = toolSchemas().substr(0); // trailing space trimmed below
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
                    int depth = 0;
                    size_t e = ob;
                    for (; e < params.size(); ++e) {
                        if (params[e] == '{')
                            ++depth;
                        else if (params[e] == '}' && --depth == 0) {
                            ++e;
                            break;
                        }
                    }
                    arguments = params.substr(ob, e - ob);
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
