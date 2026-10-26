// micro_clip.cpp - recorded animation clip for a MICRO object.
#include "voxel/micro_clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace vf::voxel {

namespace {

struct Json {
    const std::string& s;
    size_t i = 0;
    void skipWs()
    {
        while (i < s.size()) {
            if (std::isspace(unsigned(s[i]))) ++i;
            else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else break;
        }
    }
    bool eat(char c) { skipWs(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool peek(char c) { skipWs(); return i < s.size() && s[i] == c; }
    bool str(std::string& out)
    {
        skipWs();
        if (i >= s.size() || s[i] != '"') return false;
        ++i; out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            out.push_back(s[i++]);
        }
        return i < s.size() && s[i++] == '"';
    }
    bool num(float& v)
    {
        skipWs();
        char* end = nullptr;
        const char* begin = s.c_str() + i;
        v = std::strtof(begin, &end);
        if (end == begin) return false;
        i += size_t(end - begin);
        return true;
    }
    bool boolean(bool& v)
    {
        skipWs();
        if (s.compare(i, 4, "true") == 0) { i += 4; v = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; v = false; return true; }
        return false;
    }
    bool skipValue()
    {
        skipWs();
        if (i >= s.size()) return false;
        char c = s[i];
        std::string sinkStr; float sinkNum = 0.f; bool sinkBool = false;
        if (c == '"') return str(sinkStr);
        if (c == 't' || c == 'f') return boolean(sinkBool);
        if (c == '[' || c == '{') {
            char open = c, close = c == '[' ? ']' : '}';
            int depth = 0;
            while (i < s.size()) {
                char d = s[i++];
                if (d == open) ++depth;
                else if (d == close) { --depth; if (depth == 0) return true; }
                else if (d == '"') {
                    while (i < s.size() && s[i] != '"') {
                        if (s[i] == '\\' && i + 1 < s.size()) ++i;
                        ++i;
                    }
                    if (i < s.size()) ++i;
                }
            }
            return false;
        }
        char* end = nullptr;
        const char* begin = s.c_str() + i;
        std::strtof(begin, &end);
        if (end != begin) { i += size_t(end - begin); return true; }
        return false;
    }
    bool numVec3(glm::vec3& v)
    {
        if (!eat('[')) return false;
        for (int c = 0; c < 3; ++c) {
            if (!num(v[c])) return false;
            if (c < 2) eat(',');
        }
        return eat(']');
    }
};

ClipEase parseEase(const std::string& s)
{
    if (s == "step") return ClipEase::Step;
    if (s == "linear") return ClipEase::Linear;
    return ClipEase::Smooth;
}

const char* easeName(ClipEase e)
{
    switch (e) {
        case ClipEase::Step: return "step";
        case ClipEase::Linear: return "linear";
        case ClipEase::Smooth: return "smooth";
    }
    return "smooth";
}

ClipClock parseClock(const std::string& s)
{
    if (s == "distance") return ClipClock::Distance;
    return ClipClock::Time;
}

const char* clockName(ClipClock c)
{
    return c == ClipClock::Distance ? "distance" : "time";
}

} // namespace

bool parseMicroClip(const std::string& text, MicroClip& out)
{
    out = MicroClip();
    Json j { text };
    if (!j.eat('{')) return false;

    std::string k;
    while (j.str(k)) {
        if (!j.eat(':')) return false;
        if (k == "name") {
            if (!j.str(out.name)) return false;
        } else if (k == "description") {
            if (!j.str(out.description)) return false;
        } else if (k == "layer") {
            if (!j.str(out.layer)) return false;
        } else if (k == "length") {
            if (!j.num(out.length)) return false;
        } else if (k == "fps") {
            if (!j.num(out.fps)) return false;
        } else if (k == "loop") {
            if (!j.boolean(out.loop)) return false;
        } else if (k == "clock") {
            std::string v;
            if (!j.str(v)) return false;
            out.clock = parseClock(v);
        } else if (k == "speed") {
            if (!j.num(out.speed)) return false;
        } else if (k == "stride") {
            if (!j.num(out.stride)) return false;
        } else if (k == "blendIn") {
            if (!j.num(out.blendIn)) return false;
        } else if (k == "body" && j.eat('[')) {
            while (j.peek('{')) {
                BodyKey bk;
                if (!j.eat('{')) break;
                std::string bkKey;
                while (j.str(bkKey)) {
                    if (!j.eat(':')) return false;
                    if (bkKey == "t") {
                        if (!j.num(bk.t)) return false;
                    } else if (bkKey == "offset") {
                        if (!j.numVec3(bk.offset)) return false;
                    } else if (bkKey == "heading") {
                        if (!j.num(bk.headingDeg)) return false;
                    } else if (bkKey == "ease") {
                        std::string v;
                        if (!j.str(v)) return false;
                        bk.ease = parseEase(v);
                    } else {
                        if (!j.skipValue()) return false;
                    }
                    if (!j.eat(',')) break;
                }
                j.eat('}');
                out.body.push_back(bk);
                if (!j.eat(',')) break;
            }
            j.eat(']');
        } else if (k == "tracks" && j.eat('[')) {
            while (j.peek('{')) {
                BoneTrack tr;
                if (!j.eat('{')) break;
                std::string tk;
                while (j.str(tk)) {
                    if (!j.eat(':')) return false;
                    if (tk == "bone") {
                        float v = 0.f;
                        if (!j.num(v)) return false;
                        tr.boneId = (uint32_t)v;
                    } else if (tk == "name") {
                        if (!j.str(tr.name)) return false;
                    } else if (tk == "keys" && j.eat('[')) {
                        while (j.peek('{')) {
                            BoneKey kk;
                            if (!j.eat('{')) break;
                            std::string kkKey;
                            while (j.str(kkKey)) {
                                if (!j.eat(':')) return false;
                                if (kkKey == "t") {
                                    if (!j.num(kk.t)) return false;
                                } else if (kkKey == "rot") {
                                    if (!j.numVec3(kk.rotDeg)) return false;
                                } else if (kkKey == "ease") {
                                    std::string v;
                                    if (!j.str(v)) return false;
                                    kk.ease = parseEase(v);
                                } else {
                                    if (!j.skipValue()) return false;
                                }
                                if (!j.eat(',')) break;
                            }
                            j.eat('}');
                            tr.keys.push_back(kk);
                            if (!j.eat(',')) break;
                        }
                        j.eat(']');
                    } else {
                        if (!j.skipValue()) return false;
                    }
                    if (!j.eat(',')) break;
                }
                j.eat('}');
                out.tracks.push_back(tr);
                if (!j.eat(',')) break;
            }
            j.eat(']');
        } else {
            if (!j.skipValue()) return false;
        }
        if (!j.eat(',')) break;
    }
    j.eat('}');

    for (auto& tr : out.tracks)
        std::sort(tr.keys.begin(), tr.keys.end(),
                  [](const BoneKey& a, const BoneKey& b) { return a.t < b.t; });
    std::sort(out.body.begin(), out.body.end(),
              [](const BodyKey& a, const BodyKey& b) { return a.t < b.t; });

    if (out.description.empty()) out.description = out.name;
    if (out.length <= 0.f) out.length = 1.f;
    if (out.fps <= 0.f) out.fps = 30.f;
    if (out.stride <= 0.f) out.stride = 0.90f;
    if (out.speed <= 0.f) out.speed = 0.20f;
    if (out.blendIn < 0.f) out.blendIn = 0.f;
    return true;
}

std::string serializeMicroClip(const MicroClip& clip)
{
    std::ostringstream o;
    o << "{\n";
    o << "  \"format\": \"vf-anim/1\",\n";
    o << "  \"name\": \"" << clip.name << "\",\n";
    o << "  \"description\": \"" << clip.description << "\",\n";
    o << "  \"layer\": \"" << clip.layer << "\",\n";
    o << "  \"length\": " << clip.length << ",\n";
    o << "  \"fps\": " << clip.fps << ",\n";
    o << "  \"loop\": " << (clip.loop ? "true" : "false") << ",\n";
    o << "  \"clock\": \"" << (clip.clock == ClipClock::Distance ? "distance" : "time") << "\",\n";
    o << "  \"speed\": " << clip.speed << ",\n";
    o << "  \"stride\": " << clip.stride << ",\n";
    o << "  \"blendIn\": " << clip.blendIn << ",\n";

    o << "  \"body\": [";
    for (size_t i = 0; i < clip.body.size(); ++i) {
        const BodyKey& bk = clip.body[i];
        if (i) o << ",";
        o << "\n    { \"t\": " << bk.t
           << ", \"offset\": [" << bk.offset.x << ", " << bk.offset.y << ", " << bk.offset.z << "]"
           << ", \"heading\": " << bk.headingDeg
           << ", \"ease\": \"" << (bk.ease == ClipEase::Step ? "step" : bk.ease == ClipEase::Linear ? "linear" : "smooth") << "\" }";
    }
    if (!clip.body.empty()) o << "\n  ";
    o << "],\n";

    o << "  \"tracks\": [";
    for (size_t ti = 0; ti < clip.tracks.size(); ++ti) {
        const BoneTrack& tr = clip.tracks[ti];
        if (ti) o << ",";
        o << "\n    { \"bone\": " << tr.boneId
           << ", \"name\": \"" << tr.name << "\", \"keys\": [";
        for (size_t ki = 0; ki < tr.keys.size(); ++ki) {
            const BoneKey& kk = tr.keys[ki];
            if (ki) o << ",";
            o << "\n      { \"t\": " << kk.t
               << ", \"rot\": [" << kk.rotDeg.x << ", " << kk.rotDeg.y << ", " << kk.rotDeg.z << "]"
               << ", \"ease\": \"" << (kk.ease == ClipEase::Step ? "step" : kk.ease == ClipEase::Linear ? "linear" : "smooth") << "\" }";
        }
        if (!tr.keys.empty()) o << "\n    ";
        o << "] }";
    }
    if (!clip.tracks.empty()) o << "\n  ";
    o << "]\n";
    o << "}\n";
    return o.str();
}

bool readMicroClip(const std::string& path, MicroClip& out)
{
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    return parseMicroClip(buf.str(), out);
}

bool writeMicroClip(const std::string& path, const MicroClip& clip)
{
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const std::string text = serializeMicroClip(clip);
    if (std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
        std::fclose(f);
        std::remove(tmp.c_str());
        return false;
    }
    std::fclose(f);
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

float wrapClipTime(float t, float length, bool loop)
{
    if (length <= 0.f) return 0.f;
    if (loop) {
        t = std::fmod(t, length);
        if (t < 0.f) t += length;
        return t;
    }
    return std::clamp(t, 0.f, length);
}

glm::vec3 interpEuler(const BoneKey& a, const BoneKey& b, float t)
{
    const float u = (a.ease == ClipEase::Step) ? 0.f
                  : (a.ease == ClipEase::Smooth) ? (t * t * (3.f - 2.f * t)) : t;
    return a.rotDeg + (b.rotDeg - a.rotDeg) * u;
}

void interpBody(const BodyKey& a, const BodyKey& b, float t,
                glm::vec3& outOffset, float& outHeadingDeg)
{
    const float u = (a.ease == ClipEase::Step) ? 0.f
                  : (a.ease == ClipEase::Smooth) ? (t * t * (3.f - 2.f * t)) : t;
    outOffset = a.offset + (b.offset - a.offset) * u;
    outHeadingDeg = a.headingDeg + (b.headingDeg - a.headingDeg) * u;
}

void MicroClip::sample(float t, const MicroRig& rig,
                      std::vector<glm::mat3>& localRot,
                      glm::vec3& bodyOffset, float& bodyHeadingDeg) const
{
    const size_t n = rig.bones.size();
    if (localRot.size() != n) localRot.assign(n, glm::mat3(1.f));
    else for (auto& m : localRot) m = glm::mat3(1.f);

    bodyOffset = glm::vec3(0.f);
    bodyHeadingDeg = 0.f;

    if (empty()) return;

    if (!body.empty()) {
        if (body.size() == 1 || t <= body.front().t) {
            bodyOffset = body.front().offset;
            bodyHeadingDeg = body.front().headingDeg;
        } else if (t >= body.back().t) {
            bodyOffset = body.back().offset;
            bodyHeadingDeg = body.back().headingDeg;
        } else {
            for (size_t i = 0; i + 1 < body.size(); ++i) {
                if (t >= body[i].t && t <= body[i + 1].t) {
                    const float span = body[i + 1].t - body[i].t;
                    const float u = (span > 1e-6f) ? (t - body[i].t) / span : 0.f;
                    interpBody(body[i], body[i + 1], u, bodyOffset, bodyHeadingDeg);
                    break;
                }
            }
        }
    }

    for (const auto& tr : tracks) {
        int bi = -1;
        for (size_t i = 0; i < rig.bones.size(); ++i) {
            if (rig.bones[i].id != 0 && rig.bones[i].id == tr.boneId) { bi = (int)i; break; }
        }
        if (bi < 0) {
            (void)0;
        }
        if (bi < 0 && tr.boneId >= 1 && tr.boneId <= rig.bones.size()) {
            bi = (int)tr.boneId - 1;
        }
        if (bi < 0 || tr.keys.empty()) continue;

        const BoneKey* k0 = nullptr;
        const BoneKey* k1 = nullptr;
        if (tr.keys.size() == 1 || t <= tr.keys.front().t) {
            k0 = &tr.keys.front();
            k1 = &tr.keys.front();
        } else if (t >= tr.keys.back().t) {
            k0 = &tr.keys.back();
            k1 = &tr.keys.back();
        } else {
            for (size_t i = 0; i + 1 < tr.keys.size(); ++i) {
                if (t >= tr.keys[i].t && t <= tr.keys[i + 1].t) {
                    k0 = &tr.keys[i];
                    k1 = &tr.keys[i + 1];
                    break;
                }
            }
        }
        if (!k0) continue;

        glm::vec3 euler;
        if (k0 == k1) {
            euler = k0->rotDeg;
        } else {
            const float span = k1->t - k0->t;
            const float u = (span > 1e-6f) ? (t - k0->t) / span : 0.f;
            euler = interpEuler(*k0, *k1, u);
        }

        const float rx = glm::radians(euler.x);
        const float ry = glm::radians(euler.y);
        const float rz = glm::radians(euler.z);
        const glm::mat3 Rx = glm::mat3(
            1, 0, 0,
            0, std::cos(rx), std::sin(rx),
            0, -std::sin(rx), std::cos(rx));
        const glm::mat3 Ry = glm::mat3(
            std::cos(ry), 0, -std::sin(ry),
            0, 1, 0,
            std::sin(ry), 0, std::cos(ry));
        const glm::mat3 Rz = glm::mat3(
            std::cos(rz), std::sin(rz), 0,
            -std::sin(rz), std::cos(rz), 0,
            0, 0, 1);
        localRot[(size_t)bi] = Rx * Ry * Rz;
    }
}

std::vector<std::string> MicroClip::validate(const MicroRig& rig) const
{
    std::vector<std::string> issues;
    if (name.empty()) issues.push_back("clip name is empty");
    for (char c : name) {
        if (!(std::isalnum((unsigned char)c) || c == '_')) {
            issues.push_back("clip name must be [a-z0-9_]");
            break;
        }
    }
    if (length <= 0.f) issues.push_back("length must be > 0");
    if (fps <= 0.f) issues.push_back("fps must be > 0");
    if (stride <= 0.f) issues.push_back("stride must be > 0");
    if (speed <= 0.f) issues.push_back("speed must be > 0");
    if (blendIn < 0.f) issues.push_back("blendIn must be >= 0");

    for (const auto& bk : body) {
        if (bk.t < 0.f || bk.t > length + 1e-4f)
            issues.push_back("body key outside [0,length]");
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(bk.offset[c]) > 0.11f)
                issues.push_back("body offset exceeds 0.1 m");
        }
        if (std::fabs(bk.headingDeg) > 15.1f)
            issues.push_back("body heading exceeds 15 deg");
    }

    for (const auto& tr : tracks) {
        int bi = -1;
        for (size_t i = 0; i < rig.bones.size(); ++i) {
            if (rig.bones[i].id != 0 && rig.bones[i].id == tr.boneId) { bi = (int)i; break; }
        }
        if (bi < 0) {
            (void)0;
        }
        if (bi < 0 && tr.boneId >= 1 && tr.boneId <= rig.bones.size())
            bi = (int)tr.boneId - 1;
        if (bi < 0 || bi >= (int)rig.bones.size()) {
            issues.push_back("unknown bone id " + std::to_string(tr.boneId));
            continue;
        }
        if (tr.keys.empty()) issues.push_back("track has no keys");
        for (const auto& kk : tr.keys) {
            if (kk.t < 0.f || kk.t > length + 1e-4f)
                issues.push_back("key outside [0,length]");
        }
        for (size_t i = 1; i < tr.keys.size(); ++i) {
            if (tr.keys[i].t < tr.keys[i - 1].t)
                issues.push_back("keys not sorted by time");
        }
    }
    return issues;
}

} // namespace vf::voxel