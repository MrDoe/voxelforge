#pragma once
#include "ai/ollama_client.hpp"
#include "voxel/picking.hpp"
#include "voxel/editable_world.hpp"
#include <imgui.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>
#include <atomic>
#include <functional>

namespace vf::ai {

struct ChatEntry {
    std::string role; // user / assistant / system / tool
    std::string content;
    bool isThinking = false;
};

class ChatUi {
public:
    ChatUi();
    ~ChatUi();
    void init(const std::string& baseUrl, const std::string& model);
    void shutdown();

    // called each frame from App::drawHud
    void draw(vf::voxel::EditableWorld& editable,
              const vf::voxel::PickHit* hover, const vf::voxel::PickHit* selection, bool hasSelection,
              std::function<void()> rebuildFn);

    bool isSending() const { return m_sending; }
    bool wantsCaptureKeyboard() const { return m_inputFocused; }

    // direct tool execution; returns number of rejected/unknown calls
    int executeToolCalls(const std::vector<ToolCall>& calls, glm::ivec3 anchor, bool hasAnchor,
                         vf::voxel::EditableWorld& editable, std::function<void()> rebuildFn);

private:
    void pushUser(const std::string& t);
    void pushAssistant(const std::string& t, bool thinking=false);
    void launchRequest(const std::string& userText, glm::ivec3 anchor, bool hasAnchor);
    void spawnWorker();
    void pollResults(std::function<void()> rebuildFn);

    std::string buildSystemPrompt(bool hasSelection) const;

    OllamaClient m_client;
    std::vector<ChatEntry> m_history;
    std::vector<ChatMessage> m_llmHistory; // for API
    char m_inputBuf[2048] = {0};
    bool m_inputFocused = false;
    std::atomic<bool> m_sending{false};
    std::string m_status; // small status line
    bool m_showThinking = false;

    // worker thread result queue
    std::mutex m_mu;
    std::queue<ChatResult> m_pending;
    std::thread m_worker;
    // anchor captured at send time
    glm::ivec3 m_pendingAnchor{0};
    bool m_pendingHasAnchor=false;
    // unknown-tool self-correction
    int m_unknownRetries = 0;
    std::string m_lastUserText;
};

} // namespace vf::ai
