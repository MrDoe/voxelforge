#pragma once
// The AI panel is now a thin client of a DEDICATED OpenCode session. The
// Voxelforge agent (`.opencode/agents/voxelforge.md`) does all world editing
// through the vf_mcp tools; there is no local tool dispatcher and no Ollama
// client in this path. Undo works at whole-agent-turn granularity by
// snapshotting the authored files before each prompt.
#include "ai/opencode_client.hpp"
#include "voxel/picking.hpp"
#include <imgui.h>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace vf::ai {

struct ChatEntry {
    std::string role; // user / assistant / thinking / system
    std::string content;
};

struct ToolCard {
    std::string id;
    std::string name;
    std::string args;
    std::string result;
    std::string status = "running";
    int shotRetries = 0;
    std::string shotPath;
    ImTextureID shotImage = 0;
    int shotW = 0, shotH = 0;
};

class ChatUi {
public:
    using ImageFn = std::function<ImTextureID(const std::string&, int*, int*)>;

    ChatUi();
    ~ChatUi();

    struct Context {
        std::string sceneryDir;
        std::string sessionFile;              // .opencode/voxelforge_sessions.json
        std::function<void()> reloadFn;       // request a world reload after undo
        ImageFn imageFn;                      // PPM -> ImGui texture (App-owned cache)
    };
    void setContext(Context ctx) { m_ctx = std::move(ctx); }

    bool init(std::string* err = nullptr);
    void shutdown();

    // Drawn as the body of the AI section of the one docked sidebar; the
    // sidebar owns the window and the child region. Called every frame even
    // when hidden so an in-flight turn keeps receiving events.
    void drawPanel(const vf::voxel::PickHit* hover,
                   const vf::voxel::PickHit* selection, bool hasSelection);

    bool isSending() const { return m_sending; }
    bool wantsCaptureKeyboard() const { return m_inputFocused; }

    void undoLastTurn();
    bool canUndo() const { return !m_undoStack.empty(); }
    void clearHistory();
    // Normalize a sessions/model JSON file before appending a key (public for
    // the regression test: a trailing newline used to produce two objects).
    // Decode JSON string escapes (notably "\n") in SSE text before it reaches
    // the wrapped transcript. Public for the regression test.
    static std::string unescapeJsonText(std::string text);
    static std::string normalizeSessionJson(std::string text);
    // Set one flat key in the sessions JSON text (append or replace, always
    // keeping valid JSON). Public for regression tests.
    static std::string setSessionKey(std::string text, const std::string& key,
                                     const std::string& value);
    // Append the app's current Ctrl+LMB pick to a prompt so the agent can
    // actually see "here"/"this voxel" (public for the regression test).
    static std::string promptWithSelection(const std::string& text,
                                           const glm::ivec3& cell,
                                           bool hasSelection);
    // Clear a composer buffer and make the clear stick while ImGui owns the
    // multiline field's text (public for the regression test): a bare
    // buf[0] = 0 is re-applied from ImGui's internal state on the next frame.
    static void clearInputBuffer(char* buf, size_t bufSize, ImGuiID id);

private:
    void onEvent(const OpenCodeEvent& e);   // SSE worker → queue only
    void pollEvents();                      // UI thread → ImGui + history
    void sendPrompt(const std::string& text);
    void snapshotTurn(const std::string& label);
    void pushEntry(const std::string& role, const std::string& text);
    std::string statusText() const;

    Context m_ctx;
    OpenCodeClient m_client;
    std::vector<ChatEntry> m_history;
    std::vector<ToolCard> m_cards;
    std::map<std::string, std::string> m_toolNames; // call id -> tool name
    std::vector<OpenCodeModel> m_models;
    std::string m_modelId = "beam-free";       // project default
    std::string m_modelProvider = "opencode-go";
    bool m_modelFromEnv = false;
    bool m_modelsLoaded = false;
    bool m_mcpEnsured = false;
    bool m_turnHasTools = false, m_turnHasText = false;
    glm::ivec3 m_pendingSelection { 0 };
    bool m_pendingHasSelection = false;
    std::chrono::steady_clock::time_point m_lastActivity {}; // sending watchdog
    bool m_showAllModels = false;
    char m_inputBuf[4096] = {};
    bool m_inputFocused = false;
    bool m_sending = false;
    bool m_autoScroll = true;
    float m_fontScale = 0.f; // 0 = load from world.json "ui" on first draw
    bool m_fontScaleDirty = false;
    std::string m_status;
    int m_turn = 0;
    std::vector<std::string> m_undoStack; // snapshot directories, newest last
    std::map<std::string, std::string> m_lastFileSig; // skip unchanged files in snapshots

    // SSE queue (worker produces, UI consumes).
    std::mutex m_mu;
    std::queue<OpenCodeEvent> m_events;
};

} // namespace vf::ai
