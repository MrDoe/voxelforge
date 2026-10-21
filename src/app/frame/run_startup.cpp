// Startup: the --probe early exit, the headless VF_TEST_* hooks that must run
// after the world is loaded but before the first frame, the per-backend camera
// spawn, and the one-shot environment overrides.
#include "app/app.hpp"

#include "app/frame/frame.hpp"
#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"
#include "app/world/store_overlay.hpp"
#include "voxel/collision.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

// A --probe run answers from the load-time field and exits before the window
// or the device ever exists, which is why it is a method of App (it needs the
// asset dir) but not a mode of the frame loop. Returns true if the world
// could not be loaded, which the caller turns into exit status 1.
bool App::runProbe()
{
    const Args& args = m_args;
    // probes read the live layered world (ai_edits included as a layer)
    vf::voxel::LayeredWorld probeWorld;
    const std::string manifest = std::string(VOXELFORGE_ASSET_DIR) + "/world.json";
    if (!probeWorld.load(manifest)) {
        spdlog::critical("probe: cannot load world.json");
    return true;
    }
    // VF_PROBE_RELOAD=N: re-run load() N times on the warm instance to
    // measure the interactive toggle / edit reload cost
    if (const char* r = getenv("VF_PROBE_RELOAD")) {
        int n = atoi(r);
        for (int i = 0; i < n; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            probeWorld.load(manifest);
            spdlog::info("probe: warm reload {:d}: {:.0f} ms", i,
                         std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0)
                             .count());
        }
    }
    auto s = probeWorld.field().sampleWorld(args.probe);
    spdlog::info("probe({:.2f},{:.2f},{:.2f}): d={:+.3f} mat={} tex={} {}", args.probe.x,
                 args.probe.y, args.probe.z, s.d, int(s.mat), int(s.tex),
                 s.d < 0 ? "solid" : "empty");
    return false;
}

// The headless exercise hooks. Each mutates the world AFTER the load but
// BEFORE the first frame, so a --shot capture already contains the result.
// They return 1 to abort the run: a hook that cannot do its job must not
// fall through into a screenshot that quietly says nothing about it.
int App::runStartupTestHooks()
{
    // --probe-surfel X Y Z (LATTICE cell): name the splats that make up that
    // cell. Same report the Ctrl+LMB pick prints, so a splat can be identified
    // from a script as well as by clicking it. Runs here (not in the --probe
    // path) because it needs THIS app's m_layers store, not a throwaway world.
    if (m_args.probeSurfelSet) {
        // "list" (or a negative sentinel) enumerates instead of naming one cell.
        if (m_args.probeSurfel.x == -1) {
            spdlog::info("probe-surfel list");
            listFloatingSurfels();
            return 1;
        }
        spdlog::info("probe-surfel cell {} {} {}", m_args.probeSurfel.x,
                     m_args.probeSurfel.y, m_args.probeSurfel.z);
        describeSurfelsAt(m_args.probeSurfel);
        return 1;   // the report is the whole point; do not fall through
    }
    // Headless exercise of the exact GUI import path.  The comma-separated
    // form is: file,name,x,y,z,fit,mat,rotY,solid (the last three are
    // optional).  It is useful for CI and for re-authoring a known model
    // without having to drive a native ImGui window; the interactive button
    // calls the same importMeshFromGui() method.
    if (const char* tm = getenv("VF_TEST_MESH_IMPORT"); tm && *tm) {
        std::vector<std::string> fields;
        std::stringstream spec(tm);
        std::string field;
        while (std::getline(spec, field, ','))
            fields.push_back(field);
        int x = 512, y = 512, z = 512, mat = 6, solid = 1;
        float fit = 5.0f, rotY = 0.0f;
        const bool valid = fields.size() >= 6 &&
                           sscanf(fields[2].c_str(), "%d", &x) == 1 &&
                           sscanf(fields[3].c_str(), "%d", &y) == 1 &&
                           sscanf(fields[4].c_str(), "%d", &z) == 1 &&
                           sscanf(fields[5].c_str(), "%f", &fit) == 1;
        if (fields.size() >= 7)
            sscanf(fields[6].c_str(), "%d", &mat);
        if (fields.size() >= 8)
            sscanf(fields[7].c_str(), "%f", &rotY);
        if (fields.size() >= 9)
            sscanf(fields[8].c_str(), "%d", &solid);
        if (!valid || fields[0].empty() || fields[1].empty()) {
            spdlog::warn("bad VF_TEST_MESH_IMPORT; expected "
                         "file,name,x,y,z,fit[,mat,rotY,solid]");
        } else {
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", fields[0].c_str());
            std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s",
                          fields[1].c_str());
            m_meshAnchor = { x, y, z };
            m_meshUseFit = true;
            m_meshFitMeters = fit;
            m_meshMaterial = std::clamp(mat, 0, int(vf::voxel::kPaletteN) - 1);
            m_meshRotY = rotY;
            m_meshSolid = solid != 0;
            m_meshImportPrepared = true;
            rescanMeshFiles();
            if (importMeshFromGui()) {
                m_panel = Panel::Mesh;
                // The hook is deterministic: force its reload synchronously so
                // the first captured frame already contains the new layer.
                const char* oldSync = getenv("VF_SYNC_RELOAD");
                const bool hadSync = oldSync != nullptr;
                const std::string oldSyncValue = hadSync ? oldSync : "";
                setenv("VF_SYNC_RELOAD", "1", 1);
                m_layers.requestReload(m_camera.pos, true);
                if (hadSync)
                    setenv("VF_SYNC_RELOAD", oldSyncValue.c_str(), 1);
                else
                    unsetenv("VF_SYNC_RELOAD");
                m_pendingWorldReload = false;
                applyWorldReload();
            } else {
                spdlog::error("VF_TEST_MESH_IMPORT failed");
                return 1;
            }
        }
    }

    // VF_TEST_ROTATE="yaw,pitch,roll" on the layer named by VF_ROTATE_LAYER:
    // apply the trackball's commit path (manifest write) without a window, so
    // headless shots can verify the placement pipeline end-to-end.
    if (const char* tr = getenv("VF_TEST_ROTATE"); tr && *tr) {
        float y = 0.f, p = 0.f, r = 0.f;
        if (sscanf(tr, "%f,%f,%f", &y, &p, &r) == 3) {
            m_rotateDy = y; m_rotateDx = p; m_rotateDz = r;
            m_rotateLayer = "";
            if (const char* ln = getenv("VF_ROTATE_LAYER"))
                m_rotateLayer = ln;
            if (m_rotateLayer.empty()) {
                // default: the first enabled object layer in the manifest
                for (const auto& l : m_layers.layers())
                    if (l.enabled && l.role == "object" && l.file != vf::voxel::EditableWorld::kFileName) {
                        m_rotateLayer = l.file;
                        break;
                    }
            }
            if (!m_rotateLayer.empty()) {
                commitRotation();
                // the write happens after load, so a headless shot would
                // otherwise capture the pre-rotation world: force the
                // incremental rebuild now (interactive runs just wait for the
                // mtime poll)
                requestWorldReload();
                m_layers.requestReload(m_camera.pos, false);
                while (m_layers.consumeRebuild())
                    applyWorldReload();
            }
        }
    }

    // VF_TEST_BOX="x0,y0,z0,x1,y1,z1": activate the bounding-box selection
    // BEFORE the edit hook below, so a headless shot can prove the scoping:
    // a stamp outside the box changes nothing, a stamp inside changes the
    // world, and the contents report names both counts.
    if (const char* tb = getenv("VF_TEST_BOX"); tb && *tb) {
        glm::ivec3 a(0), b(0);
        if (sscanf(tb, "%d,%d,%d,%d,%d,%d", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z) ==
            6) {
            beginBoxSelection(glm::min(a, b));
            updateBoxSelection(glm::max(a, b));
            endBoxSelection();
        } else {
            spdlog::warn("bad VF_TEST_BOX; expected x0,y0,z0,x1,y1,z1");
        }
    }

    // VF_TEST_EDIT="x,y,z,carve|add|delete|paint|smooth": apply one live store
    // edit right after load so headless shots can verify the patch path
    // deterministically. VF_EDIT_DIAM / VF_EDIT_DEPTH override the brush size;
    // Smooth uses VF_SMOOTH_STRENGTH (or its VF_EDIT_STRENGTH alias).
    if (const char* te = getenv("VF_TEST_EDIT"); te && *te) {
        glm::ivec3 v(0);
        char mode[16] = {};
        if (sscanf(te, "%d,%d,%d,%15s", &v.x, &v.y, &v.z, mode) == 4) {
            m_editActive = true;
            m_editBrush = brushFromName(mode);
            if (const char* d = getenv("VF_EDIT_DIAM"))
                m_editDiameter = finiteEnv(d, m_editDiameter);
            if (const char* d = getenv("VF_EDIT_DEPTH"))
                m_editDepth = finiteEnv(d, m_editDepth);
            quantiseBrush(); // snap onto the lattice: 1 voxel is reachable
            m_hoverHit = {};
            m_hoverHit.hit = true;
            m_hoverHit.voxel = v;
            m_hoverHit.normal = storeNormalAt(v);
            adoptPickOwnership(v);
            applyEditLive();
            spdlog::info("VF_TEST_EDIT {} {} {} {}", v.x, v.y, v.z,
                         brushName(m_editBrush));
        }
    }

    // VF_TEST_STROKE="x,y,z,steps[,carve|add|delete|paint|smooth]": simulate
    // drag-painting by stamping `steps` times along +X with the same spacing
    // the interactive stroke uses; logs per-stamp latency (instant-feedback
    // gate).
    if (const char* ts = getenv("VF_TEST_STROKE"); ts && *ts) {
        glm::ivec3 v(0);
        int steps = 8;
        char mode[16] = {};
        const int nf = sscanf(ts, "%d,%d,%d,%d,%15s", &v.x, &v.y, &v.z, &steps, mode);
        if (nf >= 4) {
            m_editActive = true;
            m_editBrush = nf >= 5 ? brushFromName(mode) : EditBrush::Add;
            if (const char* d = getenv("VF_EDIT_DIAM"))
                m_editDiameter = finiteEnv(d, m_editDiameter);
            if (const char* d = getenv("VF_EDIT_DEPTH"))
                m_editDepth = finiteEnv(d, m_editDepth);
            quantiseBrush(); // snap onto the lattice: 1 voxel is reachable
            const float strokeDiameter =
                std::clamp(m_editDiameter, vf::voxel::VOXEL, 32.0f);
            const int spacing = std::max(
                1, int(strokeDiameter * 0.25f / vf::voxel::VOXEL));
            std::vector<float> ms;
            ms.reserve(size_t(steps));
            for (int i = 0; i < steps; ++i) {
                glm::ivec3 p = v + glm::ivec3(i * spacing, 0, 0);
                m_hoverHit = {};
                m_hoverHit.hit = true;
                m_hoverHit.voxel = p;
                m_hoverHit.normal = storeNormalAt(p);
                adoptPickOwnership(p);
                const auto t0 = std::chrono::steady_clock::now();
                m_dragging = true;
                applyEditLive();
                ms.push_back(float(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count()));
            }
            m_dragging = false;
            std::vector<float> sorted = ms;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0;
            for (float t : ms)
                sum += t;
            spdlog::info("stroke: {} stamps, avg {:.1f} ms, p50 {:.1f}, max {:.1f} "
                         "(spacing {} cells)",
                         steps, sum / std::max<size_t>(1, ms.size()),
                         sorted.empty() ? 0.f : sorted[sorted.size() / 2],
                         sorted.empty() ? 0.f : sorted.back(), spacing);
            if (getenv("VF_TEST_STROKE_SAVE")) {
                // simulate the stroke-end async persistence and wait for it
                m_overlayWriter.queue(m_layers.store(), overlayPath());
                m_overlayWriter.flush();
                spdlog::info("stroke: saved live overlay");
            }
        }
    }

    // VF_TEST_UNDO=1: close the pending stroke and undo it (headless check of
    // the undo path); VF_TEST_CLEAR=1: drop every runtime edit + the overlay
    // file (headless check of the "Clear live edits" button).
    if (getenv("VF_TEST_UNDO")) {
        finishStroke();
        spdlog::info("VF_TEST_UNDO: {} stroke(s) on the undo stack before",
                     m_undo.size());
        undoEdit();
    }
    if (getenv("VF_TEST_CLEAR"))
    clearLiveEdits();

    return 0;
}

// The per-backend camera spawn plus the shot-list resolution. --shot holds one
// view (camera from --cam), --shotlist holds any number; both share the
// headless capture loop, so the resolved list lives on the App (run_capture
// walks it) and a shotlist run is itself headless.
void App::setupCameraAndShots()
{
    const Args& args = m_args;
    // per-backend camera spawn
    // reference view (house.jpeg): over the pond toward the cabin, dock
    // left-of-centre, cabin right, sun raking from the west
    m_camera.pos = { 1.0f, 2.0f, 1.5f };
    glm::vec3 dir = glm::normalize(glm::vec3(5.3f, 1.0f, 11.3f) - m_camera.pos);
    m_camera.yaw = atan2(dir.z, dir.x);
    m_camera.pitch = asin(dir.y);

    // --shot holds one view (its camera comes from --cam); --shotlist holds
    // any number. Both share the headless capture loop below.
    m_shots = args.shots;
    if (m_shots.empty() && !args.shot.empty()) {
        ShotSpec s;
        s.path = args.shot;
        s.camx = args.camx; s.camy = args.camy; s.camz = args.camz;
        s.tx = args.tx;     s.ty = args.ty;     s.tz = args.tz;
        m_shots.push_back(s);
    }
    if (!m_shots.empty()) {
        m_camera.pos = { m_shots[0].camx, m_shots[0].camy, m_shots[0].camz };
        glm::vec3 d = glm::normalize(
            glm::vec3(m_shots[0].tx, m_shots[0].ty, m_shots[0].tz) - m_camera.pos);
        m_camera.yaw = atan2(d.z, d.x);
        m_camera.pitch = asin(d.y);
    } else if (args.camSet) {
        m_camera.pos = { args.camx, args.camy, args.camz };
        glm::vec3 dir = glm::normalize(glm::vec3(args.tx, args.ty, args.tz) - m_camera.pos);
        m_camera.yaw = atan2(dir.z, dir.x);
        m_camera.pitch = asin(dir.y);
    }
}

// Previous-frame camera basis, published at the END of each recorded frame
// (both paths) and seeded here so the FIRST frame already has a valid value.
//
// The unseeded default is pos = origin, right = +X, up = +Y, fwd = +Z,
// tanHalfFov = 1.0: a camera sitting at the world origin looking down +Z.
// Any reprojection against it (TAA history, motion-blur velocity) sees every
// pixel as having moved a long way, which is precisely the "motion blur smears
// a completely still frame" symptom.
void App::advancePrevCam()
{
    m_prevCam.pos = m_camera.pos;
    m_prevCam.right = m_camera.right();
    m_prevCam.up = m_camera.up();
    m_prevCam.fwd = m_camera.forward();
    m_prevCam.tanHalfFov = tanHalfFov60();
    // The swapchain aspect is the projection the G-buffer was rendered with
    // (both recorders build push.a from it), so a reprojection must use the
    // same one - a stale or offscreen-derived aspect mis-scales horizontal
    // velocity by the aspect ratio.
    m_prevCam.aspect =
        float(m_swapchain.extent().width) / float(m_swapchain.extent().height);
}

// The render-flag / photorealism / SSAO / TAA environment overrides, read once
// at startup rather than per frame: these are CI knobs, and the sidebar owns
// the interactive equivalents.
void App::runCollisionSelfCheck()
{
    // A headless DIAGNOSTIC, not a render mode: it drives the shipping paths
    // (resolveCameraMove / updateMicroBehaviours) and logs the outcome, so a
    // script can assert behaviour without a pointer. It runs before the first
    // frame, so a shot taken with VF_TEST_COLLISION=1 shows the walkers after
    // the simulated seconds - that is deliberate, the check is the artefact.
    if (!m_collisionEnabled) {
        spdlog::warn("collision check: SKIP (VF_NO_COLLIDE)");
        return;
    }
    if (!m_layers.loaded()) {
        spdlog::warn("collision check: SKIP (world not loaded)");
        return;
    }
    int failures = 0;

    // 1. Camera vs the WORLD. A 20 m drop at the start camera must be refused
    // above the terrain: the terrain is the one surface every scene has, and
    // "inside solid at the end" is the unambiguous failure.
    {
        vf::voxel::SolidTest solid = [this](const glm::vec3& p) {
            return solidAt(p);
        };
        // Start 2 m ABOVE the camera so the sweep actually travels: a drop
        // from a point already at its stop is a vacuous pass.
        const glm::vec3 from = m_camera.pos + glm::vec3(0.0f, 2.0f, 0.0f);
        const auto r = vf::voxel::moveSphere(solid, from,
                                             glm::vec3(0.0f, -20.0f, 0.0f),
                                             vf::voxel::kCameraRadius);
        const bool inside = solid(r.pos);
        const float fell = from.y - r.pos.y;
        spdlog::info("collision check: camera drop from y={:.2f}: stopped at "
                     "y={:.2f} (fell {:.2f} m, hit={}, inside={})",
                     from.y, r.pos.y, fell, r.hit, inside);
        if (inside || !r.hit || fell >= 20.0f || fell < 0.5f) {
            spdlog::error("collision check: camera FAILED to stop in terrain");
            ++failures;
        }
    }

    // 2. Camera vs a MICRO object. Approach each object's solid centre along
    // the first free cardinal direction; the sweep must stop OUTSIDE and be
    // refused. A micro-only predicate keeps a nearby world wall out of it.
    {
        const glm::vec3 trials[4] = {
            glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1),
            glm::vec3(0, 0, -1)
        };
        for (size_t i = 0; i < m_microFields.size(); ++i) {
            const vf::voxel::MicroField& f = m_microFields[i];
            const vf::voxel::MicroOccluder& o = m_microOccluders[i];
            if (f.size3() == 0 || f.solidMax.x < f.solidMin.x)
                continue;
            const glm::vec3 grid = o.worldOffset + f.origin;
            const glm::vec3 lo = grid + glm::vec3(f.solidMin) * f.voxel;
            const glm::vec3 hi = grid + glm::vec3(f.solidMax + 1) * f.voxel;
            const glm::vec3 centre = 0.5f * (lo + hi);
            // Only THIS object answers: a neighbour 0.3 m away must not make
            // every approach report the same stop.
            vf::voxel::SolidTest microOnly = [this, i](const glm::vec3& p) {
                return microFieldSolidAt(i, p);
            };
            bool tested = false;
            for (const glm::vec3& dir : trials) {
                const glm::vec3 from = centre + dir * 1.0f;
                if (microOnly(from))
                    continue; // the start is already inside this object
                const auto r = vf::voxel::moveSphere(
                    microOnly, from, -dir * 2.0f, vf::voxel::kCameraRadius);
                const bool inside = microOnly(r.pos);
                const float moved = glm::length(from - r.pos);
                spdlog::info("collision check: micro '{}' approach along "
                             "({:.0f},{:.0f},{:.0f}): stopped {:.2f} m in "
                             "(hit={}, inside={})",
                             o.layerFile, dir.x, dir.y, dir.z, moved, r.hit,
                             inside);
                if (inside || !r.hit || moved >= 2.0f) {
                    spdlog::error("collision check: micro '{}' FAILED to stop",
                                  o.layerFile);
                    ++failures;
                }
                tested = true;
                break;
            }
            if (!tested)
                spdlog::warn("collision check: micro '{}' has no free approach",
                             o.layerFile);
        }
    }

    // 3. The WALKERS. Simulate the shipping response and assert the two
    // properties the feature exists for: never below the terrain, never
    // overlapping a world object or another micro object.
    int active = 0;
    std::vector<glm::vec3> startOffsets;
    startOffsets.reserve(m_microBehaviours.size());
    for (const auto& b : m_microBehaviours) {
        startOffsets.push_back(b.offset);
        if (b.active)
            ++active;
    }
    // A zero-walker scene must FAIL, not pass vacuously: every assertion below
    // is inside the walker loop, so an empty behaviour list used to log
    // "0 walker(s) ... PASS". That is exactly what a manifest rewrite that
    // drops the layer's "behaviour" key produces, so the diagnostic names it.
    if (active == 0) {
        spdlog::error("collision check: no active walker - the micro layer "
                      "is disabled or world.json lost its 'behaviour' key");
        ++failures;
    }
    int steps = 600;
    if (const char* e = getenv("VF_TEST_COLLISION_STEPS"))
        steps = std::clamp(atoi(e), 1, 6000);
    int sunken = 0, overlapped = 0;
    float minClear = 1e9f;
    for (int s = 0; s < steps; ++s) {
        updateMicroBehaviours(1.0f / 60.0f);
        for (size_t i = 0; i < m_microBehaviours.size(); ++i) {
            const vf::voxel::MicroBehaviour& b = m_microBehaviours[i];
            if (!b.active || i >= m_microFields.size() ||
                i >= m_microOccluders.size())
                continue;
            const vf::voxel::MicroField& f = m_microFields[i];
            const vf::voxel::MicroOccluder& o = m_microOccluders[i];
            if (f.size3() == 0 || f.solidMax.x < f.solidMin.x)
                continue;
            const glm::vec3 grid = o.worldOffset + f.origin;
            const glm::vec3 lo = grid + glm::vec3(f.solidMin) * f.voxel;
            const glm::vec3 hi = grid + glm::vec3(f.solidMax + 1) * f.voxel;
            float ground =
                terrainTopAt(0.5f * (lo.x + hi.x) + b.offset.x,
                                0.5f * (lo.z + hi.z) + b.offset.z);
            const float xs[2] = { lo.x + b.offset.x, hi.x + b.offset.x };
            const float zs[2] = { lo.z + b.offset.z, hi.z + b.offset.z };
            for (float x : xs)
                for (float z : zs)
                    ground = std::max(ground, terrainTopAt(x, z));
            const float bottom = lo.y + b.offset.y;
            if (ground > -1e8f) {
                minClear = std::min(minClear, bottom - ground);
                if (bottom < ground - 0.005f)
                    ++sunken;
            }
            if (microFootprintBlocked(int(i), b.offset))
                ++overlapped;
        }
    }
    spdlog::info("collision check: {} walker(s), {} steps: {} sunken, {} "
                 "overlapping (min ground clearance {:.3f} m)",
                 active, steps, sunken, overlapped,
                 minClear > 1e8f ? 0.0f : minClear);
    // A walker that never moved would make the checks above vacuous.
    for (size_t i = 0; i < m_microBehaviours.size(); ++i) {
        if (!m_microBehaviours[i].active || i >= startOffsets.size())
            continue;
        const float moved =
            glm::length(m_microBehaviours[i].offset - startOffsets[i]);
        spdlog::info("collision check: walker {} ('{}') moved {:.2f} m",
                     i,
                     i < m_microOccluders.size()
                         ? m_microOccluders[i].layerFile.c_str()
                         : "?",
                     moved);
        if (steps >= 60 && moved < 0.05f) {
            spdlog::error("collision check: walker FAILED to move - the "
                          "obstacle checks are vacuous");
            ++failures;
        }
    }
    if (sunken) {
        spdlog::error("collision check: walker FAILED to stay on the terrain "
                      "({} frame(s) sunk)", sunken);
        ++failures;
    }
    if (overlapped) {
        spdlog::error("collision check: walker FAILED to stay clear of "
                      "objects ({} frame(s) overlapping)", overlapped);
        ++failures;
    }
    spdlog::info("collision check: {}", failures == 0 ? "PASS" : "FAIL");
}

void App::applyStartupEnvOverrides(FrameInputs& fx)
{
    // "frames:path": after N presented frames, dump the swapchain (incl. HUD)
    fx.hudShotFrame = 0;

    if (const char* hs = getenv("VF_HUD_SHOT")) {
        char* endp = nullptr;
        fx.hudShotFrame = strtoull(hs, &endp, 10);
        if (!endp || *endp != ':' || fx.hudShotFrame == 0) {
            fx.hudShotFrame = 0;
            spdlog::warn("bad VF_HUD_SHOT, expected frames:path");
        } else {
            fx.hudShotPath = endp + 1;
        }
    }
    // VF_TEST_PANEL: open the sidebar on a given section at startup. The
    // sidebar is only drawn by the INTERACTIVE path, so this is the only way
    // to reach a non-default section from a script - and the only way to
    // capture one (combine it with VF_HUD_SHOT). Without it, every panel
    // except World is invisible to automated checks.
    if (const char* panel = getenv("VF_TEST_PANEL")) {
        static const char* kNames[] = { "edit", "world", "render", "textures",
                                        "mesh", "ai" };
        bool found = false;
        for (int i = 0; i < kPanelCount; ++i) {
            if (strcasecmp(panel, kNames[i]) == 0) {
                m_panel = Panel(i);
                found = true;
                spdlog::info("VF_TEST_PANEL -> {}", kNames[i]);
                break;
            }
        }
        if (!found)
            spdlog::warn("VF_TEST_PANEL '{}' ignored (edit|world|render|"
                         "textures|mesh|ai)", panel);
    }
    // debug/CI override for the render-flag bitmask (AO/shadow/flora/water/outline)
    if (const char* rf = getenv("VF_RENDER_FLAGS"))
        m_renderFlags = atoi(rf);
    // headless/CI overrides for the photorealism toggles (default off = deterministic)
    if (const char* e = getenv("VF_VOLFOG"))
        m_volFogEnabled = atoi(e) != 0;
    if (const char* e = getenv("VF_MOTIONBLUR"))
        m_motionBlurEnabled = atoi(e) != 0;
    if (const char* e = getenv("VF_DOF"))
        m_dofEnabled = atoi(e) != 0;
    // SSAO tuning (world-scale AO; the effect itself stays opt-in via bit 6)
    if (const char* e = getenv("VF_SSAO_STRENGTH"))
        m_ssaoStrength = std::clamp(float(atof(e)), 0.0f, 1.0f);
    if (const char* e = getenv("VF_SSAO_RADIUS"))
        m_ssaoRadius = std::clamp(float(atof(e)), 0.1f, 4.0f);
    if (const char* e = getenv("VF_SSAO_DEBUG"))
        m_ssaoDebug = atoi(e);
    if (const char* e = getenv("VF_SSAO_BLUR"))
        m_ssaoBlur = atoi(e) != 0;
    // TAA history blend override (debug): 1.0 = pure reprojected history.
    // In a static scene that must stay coherent while rotating (1 frame of
    // lag); if it tears instead, the G-buffer reprojection itself is broken.
    if (const char* e = getenv("VF_TAA_BLEND"))
        m_taaBlend = std::clamp(float(atof(e)), 0.0f, 1.0f);
    // Point-light budget (adjustable cost control): global emitter cap N and
    // per-pixel nearest-K. Applied here at startup, then re-uploaded so the
    // first frame already uses them; the GUI owns the members afterwards.
    if (const char* e = getenv("VF_LIGHT_BUDGET"))
        m_lightBudget = std::clamp(atoi(e), 1, vf::voxel::kMaxLights);
    if (const char* e = getenv("VF_LIGHT_K"))
        m_lightK = std::clamp(atoi(e), 1, vf::voxel::worldfile::kLightKMax);
    // Collision escape hatch: fly through geometry (inspecting a sealed
    // interior, or an A/B that wants the pre-collision free camera).
    // VF_NO_COLLIDE=0 keeps collision on, so the variable reads as a flag.
    if (const char* e = getenv("VF_NO_COLLIDE"))
        m_collisionEnabled = atoi(e) == 0;
    uploadLightSources();
    // Headless collision diagnostic; runs AFTER the env overrides so
    // VF_NO_COLLIDE is honoured.
    if (getenv("VF_TEST_COLLISION"))
        runCollisionSelfCheck();
}

} // namespace app
} // namespace vf
