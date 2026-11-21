#include "app/chat_ui.hpp"

#include "ai/tools.hpp"
#include "app/ui/ui_primitives.hpp"
#include "app/ui/ui_types.hpp"
#include "voxel/scenery.hpp"
#include "voxel/worldfile.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace vf::ai {
namespace fs = std::filesystem;

namespace {

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

// Extract a JSON value (object / array / string / scalar) for a top-level-ish
// key. Good enough for the event envelopes the OpenCode API emits.
std::string extractValue(const std::string& json, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size()) return {};
    const char open = json[p];
    if (open == '{' || open == '[') {
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool inStr = false, esc = false;
        for (size_t i = p; i < json.size(); ++i) {
            char c = json[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == open) ++depth;
            else if (c == close && --depth == 0)
                return json.substr(p, i - p + 1);
        }
        return {};
    }
    if (open == '"') {
        size_t e = json.find('"', p + 1);
        return e == std::string::npos ? std::string() : json.substr(p, e - p + 1);
    }
    size_t e = p;
    while (e < json.size() && json[e] != ',' && json[e] != '}') ++e;
    return json.substr(p, e - p);
}

// SSE event text is JSON-encoded: a newline in a delta arrives as the two
// bytes '\' 'n', not an actual LF. jsonGetString() is a raw scanner (no
// unescaping), so without this the transcript shows literal "\n" and
// drawWrappedMultiline() never sees a line break to split on. Decode the
// standard escapes at the extraction boundary; drop CR so CRLF sources do
// not leave a stray control character at the end of each wrapped line.
std::string unescapeJsonTextImpl(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out.push_back(s[i]); continue; }
        const char c = s[++i];
        switch (c) {
            case 'n': out.push_back('\n'); break;
            case 'r': break; // CRLF -> LF
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'u': {
                // \uXXXX -> UTF-8 for the BMP. A D800-DBFF surrogate is left
                // literal so we never emit invalid UTF-8 into the ImGui
                // text buffer; the low surrogate arrives as its own escape.
                if (i + 4 < s.size()) {
                    unsigned cp = 0;
                    bool ok = true;
                    for (int k = 1; k <= 4; ++k) {
                        const char h = s[i + k];
                        unsigned v;
                        if (h >= '0' && h <= '9') v = unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') v = unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = unsigned(h - 'A' + 10);
                        else { ok = false; break; }
                        cp = cp * 16 + v;
                    }
                    if (ok && cp < 0x800) {
                        i += 4;
                        if (cp < 0x80) out.push_back(char(cp));
                        else {
                            out.push_back(char(0xC0 | (cp >> 6)));
                            out.push_back(char(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                }
                out.push_back('u');
                break;
            }
            default: out.push_back('\\'); out.push_back(c); break;
        }
    }
    return out;
}

std::string firstText(const std::string& json)
{
    std::string s;
    if (jsonGetString(json, "text", s)) return unescapeJsonTextImpl(s);
    size_t p = json.find("\"text\"");
    if (p == std::string::npos) return {};
    return unescapeJsonTextImpl(extractValue(json, "text"));
}

// ImGui::TextWrapped stops at '\n' (it does not break lines), so agent/user
// messages with newlines would otherwise show only the first line.
void drawWrappedMultiline(const std::string& text)
{
    size_t start = 0;
    for (;;) {
        const size_t e = text.find('\n', start);
        const std::string line = text.substr(start, e == std::string::npos
                                                    ? std::string::npos
                                                    : e - start);
        if (line.empty())
            ImGui::TextUnformatted(" ");
        else
            ImGui::TextWrapped("%s", line.c_str());
        if (e == std::string::npos) break;
        start = e + 1;
    }
}

// One-line argument preview, OpenCode-style: `tool(key=value, …)`.
std::string compactArgs(const std::string& args)
{
    if (args.size() < 3) return {};
    std::string s = args.substr(1, args.size() - 2); // drop { }
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    if (s.size() > 72) s = s.substr(0, 72) + "…";
    return s;
}

bool isFreeModel(const OpenCodeModel& m)
{
    std::string s = m.id + " " + m.name;
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s.find("free") != std::string::npos;
}

std::string fileSignature(const fs::path& p)
{
    std::error_code ec;
    return std::to_string(fs::last_write_time(p, ec).time_since_epoch().count()) +
           ":" + std::to_string(fs::file_size(p, ec));
}

// Flat object text for incremental appends. A previous version popped the
// last character assuming it was '}' — with a trailing newline that produced
// two concatenated JSON objects ("{...}\n, {...}\n}") on the second scenery.
std::string normalizeSessionJsonImpl(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    if (std::count(text.begin(), text.end(), '{') != 1 ||
        std::count(text.begin(), text.end(), '}') != 1 ||
        text.empty() || text.back() != '}')
        return "{"; // unrecoverable/malformed: start fresh
    text.pop_back();
    return text;
}

std::string loadSessionId(const std::string& file, const std::string& dir)
{
    std::ifstream in(file);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string id;
    return jsonGetString(text, jsonEscape(dir).c_str(), id) ? id : std::string();
}

void saveSessionId(const std::string& file, const std::string& dir, const std::string& id)
{
    if (file.empty()) return;
    fs::path p(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ifstream in(file);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text = ChatUi::setSessionKey(text, dir, id);
    const std::string tmp = file + ".tmp";
    { std::ofstream out(tmp); out << text; }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp);
}

// Model choice is app-local state, kept beside the session IDs in the same
// small JSON file (key = "<scenery dir>::model", value = "provider/id").
std::string loadModelChoice(const std::string& file, const std::string& dir)
{
    std::ifstream in(file);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string key = jsonEscape(dir + "::model");
    std::string v;
    return jsonGetString(text, key.c_str(), v) ? v : std::string();
}

void saveModelChoice(const std::string& file, const std::string& dir,
                     const std::string& choice)
{
    if (file.empty() || choice.empty()) return;
    std::ifstream in(file);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text = ChatUi::setSessionKey(text, dir + "::model", choice);
    const std::string tmp = file + ".tmp";
    { std::ofstream out(tmp); out << text; }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp);
}

} // namespace

std::string ChatUi::unescapeJsonText(std::string s)
{
    return unescapeJsonTextImpl(std::move(s));
}

std::string ChatUi::normalizeSessionJson(std::string text)
{
    return normalizeSessionJsonImpl(std::move(text));
}

std::string ChatUi::setSessionKey(std::string text, const std::string& key,
                                  const std::string& value)
{
    const std::string k = jsonEscape(key), v = jsonEscape(value);
    std::string old;
    if (jsonGetString(text, k.c_str(), old)) {
        const size_t q = text.find("\"" + k + "\"");
        const size_t c = text.find(':', q);
        const size_t b = c == std::string::npos ? std::string::npos : text.find('"', c + 1);
        const size_t e = b == std::string::npos ? std::string::npos : text.find('"', b + 1);
        if (b != std::string::npos && e != std::string::npos) {
            text.replace(b, e - b + 1, "\"" + v + "\"");
            return text;
        }
    }
    text = normalizeSessionJsonImpl(std::move(text));
    if (text.size() > 1) text += ",";
    text += "\n  \"" + k + "\": \"" + v + "\"\n}\n";
    return text;
}

std::string ChatUi::promptWithSelection(const std::string& text,
                                        const glm::ivec3& cell,
                                        bool hasSelection)
{
    if (text.empty()) return {};
    std::ostringstream o;
    o << text;
    if (hasSelection) {
        const glm::vec3 w = vf::voxel::voxelCenter(cell);
        o << "\n\n[UI context: selected voxel cell [" << cell.x << "," << cell.y
          << "," << cell.z << "], world position [" << w.x << "," << w.y << ","
          << w.z << "] metres. This is the bottom-center anchor for add_* "
             "tools; use it when the user says 'here', 'this', or 'selected'.)";
    } else {
        o << "\n\n[UI context: no voxel is selected. If the request depends "
             "on a position, ask the user to Ctrl+LMB pick one first.]";
    }
    return o.str();
}

ChatUi::ChatUi()
    : m_client([this](const OpenCodeEvent& e) { onEvent(e); })
{
    // Scripted override for CI/gates: VF_OPENCODE_MODEL=provider/id. The
    // panel dropdown still wins once the user changes it (it persists).
    if (const char* env = std::getenv("VF_OPENCODE_MODEL")) {
        const std::string spec = env;
        const size_t slash = spec.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < spec.size()) {
            m_modelProvider = spec.substr(0, slash);
            m_modelId = spec.substr(slash + 1);
            m_modelFromEnv = true;
        }
    }
}
ChatUi::~ChatUi() { shutdown(); }

bool ChatUi::init(std::string* err)
{
    if (!m_client.connect(err)) return false;
    m_status = m_client.status();
    m_client.startEventStream(err);
    return true;
}

void ChatUi::shutdown() { m_client.stop(); }

void ChatUi::clearHistory()
{
    m_history.clear();
    m_cards.clear();
}

std::string ChatUi::statusText() const
{
    if (m_sending) return "thinking… (" + m_modelId + ")";
    return m_client.status() + " · " + m_modelId;
}

void ChatUi::onEvent(const OpenCodeEvent& e)
{
    // The SSE stream is service-wide. Filter HERE on the worker so other
    // sessions' token deltas never enter our queue or touch the UI thread.
    std::string sid;
    if (jsonGetString(e.json, "sessionID", sid) &&
        sid != m_client.sessionId())
        return;
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_events.size() > 4096) return; // pathological flood guard
    m_events.push(e);
}

void ChatUi::snapshotTurn(const std::string& label)
{
    if (m_ctx.sceneryDir.empty()) return;
    const std::string dir = "/tmp/opencode/ai_undo/" + std::to_string(::getpid()) +
                            "/" + std::to_string(++m_turn);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return;
    std::vector<fs::path> files;
    files.push_back(fs::path(m_ctx.sceneryDir) / "world.json");
    files.push_back(fs::path(m_ctx.sceneryDir) / "ai_edits.vxw");
    for (const auto& entry : fs::directory_iterator(m_ctx.sceneryDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".vxw")
            files.push_back(entry.path());
    }
    ec.clear(); // a failed directory scan must not poison the copy loop
    for (const auto& f : files) {
        if (!fs::exists(f)) continue;
        const std::string name = f.filename().string();
        const std::string sig = fileSignature(f);
        auto prev = m_lastFileSig.find(name);
        // Most turns touch one or two layers: skip byte-copying the dozen
        // unchanged multi-MB .vxw files (this used to dominate prompt latency).
        if (prev != m_lastFileSig.end() && prev->second == sig)
            continue;
        fs::copy_file(f, fs::path(dir) / name,
                      fs::copy_options::overwrite_existing, ec);
        m_lastFileSig[name] = sig;
    }
    { std::ofstream out(fs::path(dir) / "label.txt"); out << label; }
    spdlog::info("ai: snapshot turn {} for undo ({} authored file(s), label: {})",
                 m_turn, std::distance(fs::directory_iterator(dir), fs::directory_iterator()) - 1,
                 label.substr(0, 60));
    m_undoStack.push_back(dir);
    if (m_undoStack.size() > 20) m_undoStack.erase(m_undoStack.begin());
}

void ChatUi::undoLastTurn()
{
    if (m_undoStack.empty() || m_ctx.sceneryDir.empty()) return;
    const std::string dir = m_undoStack.back();
    m_undoStack.pop_back();
    int restored = 0;
    std::error_code scanEc;
    for (const auto& entry : fs::directory_iterator(dir, scanEc)) {
        if (scanEc) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name == "label.txt") continue;
        const fs::path target = fs::path(m_ctx.sceneryDir) / name;
        const fs::path tmp = target.string() + ".aiundo.tmp";
        std::error_code ec;
        fs::copy_file(entry.path(), tmp, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            fs::rename(tmp, target, ec);
            if (!ec) ++restored;
            else fs::remove(tmp);
        }
    }
    if (m_ctx.reloadFn) m_ctx.reloadFn();
    pushEntry("system", "Undo: restored " + std::to_string(restored) +
                            " authored file(s) from the last agent turn.");
    spdlog::info("ai: undo restored {} files from {}", restored, dir);
}

void ChatUi::sendPrompt(const std::string& text)
{
    if (text.empty()) return;
    m_turnHasTools = false;
    m_turnHasText = false;
    m_lastActivity = std::chrono::steady_clock::now();
    std::string err;
    if (!m_client.sendPrompt(promptWithSelection(text, m_pendingSelection,
                                                 m_pendingHasSelection), &err)) {
        m_sending = false;
        pushEntry("system", "Send failed: " + err);
    }
}

void ChatUi::pollEvents()
{
    std::queue<OpenCodeEvent> q;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        std::swap(q, m_events);
    }
    while (!q.empty()) {
        OpenCodeEvent e = std::move(q.front());
        q.pop();
        const std::string& j = e.json;
        // The SSE stream is service-wide: ignore turns from other OpenCode
        // sessions (this is a dedicated per-scenery session).
        std::string eventSession;
        if (jsonGetString(j, "sessionID", eventSession) &&
            eventSession != m_client.sessionId())
            continue;
        m_lastActivity = std::chrono::steady_clock::now();
        std::string d;
        if (e.type == "session.text.delta") {
            jsonGetString(j, "delta", d);
            d = unescapeJsonText(std::move(d));
            if (!d.empty()) {
                m_turnHasText = true;
                if (m_history.empty() || m_history.back().role != "assistant")
                    pushEntry("assistant", d);
                else
                    m_history.back().content += d;
                m_autoScroll = true;
            }
        } else if (e.type == "session.tool.input.started") {
            // The SSE stream announces the tool NAME here before the call
            // event (which only carries arguments). Keep it by call id.
            std::string id, name;
            jsonGetString(j, "id", id);
            jsonGetString(j, "name", name);
            if (!id.empty() && !name.empty()) m_toolNames[id] = name;
            if (m_toolNames.size() > 64) m_toolNames.clear();
        } else if (e.type == "session.tool.called") {
            ToolCard c;
            jsonGetString(j, "id", c.id);
            auto known = m_toolNames.find(c.id);
            if (known != m_toolNames.end()) c.name = known->second;
            std::string input = extractValue(j, "input");
            c.args = input;
            // OpenCode puts the tool name at the event top level in some
            // versions and inside input in others — accept either.
            jsonGetString(j, "tool", c.name);
            if (c.name.empty()) jsonGetString(j, "toolName", c.name);
            if (c.name.empty()) jsonGetString(j, "name", c.name);
            if (c.name.empty()) jsonGetString(input, "tool", c.name);
            if (c.name.empty()) jsonGetString(input, "name", c.name);
            if (c.name.empty()) jsonGetString(input, "function", c.name);
            if (c.name.empty()) c.name = "tool";
            m_turnHasTools = true;
            m_cards.push_back(std::move(c));
            if (m_cards.size() > 24) m_cards.erase(m_cards.begin());
            m_autoScroll = true;
        } else if (e.type == "session.tool.success" || e.type == "session.tool.failed") {
            std::string id;
            jsonGetString(j, "id", id);
            for (auto& c : m_cards) {
                if (c.id != id) continue;
                c.status = e.type == "session.tool.failed" ? "failed" : "done";
                c.result = firstText(j);
                c.shotPath = extractImagePath(c.result);
                break;
            }
        } else if (e.type == "permission.asked") {
            std::string id;
            jsonGetString(j, "id", id);
            // No approval UI by decision: undo is the safety net.
            if (!id.empty()) m_client.replyPermission(id, "once");
        } else if (e.type == "session.idle") {
            // The agent prompt requires a final text answer. If a tool-only
            // turn still happens, say so honestly instead of leaving silence.
            if (m_turnHasTools && !m_turnHasText)
                pushEntry("system", "Turn finished with tool calls but no text answer "
                                    "from the model — see the tool cards above.");
            m_sending = false;
            m_status = m_client.status();
        } else if (e.type == "session.execution.failed") {
            m_sending = false;
            pushEntry("system", "Agent failed: " + firstText(j).substr(0, 300));
        }
    }
}

void ChatUi::pushEntry(const std::string& role, const std::string& text)
{
    m_history.push_back({ role, text });
    if (m_history.size() > 400) m_history.erase(m_history.begin());
}

void ChatUi::drawPanel(const vf::voxel::PickHit* hover,
                       const vf::voxel::PickHit* selection, bool hasSelection)
{
    // Session follows the active scenery: one dedicated session per scenery.
    static std::string currentSessionDir;
    if (!m_ctx.sceneryDir.empty() && currentSessionDir != m_ctx.sceneryDir) {
        currentSessionDir = m_ctx.sceneryDir;
        m_lastFileSig.clear(); // snapshots are per scenery
        std::string err;
        const std::string choice = m_modelFromEnv ? std::string()
                                   : loadModelChoice(m_ctx.sessionFile, m_ctx.sceneryDir);
        if (!choice.empty()) {
            const size_t slash = choice.find('/');
            if (slash != std::string::npos) {
                m_modelProvider = choice.substr(0, slash);
                m_modelId = choice.substr(slash + 1);
            }
        }
        // Agents/models are defined by the PROJECT (`.opencode/`), not by a
        // scenery folder. The scene itself is selected with VF_SCENERY in
        // the session environment below.
        const std::string projectDir = fs::current_path().string();
        if (!m_mcpEnsured) {
            m_client.ensureMcpConnected(projectDir, &err); // agent needs tools
            m_mcpEnsured = true;
        }
        // Retry if the service was still coming up on the first attempt.
        if (!m_modelsLoaded || m_models.empty()) {
            m_models = m_client.listModels(projectDir, &err);
            m_modelsLoaded = true;
        }
        std::string id = loadSessionId(m_ctx.sessionFile, m_ctx.sceneryDir);
        if (!id.empty()) m_client.attachSession(id, &err);
        if (m_client.sessionId().empty()) {
            m_client.createSession(projectDir, "voxelforge",
                                   m_modelId, m_modelProvider, &err);
            if (!m_client.sessionId().empty())
                saveSessionId(m_ctx.sessionFile, m_ctx.sceneryDir, m_client.sessionId());
        } else {
            // Existing sessions (including ones made before the dropdown)
            // must follow the persisted model choice.
            m_client.setSessionModel(m_modelId, m_modelProvider, &err);
        }
        m_client.setEnvironment({ { "VF_SCENERY",
                                    fs::path(vf::voxel::scenery::root()).filename().string() } });
        m_client.startEventStream();
        m_status = m_client.status();
    }

    pollEvents();
    // Explicit pixel heights in this panel must follow the UI text scale;
    // ImGui's ScaleAllSizes only affects its own widgets, not ImVec2 sizes.
    const float s = m_fontScale > 0.f ? m_fontScale : 2.0f;

    // If the SSE stream dropped (or a turn never sent idle), m_sending would
    // keep the Send button disabled forever. A silent turn is a dead turn.
    if (m_sending && std::chrono::steady_clock::now() - m_lastActivity >
                         std::chrono::seconds(120)) {
        m_sending = false;
        pushEntry("system", "Turn went silent for 2 minutes — input re-enabled. "
                            "Use Cancel/Stop if the agent is still running.");
    }

    // ---- header: connection, agent, undo ------------------------------------
    ImGui::TextColored(m_sending ? vf::app::kAccent : vf::app::kTextSecondary, "%s",
                       statusText().c_str());
    if (ImGui::SmallButton("Undo turn")) undoLastTurn();
    ImGui::SameLine();
    if (ImGui::SmallButton("New chat")) clearHistory();
    ImGui::SameLine();
    if (ImGui::SmallButton(m_showAllModels ? "Free" : "All")) {
        m_showAllModels = !m_showAllModels;
    }
    // UI-wide text size for high-res screens, persisted as a UI setting so
    // the app's manifest applier rebuilds the font atlas between frames.
    if (m_fontScale <= 0.f && !m_ctx.sceneryDir.empty()) {
        vf::voxel::worldfile::UiSettings s;
        if (vf::voxel::worldfile::loadUiManifest(
                m_ctx.sceneryDir + "/world.json", s) &&
            s.uiFontScale != vf::voxel::worldfile::UiSettings::kUiAbsent)
            m_fontScale = s.uiFontScale;
        else
            m_fontScale = 2.0f; // default for high-res displays
    }
    ImGui::SetNextItemWidth(64.0f * s);
    if (ImGui::SliderFloat("Text", &m_fontScale, 0.8f, 2.5f, "%.2f"))
        m_fontScaleDirty = true;
    // Write once on release: a slider drag fires every frame and a manifest
    // rewrite per tick would thrash the hot-reload poll and git status.
    if (m_fontScaleDirty && ImGui::IsItemDeactivatedAfterEdit() &&
        !m_ctx.sceneryDir.empty()) {
        vf::voxel::worldfile::UiSettings patch;
        patch.uiFontScale = m_fontScale;
        vf::voxel::worldfile::mergeUiManifest(m_ctx.sceneryDir + "/world.json", patch);
        m_fontScaleDirty = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("UI text size (font + widget scale)");

    // Model picker — Beam Free is the project default; the choice is stored
    // per scenery and applied to the session with POST .../model.
    std::string modelLabel = m_modelId;
    for (const auto& m : m_models)
        if (m.id == m_modelId && m.providerID == m_modelProvider) {
            modelLabel = m.name;
            break;
        }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##aiModel", modelLabel.c_str())) {
        for (const auto& m : m_models) {
            if (!m_showAllModels && !isFreeModel(m) && m.id != m_modelId)
                continue; // 423 models is a wall; show free ones + current
            const bool selected = m.id == m_modelId && m.providerID == m_modelProvider;
            std::string item = m.name + "  (" + m.providerID + ")";
            if (ImGui::Selectable(item.c_str(), selected)) {
                m_modelId = m.id;
                m_modelProvider = m.providerID;
                saveModelChoice(m_ctx.sessionFile, m_ctx.sceneryDir,
                                m_modelProvider + "/" + m_modelId);
                std::string err;
                m_client.setSessionModel(m_modelId, m_modelProvider, &err);
                if (!err.empty()) pushEntry("system", "Model switch failed: " + err);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Chat model (%d available)", int(m_models.size()));

    // Capture the current Ctrl+LMB pick at draw time so sendPrompt() can
    // inject it into the message — the agent otherwise cannot see "here".
    m_pendingHasSelection = hasSelection && selection;
    if (m_pendingHasSelection) {
        m_pendingSelection = selection->voxel;
        glm::vec3 w = vf::voxel::voxelCenter(selection->voxel);
        vf::app::hintLine("Selected voxel %d %d %d · world %.1f %.1f %.1f",
                     selection->voxel.x, selection->voxel.y, selection->voxel.z,
                     w.x, w.y, w.z);
    } else {
        vf::app::hintLine("Ctrl+LMB picks a voxel; describe what to build or change.");
    }
    if (hover && hover->hit)
        ImGui::TextDisabled("Hover %d %d %d", hover->voxel.x, hover->voxel.y, hover->voxel.z);
    ImGui::Separator();

    // ---- tool cards ----------------------------------------------------------
    if (!m_cards.empty()) {
        // OpenCode's lowest verbosity: one line per tool call, collapsed by
        // default; click to see args/result/screenshot.
        const float h = std::min(104.0f, 8.0f + 21.0f * float(m_cards.size())) * s;
        ImGui::BeginChild("aiTools", ImVec2(0, h), true);
        for (auto& c : m_cards) {
            const char* glyph = c.status == "failed" ? "ERR"
                              : c.status == "done" ? "ok" : "...";
            std::string label = std::string(glyph) + "  " + c.name;
            const std::string small = compactArgs(c.args);
            if (!small.empty()) label += "  " + small;
            if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                ImGui::TextDisabled("args: %s", c.args.substr(0, 400).c_str());
                if (!c.result.empty())
                    drawWrappedMultiline(c.result.substr(0, 500));
            if (!c.shotPath.empty()) {
                // The PPM can be written a moment after the tool event; retry
                // occasionally instead of decoding a missing file every frame.
                if (!c.shotImage && m_ctx.imageFn && (c.shotRetries % 30) == 0)
                    c.shotImage = m_ctx.imageFn(c.shotPath, &c.shotW, &c.shotH);
                if (!c.shotImage) ++c.shotRetries;
                if (c.shotImage) {
                    const float w = ImGui::GetContentRegionAvail().x;
                    ImGui::Image(c.shotImage,
                                 ImVec2(w, w * float(c.shotH) / float(std::max(c.shotW, 1))));
                    ImGui::TextDisabled("%s (%dx%d)", c.shotPath.c_str(), c.shotW, c.shotH);
                } else {
                    ImGui::TextDisabled("shot: %s", c.shotPath.c_str());
                }
            }
                ImGui::TreePop();
            }
            ImGui::Separator();
        }
        ImGui::EndChild();
    }

    // ---- transcript ----------------------------------------------------------
    ImGui::BeginChild("aiHistory", ImVec2(0, -96.0f * s), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
    for (const auto& e : m_history) {
        ImVec4 col = e.role == "user" ? vf::app::kAccent
                   : e.role == "system" ? vf::app::kTextTertiary : vf::app::kTextPrimary;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        drawWrappedMultiline(e.content);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (m_autoScroll && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 4.f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // ---- composer ------------------------------------------------------------
    if (ImGui::SmallButton("Path example")) {
        std::snprintf(m_inputBuf, sizeof(m_inputBuf),
                      "Draw a stony path from the lighthouse to the barn.");
    }
    ImGui::InputTextMultiline("##aiInput", m_inputBuf, sizeof(m_inputBuf),
                              ImVec2(-1.0f, 52.0f * s));
    m_inputFocused = ImGui::IsItemActive();
    vf::app::hintLine("Enter sends · Shift+Enter newline");
    // Enter submits from a multiline field: InputText has already inserted
    // the '\n' by the time we see the key, so drop it before sending.
    bool submit = false;
    if (m_inputFocused &&
        !ImGui::IsKeyDown(ImGuiKey_LeftShift) &&
        !ImGui::IsKeyDown(ImGuiKey_RightShift) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
        size_t n = std::strlen(m_inputBuf);
        while (n > 0 && (m_inputBuf[n - 1] == '\n' || m_inputBuf[n - 1] == '\r'))
            m_inputBuf[--n] = '\0';
        submit = m_inputBuf[0] != '\0';
    }
    // A new command is allowed while a turn is still running: interrupt the
    // old turn first instead of forcing the user to click Cancel. The old
    // turn's late SSE events (text/idle) are filtered by session id, not turn,
    // so a straggler could still flip m_sending — clear it again here and let
    // the fresh turn's activity drive the watchdog.
    ImGui::BeginDisabled(m_inputBuf[0] == 0);
    if (ImGui::Button("Send", ImVec2(-1.0f, vf::app::kUiRowH * s)) || submit) {
        const std::string text = m_inputBuf;
        m_inputBuf[0] = 0;
        if (m_sending) {
            m_client.interrupt();
            m_sending = false;
            pushEntry("system", "Previous turn interrupted — starting a new one.");
        }
        pushEntry("user", text);
        snapshotTurn(text);
        m_sending = true;
        sendPrompt(text);
    }
    ImGui::EndDisabled();
    if (m_sending && ImGui::Button("Cancel", ImVec2(-1.0f, vf::app::kUiRowH * s - 4))) {
        m_client.interrupt();
        m_sending = false; // do not wait for an idle event that may not arrive
    }
}

} // namespace vf::ai
