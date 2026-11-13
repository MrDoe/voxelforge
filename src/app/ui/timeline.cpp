// Bottom-dock timeline for the animation studio.
//
// The dock is anchored to the SIDEBAR's current width, not a hardcoded 334:
// the sidebar is user-resizable, and a fixed origin left the timeline either
// under the rail or with a gap the width of a drag.
#include "app/app.hpp"

#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

namespace vf {
namespace app {

void App::drawTimeline()
{
    if (!m_studio.active) return;

    const float kTimelineH = 150.f;
    const float winW = (float)ImGui::GetIO().DisplaySize.x;
    const float winH = (float)ImGui::GetIO().DisplaySize.y;
    const float sidebarW = sidebarWidthFor(m_sidebarWidth, winW);

    ImGui::SetNextWindowPos(ImVec2(sidebarW, winH - kTimelineH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW - sidebarW, kTimelineH), ImGuiCond_Always);
    ImGui::Begin("Voxelforge##Timeline", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    // A studio with no object (a typo'd --anim-layer) or no clip still has a
    // timeline; say why it is empty instead of drawing nothing.
    // The target is the layer named by --anim-layer (resolved in
    // loadMicroObjects): index 0 would be the vase.
    auto* target = studioBehaviour();
    if (!target || !target->clip) {
        ImGui::TextDisabled(!target ? "No micro object loaded."
                                    : "No clip selected.");
        ImGui::End();
        return;
    }

    auto& behaviour = *target;
    auto& clip = const_cast<vf::voxel::MicroClip&>(*behaviour.clip);
    // The behaviour owns the clock; the dock displays and scrubs it.
    if (!m_studio.pinned)
        m_studio.time = behaviour.clipTime;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float rowH = 18.f;
    const float rulerH = 20.f;
    const float labelW = 90.f;
    const float pxPerSec = 120.f;
    const float width = winW - sidebarW;

    // Ruler.
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + rulerH),
                      IM_COL32(30, 35, 45, 255));
    for (float t = 0; t <= clip.length + 0.001f; t += 0.5f) {
        float x = origin.x + labelW + t * pxPerSec;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + rulerH), IM_COL32(80, 90, 110, 255));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", t);
        dl->AddText(ImVec2(x + 2, origin.y + 2), IM_COL32(180, 190, 210, 255), buf);
    }

    // Rows: root + bones.
    float y = origin.y + rulerH;
    // Root row.
    dl->AddText(ImVec2(origin.x + 4, y + 2), IM_COL32(200, 200, 200, 255), "root");
    // Body keys.
    for (const auto& bk : clip.body) {
        float x = origin.x + labelW + bk.t * pxPerSec;
        dl->AddCircleFilled(ImVec2(x, y + rowH * 0.5f), 4.f, IM_COL32(100, 200, 255, 255));
    }
    y += rowH;

    const auto& rig = behaviour.rig;
    for (size_t bi = 0; bi < rig.bones.size(); ++bi) {
        dl->AddText(ImVec2(origin.x + 4, y + 2), IM_COL32(180, 180, 180, 255),
                    rig.bones[bi].name.c_str());
        // Find track for this bone.
        for (const auto& tr : clip.tracks) {
            if (tr.boneId != 0 && tr.boneId == rig.bones[bi].id) {
                for (const auto& kk : tr.keys) {
                    float x = origin.x + labelW + kk.t * pxPerSec;
                    ImU32 col = (kk.ease == vf::voxel::ClipEase::Step) ? IM_COL32(255, 100, 100, 255) :
                                (kk.ease == vf::voxel::ClipEase::Linear) ? IM_COL32(100, 255, 100, 255) :
                                IM_COL32(100, 100, 255, 255);
                    dl->AddCircleFilled(ImVec2(x, y + rowH * 0.5f), 4.f, col);
                }
            }
        }
        y += rowH;
    }

    // Playhead.
    const float phX = origin.x + labelW + m_studio.time * pxPerSec;
    dl->AddLine(ImVec2(phX, origin.y), ImVec2(phX, origin.y + kTimelineH),
                IM_COL32(255, 200, 50, 255), 2.f);

    // Click to scrub. Scrubbing PAUSES: a pinned time and a live clock that
    // both write clipTime fight over the same field and the playhead jitters.
    // Shift+click ADDS a body key at the playhead instead (authoring).
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
        ImVec2 mp = ImGui::GetMousePos();
        float t = (mp.x - origin.x - labelW) / pxPerSec;
        t = std::clamp(t, 0.f, clip.length);
        if (ImGui::GetIO().KeyShift) {
            vf::voxel::BodyKey bk;
            bk.t = t;
            bk.offset = behaviour.bodyOffset;
            bk.headingDeg = 0.f;
            bk.ease = vf::voxel::ClipEase::Smooth;
            clip.body.push_back(bk);
            std::sort(clip.body.begin(), clip.body.end(),
                      [](const vf::voxel::BodyKey& a, const vf::voxel::BodyKey& b) {
                          return a.t < b.t; });
        } else {
            m_studio.time = t;
            m_studio.playing = false;
            behaviour.clipTime = t;
            behaviour.clipPlaying = false;
        }
    }

    ImGui::End();
}

} // namespace app
} // namespace vf
