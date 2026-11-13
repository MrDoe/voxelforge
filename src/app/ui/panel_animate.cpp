// The sidebar's animation section: clip playback, rig inspection, transport.
//
// The BEHAVIOUR owns the clip clock. This panel only:
//   * chooses which clip the object plays (the studio's override of the
//     game's policy),
//   * writes play/pause/speed/scrub onto the behaviour,
//   * reads the resulting time back for the slider and the timeline.
// It must never advance a clock of its own: the duplicate `time += dt`
// here ran on top of the behaviour's advance, so the studio played at double
// speed while a distance-clocked clip played at the authored speed.
#include "app/app.hpp"

#include "voxel/micro_clip.hpp"

#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::drawPanelAnimate()
{
    paneBand("Animation studio", "Ctrl+7");

    if (!m_studio.active) {
        ImGui::TextDisabled("Launch with --anim to open the studio.");
        return;
    }

    // The studio targets ONE object: the layer named by --anim-layer. It used
    // to bind to index 0, which in the shipped manifest is the VASE - so
    // "--anim-layer micro/alpaca.vxw" showed and drove the vases. The target
    // is resolved in loadMicroObjects(); a typo'd name leaves it unset.
    auto* target = studioBehaviour();
    auto* targetClips = studioClips();
    if (!target || !targetClips) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1),
                           "No micro object loaded for '%s'.",
                           m_studio.layerFile.c_str());
        ImGui::TextDisabled("Check the layer name (micro/<name>.vxw).");
        return;
    }

    ImGui::Text("Object: %s", m_studio.layerFile.c_str());

    // ---- clip selection -------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Clip set:");
    if (!targetClips->empty()) {
        auto& clips = *targetClips;
        for (size_t ci = 0; ci < clips.size(); ++ci) {
            const bool playing = (target->clip == clips[ci].get());
            bool sel = (m_studio.selectedClip == (int)ci);
            if (ImGui::Selectable(clips[ci]->name.c_str(), sel)) {
                m_studio.selectedClip = (int)ci;
                m_studio.time = 0.f;
                m_studio.playing = true;
                // startClip resets the phase and picks the stance from the
                // clip's clock (distance = travels, time = in place).
                target->startClip(clips[ci].get());
            }
            if (playing) {
                ImGui::SameLine();
                statusPill("active", kAccent);
            }
            if (ImGui::IsItemHovered() && !clips[ci]->description.empty())
                ImGui::SetTooltip("%s", clips[ci]->description.c_str());
        }
    } else {
        ImGui::TextDisabled("No clips loaded. Add .anim files to the manifest.");
    }

    // ---- transport ------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Transport:");
    auto& b = *target;
    const bool hasClip = (b.clip != nullptr && b.clipActive);
    if (!hasClip) ImGui::BeginDisabled();

    if (ImGui::Button(m_studio.playing ? "Pause" : "Play")) {
        m_studio.playing = !m_studio.playing;
        b.clipPlaying = m_studio.playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        m_studio.playing = false;
        m_studio.time = 0.f;
        b.clipTime = 0.f;
        b.clipPlaying = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("<<")) {
        m_studio.playing = false;
        m_studio.time = std::max(0.f, m_studio.time - 1.f / 30.f);
        b.clipTime = m_studio.time;
        b.clipPlaying = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(">>")) {
        m_studio.playing = false;
        if (b.clip)
            m_studio.time = std::min(b.clip->length, m_studio.time + 1.f / 30.f);
        b.clipTime = m_studio.time;
        b.clipPlaying = false;
    }

    if (hasClip) {
        const auto& clip = *b.clip;
        // Read the clock back from the behaviour (it is the one advancing).
        if (!m_studio.pinned)
            m_studio.time = b.clipTime;
        ImGui::SliderFloat("Time", &m_studio.time, 0.f, clip.length, "%.3f s");
        if (ImGui::IsItemEdited()) {
            m_studio.playing = false;
            b.clipTime = m_studio.time;
            b.clipPlaying = false;
        }
        ImGui::SliderFloat("Speed", &m_studio.speed, 0.1f, 3.f, "%.2f x");
        if (ImGui::IsItemEdited())
            b.clipSpeed = m_studio.speed;

        ImGui::Text("Clock: %s  Stride: %.2f m  Blend: %.2f s",
                    clip.clock == vf::voxel::ClipClock::Distance ? "distance" : "time",
                    clip.stride, clip.blendIn);
        if (m_studio.pinned)
            ImGui::TextDisabled("Pinned at %.3f s (--anim-time)", m_studio.time);
        if (!clip.description.empty())
            ImGui::TextWrapped("%s", clip.description.c_str());
    } else {
        ImGui::TextDisabled("No clip selected; the rig is driven by the gait.");
    }
    if (!hasClip) ImGui::EndDisabled();

    // ---- authoring ------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Authoring:");
    vf::voxel::MicroClip* editClip = const_cast<vf::voxel::MicroClip*>(target->clip);
    static char newName[128] = "new_clip";
    ImGui::InputText("Name", newName, sizeof(newName));
    if (ImGui::Button("New clip")) {
        auto clip = std::make_unique<vf::voxel::MicroClip>();
        clip->name = newName;
        clip->description = "authored in studio";
        clip->length = 1.f;
        clip->fps = 30.f;
        clip->loop = true;
        clip->clock = vf::voxel::ClipClock::Time;
        clip->speed = 0.20f;
        clip->stride = 0.90f;
        clip->blendIn = 0.20f;
        targetClips->push_back(std::move(clip));
        m_studio.selectedClip = (int)targetClips->size() - 1;
        m_studio.time = 0.f;
        m_studio.playing = true;
        target->startClip(targetClips->back().get());
    }
    ImGui::SameLine();
    if (ImGui::Button("Add body key")) {
        if (editClip) {
            vf::voxel::BodyKey bk;
            bk.t = m_studio.time;
            bk.offset = target->bodyOffset;
            bk.headingDeg = 0.f;
            bk.ease = vf::voxel::ClipEase::Smooth;
            editClip->body.push_back(bk);
            auto& body = editClip->body;
            std::sort(body.begin(), body.end(),
                      [](const vf::voxel::BodyKey& a, const vf::voxel::BodyKey& b) {
                          return a.t < b.t; });
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add bone key")) {
        if (editClip && m_studio.selectedBone >= 0 &&
            (size_t)m_studio.selectedBone < target->rig.bones.size()) {
            vf::voxel::BoneTrack* track = nullptr;
            uint32_t bid = target->rig.bones[m_studio.selectedBone].id;
            for (auto& tr : editClip->tracks)
                if (tr.boneId == bid) { track = &tr; break; }
            if (!track) {
                editClip->tracks.push_back({});
                track = &editClip->tracks.back();
                track->boneId = bid;
                track->name = target->rig.bones[m_studio.selectedBone].name;
            }
            vf::voxel::BoneKey kk;
            kk.t = m_studio.time;
            kk.rotDeg = glm::vec3(0.f);
            kk.ease = vf::voxel::ClipEase::Smooth;
            track->keys.push_back(kk);
            std::sort(track->keys.begin(), track->keys.end(),
                      [](const vf::voxel::BoneKey& a, const vf::voxel::BoneKey& b) {
                          return a.t < b.t; });
        }
    }
    if (ImGui::Button("Save clip")) {
        if (editClip) {
            const char* path = m_studio.clipFile.empty()
                ? "assets/micro/studio_clip.anim" : m_studio.clipFile.c_str();
            if (vf::voxel::writeMicroClip(path, *editClip))
                spdlog::info("studio: saved clip to {}", path);
            else
                spdlog::warn("studio: failed to save clip to {}", path);
        }
    }

    // ---- skeleton -------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Skeleton:");
    {
        auto& rig = b.rig;
        for (size_t i = 0; i < rig.bones.size(); ++i) {
            bool sel = (m_studio.selectedBone == (int)i);
            char label[128];
            std::snprintf(label, sizeof(label), "%s (id %u, %s)",
                          rig.bones[i].name.c_str(), rig.bones[i].id,
                          rig.bones[i].role == vf::voxel::BoneRole::Spine ? "spine" :
                          rig.bones[i].role == vf::voxel::BoneRole::Leg ? "leg" :
                          rig.bones[i].role == vf::voxel::BoneRole::Head ? "head" :
                          rig.bones[i].role == vf::voxel::BoneRole::Tail ? "tail" : "neck");
            if (ImGui::Selectable(label, sel))
                m_studio.selectedBone = (int)i;
        }

        // Validation.
        auto issues = vf::voxel::validateRig(rig);
        if (!issues.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Rig issues: %d", (int)issues.size());
            for (const auto& iss : issues)
                ImGui::BulletText("%s", iss.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Rig OK (%d bones)", (int)rig.bones.size());
        }

        // ---- the key at the playhead -------------------------------------
        // The gizmo writes here; these fields show WHAT it wrote and allow a
        // precise nudge. Editing an axis AUTO-KEYS: it creates or updates the
        // key at the playhead, the same rule a ring drag follows, so there is
        // one answer to "how does a pose become data".
        //
        // Read-only until a clip exists - the panel must NOT create one just
        // because it was opened, or looking at the studio would switch the
        // animal off its gait.
        if (m_studio.selectedBone >= 0 &&
            (size_t)m_studio.selectedBone < rig.bones.size()) {
            const uint32_t selId = rig.bones[size_t(m_studio.selectedBone)].id;
            auto* clip = target->clip
                             ? const_cast<vf::voxel::MicroClip*>(target->clip)
                             : nullptr;
            vf::voxel::BoneKey* key = nullptr;
            if (clip) {
                const float step = (clip->fps > 1e-3f) ? 1.f / clip->fps : 1.f / 30.f;
                const float tq = std::round(m_studio.time / step) * step;
                for (auto& tr : clip->tracks) {
                    if (tr.boneId != selId)
                        continue;
                    for (auto& k : tr.keys)
                        if (std::abs(k.t - tq) < 0.5f * step) { key = &k; break; }
                    break;
                }
            }
            ImGui::Separator();
            if (!clip) {
                ImGui::TextDisabled("No clip: drag a ring to author the first key.");
            } else if (!key) {
                ImGui::TextDisabled("No key at t=%.3f: drag a ring to add one.",
                                    m_studio.time);
            } else {
                ImGui::Text("Key at t=%.3f", key->t);
                glm::vec3 r = key->rotDeg;
                bool edited = false;
                edited |= ImGui::DragFloat("Rot X (deg)", &r.x, 0.4f, -360.f, 360.f,
                                           "%.1f");
                edited |= ImGui::DragFloat("Rot Y (deg)", &r.y, 0.4f, -360.f, 360.f,
                                           "%.1f");
                edited |= ImGui::DragFloat("Rot Z (deg)", &r.z, 0.4f, -360.f, 360.f,
                                           "%.1f");
                if (edited) {
                    key->rotDeg = r;
                    // Same path the gizmo uses: re-sample now so the viewport
                    // and the stored data cannot disagree for a frame.
                    if (auto* bb = studioBehaviour()) {
                        bb->clipTime = m_studio.time;
                        bb->animateBones();
                    }
                    m_taaFirstFrame = true;
                }
            }
        }

        // ---- cylindrical influence radius (adjustable) -----------------
        ImGui::Separator();
        ImGui::Text("Influence radius (cylindrical):");
        if (m_studio.selectedBone >= 0 &&
            (size_t)m_studio.selectedBone < rig.bones.size()) {
            auto& selBone = rig.bones[m_studio.selectedBone];
            ImGui::Text("Bone: %s  (id %u)", selBone.name.c_str(), selBone.id);
            float r = selBone.radius;
            if (ImGui::SliderFloat("radius (m)", &r, 0.0f, 0.15f, "%.4f")) {
                selBone.radius = r;
                spdlog::info("studio: bone %s radius -> %.4f", selBone.name, r);
            }
            ImGui::TextDisabled("Radius is the cylindrical influence radius used "
                                "for bone ownership (capsule test).");

            // ---- persist the (possibly edited) rig -------------------------
            ImGui::Separator();
            if (ImGui::Button("Save rig (override)")) {
                // Rig override path: assets/micro/alpaca.rig.json
                const std::string stem = m_studio.layerFile;
                const size_t slash = stem.find_last_of('/');
                const std::string name = (slash == std::string::npos)
                    ? stem : stem.substr(slash + 1);
                const size_t dot = name.find_last_of('.');
                const std::string base = (dot == std::string::npos)
                    ? name : name.substr(0, dot);
                const std::string dir = std::filesystem::path(m_manifestPath).parent_path().string();
                const size_t lastSlash = m_studio.layerFile.find_last_of('/');
                const std::string layerDir = (lastSlash == std::string::npos)
                    ? dir : (dir + "/" + m_studio.layerFile.substr(0, lastSlash));
                const std::string overridePath = layerDir + "/" + base + ".rig.json";
                const std::string layerPath = layerDir + "/" + base + ".vxw";
                std::error_code ec;
                const uint64_t lsize = std::filesystem::file_size(layerPath, ec);
                uint64_t lmtime = 0;
                {
                    auto t = std::filesystem::last_write_time(layerPath, ec);
                    if (!ec) lmtime = uint64_t(t.time_since_epoch().count());
                }
                if (vf::voxel::writeMicroRigOverride(overridePath, b.rig, lsize, lmtime))
                    spdlog::info("studio: saved rig override to {}", overridePath);
                else
                    spdlog::warn("studio: failed to save rig override to {}", overridePath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload rig")) {
                loadMicroObjects();
                spdlog::info("studio: reloaded micro objects");
            }
        }
    }
}

} // namespace app
} // namespace vf
