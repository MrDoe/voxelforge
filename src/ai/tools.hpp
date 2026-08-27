#pragma once
// Tool-name normalization + argument defaults for the Gemma chat bridge.
//
// Small models frequently invent tool names ("add_rock", "rock_1", "place
// box") or omit arguments. normalizeToolCall() maps any reasonable variant
// onto the six canonical tools and injects conservative defaults so a fuzzy
// call still produces a sensible, visible object instead of dead-ending with
// "unknown tool".
//
// Header-only on purpose: unit-testable without linking ImGui/Ollama.
#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>
#include <cstring>

namespace vf::ai {

struct StampCellLite {
    int dx = 0, dy = 0, dz = 0;
    int mat = 4;
};

struct NormalizedCall {
    std::string name;     // canonical tool name ("" when !ok)
    std::string argsJson; // possibly augmented arguments object
    bool ok = false;
    std::string error;    // human-readable hint when !ok
};

inline const char* kValidTools =
    "create_box, create_cylinder, create_ellipsoid, create_stamp, create_voxels, "
    "write_object, read_object, delete_object, list_world, probe";

// lowercase alnum key with verb prefixes and trailing instance counters stripped:
// "Add_Rock_2" -> "rock", "create-box" -> "box", "LIST_WORLD" -> "listworld"
inline std::string toolKey(const std::string& raw)
{
    std::string k;
    for (char c : raw)
        if (std::isalnum((unsigned char)c))
            k += (char)std::tolower((unsigned char)c);

    static const char* prefixes[] = { "add",   "create", "make",  "place",
                                      "put",   "spawn",  "build", "new" };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* p : prefixes) {
            size_t n = std::strlen(p);
            if (k.size() > n && k.compare(0, n, p) == 0) {
                k = k.substr(n);
                changed = true;
                break;
            }
        }
    }
    while (!k.empty() && std::isdigit((unsigned char)k.back()))
        k.pop_back(); // rock1 -> rock
    return k;
}

// map a fuzzy key onto a canonical tool name; "" when unrecognized
inline std::string canonicalToolName(const std::string& key)
{
    auto has = [&](const char* sub) { return key.find(sub) != std::string::npos; };
    // structural nouns first: "stone_pillar" is a pillar, not a boulder
    if (has("cylinder") || has("pillar") || has("column") || has("post"))
        return "create_cylinder";
    if (has("ellipsoid") || has("sphere") || has("boulder") || has("rock") ||
        has("stone") || has("pebble") || has("boulder"))
        return "create_ellipsoid";
    if (has("stamp") || has("mosaic") || has("pixel"))
        return "create_stamp";
    // listing/probing must win before the "layer"/"object" rules below, since
    // "list_layers" / "worldlist" contain those substrings
    if (has("listworld") || key == "list" || has("layers") || has("worldlist"))
        return "list_world";
    if (key == "probe")
        return "probe";
    // object-layer file ops (read/modify/write/delete standalone .vxw layers)
    if (has("read") || has("inspect") || has("getobject") || has("dump"))
        return "read_object";
    if (has("delete") || has("remove") || has("erase"))
        return "delete_object";
    if (has("layer") || has("saveobject") || has("writelayer") ||
        has("export") || has("model") || has("mesh"))
        return "write_object";
    // fully arbitrary voxel authoring (freeform, vs the fixed primitives)
    if (has("voxel") || has("sculpt") || has("freeform") || has("custom") ||
        has("object"))
        return "create_voxels";
    // box family last: "crate"/"chest"/"block" also contain no earlier keywords
    if (has("box") || has("crate") || has("cube") || has("block") || has("chest") ||
        has("brick"))
        return "create_box";
    return "";
}

// append missing default arguments for aliased calls (extractors are
// find-by-key, so splicing into the object is sufficient)
inline std::string injectToolDefaults(const std::string& canonical,
                                      const std::string& argsJson,
                                      const std::string& lowerRawName)
{
    std::string a = argsJson.empty() ? "{}" : argsJson;
    if (a.find('{') == std::string::npos)
        a = "{}";

    auto need = [&](const char* key) {
        return a.find(std::string("\"") + key + "\"") == std::string::npos;
    };
    std::string missing;
    auto add = [&](const std::string& kv) { missing += "," + kv; };

    int mat = -1; // -1: leave to handler default
    auto has = [&](const char* sub) { return lowerRawName.find(sub) != std::string::npos; };
    // light variants first: "light rock" must win over plain "rock"
    if (has("white") || has("gray") || has("grey") ||
        (has("light") && has("rock")))
        mat = 5;
    else if (has("rock") || has("stone") || has("boulder") || has("pebble"))
        mat = 4;
    else if (has("wood") || has("log") || has("plank") || has("crate") || has("chest"))
        mat = 6;
    else if (has("leaf") || has("foliage") || has("hedge"))
        mat = 8;
    else if (has("sand"))
        mat = 3;
    else if (has("soil") || has("dirt"))
        mat = 2;

    if (canonical == "create_box") {
        if (need("size"))
            add("\"size\":[4,4,4]");
    } else if (canonical == "create_cylinder") {
        if (need("radius"))
            add("\"radius\":0.35");
        if (need("height"))
            add("\"height\":0.9");
    } else if (canonical == "create_ellipsoid") {
        if (need("radius")) {
            if (mat == 4)
                add("\"radius\":[0.6,0.45,0.6]"); // flat-ish boulder
            else
                add("\"radius\":[0.5,0.5,0.5]");
        }
    }
    if (canonical != "list_world" && canonical != "probe" && need("material") && mat >= 0)
        add("\"material\":" + std::to_string(mat));

    if (!missing.empty()) {
        size_t close = a.rfind('}');
        if (close == std::string::npos)
            return a;
        std::string head = a.substr(0, close);
        bool onlyBrace = head.find_first_not_of(" \t\n\r{") == std::string::npos;
        a = head + (onlyBrace ? missing.substr(1) : missing) + "}";
    }
    return a;
}

inline NormalizedCall normalizeToolCall(const std::string& rawName,
                                        const std::string& argsJson)
{
    NormalizedCall n;
    std::string low;
    for (char c : rawName)
        low += (char)std::tolower((unsigned char)c);

    std::string canon = canonicalToolName(toolKey(rawName));
    if (canon.empty()) {
        n.error = std::string("unknown tool '") + rawName +
                  "'. Valid tools: " + kValidTools;
        return n;
    }
    n.ok = true;
    n.name = canon;
    n.argsJson = injectToolDefaults(canon, argsJson, low);
    return n;
}

// parse cells:[{dx,dy,dz,mat},...] from a stamp arguments object.
// Returns false when the array is absent or exceeds maxCells.
inline bool parseStampCells(const std::string& json, std::vector<StampCellLite>& out,
                            size_t maxCells = 1000)
{
    out.clear();
    size_t arr = json.find("[");
    if (arr == std::string::npos)
        return false;
    size_t pos = arr;
    while (out.size() < maxCells) {
        size_t obj = json.find('{', pos);
        if (obj == std::string::npos)
            break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos)
            break;
        std::string cell = json.substr(obj, end - obj + 1);
        auto grab = [&](const char* key) -> int {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = cell.find(pat);
            if (p == std::string::npos)
                return 0;
            p = cell.find(':', p);
            if (p == std::string::npos)
                return 0;
            return std::atoi(cell.c_str() + p + 1);
        };
        StampCellLite c;
        c.dx = grab("dx");
        c.dy = grab("dy");
        c.dz = grab("dz");
        c.mat = grab("mat");
        out.push_back(c);
        pos = end + 1;
    }
    return !out.empty();
}

// --- tiny find-by-key JSON argument extractors ------------------------------
// Shared by the in-game ChatUi tool dispatcher and the MCP server. They are
// deliberately forgiving: whitespace-tolerant, silent on type mismatches.
inline bool jsonGetInt(const std::string& json, const char* key, int& out)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t c = json.find(':', p);
    if (c == std::string::npos)
        return false;
    size_t s = c + 1;
    while (s < json.size() && std::isspace((unsigned char)json[s]))
        ++s;
    size_t e = s;
    while (e < json.size() &&
           (std::isdigit((unsigned char)json[e]) || json[e] == '-' || json[e] == '+'))
        ++e;
    if (e == s)
        return false;
    try {
        out = std::stoi(json.substr(s, e - s));
        return true;
    } catch (...) {
        return false;
    }
}

inline bool jsonGetFloat(const std::string& json, const char* key, float& out)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t c = json.find(':', p);
    if (c == std::string::npos)
        return false;
    size_t s = c + 1;
    while (s < json.size() && std::isspace((unsigned char)json[s]))
        ++s;
    size_t e = s;
    while (e < json.size() &&
           (std::isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == '-' ||
            json[e] == '+' || json[e] == 'e' || json[e] == 'E'))
        ++e;
    if (e == s)
        return false;
    try {
        out = std::stof(json.substr(s, e - s));
        return true;
    } catch (...) {
        return false;
    }
}

inline bool jsonGetIntArray(const std::string& json, const char* key,
                            std::vector<int>& out, size_t expected)
{
    out.clear();
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t a = json.find('[', p);
    size_t b = json.find(']', a);
    if (a == std::string::npos || b == std::string::npos)
        return false;
    std::string inside = json.substr(a + 1, b - a - 1);
    std::string tok;
    for (size_t i = 0; i <= inside.size(); ++i) {
        if (i == inside.size() || inside[i] == ',') {
            if (!tok.empty()) {
                try {
                    out.push_back(std::stoi(tok));
                } catch (...) {
                }
            }
            tok.clear();
        } else if (!std::isspace((unsigned char)inside[i])) {
            tok += inside[i];
        }
    }
    return out.size() == expected;
}

inline bool jsonGetFloatArray(const std::string& json, const char* key,
                              std::vector<float>& out, size_t expected)
{
    out.clear();
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return false;
    size_t a = json.find('[', p);
    size_t b = json.find(']', a);
    if (a == std::string::npos || b == std::string::npos)
        return false;
    std::string inside = json.substr(a + 1, b - a - 1);
    std::string tok;
    for (size_t i = 0; i <= inside.size(); ++i) {
        if (i == inside.size() || inside[i] == ',') {
            if (!tok.empty()) {
                try {
                    out.push_back(std::stof(tok));
                } catch (...) {
                }
            }
            tok.clear();
        } else if (!std::isspace((unsigned char)inside[i])) {
            tok += inside[i];
        }
    }
    return out.size() == expected;
}

} // namespace vf::ai
