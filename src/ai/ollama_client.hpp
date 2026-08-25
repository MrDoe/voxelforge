#pragma once
#include <string>
#include <vector>
#include <functional>

namespace vf::ai {

struct ChatMessage {
    std::string role; // system/user/assistant/tool
    std::string content;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string argumentsJson; // raw JSON object string
    // parsed convenience for our create_* tools:
    std::string primitive; // box/cylinder/ellipsoid/stamp inferred
};

struct ChatResult {
    bool ok = false;
    std::string content;
    std::string thinking;
    std::vector<ToolCall> toolCalls;
    std::string rawResponse;
    std::string error;
};

// Minimal Ollama /api/chat client using POSIX sockets, no extra deps.
// Supports both non-streaming and streaming (callback per chunk).
class OllamaClient {
public:
    OllamaClient(std::string baseUrl = "http://127.0.0.1:11434",
                 std::string model = "gemma4:12b",
                 int timeoutMs = 120000);

    void setBaseUrl(std::string u) {
        while (!u.empty() && u.back()=='/') u.pop_back();
        if (u.size()>=3 && u.compare(u.size()-3,3,"/v1")==0){ u.erase(u.size()-3); m_chatPath="/v1/chat/completions"; } else if(m_chatPath=="/v1/chat/completions") m_chatPath="/api/chat";
        m_baseUrl = std::move(u);
    }
    void setModel(std::string m) { m_model = std::move(m); }
    const std::string& model() const { return m_model; }
    const std::string& baseUrl() const { return m_baseUrl; }

    // Blocking request. Messages should NOT include system prompt; pass separately.
    ChatResult chat(const std::vector<ChatMessage>& history,
                    const std::string& systemPrompt,
                    bool enableTools = true) const;

    // Streaming: calls onToken(content/thinking/toolDelta) repeatedly.
    // Returns final ChatResult as well.
    ChatResult chatStreaming(const std::vector<ChatMessage>& history,
                             const std::string& systemPrompt,
                             bool enableTools,
                             std::function<void(const std::string& token, bool isThinking)> onToken) const;

    // Quick health check: GET /api/tags or POST /api/show
    bool ping(std::string* outError = nullptr) const;

private:
    std::string m_baseUrl;
    std::string m_model;
    int m_timeoutMs;
    // endpoint that last worked ("/api/chat" for Ollama,
    // "/v1/chat/completions" for llama.cpp / OpenAI-compatible servers);
    // auto-detected from the URL and sticky across calls
    mutable std::string m_chatPath = "/api/chat";

    std::string buildChatBody(const std::vector<ChatMessage>& history,
                              const std::string& systemPrompt,
                              bool enableTools,
                              bool stream) const;
    ChatResult parseChatResponse(const std::string& body) const;
    // low-level HTTP
    bool httpPost(const std::string& path, const std::string& body,
                  std::string& outResponseBody, std::string& outError,
                  int* outStatus = nullptr) const;
    bool httpGet(const std::string& path, std::string& outBody, std::string& outError) const;
};

// Helpers for ChatUi
std::string escapeJsonString(const std::string& s);
std::string buildToolDefsJson(); // returns JSON array string for our 4 tools
bool parseToolCallsFromResponse(const std::string& json, std::vector<ToolCall>& out, std::string& outContent, std::string& outThinking);
}
