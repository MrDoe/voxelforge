// App-side consumer for vf_mcp's live-lane intents.
//
// vf_mcp is a separate stdio process with no ChunkStore, LiveEditor, GPU pass
// or undo stack. It must NOT link the store code and must NEVER write
// runtime_edits.vxw. Instead it appends a JSON line to
// scenery/<name>/mcp_intents.jsonl; this file executes it between frames via
// the App's real input paths — App::pickAtWindowPos for a click in the view,
// App::applyEditLive for the five cell brushes, and the same commit/undo/
// clear functions the sidebar buttons call. Undo, surfel and SVO patches, the
// height texture, water and the async overlay save all come along for free,
// so GUI and MCP cannot drift into two truths.
#include "app/app.hpp"
#include "ai/tools.hpp"
#include "app/ui/ui_primitives.hpp"
#include "app/world/store_overlay.hpp"
#include "voxel/scenery.hpp"
#include "voxel/chunk_index.hpp"
#include "voxel/picking.hpp"
#include "voxel/common.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace vf {
namespace app {

namespace {

std::string jsonStr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o + "\"";
}

std::string jsonNum(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string jsonInts(const std::vector<int>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        o += std::to_string(v[i]);
    }
    return o + "]";
}

std::string jsonFloats(const std::vector<float>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        o += jsonNum(v[i]);
    }
    return o + "]";
}

// The intent args come from an LLM as JSON booleans; ai/tools.hpp has no
// bool getter, so read the literal (with an optional space after the colon).
bool jsonBool(const std::string& json, const char* key, bool fallback)
{
    const std::string pat = std::string("\"") + key + "\"";
    const size_t p = json.find(pat);
    if (p == std::string::npos) return fallback;
    size_t c = json.find(':', p + pat.size());
    if (c == std::string::npos) return fallback;
    ++c;
    while (c < json.size() && std::isspace(static_cast<unsigned char>(json[c]))) ++c;
    if (json.compare(c, 4, "true") == 0) return true;
    if (json.compare(c, 5, "false") == 0) return false;
    return fallback;
}

} // namespace

void App::consumeMcpIntents()
{
    const std::string dir = voxel::scenery::root();
    const std::string qpath = dir + "/mcp_intents.jsonl";
    const std::string rpath = dir + "/mcp_results.jsonl";
    std::ifstream q(qpath);
    if (!q)
        return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(q, line))
        if (!line.empty())
            lines.push_back(line);
    q.close();
    if (lines.empty())
        return;

    // Rewrite the queue to empty BEFORE executing: a crash mid-stamp leaves an
    // intent unacknowledged rather than a duplicate replay on the next launch.
    {
        std::ofstream trunc(qpath, std::ios::trunc);
        if (!trunc) {
            spdlog::warn("mcp intents: cannot truncate queue - not executing");
            return;
        }
    }

    // Save the whole input state. An MCP edit must not leave the GUI brush
    // armed at a different mode/size, or move the hover outline. (Transform
    // intents are the exception: their staged pose IS their visible result.)
    auto snapshotInput = [this]() {
        struct Saved {
            decltype(m_hoverHit) hover;
            decltype(m_editBrush) brush;
            float diameter = 0.f, depth = 0.f, strength = 0.f;
            uint8_t mat = 0;
            vf::voxel::EditableWorld::FalloffCurve curve;
            decltype(m_boxSelect) box;
            bool dragging = false;
        };
        return Saved{ m_hoverHit, m_editBrush, m_editDiameter, m_editDepth,
                      m_smoothStrength, m_editMat, m_editFalloffCurve,
                      m_boxSelect, m_dragging };
    };
    auto restoreInput = [this](const auto& s) {
        m_hoverHit = s.hover;
        m_editBrush = s.brush;
        m_editDiameter = s.diameter;
        m_editDepth = s.depth;
        m_smoothStrength = s.strength;
        m_editMat = s.mat;
        m_editFalloffCurve = s.curve;
        m_boxSelect = s.box;
        m_dragging = s.dragging;
    };

    auto result = [](const std::string& id, const std::string& tool, bool ok,
                     const std::string& extra) {
        return "{\"id\":" + jsonStr(id) + ",\"tool\":" + jsonStr(tool) +
               ",\"ok\":" + (ok ? "true" : "false") + extra + "}";
    };
    auto errorResult = [&result](const std::string& id, const std::string& tool,
                                 const std::string& msg) {
        return result(id, tool, false, ",\"error\":" + jsonStr(msg));
    };

    // Resolve a click target. "screen:[x,y]" is the real view click: the
    // app's own screen->ray conversion, store march and micro-vs-world
    // competition (pickAtWindowPos). A cell/world triple stays supported as
    // the deterministic, camera-independent form used by the existing gate.
    auto resolveTarget = [this](const std::string& args, voxel::PickHit& hit,
                                std::string& err) -> bool {
        std::vector<float> scr;
        if (ai::jsonGetFloatArray(args, "screen", scr, 2) ||
            ai::jsonGetFloatArray(args, "pixel", scr, 2)) {
            hit = pickAtWindowPos(scr[0], scr[1]);
            if (!hit.hit) {
                err = "screen click at (" + jsonNum(scr[0]) + "," +
                      jsonNum(scr[1]) + ") missed all geometry";
                return false;
            }
            return true;
        }
        std::vector<int> c;
        if (ai::jsonGetIntArray(args, "cell", c, 3)) {
            const glm::ivec3 v(c[0], c[1], c[2]);
            hit = {};
            hit.hit = true;
            hit.voxel = v;
            hit.pos = voxel::voxelCenter(v);
            hit.normal = storeNormalAt(v);
            if (m_layers.loaded()) {
                const auto smp = m_layers.field().sampleWorld(hit.pos);
                hit.mat = smp.mat;
                hit.object = smp.obj;
                hit.layer = smp.layer;
            }
            return true;
        }
        float x = 0.f, y = 0.f, z = 0.f;
        if (ai::jsonGetFloat(args, "x", x) && ai::jsonGetFloat(args, "y", y) &&
            ai::jsonGetFloat(args, "z", z)) {
            const glm::ivec3 v = voxel::worldToVoxel({ x, y, z });
            hit = {};
            hit.hit = true;
            hit.voxel = v;
            hit.pos = voxel::voxelCenter(v);
            hit.normal = storeNormalAt(v);
            if (m_layers.loaded()) {
                const auto smp = m_layers.field().sampleWorld(hit.pos);
                hit.mat = smp.mat;
                hit.object = smp.obj;
                hit.layer = smp.layer;
            }
            return true;
        }
        err = "need screen:[x,y] (a click in the view), cell:[x,y,z], or x/y/z in metres";
        return false;
    };

    // The pick report doubles as the result for view_pick and the extra
    // context on a click. An owner id is resolved through the live layer map
    // so "what did I click" names the same .vxw the GUI would name.
    auto pickFields = [this](const voxel::PickHit& hit) {
        std::string owner;
        if (!hit.microFile.empty()) owner = hit.microFile;
        else if (hit.layer) owner = m_layers.layerFile(hit.layer);
        const std::vector<int> cell = { hit.voxel.x, hit.voxel.y, hit.voxel.z };
        const std::vector<float> wp = { hit.pos.x, hit.pos.y, hit.pos.z };
        return std::string(",\"hit\":") + (hit.hit ? "true" : "false") +
               ",\"cell\":" + jsonInts(cell) +
               ",\"world\":" + jsonFloats(wp) +
               ",\"material\":" + std::to_string(unsigned(hit.mat)) +
               ",\"owner\":" + jsonStr(owner) +
               ",\"dist\":" + jsonNum(hit.dist) +
               ",\"pick\":" + jsonStr(hit.object ? "object" : "terrain") +
               ",\"micro\":" + jsonStr(hit.microFile);
    };

    // One cell-stamp, routed through applyEditLive exactly as the GUI brush is.
    auto applyStamp = [this, &result, &errorResult, &snapshotInput,
                       &restoreInput, &pickFields](
        const std::string& id, const std::string& tool,
        const std::string& args, const voxel::PickHit& hit) -> std::string {
        std::string mode;
        ai::jsonGetString(args, "mode", mode);
        if (mode.empty()) ai::jsonGetString(args, "brush", mode);
        const bool validMode = mode == "carve" || mode == "add" ||
                               mode == "delete" || mode == "paint" ||
                               mode == "smooth";
        if (!validMode)
            return errorResult(id, tool, "needs mode: carve|add|delete|paint|smooth");
        if (!hit.microFile.empty())
            return errorResult(id, tool,
                "the click hit micro object '" + hit.microFile +
                "'; world brushes cannot edit a micro world");

        auto saved = snapshotInput();
        auto& store = m_layers.store();
        const glm::ivec3 cell = hit.voxel;
        const bool solidBefore = store.cellAt(cell.x, cell.y, cell.z).solid;
        const size_t undoBefore = m_undo.size();

        m_hoverHit = {};
        m_hoverHit.hit = true;
        m_hoverHit.voxel = cell;
        m_hoverHit.normal = storeNormalAt(cell);
        m_hoverHit.object = hit.object;
        m_hoverHit.layer = hit.layer;
        adoptPickOwnership(cell);
        m_editBrush = brushFromName(mode.c_str());
        m_editDiameter = 2.f;
        m_editDepth = 1.5f;
        m_smoothStrength = 0.65f;
        float v = 0.f;
        if (ai::jsonGetFloat(args, "diameter", v)) m_editDiameter = v;
        if (ai::jsonGetFloat(args, "depth", v)) m_editDepth = v;
        if (ai::jsonGetFloat(args, "strength", v)) m_smoothStrength = v;
        int mat = -1;
        if (ai::jsonGetInt(args, "mat", mat) && mat >= 0 &&
            mat < int(voxel::kPaletteN))
            m_editMat = uint8_t(mat);
        quantiseBrush(); // 1 voxel stays exactly 1 cell (the documented trap)
        std::vector<int> box;
        if (ai::jsonGetIntArray(args, "box_scope", box, 6))
            m_boxSelect.set({ box[0], box[1], box[2] }, { box[3], box[4], box[5] });
        else
            m_boxSelect.clear();
        m_dragging = false; // a lone click is one whole stroke / undo step

        m_lastChangedChunks.clear();
        m_lastEditCells = 0;
        m_lastEditChunks = 0;
        m_lastEditSurfels = 0;
        m_lastEditMs = 0.f;
        applyEditLive();
        const bool solidAfter = store.cellAt(cell.x, cell.y, cell.z).solid;
        // The GUI queues persistence on mouse release. An MCP stamp has no
        // mouse release, so do that explicitly or the edit vanishes on exit.
        if (m_lastEditCells > 0 && m_liveEditor.attached())
            queueOverlaySave();

        std::string extra =
            std::string(",\"mode\":") + jsonStr(mode) +
            ",\"cells\":" + jsonNum(m_lastEditCells) +
            ",\"chunks\":" + jsonNum(m_lastEditChunks) +
            ",\"runSurfels\":" + jsonNum(m_lastEditSurfels) +
            ",\"ms\":" + jsonNum(m_lastEditMs) +
            ",\"pick\":" + jsonStr(m_lastPickObject > 0 ? "object" : "terrain") +
            ",\"undoSteps\":" + jsonNum(m_undo.size() - undoBefore) +
            ",\"cellSolidBefore\":" + jsonNum(solidBefore ? 1 : 0) +
            ",\"cellSolidAfter\":" + jsonNum(solidAfter ? 1 : 0) + pickFields(hit);
        spdlog::info("mcp {} {}: {} cell(s), {} chunk(s), {} run surfels, "
                     "{:.1f} ms", tool, id, m_lastEditCells, m_lastEditChunks,
                     m_lastEditSurfels, m_lastEditMs);
        restoreInput(saved);
        return result(id, tool, true, extra);
    };

    // One natural cave, composed from the SAME brushes the GUI uses. Every
    // sub-stamp runs with m_dragging=true, so the mouth, tunnel, chamber and
    // optional dripstone share ONE undo step. The stdio server only validates
    // and posts this intent; all picking and rasterisation stays in the App.
    auto applyCave = [this, &result, &errorResult, &snapshotInput,
                      &restoreInput, &pickFields](
        const std::string& id, const std::string& args,
        const voxel::PickHit& hit) -> std::string {
        if (!hit.microFile.empty())
            return errorResult(id, "dig_cave",
                "the click hit micro object '" + hit.microFile +
                "'; world brushes cannot edit a micro world");

        float radius = 1.0f, depth = 3.0f;
        ai::jsonGetFloat(args, "radius", radius);
        ai::jsonGetFloat(args, "depth", depth);
        if (!std::isfinite(radius) || !std::isfinite(depth) ||
            radius < 0.3f || radius > 2.5f || depth < 0.5f || depth > 10.f)
            return errorResult(id, "dig_cave", "radius 0.3..2.5 m, depth 0.5..10 m");
        std::string style;
        if (!ai::jsonGetString(args, "style", style) || style.empty())
            style = "dripstone";
        if (style == "simple") style = "plain";
        if (style != "dripstone" && style != "plain")
            return errorResult(id, "dig_cave", "style: dripstone|plain");

        auto& store = m_layers.store();
        const glm::ivec3 cell = hit.voxel;
        if (!store.cellAt(cell.x, cell.y, cell.z).solid)
            return errorResult(id, "dig_cave", "selected cell [" +
                std::to_string(cell.x) + "," + std::to_string(cell.y) + "," +
                std::to_string(cell.z) + "] is not solid");
        if (m_dragging || !m_undoPending.empty())
            return errorResult(id, "dig_cave", "finish the current brush stroke first");

        glm::vec3 n = hit.normal;
        if (glm::length(n) < 1e-3f) n = glm::vec3(0.f, 1.f, 0.f);
        n = glm::normalize(n);
        const glm::vec3 inward = -n;
        const glm::vec3 centreW = voxel::voxelCenter(cell) + inward * depth;
        const glm::ivec3 chamber = voxel::worldToVoxel(centreW);
        const int latN = store.latN();
        if (chamber.x < 0 || chamber.y < 0 || chamber.z < 0 ||
            chamber.x >= latN || chamber.y >= latN || chamber.z >= latN)
            return errorResult(id, "dig_cave", "cave chamber leaves the world lattice");
        if (centreW.y - radius < voxel::WATER_LEVEL + 0.2f)
            return errorResult(id, "dig_cave", "cave at [" +
                std::to_string(cell.x) + "," + std::to_string(cell.y) + "," +
                std::to_string(cell.z) + "] normal [" + jsonNum(n.x) + "," +
                jsonNum(n.y) + "," + jsonNum(n.z) + "] would breach the water "
                "plane (chamber bottom " + jsonNum(centreW.y - radius) + " m)");

        // Conservative unique-cell estimate keeps the whole feature inside
        // the GUI's 250k-cell undo budget (overlap only makes this smaller).
        const double v = voxel::VOXEL;
        const double estimate =
            (4.18879020479 * (std::pow(radius, 3) +
                              std::pow(radius * 0.75, 3)) +
             3.14159265359 * std::pow(radius * 0.45, 2) * depth) /
                (v * v * v) + 2000.0;
        if (estimate > double(kUndoMaxCells) * 0.8)
            return errorResult(id, "dig_cave", "cave is too large for one undo step");

        auto saved = snapshotInput();
        const bool solidBefore = true;
        const size_t undoBefore = m_undo.size();
        const auto t0 = std::chrono::steady_clock::now();
        m_boxSelect.clear();
        m_editMat = 4; // rock for the dripstone
        m_editFalloffCurve = voxel::EditableWorld::FalloffCurve::Constant;
        m_lastChangedChunks.clear();
        m_dragging = true; // accumulate one stroke across every sub-stamp
        int stamps = 0;
        double firstStampMs = -1.0;

        auto caveStamp = [&](const glm::ivec3& at, const glm::vec3& normal,
                             EditBrush brush, float diameter, float length,
                             voxel::EditableWorld::FalloffCurve curve) {
            const auto stampStart = std::chrono::steady_clock::now();
            m_hoverHit = {};
            m_hoverHit.hit = true;
            m_hoverHit.voxel = at;
            m_hoverHit.pos = voxel::voxelCenter(at);
            m_hoverHit.normal = normal;
            m_hoverHit.object = store.cellAt(at.x, at.y, at.z).obj;
            m_editBrush = brush;
            m_editDiameter = diameter;
            m_editDepth = length;
            m_editFalloffCurve = curve;
            quantiseBrush();
            applyEditLive();
            if (stamps == 0)
                firstStampMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - stampStart).count();
            ++stamps;
        };

        // Open mouth, narrow inward tunnel, then a wider chamber.
        caveStamp(cell, n, EditBrush::Delete, radius * 1.5f, 0.1f,
                  voxel::EditableWorld::FalloffCurve::Constant);
        caveStamp(cell, n, EditBrush::Carve, radius * 0.9f, depth,
                  voxel::EditableWorld::FalloffCurve::Constant);
        caveStamp(chamber, n, EditBrush::Delete, radius * 2.0f, 0.1f,
                  voxel::EditableWorld::FalloffCurve::Constant);

        if (style == "dripstone") {
            // Deterministic pairs on the chamber ceiling/floor. The first
            // solid cell met while scanning out from the carved centre is the
            // boundary; Add grows back into the air along the opposite axis.
            static const float kOffsets[5][2] = {
                {0.f, 0.f}, {0.45f, 0.f}, {-0.45f, 0.f},
                {0.f, 0.45f}, {0.f, -0.45f}
            };
            const int reach = int(std::ceil(radius / voxel::VOXEL)) + 2;
            for (int i = 0; i < 5; ++i) {
                const int x = std::clamp(
                    chamber.x + int(std::lround(kOffsets[i][0] * radius / voxel::VOXEL)),
                    0, latN - 1);
                const int z = std::clamp(
                    chamber.z + int(std::lround(kOffsets[i][1] * radius / voxel::VOXEL)),
                    0, latN - 1);
                glm::ivec3 ceiling, floor;
                bool haveCeiling = false, haveFloor = false;
                for (int dy = 0; dy <= reach; ++dy) {
                    const int yu = chamber.y + dy, yd = chamber.y - dy;
                    if (!haveCeiling && yu >= 0 && yu < latN &&
                        store.cellAt(x, yu, z).solid) {
                        ceiling = { x, yu, z };
                        haveCeiling = true;
                    }
                    if (!haveFloor && yd >= 0 && yd < latN &&
                        store.cellAt(x, yd, z).solid) {
                        floor = { x, yd, z };
                        haveFloor = true;
                    }
                    if (haveCeiling && haveFloor) break;
                }
                const float diameter = 0.2f + 0.05f * float(i % 3);
                const float length = 0.3f + 0.1f * float(i % 4);
                const auto sharp = voxel::EditableWorld::FalloffCurve::Sharp;
                if (haveCeiling)
                    caveStamp(ceiling, glm::vec3(0.f, -1.f, 0.f), EditBrush::Add,
                              diameter, length, sharp);
                if (haveFloor)
                    caveStamp(floor, glm::vec3(0.f, 1.f, 0.f), EditBrush::Add,
                              diameter, length, sharp);
            }
        }

        std::vector<int> changed = m_lastChangedChunks;
        std::sort(changed.begin(), changed.end());
        changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        const size_t touched = m_undoPending.size();
        const bool undoable = !m_undoOverflow && touched > 0;
        m_dragging = false;
        finishStroke();
        if (touched > 0 && m_liveEditor.attached())
            queueOverlaySave();

        // Final run sizes across the touched chunks (not a sum over sub-stamps,
        // which would count a chunk several times).
        const vf::voxel::SurfelParams sp = surfelParams(false);
        size_t runSurfels = 0;
        for (int ci : changed)
            runSurfels += m_liveEditor.chunkRun(ci, sp).size();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        const bool solidAfter = store.cellAt(cell.x, cell.y, cell.z).solid;
        m_lastEditCells = touched;
        m_lastEditChunks = changed.size();
        m_lastEditSurfels = runSurfels;
        m_lastEditMs = float(ms);
        restoreInput(saved);

        spdlog::info("mcp dig_cave {}: style={} r={:.2f} depth={:.2f}, {} stamps, "
                     "{} cells, {} chunks, {} run surfels, {:.1f} ms, undo={}",
                     id, style, radius, depth, stamps, touched, changed.size(),
                     runSurfels, ms, undoable);
        if (touched == 0)
            return errorResult(id, "dig_cave", "cave changed no cells");
        return result(id, "dig_cave", true,
            ",\"style\":" + jsonStr(style) +
            ",\"radius\":" + jsonNum(radius) +
            ",\"depth\":" + jsonNum(depth) +
            ",\"stamps\":" + jsonNum(stamps) +
            ",\"firstStampMs\":" + jsonNum(firstStampMs) +
            ",\"cells\":" + jsonNum(touched) +
            ",\"chunks\":" + jsonNum(changed.size()) +
            ",\"runSurfels\":" + jsonNum(runSurfels) +
            ",\"ms\":" + jsonNum(ms) +
            ",\"undoable\":" + (undoable ? "true" : "false") +
            ",\"undoSteps\":" + jsonNum(m_undo.size() - undoBefore) +
            ",\"cellSolidBefore\":" + jsonNum(solidBefore ? 1 : 0) +
            ",\"cellSolidAfter\":" + jsonNum(solidAfter ? 1 : 0) +
            ",\"persisted\":true" + pickFields(hit));
    };

    // Rotate / Move reuse the sidebar's Apply functions, including the
    // micro-object fast path. An explicit "layer" overrides the object under
    // the click (which is handy when the click landed on a thin limb).
    auto applyTransform = [this, &result, &errorResult](
        const std::string& id, const std::string& tool,
        const std::string& args, const voxel::PickHit& hit,
        bool rotate) -> std::string {
        std::string layer;
        ai::jsonGetString(args, "layer", layer);
        if (layer.empty()) layer = rotateTargetLayer(hit);
        if (layer.empty())
            return errorResult(id, tool,
                "no object under the click and no \"layer\" given");
        auto it = std::find_if(m_worldLayers.begin(), m_worldLayers.end(),
            [&](const vf::voxel::worldfile::WorldLayer& l) {
                return l.file == layer || l.name == layer ||
                       l.file == layer + ".vxw";
            });
        if (it == m_worldLayers.end())
            return errorResult(id, tool, "layer '" + layer + "' is not in the manifest");
        const bool commit = jsonBool(args, "commit", true);
        // The commit functions look the layer up by its manifest FILE; an
        // agent may have named it by its display name, so normalise here.
        const std::string target = it->file;

        if (rotate) {
            float y = 0.f, p = 0.f, r = 0.f;
            if (!ai::jsonGetFloat(args, "yaw", y)) ai::jsonGetFloat(args, "rot", y);
            if (!ai::jsonGetFloat(args, "pitch", p)) ai::jsonGetFloat(args, "rotX", p);
            if (!ai::jsonGetFloat(args, "roll", r)) ai::jsonGetFloat(args, "rotZ", r);
            if (std::abs(y) > 3600.f || std::abs(p) > 3600.f || std::abs(r) > 3600.f)
                return errorResult(id, tool, "yaw/pitch/roll must be within +-3600 degrees");
            m_rotateLayer = target;
            m_rotateDy = y;
            m_rotateDx = p;
            m_rotateDz = r;
            m_rotationStaged = true;
            if (commit) {
                commitRotation();
            } else {
                m_editActive = true;
                m_panel = Panel::Edit;
                m_editBrush = EditBrush::Rotate;
                m_rotating = false;
                m_rotateHandle = TrackballHandle::None;
                m_taaFirstFrame = true;
            }
            const std::vector<float> delta = { y, p, r };
            const std::vector<float> placed = { it->rotDeg, it->rotX, it->rotZ };
            return result(id, tool, true,
                ",\"layer\":" + jsonStr(target) +
                ",\"delta\":" + jsonFloats(delta) +
                ",\"placed\":" + jsonFloats(placed) +
                ",\"committed\":" + (commit ? "true" : "false"));
        }

        float dx = 0.f, dy = 0.f, dz = 0.f;
        if (!ai::jsonGetFloat(args, "dx", dx)) ai::jsonGetFloat(args, "x_move", dx);
        if (!ai::jsonGetFloat(args, "dy", dy)) ai::jsonGetFloat(args, "y_move", dy);
        if (!ai::jsonGetFloat(args, "dz", dz)) ai::jsonGetFloat(args, "z_move", dz);
        if (std::abs(dx) > 100.f || std::abs(dy) > 100.f || std::abs(dz) > 100.f)
            return errorResult(id, tool, "move delta must be within +-100 metres");
        m_moveLayer = target;
        m_moveDelta = { dx, dy, dz };
        m_moveStaged = true;
        const float ax = std::abs(dx), ay = std::abs(dy), az = std::abs(dz);
        m_moveAxis = (ax >= ay && ax >= az) ? MoveAxis::X
                   : (ay >= az) ? MoveAxis::Y : MoveAxis::Z;
        if (commit) {
            commitMove();
        } else {
            m_editActive = true;
            m_panel = Panel::Edit;
            m_editBrush = EditBrush::Move;
            m_moving = false;
            m_taaFirstFrame = true;
        }
        const std::vector<float> delta = { dx, dy, dz };
        const std::vector<float> placed = { it->pos[0], it->pos[1], it->pos[2] };
        return result(id, tool, true,
            ",\"layer\":" + jsonStr(target) +
            ",\"delta\":" + jsonFloats(delta) +
            ",\"placed\":" + jsonFloats(placed) +
            ",\"committed\":" + (commit ? "true" : "false"));
    };

    std::vector<std::string> results;
    for (const std::string& ln : lines) {
        std::string id, tool;
        ai::jsonGetString(ln, "id", id);
        ai::jsonGetString(ln, "tool", tool);
        // The args object is nested; jsonGetString/Int parse it fine because
        // they scan for the first occurrence of the key anywhere in the line.
        const std::string args = ln;
        std::string res;

        if (tool == "commit_overlay") {
            auto& store = m_layers.store();
            const std::vector<int> chunks = store.editedChunks();
            int additions = 0, deletions = 0;
            std::vector<voxel::VoxelRecord> recs;
            for (int ci : chunks) {
                int lo[3], hi[3];
                if (!store.chunkEditBounds(ci, lo, hi))
                    continue;
                for (int z = lo[2]; z <= hi[2]; ++z)
                    for (int y = lo[1]; y <= hi[1]; ++y)
                        for (int x = lo[0]; x <= hi[0]; ++x) {
                            const auto c = store.cellAt(x, y, z);
                            const auto wp = voxel::voxelCenter({ x, y, z });
                            const auto baked = m_layers.field().sampleWorld(wp);
                            const bool bakedSolid = baked.d <= 0.f;
                            if (!c.solid) {
                                if (bakedSolid)
                                    ++deletions; // record layers cannot express air
                                continue;
                            }
                            if (bakedSolid && int(baked.mat) == int(c.mat))
                                continue;
                            ++additions;
                            recs.push_back(voxel::makeVoxelRecord(
                                x, y, z, c.mat, c.r, c.g, c.b,
                                c.reflectivity, c.roughness, c.tags));
                        }
            }
            if (deletions > 0) {
                res = "{\"id\":" + jsonStr(id) + ",\"ok\":false,\"error\":" +
                      jsonStr("commit_overlay refuses " + std::to_string(deletions) +
                              " deleted cell(s): record layers cannot express air. "
                              "Use delete_voxels on the baked layer.") +
                      ",\"additions\":" + jsonNum(additions) + "}";
                results.push_back(res);
                continue;
            }
            // Re-read from disk first: vf_mcp may have appended ai_edits records
            // (add_voxels / place_copy) while this app process was running.
            m_editable.load();
            const size_t added = m_editable.append(recs);
            m_editable.save();
            clearLiveEdits(); // flush writer, drop overlay, reload from baked pools
            res = "{\"id\":" + jsonStr(id) + ",\"tool\":\"commit_overlay\",\"ok\":true" +
                  ",\"chunks\":" + jsonNum(chunks.size()) +
                  ",\"additions\":" + jsonNum(additions) +
                  ",\"appended\":" + jsonNum(added) +
                  ",\"deletions\":0}";
            results.push_back(res);
            spdlog::info("mcp commit_overlay {}: {} chunks, {} additions ({} new "
                         "records) promoted into ai_edits.vxw", id, chunks.size(),
                         additions, added);
            continue;
        }

        if (tool == "undo") {
            finishStroke();
            const size_t before = m_undo.size();
            undoEdit();
            results.push_back(result(id, tool, true,
                ",\"undone\":" + jsonNum(before - m_undo.size()) +
                ",\"remaining\":" + jsonNum(m_undo.size())));
            continue;
        }
        if (tool == "clear") {
            clearLiveEdits();
            results.push_back(result(id, tool, true,
                ",\"remaining\":" + jsonNum(m_undo.size())));
            continue;
        }
        if (tool == "box") {
            std::vector<int> box;
            if (jsonBool(args, "clear", false) ||
                !ai::jsonGetIntArray(args, "box", box, 6)) {
                clearBoxSelection();
                results.push_back(result(id, tool, true, ",\"cleared\":true"));
            } else {
                m_boxSelect.set({ box[0], box[1], box[2] },
                                { box[3], box[4], box[5] });
                results.push_back(result(id, tool, true,
                    ",\"cells\":" + jsonNum(m_boxSelect.volumeCells()) +
                    ",\"lo\":" + jsonInts({ m_boxSelect.lo.x, m_boxSelect.lo.y,
                                            m_boxSelect.lo.z }) +
                    ",\"hi\":" + jsonInts({ m_boxSelect.hi.x, m_boxSelect.hi.y,
                                            m_boxSelect.hi.z })));
            }
            continue;
        }

        if (tool == "camera_pose") {
            // Read-only live-pose probe for vf_mcp. The camera is the App's
            // own (a separate process must not guess it from the manifest:
            // the user may have flown away since the ui block was written).
            const auto& c = m_camera;
            const glm::vec3 f = c.forward();
            results.push_back(result(id, tool, true,
                ",\"pos\":" + jsonFloats({ c.pos.x, c.pos.y, c.pos.z }) +
                ",\"yawDeg\":" + jsonNum(glm::degrees(c.yaw)) +
                ",\"pitchDeg\":" + jsonNum(glm::degrees(c.pitch)) +
                ",\"forward\":" + jsonFloats({ f.x, f.y, f.z })));
            continue;
        }

        if (tool != "stamp" && tool != "pick" && tool != "click" &&
            tool != "dig_cave") {
            results.push_back(errorResult(id, tool, "unknown intent"));
            continue;
        }

        voxel::PickHit hit;
        std::string err;
        if (!resolveTarget(args, hit, err)) {
            results.push_back(errorResult(id, tool, err));
            continue;
        }

        if (tool == "dig_cave") {
            results.push_back(applyCave(id, args, hit));
            continue;
        }

        if (tool == "pick") {
            results.push_back(result(id, tool, true, pickFields(hit)));
            continue;
        }

        if (tool == "click") {
            std::string mode;
            ai::jsonGetString(args, "mode", mode);
            if (mode.empty()) ai::jsonGetString(args, "brush", mode);
            if (mode == "select" || mode == "pick") {
                m_selectedHit = hit;
                m_hasSelection = hit.hit;
                const std::string owner = rotateTargetLayer(hit);
                m_selectedLayer = owner;
                results.push_back(result(id, tool, true,
                    ",\"mode\":\"select\",\"selected\":" + jsonStr(owner) +
                    pickFields(hit)));
                continue;
            }
            if (mode == "rotate" || mode == "move") {
                results.push_back(applyTransform(id, tool, args, hit,
                                                 mode == "rotate"));
                continue;
            }
        }

        // stamp and click-with-a-cell-brush both land here.
        results.push_back(applyStamp(id, tool, args, hit));
    }

    std::ofstream r(rpath, std::ios::app);
    if (!r) {
        spdlog::warn("mcp intents: cannot write result file {}", rpath);
        return;
    }
    for (const auto& res : results)
        r << res << "\n";
}

} // namespace app
} // namespace vf
