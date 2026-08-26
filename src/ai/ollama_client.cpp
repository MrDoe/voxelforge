#include "ai/ollama_client.hpp"
#include "ai/system_prompt.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <sstream>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace vf::ai {

OllamaClient::OllamaClient(std::string baseUrl, std::string model, int timeoutMs)
    : m_baseUrl(std::move(baseUrl)), m_model(std::move(model)), m_timeoutMs(timeoutMs) {
    // env overrides
    if (const char* e = getenv("VF_LLM_URL")) m_baseUrl = e;
    if (const char* e = getenv("OLLAMA_HOST")) {
        std::string h=e;
        if (h.find("http")!=0) h="http://"+h;
        m_baseUrl = h;
    }
    if (const char* e = getenv("VF_LLM_MODEL")) m_model = e;
    if (const char* e = getenv("OLLAMA_MODEL")) m_model = e;
    // strip trailing slash
    while (!m_baseUrl.empty() && m_baseUrl.back()=='/') m_baseUrl.pop_back();
    // llama.cpp / OpenAI-compatible servers are usually configured as
    // http://host:8080/v1 - move the suffix into the chat path so requests
    // don't end up at /v1/v1/...
    if (m_baseUrl.size() >= 3 && m_baseUrl.compare(m_baseUrl.size()-3, 3, "/v1") == 0) {
        m_baseUrl.erase(m_baseUrl.size()-3);
        m_chatPath = "/v1/chat/completions";
    }
}

std::string escapeJsonString(const std::string& s) {
    std::string o; o.reserve(s.size()+4);
    for (char c: s) {
        switch(c){
            case '"': o+="\\\""; break;
            case '\\': o+="\\\\"; break;
            case '\n': o+="\\n"; break;
            case '\r': o+="\\r"; break;
            case '\t': o+="\\t"; break;
            default: if ((unsigned char)c<0x20) { char b[7]; snprintf(b,7,"\\u%04x",c); o+=b; } else o+=c; break;
        }
    }
    return o;
}

std::string buildToolDefsJson() {
    // 4 tools matching editable_world primitives + anchor handling
    // Notes: gemma4 understands standard OpenAI tool format; Ollama forwards it.
    return R"JSON([
      {"type":"function","function":{"name":"create_box","description":"Create an axis-aligned box at the selected voxel or explicit anchor","parameters":{"type":"object","properties":{"name":{"type":"string","description":"object name id"},"size":{"type":"array","items":{"type":"integer"},"minItems":3,"maxItems":3,"description":"size in voxels [sx,sy,sz] e.g. [3,2,3]"},"material":{"type":"integer","minimum":0,"maximum":8,"description":"palette id 0..8"},"anchor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"optional world lattice anchor [ix,iy,iz]; if omitted uses selected voxel"}},"required":["name","size","material"]}}},
      {"type":"function","function":{"name":"create_cylinder","description":"Vertical cylinder at selection","parameters":{"type":"object","properties":{"name":{"type":"string"},"radius":{"type":"number","description":"meters"},"height":{"type":"number","description":"meters"},"material":{"type":"integer","minimum":0,"maximum":8}},"required":["name","radius","height","material"]}}},
      {"type":"function","function":{"name":"create_ellipsoid","description":"Ellipsoid/boulder at selection","parameters":{"type":"object","properties":{"name":{"type":"string"},"radius":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"radii meters [rx,ry,rz]"},"material":{"type":"integer","minimum":0,"maximum":8}},"required":["name","radius","material"]}}},
      {"type":"function","function":{"name":"create_stamp","description":"Literal voxel stamp for signs/mosaics relative to anchor","parameters":{"type":"object","properties":{"name":{"type":"string"},"cells":{"type":"array","items":{"type":"object","properties":{"dx":{"type":"integer"},"dy":{"type":"integer"},"dz":{"type":"integer"},"mat":{"type":"integer","minimum":0,"maximum":8}},"required":["dx","dy","dz","mat"]}}},"required":["name","cells"]}}}
    ])JSON";
}

std::string OllamaClient::buildChatBody(const std::vector<ChatMessage>& history,
                                        const std::string& systemPrompt,
                                        bool enableTools, bool stream) const {
    std::ostringstream oss;
    oss << "{\"model\":\"" << escapeJsonString(m_model) << "\",\"stream\":" << (stream?"true":"false") << ",\"messages\":[";
    bool first=true;
    if (!systemPrompt.empty()) {
        oss << "{\"role\":\"system\",\"content\":\"" << escapeJsonString(systemPrompt) << "\"}";
        first=false;
    }
    for (auto &m: history) {
        if (!first) oss << ",";
        oss << "{\"role\":\"" << escapeJsonString(m.role) << "\",\"content\":\"" << escapeJsonString(m.content) << "\"}";
        first=false;
    }
    oss << "]";
    if (enableTools) oss << ",\"tools\":" << buildToolDefsJson();
    oss << "}";
    return oss.str();
}

// Minimal URL parse: http://host:port[/prefix]
struct ParsedUrl { std::string host; int port=80; std::string prefix; };
static ParsedUrl parseBase(const std::string& base) {
    ParsedUrl u;
    std::string s=base;
    if (s.rfind("http://",0)==0) s=s.substr(7);
    else if (s.rfind("https://",0)==0) s=s.substr(8);
    size_t slash = s.find('/');
    std::string hostport = slash==std::string::npos? s: s.substr(0,slash);
    u.prefix = slash==std::string::npos? "" : s.substr(slash);
    size_t colon = hostport.find(':');
    if (colon!=std::string::npos) { u.host=hostport.substr(0,colon);
        try { u.port=std::stoi(hostport.substr(colon+1)); } catch(...) { u.port=80; } }
    else { u.host=hostport; u.port=80; }
    return u;
}

static std::string readAllFromFd(int fd, int timeoutMs) {
    std::string out; out.reserve(8192);
    char buf[8192];
    struct pollfd pfd{fd, POLLIN, 0};
    auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        int remain = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count());
        int pr = poll(&pfd,1, remain);
        if (pr<=0) { if(pr==0) break; else if(errno==EINTR) continue; else break; }
        if (pfd.revents & POLLIN) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n<=0) break;
            out.append(buf, size_t(n));
        }
        if (pfd.revents & (POLLHUP|POLLERR)) break;
    }
    return out;
}

bool OllamaClient::httpPost(const std::string& path, const std::string& body,
                             std::string& outResponseBody, std::string& outError, int* outStatus) const {
    ParsedUrl u = parseBase(m_baseUrl);
    std::string fullPath = u.prefix + path;
    // resolve host
    struct addrinfo hints{}, *res=nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(u.port);
    if (getaddrinfo(u.host.c_str(), portStr.c_str(), &hints, &res)!=0) { outError="dns failed "+u.host; return false; }
    int fd=-1;
    for(auto *p=res;p;p=p->ai_next) { fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue; 
        // nonblock connect with timeout
        int flags=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,flags|O_NONBLOCK);
        int rc=connect(fd,p->ai_addr,p->ai_addrlen);
        if(rc==0) break;
        if(errno==EINPROGRESS) {
            struct pollfd pf{fd,POLLOUT,0};
            int pr=poll(&pf,1,3000);
            if(pr>0 && (pf.revents&POLLOUT)) { int err=0; socklen_t el=sizeof(err); getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&el); if(err==0) break; }
        }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);
    if(fd<0){ outError="connect failed "+u.host+":"+portStr; return false; }
    // back to blocking for send/recv
    int fl=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL, fl & ~O_NONBLOCK);
    // build request
    std::ostringstream req;
    req << "POST " << fullPath << " HTTP/1.1\r\n";
    req << "Host: " << u.host << ":" << u.port << "\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n\r\n";
    req << body;
    std::string rs = req.str();
    size_t sent=0; while(sent<rs.size()){ ssize_t n=send(fd, rs.data()+sent, rs.size()-sent,0); if(n<=0){ if(errno==EINTR) continue; outError="send failed"; close(fd); return false;} sent+=size_t(n); }
    std::string raw = readAllFromFd(fd, m_timeoutMs);
    close(fd);
    // parse HTTP response: status line + header \r\n\r\n + body
    size_t hdrEnd = raw.find("\r\n\r\n");
    if(hdrEnd==std::string::npos){ outError="no http header"; return false; }
    std::string header = raw.substr(0,hdrEnd);
    outResponseBody = raw.substr(hdrEnd+4);
    // handle chunked encoding
    if(header.find("Transfer-Encoding: chunked")!=std::string::npos || header.find("transfer-encoding: chunked")!=std::string::npos){
        std::string decoded; size_t pos=0;
        while(pos<outResponseBody.size()){
            size_t eol = outResponseBody.find("\r\n",pos);
            if(eol==std::string::npos) break;
            std::string hex = outResponseBody.substr(pos,eol-pos);
            size_t chunk=0; try{chunk=std::stoul(hex,nullptr,16);}catch(...){break;}
            if(chunk==0) break;
            pos=eol+2;
            if(pos+chunk > outResponseBody.size()) break;
            decoded.append(outResponseBody.data()+pos, chunk);
            pos+=chunk+2;
        }
        outResponseBody=decoded;
    }
    int status=0;
    {
        size_t eol=header.find("\r\n");
        std::string sl = header.substr(0,eol);
        // HTTP/1.1 200 OK
        size_t sp=sl.find(' ');
        if(sp!=std::string::npos){ status = std::stoi(sl.substr(sp+1)); }
    }
    if(outStatus) *outStatus=status;
    if(status<200||status>=300){ outError="http "+std::to_string(status)+" "+outResponseBody.substr(0,200); return false;}
    return true;
}
bool OllamaClient::httpGet(const std::string& path, std::string& outBody, std::string& outError) const {
    ParsedUrl u = parseBase(m_baseUrl);
    std::string fullPath = u.prefix + path;
    struct addrinfo hints{},*res=nullptr; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    std::string ps=std::to_string(u.port);
    if(getaddrinfo(u.host.c_str(),ps.c_str(),&hints,&res)!=0){ outError="dns"; return false; }
    int fd=-1; for(auto *p=res;p;p=p->ai_next){ fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue; if(connect(fd,p->ai_addr,p->ai_addrlen)==0) break; close(fd); fd=-1; } freeaddrinfo(res); if(fd<0){outError="connect"; return false;}
    std::ostringstream req; req<<"GET "<<fullPath<<" HTTP/1.1\r\nHost: "<<u.host<<":"<<u.port<<"\r\nConnection: close\r\n\r\n";
    std::string rs=req.str();
    size_t sent=0;
    while(sent<rs.size()){ ssize_t n=send(fd,rs.data()+sent,rs.size()-sent,0); if(n<=0){ if(errno==EINTR) continue; outError="send failed"; close(fd); return false;} sent+=size_t(n); }
    std::string raw=readAllFromFd(fd, 3000); close(fd);
    size_t e=raw.find("\r\n\r\n"); if(e==std::string::npos){outError="no header"; return false;}
    std::string header=raw.substr(0,e);
    // require a 2xx status: a 404 from the wrong endpoint kind must not
    // count as "server online" in ping()
    int status=0;
    {
        size_t eol=header.find("\r\n");
        std::string sl=header.substr(0, eol==std::string::npos?header.size():eol);
        size_t sp=sl.find(' ');
        if(sp!=std::string::npos){ try{ status=std::stoi(sl.substr(sp+1)); }catch(...){} }
    }
    if(status<200||status>=300){ outError="http "+std::to_string(status); return false; }
    outBody=raw.substr(e+4);
    return true;
}

bool OllamaClient::ping(std::string* outError) const {
    std::string body, err;
    // try /api/tags as Ollama defines, else fallback to /v1/models
    if(httpGet("/api/tags", body, err)) return true;
    if(httpGet("/v1/models", body, err)) return true;
    if(outError) *outError=err;
    return false;
}

// very small JSON string extractor helpers (no full parser, just search)
static std::string jsonExtractString(const std::string& json, const std::string& key) {
    // find "key" : "value"  (with optional spaces)
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if(pos==std::string::npos) return "";
    pos=json.find(':',pos); if(pos==std::string::npos) return "";
    size_t q=json.find('"',pos+1);
    if(q==std::string::npos) return "";
    size_t q2=q+1;
    std::string out;
    while(q2<json.size()){
        char c=json[q2];
        if(c=='\\' && q2+1<json.size()){ char n=json[q2+1]; if(n=='n') out+='\n'; else if(n=='"') out+='"'; else if(n=='\\') out+='\\'; else out+=n; q2+=2; continue; }
        if(c=='"') break;
        out+=c; ++q2;
    }
    return out;
}

// read a JSON string literal starting at the opening quote; returns decoded
// text and advances pos past the closing quote. Returns false on garbage.
static bool readJsonStringAt(const std::string& json, size_t& pos, std::string& out) {
    out.clear();
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            char n = json[pos + 1];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    if (pos + 5 < json.size()) {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = json[pos + 2 + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                        }
                        // utf-8 encode (BMP only, no surrogate pairs)
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                        pos += 6;
                        continue;
                    }
                    break;
                }
                default: out += n; break;
            }
            pos += 2;
            continue;
        }
        if (c == '"') { ++pos; return true; }
        out += c;
        ++pos;
    }
    return false;
}

// value reader: at a position just after a colon; handles string / null /
// balanced-brace object. For objects returns raw text.
static bool readValueAt(const std::string& json, size_t pos, std::string& out,
                        bool& isString) {
    while (pos < json.size() && isspace((unsigned char)json[pos])) ++pos;
    if (pos >= json.size()) return false;
    isString = json[pos] == '"';
    if (isString) return readJsonStringAt(json, pos, out);
    if (json.compare(pos, 4, "null") == 0) { pos += 4; out.clear(); return true; }
    // object / array: brace-match
    size_t open = json.find_first_of("{[", pos);
    if (open != std::string::npos) {
        char oc = json[open], cc = oc == '{' ? '}' : ']';
        int depth = 0;
        size_t e = open;
        for (; e < json.size(); ++e) {
            char c = json[e];
            if (c == '"') { std::string sink; readJsonStringAt(json, e, sink); --e; continue; }
            if (c == oc) ++depth;
            else if (c == cc && --depth == 0) { ++e; break; }
        }
        out = json.substr(open, e - open);
        return true;
    }
    // bare number/bool
    size_t e = pos;
    while (e < json.size() && (isdigit((unsigned char)json[e]) || strchr(".-+truefalsn", json[e]))) ++e;
    out = json.substr(pos, e - pos);
    return true;
}

bool parseToolCallsFromResponse(const std::string& json, std::vector<ToolCall>& out, std::string& outContent, std::string& outThinking) {
    out.clear();
    outThinking = jsonExtractString(json, "thinking");
    if (outThinking.empty()) outThinking = jsonExtractString(json, "reasoning_content");

    // locate the assistant message block: OpenAI nests it under
    // choices[0].message; Ollama uses a top-level "message"
    std::string msg = json;
    size_t mp = json.find("\"choices\"");
    if (mp != std::string::npos) mp = json.find("\"message\"", mp);
    if (mp == std::string::npos) mp = json.find("\"message\"");
    if (mp != std::string::npos) {
        size_t ob = json.find('{', mp);
        if (ob != std::string::npos) {
            int depth = 0;
            size_t e = ob;
            for (; e < json.size(); ++e) {
                char c = json[e];
                if (c == '"') { std::string sink; readJsonStringAt(json, e, sink); --e; continue; }
                if (c == '{') ++depth;
                else if (c == '}') { if (--depth == 0) { ++e; break; } }
            }
            msg = json.substr(ob, e - ob);
        }
    }

    // content (null-safe)
    {
        size_t cp = msg.find("\"content\"");
        if (cp == std::string::npos) cp = json.find("\"content\"");
        if (cp != std::string::npos) {
            size_t colon = msg.find(':', cp);
            bool isStr = false;
            std::string val;
            if (colon != std::string::npos && readValueAt(msg, colon + 1, val, isStr))
                outContent = isStr ? val : "";
        }
    }

    auto extractToolCallsBlock = [&](const std::string& block) {
        size_t tc = block.find("\"tool_calls\"");
        if (tc == std::string::npos) tc = block.find("\"toolCalls\"");
        if (tc == std::string::npos) return;
        size_t arr = block.find('[', tc);
        if (arr == std::string::npos) return;
        // Walk the elements of the [...] array with a string-aware bracket
        // scan and parse each call object in isolation: arguments may be a
        // nested object carrying its own "name" key (our schemas require
        // one), which must never be mistaken for the next tool's name.
        struct Range { size_t begin, end; };
        std::vector<Range> elems;
        int lvl = 0;
        bool inStr = false;
        size_t start = std::string::npos;
        for (size_t e = arr; e < block.size(); ++e) {
            char c = block[e];
            if (inStr) {
                if (c == '\\')
                    ++e;
                else if (c == '"')
                    inStr = false;
                continue;
            }
            if (c == '"') {
                inStr = true;
            } else if (c == '[' || c == '{') {
                ++lvl;
                if (lvl == 2 && c == '{' && start == std::string::npos)
                    start = e;
            } else if (c == ']' || c == '}') {
                if (lvl == 2 && c == '}' && start != std::string::npos) {
                    elems.push_back({ start, e + 1 });
                    start = std::string::npos;
                }
                --lvl;
                if (lvl <= 0)
                    break;
            }
        }
        for (const Range& rng : elems) {
            std::string elem = block.substr(rng.begin, rng.end - rng.begin);
            size_t fn = elem.find("\"name\"");
            if (fn == std::string::npos)
                continue;
            size_t colon = elem.find(':', fn);
            if (colon == std::string::npos)
                continue;
            size_t qs = colon + 1;
            while (qs < elem.size() && isspace((unsigned char)elem[qs]))
                ++qs;
            std::string name;
            if (!readJsonStringAt(elem, qs, name) || name.empty())
                continue;

            std::string argsJson = "{}";
            size_t argPos = elem.find("\"arguments\"", qs);
            if (argPos != std::string::npos) {
                size_t acolon = elem.find(':', argPos);
                bool isStr = false;
                std::string val;
                if (acolon != std::string::npos &&
                    readValueAt(elem, acolon + 1, val, isStr)) {
                    // OpenAI/llama.cpp: arguments is an escaped STRING holding
                    // the JSON object; Ollama: a nested JSON object
                    argsJson = val.empty() ? "{}" : val;
                }
            }
            ToolCall tcx;
            tcx.name = name;
            tcx.argumentsJson = argsJson;
            out.push_back(std::move(tcx));
        }
    };
    extractToolCallsBlock(msg);
    if (out.empty()) extractToolCallsBlock(json);

    // models without native function calling (gemma templates on llama.cpp
    // without --jinja) write the call into plain content. Synthesize tool
    // calls from {"name":"create_*","arguments":{...}} patterns so every
    // backend works through the same dispatcher.
    if (out.empty() && !outContent.empty()) {
        size_t pos = 0;
        while (true) {
            size_t nm = outContent.find("\"name\"", pos);
            if (nm == std::string::npos) break;
            size_t colon = outContent.find(':', nm);
            size_t qs = colon + 1;
            while (qs < outContent.size() && isspace((unsigned char)outContent[qs])) ++qs;
            std::string name;
            size_t after = qs;
            if (!readJsonStringAt(outContent, after, name)) break;
            bool ours = name.rfind("create_", 0) == 0 || name == "list_world" ||
                        name == "probe";
            size_t argPos = outContent.find("\"arguments\"", after);
            size_t nextName = outContent.find("\"name\"", after);
            if (ours && argPos != std::string::npos &&
                (nextName == std::string::npos || argPos < nextName)) {
                size_t acolon = outContent.find(':', argPos);
                bool isStr = false;
                std::string val;
                if (acolon != std::string::npos &&
                    readValueAt(outContent, acolon + 1, val, isStr)) {
                    ToolCall tcx;
                    tcx.name = name;
                    tcx.argumentsJson = val.empty() ? "{}" : val;
                    out.push_back(std::move(tcx));
                }
            }
            pos = after;
        }
    }

    for (auto& t : out) {
        if (t.name == "create_box") t.primitive = "box";
        else if (t.name == "create_cylinder") t.primitive = "cylinder";
        else if (t.name == "create_ellipsoid") t.primitive = "ellipsoid";
        else if (t.name == "create_stamp") t.primitive = "stamp";
        else t.primitive = t.name;
    }
    return true;
}

ChatResult OllamaClient::parseChatResponse(const std::string& body) const {
    ChatResult r;
    r.rawResponse=body;
    std::vector<ToolCall> tcs; std::string cnt, thinking;
    parseToolCallsFromResponse(body, tcs, cnt, thinking);
    r.content = cnt; r.thinking=thinking; r.toolCalls = std::move(tcs);
    r.ok=true;
    return r;
}

ChatResult OllamaClient::chat(const std::vector<ChatMessage>& history, const std::string& systemPrompt, bool enableTools) const {
    std::string resp, err; int st=0;
    auto buildOpenAI = [&](){
        std::ostringstream o;
        o<<"{\"model\":\""<<escapeJsonString(m_model)<<"\",\"messages\":[";
        bool first=true;
        if(!systemPrompt.empty()){ o<<"{\"role\":\"system\",\"content\":\""<<escapeJsonString(systemPrompt)<<"\"}"; first=false; }
        for(auto &m: history){ if(!first) o<<","; o<<"{\"role\":\""<<escapeJsonString(m.role)<<"\",\"content\":\""<<escapeJsonString(m.content)<<"\"}"; first=false; }
        o<<"]";
        if(enableTools) o<<",\"tools\":"<<buildToolDefsJson();
        o<<"}";
        return o.str();
    };
    auto buildOllama = [&](){ return buildChatBody(history, systemPrompt, enableTools, false); };
    // try sticky path with matching body
    if(m_chatPath=="/v1/chat/completions"){
        std::string body = buildOpenAI();
        if(httpPost(m_chatPath, body, resp, err, &st)) return parseChatResponse(resp);
        // fallback to Ollama
        std::string alt = buildOllama();
        const char* other="/api/chat";
        if(httpPost(other, alt, resp, err, &st)){ m_chatPath=other; return parseChatResponse(resp); }
        ChatResult cr; cr.ok=false; cr.error=err; cr.rawResponse=resp; return cr;
    }else{
        std::string body = buildOllama();
        if(httpPost(m_chatPath, body, resp, err, &st)) return parseChatResponse(resp);
        std::string alt = buildOpenAI();
        const char* other="/v1/chat/completions";
        if(httpPost(other, alt, resp, err, &st)){ m_chatPath=other; return parseChatResponse(resp); }
        ChatResult cr; cr.ok=false; cr.error=err; cr.rawResponse=resp; return cr;
    }
}

ChatResult OllamaClient::chatStreaming(const std::vector<ChatMessage>& history,
                                       const std::string& systemPrompt,
                                       bool enableTools,
                                       std::function<void(const std::string&,bool)> onToken) const {
    // For now, non-streaming is used and tokens simulated via content split.
    // Full streaming would require chunked reading and NDJSON parsing.
    // Implemented as fallback: call chat() then fire callbacks.
    ChatResult r = chat(history, systemPrompt, enableTools);
    if(onToken && r.ok){
        if(!r.thinking.empty()) onToken(r.thinking, true);
        if(!r.content.empty()) onToken(r.content, false);
    }
    return r;
}

}
