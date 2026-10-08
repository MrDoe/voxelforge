#include "voxel/irradiance_volume.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace vf::voxel {
namespace {

// Same bilinear convention as surfelize.cpp's heightAt and the shader's
// heightAt(): texel centre at world centre, edge-extended. Copying the
// convention rather than reinventing it matters because the shadow decision
// below must agree with the splat path's, and a half-texel offset here would
// put a shadow edge in a different place from the direct term it sits next to.
float heightAt(const VoxelField& field, float wx, float wz) {
    const auto& ht = field.heightTexture();
    const int sz = field.latN();
    if (sz <= 0 || ht.size() < size_t(sz) * size_t(sz)) return -1e9f;
    const glm::vec2 tc = glm::clamp(glm::vec2(wx, wz) / WORLD + 0.5f,
                                    glm::vec2(0.0f), glm::vec2(1.0f)) *
                        glm::vec2(sz - 1);
    const glm::ivec2 i0(glm::floor(tc)), i1(glm::min(i0 + 1, sz - 1));
    const glm::vec2 f = tc - glm::vec2(i0);
    const float h00 = ht[size_t(i0.y) * sz + size_t(i0.x)].r;
    const float h10 = ht[size_t(i0.y) * sz + size_t(i1.x)].r;
    const float h01 = ht[size_t(i1.y) * sz + size_t(i0.x)].r;
    const float h11 = ht[size_t(i1.y) * sz + size_t(i1.x)].r;
    return glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.y), f.y);
}

// Coarse object distance in METRES, matching objDist() in common_base.glsl:
// r8_snorm holding round(d / 1.26 * 127), and empty space stored as exactly
// +127, which reads back as +kObjVolMax. Treating "no information" as a finite
// distance is deliberate in the shader (it clamps stepping), so it is
// deliberate here too: using +inf would make every march take maximum steps.
float objDistAt(const VoxelField& field, glm::vec3 p) {
    const auto& ov = field.objectVolume();
    const int n = VoxelField::kObjVolN;
    if (ov.size() < size_t(n) * size_t(n) * size_t(n)) return VoxelField::kObjVolMax;
    const glm::vec3 tc = glm::clamp(p / WORLD + 0.5f, glm::vec3(0.0f),
                                    glm::vec3(1.0f)) *
                         glm::vec3(float(n - 1));
    const glm::ivec3 i0(glm::floor(tc)), i1(glm::min(i0 + 1, n - 1));
    const glm::vec3 f = tc - glm::vec3(i0);
    auto tex = [&](int x, int y, int z) {
        return float(ov[(size_t(z) * size_t(n) + size_t(y)) * size_t(n) +
                        size_t(x)]) /
               127.0f * VoxelField::kObjVolMax;
    };
    const glm::vec3 c00 = glm::mix(glm::vec3(tex(i0.x, i0.y, i0.z)),
                                   glm::vec3(tex(i1.x, i0.y, i0.z)), f.x);
    const glm::vec3 c10 = glm::mix(glm::vec3(tex(i0.x, i1.y, i0.z)),
                                   glm::vec3(tex(i1.x, i1.y, i0.z)), f.x);
    const glm::vec3 c01 = glm::mix(glm::vec3(tex(i0.x, i0.y, i1.z)),
                                   glm::vec3(tex(i1.x, i0.y, i1.z)), f.x);
    const glm::vec3 c11 = glm::mix(glm::vec3(tex(i0.x, i1.y, i1.z)),
                                   glm::vec3(tex(i1.x, i1.y, i1.z)), f.x);
    return glm::mix(glm::mix(c00, c10, f.y), glm::mix(c01, c11, f.y), f.z).y;
}

// Ported from lightVisibilitySplat in common_splat.glsl, including its
// both-endpoints-buried shortcut: a heightfield can only answer "solid" for
// any point below its own column, so a lamp in a room cut into a hillside
// reads as buried at both ends and would return 0 at the first tap. A
// heightfield cannot have overhangs, so a segment that never crosses the
// surface cannot be crossing terrain. Kept as a port rather than a call site
// because the splat version is GLSL in a shader that cannot be linked from CPU
// - if one side is edited, this must be edited too.
float visibilityTo(const VoxelField& field, glm::vec3 ro, glm::vec3 rdIn,
                   float maxDist) {
    const glm::vec3 rd = glm::normalize(rdIn);
    const glm::vec3 end = ro + rd * maxDist;
    const bool buriedBoth = (heightAt(field, ro.x, ro.z) - ro.y > 0.05f) &&
                            (heightAt(field, end.x, end.z) - end.y > 0.05f);
    float t = 0.05f;
    for (int i = 0; i < 16; ++i) {
        const glm::vec3 sp = ro + rd * t;
        const float sHf = sp.y - heightAt(field, sp.x, sp.z);
        if (!buriedBoth && sHf < -0.03f) return 0.0f;
        const float sObj = objDistAt(field, sp);
        if (sObj < 1.2474f && sObj < -0.05f) return 0.0f;
        // Mirrors lightVisibilitySplat exactly, INCLUDING the max(..., voxel*
        // 0.35) floor before the 0.85 scale. An earlier version of this port
        // folded that floor away on the grounds that the following clamp(.., 0.05,
        // 1.2) made it redundant - and today it IS redundant, because for any
        // distance under ~0.04 m both forms land on the 0.05 floor. That is
        // exactly why it is dangerous: the two agree only by coincidence of
        // constants, so editing VOXEL or the 0.85 would make this CPU march
        // disagree with the GPU one it is supposed to mirror, in a direction
        // (fewer, longer steps) that leaks MORE light through thin geometry.
        t += buriedBoth ? std::max(sObj, VOXEL * 0.35f)
                        : glm::clamp(
                              std::max(std::min(sHf, sObj), VOXEL * 0.35f) * 0.85f,
                              0.05f, 1.2f);
        if (t >= maxDist) return 1.0f;
    }
    return 1.0f;
}

// Fraction of the sky this cell can see, on the same terms the bit-8 enclosure
// test uses: one short upward march over the coarse object volume. It is only
// there to separate "indoors, so emitter light only" from "outdoors, so some
// sky too" - the actual sky radiance is added analytically at shading time, so
// this deliberately does NOT try to be an accurate sky-visibility integral.
float skyVisibility(const VoxelField& field, glm::vec3 p) {
    float t = 0.5f;
    for (int i = 0; i < 8; ++i) {
        if (objDistAt(field, p + glm::vec3(0.0f, t, 0.0f)) < 0.0f)
            return 0.0f;
        t += 1.5f;
        if (t > 24.0f) return 1.0f;
    }
    return 1.0f;
}

} // namespace

IrradianceVolume buildIrradianceVolume(const VoxelField& field,
                                       const worldfile::LightUBO& lights,
                                       glm::vec3 sunDir,
                                       IrradianceBakeStats* statsOut) {
    IrradianceVolume out;
    IrradianceBakeStats st;

    constexpr int kN = IrradianceVolume::kN;
    constexpr float kCell = IrradianceVolume::kCellM;

    // Collect the live emitters up front so the hot loop reads a flat array and
    // the thread count is bounded by cells, not by light count.
    struct Emit {
        glm::vec3 pos;
        glm::vec3 color;   // colour * intensity, pre-multiplied
        float radius;
    };
    std::vector<Emit> emit;
    emit.reserve(size_t(worldfile::kMaxLights));
    const int n = glm::clamp(lights.count, 0, worldfile::kMaxLights);
    for (int i = 0; i < n; ++i) {
        const glm::vec4 pr = lights.posRadius[i];
        const glm::vec4 ci = lights.colorIntensity[i];
        if (pr.w <= 0.0f || ci.w <= 0.0f) continue;  // dropped upstream
        Emit e;
        e.pos = glm::vec3(pr);
        e.color = glm::vec3(ci) * ci.w;
        e.radius = pr.w;
        emit.push_back(e);
        ++st.seen;
    }

    out.cells.assign(size_t(kN) * size_t(kN) * size_t(kN), glm::vec4(0.0f));

    // VF_NO_IRR_VOLUME=1 -> the bit-exact "term absent" state, for A/B-ing the
    // volume without a rebuild. Read here rather than at the call site because
    // this bake already takes its switches from the environment the same way
    // SurfelParams does for VF_LOD / VF_MICRO / VF_ANISO, and because it needs
    // no cooperation from whoever wires the upload.
    //
    // Also useful on its own: with the volume off the shading term reduces to
    // `alb * 0 * ...`, so the volume's own contribution to the frame should be
    // nil. Whether the frame is BIT-IDENTICAL to a build that never had the
    // feature is NOT yet established - that needs a render against a pre-feature
    // binary, and no such A/B has been run. Do not record it as verified.
    if (const char* offEnv = getenv("VF_NO_IRR_VOLUME")) {
        if (atoi(offEnv) != 0) {
            if (statsOut) *statsOut = st;
            return out; // all-zero: irradianceVolume() reads 0 and drops out
        }
    }

    if (emit.empty()) {
        // No emitters is a legitimate state (a daylight scene with nothing
        // emissive), so this is not an error - but the caller still logs seen=0
        // so "no indirect light" is a recorded fact rather than a silent gap.
        if (statsOut) *statsOut = st;
        return out;
    }

    // The emitters AS THE BAKE SEES THEM, against the cell centres this bake
    // will actually visit. A caller-side table and an in-bake table that
    // disagree is exactly the failure this census exists to catch, and the only
    // way to settle it is both in one output. Declared here, before the sun
    // term, so it reads before the census struct below.
    const bool irrDiag = [] {
        const char* e = getenv("VF_IRR_DIAG");
        return e && atoi(e) != 0;
    }();
    if (irrDiag) {
        std::printf("[VF_IRR_DIAG] bake sees %zu emitters (count=%d)\n",
                    emit.size(), n);
        for (size_t i = 0; i < emit.size(); ++i) {
            int inRange = 0;
            float minDist = 1e9f;
            for (int z = 0; z < kN; ++z)
                for (int y = 0; y < kN; ++y)
                    for (int x = 0; x < kN; ++x) {
                        const glm::vec3 cc((float(x) + 0.5f) * kCell - IrradianceVolume::kOriginOffset,
                                           (float(y) + 0.5f) * kCell - IrradianceVolume::kOriginOffset,
                                           (float(z) + 0.5f) * kCell - IrradianceVolume::kOriginOffset);
                        const float d = glm::length(emit[i].pos - cc);
                        if (d < minDist) minDist = d;
                        if (d < emit[i].radius) ++inRange;
                    }
            std::printf("   emitter %2zu  pos(%+7.3f,%+7.3f,%+7.3f)  r=%5.3f  "
                        "inRange=%d  minDist=%6.3f\n",
                        i, emit[i].pos.x, emit[i].pos.y, emit[i].pos.z,
                        emit[i].radius, inRange, minDist);
        }
    }

    // Sun-driven sky presence only; its magnitude belongs to the analytic sky
    // at shading time, so this stays a 0..1 term rather than a second light
    // budget that would double-count the sun the shading path already has.
    const float sunUp = glm::clamp(sunDir.y * 2.0f, 0.0f, 1.0f);

    // One bit per emitter, so "used" means what the header says it means:
    // an emitter that reached no cell centre is reported as NOT used rather
    // than silently counted. A bitmask rather than a counter because the test
    // happens once per (cell, emitter) pair and only needs to be sticky.
    std::atomic<unsigned> usedMask{0u};
    std::atomic<int> lit{0};

    // Rejection census for the pairs that survive the radius test.
    // Deliberately inside this file rather than re-derived in a scratch program:
    // the question is which of THIS function's predicates rejects, so a second
    // implementation of the predicate would be the closed loop all over again.
    // It answers what the app's own log cannot: "14 seen / 0 used" says some
    // (cell,emitter) pair failed, not WHICH test and at which tap.
    struct Rej {
        std::atomic<int> inRadius{0}, hfFirstTap{0}, objBlocked{0},
            lateTap{0}, visible{0}, zeroDist{0}, slicesClaimed{0};
        std::mutex mtx;
    } diag;

    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    hc = std::min<unsigned>(hc, unsigned(kN));
    std::vector<std::thread> threads;
    threads.reserve(hc);
    std::atomic<int> nextSlice{0};
    // kSlice is the number of SLICES, so the slice STRIDE must be kN/kSlice, not
    // kSlice. Written as `sz * kSlice` the loop covered only z in
    // [0, kSlice*kSlice) = [0,16) of 64 - one quarter of the volume, and the
    // quarter that survives is the world edge at negative z, while 15 of the 16
    // emissive clusters sit near z +7 to +15. Every (cell,emitter) pair missed
    // the radius test, so the bake reported 0 used / 0 cells lit, which reads
    // exactly like an occlusion failure.
    //
    // The unit tests could not see it: they place a synthetic emitter wherever
    // the scan finds air, and the truncated range happened to include it. A test
    // that chooses its own subject cannot notice the subject set is a quarter of
    // what it should be. What found it was an instrumented census INSIDE this
    // loop disagreeing with an independent caller's table about the same light
    // set - 40 pairs in range against 0 visited.
    constexpr int kSlice = 4;                 // 4 slices
    constexpr int kZPerSlice = kN / kSlice;   // 16 z each => all 64 covered
    for (unsigned t = 0; t < hc; ++t) {
        threads.emplace_back([&] {
            for (;;) {
                const int sz = nextSlice.fetch_add(1);
                if (sz >= kSlice) return;
                if (irrDiag) ++diag.slicesClaimed;
                for (int z = sz * kZPerSlice; z < (sz + 1) * kZPerSlice; ++z) {
                    for (int y = 0; y < kN; ++y) {
                        for (int x = 0; x < kN; ++x) {
                            // Origin-CENTRED cell centre. The subtraction is
                            // load-bearing and was absent in the first version:
                            // (x+0.5)*kCell alone spans 0..WORLD, but the world
                            // is centred on the origin (-WORLD/2..+WORLD/2) - see
                            // VoxelField::sampleWorld, which is the authority:
                            // int((p.x + 0.5f * WORLD) / VOXEL). The shader's
                            // irradianceVolume() samples in that same centred
                            // frame (common_irradiance.glsl does
                            // clamp(p/pc.b.x + 0.5)), so an uncentred bake writes
                            // every cell 51.2 m = 32 cells from where the GPU
                            // reads it. The visible symptom was not a subtle
                            // offset but a total inversion of the term: an
                            // interior point read the cell the CPU evaluated for
                            // open sky 51.2 m above it, so sealed rooms washed
                            // with sky and received no lamp light at all.
                            //
                            // It survived 232 assertions because the TEST copied
                            // this same uncentred formula - bake and test agreed
                            // with each other and neither had an external
                            // reference point. See tests/test_world.cpp's
                            // irrCellCentre, which now carries the same comment
                            // and, more usefully, a test that fails without this
                            // subtraction.
                            const glm::vec3 p((float(x) + 0.5f) * kCell - IrradianceVolume::kOriginOffset,
                                              (float(y) + 0.5f) * kCell - IrradianceVolume::kOriginOffset,
                                              (float(z) + 0.5f) * kCell - IrradianceVolume::kOriginOffset);
                            glm::vec3 acc(0.0f);
                            // applyLights' OWN attenuation, (1 - d/r)^2, not
                            // inverse-square: matching the direct term is what
                            // keeps one light additive instead of showing two
                            // falloff curves in the same frame.
                            for (const Emit& e : emit) {
                                const glm::vec3 toL = e.pos - p;
                                const float dist = glm::length(toL);
                                if (dist >= e.radius) continue;
                                if (irrDiag) {
                                    // Census uses the SAME helper values the
                                    // march is about to take, not a re-derivation.
                                    const float sHf0 =
                                        p.y - heightAt(field, p.x, p.z);
                                    const float sObj0 = objDistAt(field, p);
                                    int why = 0;
                                    if (!(dist <= 1e-4f)) {
                                        if (sHf0 < -0.03f) why = 1;
                                        else if (sObj0 < -0.05f) why = 2;
                                        else {
                                            why = visibilityTo(field, p, toL,
                                                               dist) > 0.0f
                                                      ? 0
                                                      : 3;
                                        }
                                    } else
                                        why = 4;
                                    std::lock_guard<std::mutex> lk(diag.mtx);
                                    if (why == 1)
                                        ++diag.hfFirstTap;
                                    else if (why == 2)
                                        ++diag.objBlocked;
                                    else if (why == 3)
                                        ++diag.lateTap;
                                    else if (why == 0)
                                        ++diag.visible;
                                    else if (why == 4)
                                        ++diag.zeroDist;
                                    ++diag.inRadius;
                                }
                                // A cell that CONTAINS the emitter is that
                                // emitter's brightest cell, not a dark hole:
                                // (1 - d/r)^2 is 1.0 at d=0, so the falloff wants
                                // the maximum there. Skip the march instead of
                                // attempting it, because visibilityTo() normalises
                                // the direction and normalize() of a zero-length
                                // vector is undefined. Without this the cell under
                                // a hearth came out BLACK while every neighbour lit
                                // correctly - caught by the leak test, not by eye.
                                const float vis =
                                    dist <= 1e-4f ? 1.0f : visibilityTo(field, p, toL, dist);
                                if (vis <= 0.0f) continue;
                                float atten =
                                    glm::clamp(1.0f - dist / e.radius, 0.0f, 1.0f);
                                atten *= atten;
                                acc += e.color * (atten * vis);
                                usedMask.fetch_or(1u << (&e - emit.data()),
                                                  std::memory_order_relaxed);
                            }
                            if (acc.r + acc.g + acc.b > 0.0f) ++lit;
                            const float sky = skyVisibility(field, p) * sunUp;
                            // Written through IrradianceVolume::indexOf so the linear order exists in
                            // exactly one place on the CPU side. The TEST keeps its
                            // own independent irrIndex on purpose - that is the
                            // consumer outside the pair which is able to disagree,
                            // and collapsing it into this one would re-create the
                            // closed loop that hid the frame bug.
                            out.cells[IrradianceVolume::indexOf(x, y, z)] =
                                glm::vec4(acc, sky);
                        }
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    if (irrDiag) {
        std::printf("\n[VF_IRR_DIAG] slices claimed: %d of kSlice=%d, "
                    "zPerSlice=%d -> z covered %d of %d\n",
                    diag.slicesClaimed.load(), kSlice, kZPerSlice,
                    diag.slicesClaimed.load() * kZPerSlice, kN);
        std::printf("[VF_IRR_DIAG] (cell,emitter) pairs with dist < radius: %d\n",
                    diag.inRadius.load());
        std::printf("  rejected by FIRST TAP heightfield (sHf < -0.03): %d\n",
                    diag.hfFirstTap.load());
        std::printf("  rejected by coarse objVol (sObj < -0.05):        %d\n",
                    diag.objBlocked.load());
        std::printf("  rejected by a LATER tap of visibilityTo:          %d\n",
                    diag.lateTap.load());
        std::printf("  VISIBLE (contributed):                           %d\n",
                    diag.visible.load());
        std::printf("  emitter inside the cell (dist ~ 0):              %d\n\n",
                    diag.zeroDist.load());
    }

    const unsigned mask = usedMask.load(std::memory_order_relaxed);
    int used = 0;
    for (size_t i = 0; i < emit.size(); ++i)
        if (mask & (1u << i)) ++used;
    st.used = used;
    st.cellsLit = int(lit.load());
    if (statsOut) *statsOut = st;
    return out;
}

} // namespace vf::voxel