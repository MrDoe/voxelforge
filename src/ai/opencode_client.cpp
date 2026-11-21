#include "ai/opencode_client.hpp"
#include "ai/tools.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace vf::ai {
namespace {

std::string envOr(const char* key, const std::string& fallback)
{
    const char* e = std::getenv(key);
    return e && *e ? std::string(e) : fallback;
}

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(unsigned(c)));
    return s;
}

// Parse "http://host:port" into host/port. Only local HTTP is supported —
// the OpenCode background service is loopback; TLS is deliberately out of
// scope for this in-app client.
bool parseUrl(const std::string& url, std::string& host, int& port, std::string* err)
{
    std::string u = url;
    const auto scheme = u.find("://");
    if (scheme == std::string::npos || lower(u.substr(0, scheme)) != "http") {
        if (err) *err = "OpenCode URL must be http://host:port";
        return false;
    }
    u = u.substr(scheme + 3);
    size_t slash = u.find('/');
    if (slash != std::string::npos) u.resize(slash);
    size_t colon = u.rfind(':');
    if (colon == std::string::npos) {
        host = u;
        port = 80;
    } else {
        host = u.substr(0, colon);
        port = std::atoi(u.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0 || port > 65535) {
        if (err) *err = "bad OpenCode host/port: " + url;
        return false;
    }
    return true;
}

int connectSocket(const std::string& host, int port, std::string* err)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string p = std::to_string(port);
    if (getaddrinfo(host.c_str(), p.c_str(), &hints, &list) != 0 || !list) {
        if (err) *err = "cannot resolve " + host;
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = list; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0 && err) *err = "cannot connect to " + host + ":" + p;
    return fd;
}

std::string urlEncode(const std::string& in)
{
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
            c == '_' || c == '~')
            out.push_back(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

std::string readAll(int fd)
{
    std::string out;
    char buf[16384];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, size_t(n));
            if (out.size() > 32u << 20) break;
        } else {
            break;
        }
    }
    return out;
}

// Incremental HTTP chunked-transfer decoder for SSE / large responses.
class ChunkDecoder {
public:
    std::string feed(const std::string& wire)
    {
        m_raw += wire;
        std::string out;
        for (;;) {
            if (m_size < 0) {
                size_t eol = m_raw.find("\r\n");
                if (eol == std::string::npos) break;
                std::string hex = m_raw.substr(0, eol);
                m_raw.erase(0, eol + 2);
                m_size = int(std::strtoul(hex.c_str(), nullptr, 16));
                if (m_size == 0) { m_done = true; break; }
            }
            if (m_raw.size() < size_t(m_size) + 2) break;
            out.append(m_raw, 0, size_t(m_size));
            m_raw.erase(0, size_t(m_size) + 2); // payload + CRLF
            m_size = -1;
        }
        return out;
    }
    bool done() const { return m_done; }

private:
    std::string m_raw;
    int m_size = -1;
    bool m_done = false;
};

} // namespace

std::string OpenCodeClient::base64(const std::string& in)
{
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(in[i]);
        unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        out.push_back(kAlpha[(a >> 2) & 63]);
        out.push_back(kAlpha[((a & 3) << 4) | ((b >> 4) & 15)]);
        out.push_back(i + 1 < in.size() ? kAlpha[((b & 15) << 2) | ((c >> 6) & 3)] : '=');
        out.push_back(i + 2 < in.size() ? kAlpha[c & 63] : '=');
    }
    return out;
}

std::string OpenCodeClient::servicePassword()
{
    std::string dir = envOr("XDG_STATE_HOME", std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/state");
    for (const std::string& path : { dir + "/opencode/service.json",
                                     std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/opencode/service.json" }) {
        std::ifstream in(path);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t p = text.find("\"password\"");
        if (p == std::string::npos) continue;
        p = text.find(':', p);
        if (p == std::string::npos) continue;
        p = text.find('"', p + 1);
        if (p == std::string::npos) continue;
        size_t e = text.find('"', p + 1);
        if (e != std::string::npos) return text.substr(p + 1, e - p - 1);
    }
    return envOr("VF_OPENCODE_PASSWORD", "");
}

std::string OpenCodeClient::decodeChunked(const std::string& wire)
{
    ChunkDecoder d;
    return d.feed(wire);
}

std::string OpenCodeClient::parseEventType(const std::string& json)
{
    size_t p = json.find("\"type\"");
    if (p == std::string::npos) return {};
    p = json.find('"', p + 6);
    if (p == std::string::npos) return {};
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return {};
    return json.substr(p + 1, e - p - 1);
}

struct OpenCodeClient::Impl {
    EventFn onEvent;
    std::string url;
    std::string host;
    int port = 49374;
    std::string auth;
    std::thread worker;
    std::atomic<bool> stop { false };
    std::atomic<bool> ready { false };
    std::atomic<int> sseFd { -1 };

    std::string address() const { return host + ":" + std::to_string(port); }
};

OpenCodeClient::OpenCodeClient(EventFn onEvent) : m(new Impl)
{
    m->onEvent = std::move(onEvent);
    m->url = envOr("VF_OPENCODE_URL", "http://127.0.0.1:49374");
    std::string pw = servicePassword();
    if (!pw.empty())
        m->auth = "Authorization: Basic " + base64("opencode:" + pw) + "\r\n";
}

OpenCodeClient::~OpenCodeClient() { stop(); }

void OpenCodeClient::stop()
{
    m->stop.store(true);
    // The worker owns close(): closing here as well could close an unrelated
    // fd if the number was reused between shutdown and the worker's cleanup.
    const int fd = m->sseFd.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (m->worker.joinable()) m->worker.join();
    m_connected.store(false);
}

std::string OpenCodeClient::sessionId() const
{
    std::lock_guard<std::mutex> lk(m_sessionMu);
    return m_sessionId;
}

void OpenCodeClient::setSessionId(std::string id)
{
    std::lock_guard<std::mutex> lk(m_sessionMu);
    m_sessionId = std::move(id);
}

std::string OpenCodeClient::status() const
{
    if (!m_connected.load()) return "OpenCode offline";
    const std::string sid = sessionId();
    return "OpenCode " + m->address() + (sid.empty() ? "" : " · " + sid.substr(0, 12));
}

std::string OpenCodeClient::request(const std::string& method, const std::string& path,
                                    const std::string& body, std::string* err)
{
    if (!parseUrl(m->url, m->host, m->port, err))
        return {};
    int fd = connectSocket(m->host, m->port, err);
    if (fd < 0) return {};
    // A remote/unresponsive service must never stall the app's startup or a
    // UI frame for the full default recv timeout.
    timeval tv { 1, 0 }; // 1 s
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << m->host << ":" << m->port << "\r\n"
        << m->auth
        << "Accept: application/json\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
    const std::string wire = req.str();
    size_t sent = 0;
    while (sent < wire.size()) {
        ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += size_t(n);
    }
    std::string response = readAll(fd);
    ::close(fd);
    size_t hdrEnd = response.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        if (err) *err = "malformed OpenCode response";
        return {};
    }
    std::string headers = lower(response.substr(0, hdrEnd));
    std::string payload = response.substr(hdrEnd + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos)
        payload = decodeChunked(payload);
    int code = 0;
    size_t sp = response.find(' ');
    if (sp != std::string::npos) code = std::atoi(response.c_str() + sp + 1);
    if (code < 200 || code >= 300) {
        if (err) *err = "OpenCode HTTP " + std::to_string(code) + ": " + payload.substr(0, 200);
        return {};
    }
    return payload;
}

bool OpenCodeClient::connect(std::string* err)
{
    std::string body = request("GET", "/api/info", "", err);
    if (body.empty()) {
        // The background service can be restarted with a rotated password;
        // refresh the registration file once before giving up.
        if (err && err->find("401") != std::string::npos) {
            const std::string pw = servicePassword();
            if (!pw.empty()) {
                m->auth = "Authorization: Basic " + base64("opencode:" + pw) + "\r\n";
                err->clear();
                body = request("GET", "/api/info", "", err);
            }
        }
        if (body.empty()) return false;
    }
    m_connected.store(true);
    return true;
}

bool OpenCodeClient::createSession(const std::string& directory, const std::string& agent,
                                   const std::string& modelId,
                                   const std::string& providerID,
                                   std::string* err)
{
    std::ostringstream b;
    b << "{\"agent\":\"" << agent << "\",\"location\":{\"directory\":\""
      << directory << "\"},\"title\":\"Voxelforge AI panel\"";
    if (!modelId.empty() && !providerID.empty())
        b << ",\"model\":{\"id\":\"" << modelId << "\",\"providerID\":\""
          << providerID << "\"}";
    b << "}";
    std::string res = request("POST", "/api/session", b.str(), err);
    if (res.empty()) return false;
    size_t p = res.find("\"id\"");
    if (p == std::string::npos) {
        if (err) *err = "session create response has no id";
        return false;
    }
    p = res.find('"', p + 5);
    size_t e = p == std::string::npos ? std::string::npos : res.find('"', p + 1);
    if (e == std::string::npos) {
        if (err) *err = "session create response has a malformed id";
        return false;
    }
    setSessionId(res.substr(p + 1, e - p - 1));
    return true;
}

bool OpenCodeClient::ensureMcpConnected(const std::string& directory, std::string* err)
{
    const std::string q = "?location%5Bdirectory%5D=" + urlEncode(directory);
    const std::string status = request("GET", "/api/mcp" + q, "", err);
    if (status.empty()) return false;
    // Already connected (or connecting) — do not spam the lazy plugin.
    if (status.find("\"connected\"") != std::string::npos ||
        status.find("\"pending\"") != std::string::npos)
        return true;
    if (status.find("\"voxelforge\"") == std::string::npos) {
        if (err) *err = "voxelforge MCP server not configured in OpenCode";
        return false;
    }
    return !request("POST", "/api/experimental/mcp/voxelforge/connect" + q,
                    "", err).empty();
}

std::vector<OpenCodeModel> OpenCodeClient::listModels(const std::string& directory,
                                                       std::string* err)
{
    const std::string enc = urlEncode(directory);
    std::string res = request("GET",
                              "/api/model?location%5Bdirectory%5D=" + enc, "", err);
    std::vector<OpenCodeModel> out;
    if (res.empty()) return out;
    // Find the "data" array, then walk balanced objects (string-aware).
    size_t arr = res.find("\"data\"");
    if (arr == std::string::npos) return out;
    arr = res.find('[', arr);
    if (arr == std::string::npos) return out;
    size_t i = arr + 1;
    while (i < res.size()) {
        size_t open = res.find('{', i);
        if (open == std::string::npos) break;
        int depth = 0;
        bool inStr = false, esc = false;
        size_t end = open;
        for (; end < res.size(); ++end) {
            char c = res[end];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { ++end; break; }
        }
        std::string obj = res.substr(open, end - open);
        i = end;
        OpenCodeModel m;
        if (jsonGetString(obj, "id", m.id) &&
            jsonGetString(obj, "name", m.name) &&
            jsonGetString(obj, "providerID", m.providerID)) {
            std::string type;
            if (!jsonGetString(obj, "type", type) || type == "chat")
                out.push_back(std::move(m));
        }
        if (res.find(',', i) == std::string::npos) break;
        i = res.find(',', i) + 1;
    }
    // Beam Free first (the project default), then alphabetical by name.
    std::sort(out.begin(), out.end(), [](const OpenCodeModel& a, const OpenCodeModel& b) {
        if ((a.id == "beam-free") != (b.id == "beam-free"))
            return a.id == "beam-free";
        return a.name < b.name;
    });
    return out;
}

bool OpenCodeClient::setSessionModel(const std::string& modelId,
                                     const std::string& providerID,
                                     std::string* err)
{
    if (sessionId().empty() || modelId.empty() || providerID.empty()) return false;
    return !request("POST", "/api/session/" + m_sessionId + "/model",
                    "{\"model\":{\"id\":\"" + modelId + "\",\"providerID\":\"" +
                        providerID + "\"}}", err).empty();
}

bool OpenCodeClient::attachSession(const std::string& id, std::string* err)
{
    if (id.empty()) return false;
    std::string res = request("GET", "/api/session/" + id, "", err);
    if (res.empty()) return false;
    setSessionId(id);
    return true;
}

bool OpenCodeClient::startEventStream(std::string* err)
{
    if (m->worker.joinable())
        return true;
    if (!parseUrl(m->url, m->host, m->port, err))
        return false;
    m->stop.store(false);
    m->worker = std::thread([this]() {
        while (!m->stop.load()) {
            int fd = connectSocket(m->host, m->port, nullptr);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            m->sseFd.store(fd);
            // Repeated `types` params are the V2 way to filter (a single
            // comma-separated value returns 400). Cuts service-wide noise.
            static const char* kTypes[] = {
                "session.text.delta", "session.tool.input.started",
                "session.tool.called", "session.tool.success",
                "session.tool.failed", "session.idle",
                "session.execution.failed", "permission.asked" };
            std::string eventPath = "/api/event?";
            for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); ++i) {
                if (i) eventPath += '&';
                eventPath += "types=";
                eventPath += kTypes[i];
            }
            std::string req = std::string("GET ") + eventPath +
                              " HTTP/1.1\r\nHost: " +
                              m->host + ":" + std::to_string(m->port) + "\r\n" +
                              m->auth +
                              "Accept: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Connection: keep-alive\r\n\r\n";
            size_t sent = 0;
            while (sent < req.size()) {
                ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) break;
                sent += size_t(n);
            }
            // Response headers (the server may hold the connection open).
            std::string headerBuf;
            bool ok = false;
            while (!m->stop.load()) {
                char c;
                ssize_t n = ::recv(fd, &c, 1, 0);
                if (n <= 0) break;
                headerBuf.push_back(c);
                if (headerBuf.size() > 100000) break;
                if (headerBuf.size() >= 4 &&
                    headerBuf.compare(headerBuf.size() - 4, 4, "\r\n\r\n") == 0) {
                    ok = headerBuf.find(" 200 ") != std::string::npos;
                    break;
                }
            }
            if (ok) {
                // The service may have come up after ChatUi::init() failed;
                // a live SSE stream is definitive proof it is reachable.
                m_connected.store(true);
                ChunkDecoder dec;
                std::string lines;
                char buf[16384];
                while (!m->stop.load()) {
                    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    lines += dec.feed(std::string(buf, size_t(n)));
                    size_t eol;
                    while ((eol = lines.find('\n')) != std::string::npos) {
                        std::string line = lines.substr(0, eol);
                        lines.erase(0, eol + 1);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.rfind("data:", 0) == 0) {
                            std::string json = line.substr(5);
                            while (!json.empty() && json.front() == ' ') json.erase(json.begin());
                            if (!json.empty() && m->onEvent)
                                m->onEvent({ parseEventType(json), json });
                        }
                    }
                }
            }
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            if (m->sseFd.load() == fd) m->sseFd.store(-1);
            if (!m->stop.load())
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        m_connected.store(false);
    });
    return true;
}

bool OpenCodeClient::sendPrompt(const std::string& text, std::string* err)
{
    if (sessionId().empty()) {
        if (err) *err = "no OpenCode session";
        return false;
    }
    std::ostringstream b;
    b << "{\"text\":\"";
    for (char c : text) {
        if (c == '"' || c == '\\') b << '\\';
        if (c == '\n') b << "\\n";
        else b << c;
    }
    b << "\"}";
    return !request("POST", "/api/session/" + m_sessionId + "/prompt", b.str(), err).empty();
}

bool OpenCodeClient::interrupt(std::string* err)
{
    if (sessionId().empty()) return false;
    return !request("POST", "/api/session/" + m_sessionId + "/interrupt", "", err).empty();
}

bool OpenCodeClient::replyPermission(const std::string& id, const std::string& decision,
                                     std::string* err)
{
    if (sessionId().empty()) return false;
    return !request("POST", "/api/session/" + m_sessionId + "/permission/" + id + "/reply",
                    "{\"decision\":\"" + decision + "\"}", err).empty();
}

bool OpenCodeClient::setEnvironment(const std::map<std::string, std::string>& vars,
                                    std::string* err)
{
    if (sessionId().empty()) return false;
    std::ostringstream b;
    b << "{\"variables\":{";
    bool first = true;
    for (const auto& kv : vars) {
        if (!first) b << ',';
        first = false;
        b << '"' << kv.first << "\":\"";
        for (char c : kv.second) {
            if (c == '"' || c == '\\') b << '\\';
            b << c;
        }
        b << '"';
    }
    b << "}}";
    return !request("PUT", "/api/session/" + m_sessionId + "/environment", b.str(), err).empty();
}

} // namespace vf::ai
