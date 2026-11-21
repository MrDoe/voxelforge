#pragma once
// Thin C++ client for the OpenCode V2 HTTP API. The in-app AI panel talks to
// a DEDICATED session in the local OpenCode service; all world authoring is
// done by the "voxelforge" OpenCode agent through the vf_mcp tools.
//
// Transport is plain HTTP on loopback with HTTP Basic auth read from the
// service registration file (same discovery the `opencode api` CLI uses).
// SSE events are consumed on a worker thread and handed to the UI as a
// callback; no UI code ever blocks on a socket.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vf::ai {

struct OpenCodeEvent {
    std::string type;
    std::string json;
};

struct OpenCodeModel {
    std::string id;         // e.g. "beam-free"
    std::string name;       // e.g. "Beam Free"
    std::string providerID; // e.g. "opencode-go"
};

class OpenCodeClient {
public:
    using EventFn = std::function<void(const OpenCodeEvent&)>;

    explicit OpenCodeClient(EventFn onEvent);
    ~OpenCodeClient();

    // Connect to the service (`VF_OPENCODE_URL`, default 127.0.0.1 from the
    // service registration file) and verify /api/info.
    bool connect(std::string* err = nullptr);
    // Create a dedicated session for one scenery with a chosen model
    // (defaults to Beam Free). The returned ID is cached.
    bool createSession(const std::string& directory, const std::string& agent,
                       const std::string& modelId = "beam-free",
                       const std::string& providerID = "opencode-go",
                       std::string* err = nullptr);
    // The mcp-lazy plugin starts with vf_mcp disconnected; connect it for
    // the project location so the agent actually sees its 44 tools.
    bool ensureMcpConnected(const std::string& directory, std::string* err = nullptr);
    // Chat-capable models from GET /api/model (used by the panel dropdown).
    std::vector<OpenCodeModel> listModels(const std::string& directory,
                                          std::string* err = nullptr);
    bool setSessionModel(const std::string& modelId, const std::string& providerID,
                         std::string* err = nullptr);
    bool attachSession(const std::string& id, std::string* err = nullptr);
    // Start the SSE subscription worker (idempotent); events arrive via the
    // callback supplied to the constructor.
    bool startEventStream(std::string* err = nullptr);
    // Returns a COPY: the SSE worker filters events while the UI may switch
    // sessions, so the ID must never be read as a shared mutable reference.
    std::string sessionId() const;
    bool sendPrompt(const std::string& text, std::string* err = nullptr);
    bool interrupt(std::string* err = nullptr);
    bool replyPermission(const std::string& id, const std::string& decision,
                         std::string* err = nullptr);
    bool setEnvironment(const std::map<std::string, std::string>& vars,
                        std::string* err = nullptr);
    // Raw API call (mostly for the model/agent pickers). Returns body text.
    std::string request(const std::string& method, const std::string& path,
                        const std::string& body, std::string* err = nullptr);
    void stop();
    bool connected() const { return m_connected.load(); }
    std::string status() const;

    // Pure helpers (unit-tested without a service).
    static std::string decodeChunked(const std::string& wire);
    static std::string parseEventType(const std::string& json);
    static std::string servicePassword();
    static std::string base64(const std::string& in);

private:
    void setSessionId(std::string id);
    struct Impl;
    std::unique_ptr<Impl> m; // Impl owns the SSE fd; stop() only shuts it down
    std::atomic<bool> m_connected { false };
    std::string m_sessionId;
    mutable std::mutex m_sessionMu;
    std::mutex m_mu;
};

} // namespace vf::ai
