#include "app/chat_ui.hpp"
#include "ai/system_prompt.hpp"
#include "ai/tools.hpp"
#include "voxel/common.hpp"
#include "voxel/worldfile.hpp"
#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <sstream>
#include <cctype>

namespace vf::ai {


ChatUi::ChatUi() {}
ChatUi::~ChatUi(){ shutdown(); }
void ChatUi::init(const std::string& baseUrl, const std::string& model){
    m_client.setBaseUrl(baseUrl);
    m_client.setModel(model);
    m_status = "model " + model + " @ " + baseUrl;
    std::string err;
    if(!m_client.ping(&err)) m_status = "LLM offline: "+err;
    pushAssistant("Hi! Ctrl+LMB a voxel, then tell me what to build. Example: 'put a 3x3 wood crate here' or 'add a small rock'");
}
void ChatUi::shutdown(){
    if(m_worker.joinable()){
        if(m_sending) m_worker.join();
        else m_worker.join();
    }
}
std::string ChatUi::buildSystemPrompt(bool hasSelection) const {
    std::string p = kGemmaSystemPrompt;
    if(hasSelection) p += "\nCURRENT SELECTION: available (use anchor). User's last pick is the bottom-center for the next object.\n";
    else p += "\nCURRENT SELECTION: none. If user says 'here/this' you must ask them to Ctrl+LMB pick first. Do not guess coordinates.\n";
    return p;
}
void ChatUi::pushUser(const std::string& t){
    m_history.push_back({"user", t, false});
    m_llmHistory.push_back({"user", t});
    if(m_llmHistory.size()>20){ m_llmHistory.erase(m_llmHistory.begin()); }
}
void ChatUi::pushAssistant(const std::string& t, bool thinking){
    m_history.push_back({"assistant", t, thinking});
}

void ChatUi::launchRequest(const std::string& userText, glm::ivec3 anchor, bool hasAnchor){
    if(m_sending) return;
    m_sending = true;
    m_pendingAnchor = anchor; m_pendingHasAnchor = hasAnchor;
    m_lastUserText = userText;
    m_unknownRetries = 0;
    pushUser(userText);
    spawnWorker();
}

// send the current LLM history (incl. any correction messages) to the model
void ChatUi::spawnWorker(){
    std::vector<ChatMessage> hist = m_llmHistory;
    std::string sys = buildSystemPrompt(m_pendingHasAnchor);
    if(m_pendingHasAnchor){
        glm::vec3 wpos = vf::voxel::voxelCenter(m_pendingAnchor);
        char buf[256];
        snprintf(buf,sizeof(buf)," [selected voxel %d %d %d world %.2f %.2f %.2f - this is bottom-center for new object]",
                 m_pendingAnchor.x, m_pendingAnchor.y, m_pendingAnchor.z, wpos.x, wpos.y, wpos.z);
        if(!hist.empty()) hist.back().content += buf;
    }
    if(m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this, hist, sys](){
        ChatResult r = m_client.chat(hist, sys, true);
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_pending.push(std::move(r));
        }
    });
}

int ChatUi::executeToolCalls(const std::vector<ToolCall>& calls, glm::ivec3 anchor, bool hasAnchor,
                             vf::voxel::EditableWorld& editable, vf::voxel::LayeredWorld& world,
                             std::function<void()> rebuildFn){
    int rejected = 0;
    for(auto &tc: calls){
        spdlog::info("chat tool {} {}", tc.name, tc.argumentsJson);

        // fuzzy-map invented names ("rock_1", "add_box") onto canonical tools
        NormalizedCall norm = normalizeToolCall(tc.name, tc.argumentsJson);
        if(!norm.ok){
            pushAssistant(norm.error);
            m_llmHistory.push_back({"tool", norm.error + " Re-issue the corrected tool call now."});
            ++rejected;
            continue;
        }
        const std::string& name = norm.name;
        const std::string& args = norm.argsJson;

        if(name=="create_box"){
            std::vector<int> sz; int mat=6;
            if(!jsonGetIntArray(args,"size",sz,3)){
                pushAssistant("create_box needs size:[sx,sy,sz] in voxels.");
                ++rejected;
                continue;
            }
            jsonGetInt(args,"material",mat) || jsonGetInt(args,"mat",mat);
            mat = std::clamp(mat,0,8);
            glm::ivec3 useAnchor = anchor;
            bool haveAnchor = hasAnchor;
            std::vector<int> anc;
            if(jsonGetIntArray(args,"anchor",anc,3)){ useAnchor={anc[0],anc[1],anc[2]}; haveAnchor=true; }
            if(!haveAnchor){ pushAssistant("No voxel selected. Ctrl+LMB a voxel first, then retry."); continue; }
            glm::ivec3 s(sz[0],sz[1],sz[2]);
            s = glm::clamp(s, glm::ivec3(1), glm::ivec3(32));
            auto recs = editable.makeBox(useAnchor, s, uint8_t(mat));
            size_t added = editable.append(recs);
            if(added==0) pushAssistant("Box overlapped existing voxels; nothing added.");
            else if(rebuildFn) rebuildFn();
        } else if(name=="create_cylinder"){
            float r=0.5f,h=1.f; int mat=6;
            jsonGetFloat(args,"radius",r);
            jsonGetFloat(args,"height",h);
            jsonGetInt(args,"material",mat) || jsonGetInt(args,"mat",mat);
            mat=std::clamp(mat,0,8);
            if(!hasAnchor){ pushAssistant("Pick a voxel first (Ctrl+LMB)."); continue; }
            r=std::clamp(r,0.1f,5.f); h=std::clamp(h,0.1f,10.f);
            auto recs = editable.makeCylinderY(anchor, r, h, uint8_t(mat));
            size_t added=editable.append(recs);
            if(added && rebuildFn) rebuildFn();
        } else if(name=="create_ellipsoid"){
            std::vector<float> rad; int mat=4;
            jsonGetFloatArray(args,"radius",rad,3);
            if(rad.size()!=3) rad={0.6f,0.45f,0.6f};
            jsonGetInt(args,"material",mat) || jsonGetInt(args,"mat",mat);
            if(!hasAnchor){ pushAssistant("Pick a voxel first."); continue; }
            glm::vec3 rr(rad[0],rad[1],rad[2]);
            rr = glm::clamp(rr, glm::vec3(0.1f), glm::vec3(5.f));
            auto recs = editable.makeEllipsoid(anchor, rr, uint8_t(std::clamp(mat,0,8)));
            size_t added=editable.append(recs);
            if(added==0) pushAssistant("Nothing added (overlaps existing edits?).");
            else if(rebuildFn) rebuildFn();
        } else if(name=="create_stamp"){
            std::vector<StampCellLite> cells;
            if(!parseStampCells(args, cells)){ pushAssistant("create_stamp needs cells:[{dx,dy,dz,mat},...]."); ++rejected; continue; }
            if(cells.size()>1000) cells.resize(1000);
            if(!hasAnchor){ pushAssistant("Pick a voxel first."); continue; }
            std::vector<vf::voxel::StampCell> scs;
            for(auto &c : cells) scs.push_back({ int8_t(c.dx), int8_t(c.dy), int8_t(c.dz), uint8_t(std::clamp(c.mat,0,8)) });
            size_t added = editable.append(editable.makeStamp(anchor, scs));
            if(added && rebuildFn) rebuildFn();
        } else if(name=="list_world"){
            std::vector<vf::voxel::worldfile::WorldLayer> layers;
            std::string out = "layers:";
            if(vf::voxel::worldfile::loadManifest(std::string(VOXELFORGE_ASSET_DIR)+"/world.json", layers))
                for(auto &l : layers)
                    if(l.role!="packed") out += " "+l.name+"["+std::string(l.enabled?"on":"off")+"]";
            out += " | ai_edits voxels this session: "+std::to_string(editable.size());
            pushAssistant(out);
        } else if(name=="probe"){
            float x=0,y=0,z=0;
            jsonGetFloat(args,"x",x); jsonGetFloat(args,"y",y); jsonGetFloat(args,"z",z);
            auto s = world.field().sampleWorld({x,y,z});
            char buf[128];
            snprintf(buf,sizeof(buf),"probe(%.1f,%.1f,%.1f): d=%+.2f mat=%d%s",
                     x,y,z,s.d,int(s.mat), s.d<0?" solid":" empty");
            pushAssistant(buf);
        } else {
            // normalizeToolCall should have caught everything, belt-and-braces
            std::string err = std::string("unknown tool '")+name+"'. Valid: "+kValidTools;
            pushAssistant(err);
            m_llmHistory.push_back({"tool", err + " Re-issue the corrected tool call now."});
            ++rejected;
        }
    }
    return rejected;
}

void ChatUi::draw(vf::voxel::EditableWorld& editable, vf::voxel::LayeredWorld& world,
                  const vf::voxel::PickHit* hover, const vf::voxel::PickHit* selection, bool hasSelection,
                  std::function<void()> rebuildFn){
    // poll async results
    {
        std::queue<ChatResult> toProcess;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            std::swap(toProcess, m_pending);
        }
        while(!toProcess.empty()){
            ChatResult r = std::move(toProcess.front()); toProcess.pop();
            m_sending = false;
            if(m_worker.joinable()) m_worker.join();
            if(!r.ok){
                pushAssistant("LLM error: "+r.error);
                m_status = "error";
                spdlog::error("chat llm error: {}", r.error);
                continue;
            }
            if(!r.thinking.empty() && m_showThinking) pushAssistant(r.thinking, true);
            if(!r.content.empty()) pushAssistant(r.content);
            if(!r.toolCalls.empty()){
                int rejected = executeToolCalls(r.toolCalls, m_pendingAnchor, m_pendingHasAnchor, editable, world, rebuildFn);
                char buf[128]; snprintf(buf,sizeof(buf),"executed %zu tool call(s)", r.toolCalls.size());
                pushAssistant(buf);
                m_llmHistory.push_back({"assistant", r.content + " [tools executed]"});
                m_status = "placed";
                // self-correction: let the model re-issue rejected calls once
                if(rejected>0 && m_unknownRetries<2){
                    ++m_unknownRetries;
                    m_llmHistory.push_back({"user",
                        std::string("SYSTEM: ") + std::to_string(rejected) +
                        " tool call(s) were rejected. Use exactly one of: " + kValidTools +
                        ". Re-send only the corrected tool call."});
                    pushAssistant("Retrying with corrected tool call...", true);
                    spawnWorker();
                } else if (rejected==0) {
                    m_unknownRetries = 0;
                }
            } else {
                if(!r.content.empty()) m_llmHistory.push_back({"assistant", r.content});
                m_status = "ready";
            }
        }
    }

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 380, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(368, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("AI Chat", nullptr, 0);

    // self-heal layouts saved by wider windows: never let the panel drift
    // beyond reach of the current display
    {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const float maxX = ImGui::GetIO().DisplaySize.x - 80.f;
        const float maxY = ImGui::GetIO().DisplaySize.y - 48.f;
        if (wp.x > maxX || wp.y > maxY || wp.x + ws.x < 80.f) {
            ImGui::SetWindowPos(ImVec2(
                std::max(12.f, std::min(wp.x, maxX)), std::max(12.f, std::min(wp.y, maxY))));
        }
    }

    ImGui::TextDisabled("%s", m_status.c_str());
    if(m_sending) { ImGui::SameLine(); ImGui::Text("⋯"); }

    if(hasSelection){
        glm::vec3 w = vf::voxel::voxelCenter(selection->voxel);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f,0.22f,0.12f,1));
        ImGui::BeginChild("selBar", ImVec2(0, 38), true);
        ImGui::Text("Selected %d %d %d  (%.1f, %.1f, %.1f) mat %d",
                    selection->voxel.x, selection->voxel.y, selection->voxel.z,
                    w.x, w.y, w.z, int(selection->mat));
        ImGui::TextDisabled("Center-bottom for new object • Ctrl+LMB to move");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f,0.6f,0.6f,1));
        ImGui::TextWrapped("No voxel selected — Ctrl+LMB on terrain/house to pick.");
        ImGui::PopStyleColor();
    }
    if(hover && hover->hit){
        ImGui::TextDisabled("Hover %d %d %d", hover->voxel.x, hover->voxel.y, hover->voxel.z);
    }

    ImGui::Separator();
    ImGui::BeginChild("history", ImVec2(0, -70), true);
    for(auto &e: m_history){
        if(e.role=="user"){
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f,0.85f,1.f,1));
            ImGui::TextWrapped("You: %s", e.content.c_str());
            ImGui::PopStyleColor();
        } else {
            if(e.isThinking){
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f,0.7f,0.7f,1));
                ImGui::TextWrapped("[thinking] %s", e.content.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextWrapped("Gemma: %s", e.content.c_str());
            }
        }
        ImGui::Spacing();
    }
    if(ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::PushItemWidth(-70);
    bool enter = ImGui::InputTextMultiline("##chatIn", m_inputBuf, sizeof(m_inputBuf),
                                           ImVec2(0, 48), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    m_inputFocused = ImGui::IsItemActive();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    bool doSend=false;
    ImGui::BeginDisabled(m_sending || m_inputBuf[0]==0);
    if(ImGui::Button("Send", ImVec2(60,48))) doSend=true;
    ImGui::EndDisabled();
    if(enter && m_inputBuf[0]!=0) doSend=true;
    if(doSend){
        std::string txt = m_inputBuf;
        if(!txt.empty()){
            // trim
            size_t a=txt.find_first_not_of(" \n\r\t");
            size_t b=txt.find_last_not_of(" \n\r\t");
            if(a!=std::string::npos && b!=std::string::npos) txt=txt.substr(a,b-a+1);
            else txt="";
        }
        if(!txt.empty()){
            glm::ivec3 anc2 = hasSelection? selection->voxel : glm::ivec3(0);
            launchRequest(txt, anc2, hasSelection);
            m_inputBuf[0]=0;
        }
    }
    if(ImGui::Button("Clear chat")) { m_history.clear(); m_llmHistory.clear(); }
    ImGui::SameLine();
    if(ImGui::Button("Clear AI edits")) { editable.clear(); if(rebuildFn) rebuildFn(); pushAssistant("Cleared all AI edits."); }
    ImGui::SameLine();
    ImGui::Checkbox("thinking", &m_showThinking);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Show model thinking traces");
    ImGui::End();
}

} // namespace vf::ai
