// Tests for the command line (src/app/cli/args.*) and the sidebar's shared
// vocabulary (src/app/ui/ui_primitives.*, src/app/ui/ui_types.hpp).
//
// Both were unreachable from a test before the split: parseArgs lived in the
// same translation unit as main(), and the sidebar helpers were file-locals in
// an anonymous namespace. parseArgs is the more valuable of the two to pin,
// because every mode the test suite drives (--shot, --shotlist, --selftest,
// --smoke, --probe, --cam, --sun, --mode) is a switch in it, and a silently
// dropped flag turns a whole gate into a test of nothing.
#include "app/cli/args.hpp"
#include "app/sun_angles.hpp"
#include "app/ui/sun_time.hpp"
#include "app/ui/ui_primitives.hpp"
#include "app/ui/ui_types.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace vf::app;

namespace {

// parseArgs takes (argc, argv), so drive it the way main() does.
Args parse(std::vector<const char*> argv)
{
    std::vector<std::string> owned;
    owned.reserve(argv.size());
    for (const char* a : argv)
        owned.emplace_back(a);
    std::vector<char*> raw;
    raw.reserve(owned.size() + 1);
    for (auto& s : owned)
        raw.push_back(s.data());
    raw.push_back(nullptr);
    return parseArgs(int(raw.size() - 1), raw.data());
}

} // namespace

TEST_CASE("args defaults are the documented ones")
{
    const Args a = parse({ "voxelforge" });
    CHECK_FALSE(a.selftest);
    CHECK(a.smokeFrames == 0);
    CHECK(a.width == 1600);
    CHECK(a.height == 900);
    CHECK(a.mode == "splat");          // splat is the default backend
    CHECK(a.shot.empty());
    CHECK(a.shots.empty());
    CHECK_FALSE(a.probeSet);
    CHECK_FALSE(a.camSet);
    CHECK_FALSE(a.sunSet);
    CHECK(a.tonemap == 2);             // AgX Punchy
    CHECK_FALSE(a.llmUrl.empty());
    CHECK_FALSE(a.llmModel.empty());
}

TEST_CASE("args reads the render mode and rejects nothing silently")
{
    CHECK(parse({ "voxelforge", "--mode", "svo" }).mode == "svo");
    CHECK(parse({ "voxelforge", "--mode=splat" }).mode == "splat");
}

TEST_CASE("args reads the headless capture modes")
{
    const Args a = parse({ "voxelforge", "--shot", "/tmp/x.ppm" });
    CHECK(a.shot == "/tmp/x.ppm");
    CHECK(parse({ "voxelforge", "--selftest" }).selftest);
    CHECK(parse({ "voxelforge", "--smoke", "42" }).smokeFrames == 42);
    CHECK(parse({ "voxelforge", "--width", "800", "--height", "450" }).width == 800);
    CHECK(parse({ "voxelforge", "--height", "450" }).height == 450);
    // animtime freezes the clock for a deterministic shot.
    CHECK(parse({ "voxelforge", "--animtime", "2.5" }).animTime == doctest::Approx(2.5f));
}

TEST_CASE("args --cam sets camSet and all six components")
{
    const Args a = parse({ "voxelforge", "--cam", "1", "2", "3", "4", "5", "6" });
    REQUIRE(a.camSet);
    CHECK(a.camx == doctest::Approx(1.f));
    CHECK(a.camy == doctest::Approx(2.f));
    CHECK(a.camz == doctest::Approx(3.f));
    CHECK(a.tx == doctest::Approx(4.f));
    CHECK(a.ty == doctest::Approx(5.f));
    CHECK(a.tz == doctest::Approx(6.f));
}

TEST_CASE("args --sun sets sunSet with the documented default angles")
{
    const Args d = parse({ "voxelforge" });
    CHECK(d.sunElev == doctest::Approx(34.f));
    CHECK(d.sunAzim == doctest::Approx(238.f));
    const Args a = parse({ "voxelforge", "--sun", "12", "30" });
    REQUIRE(a.sunSet);
    CHECK(a.sunElev == doctest::Approx(12.f));
    CHECK(a.sunAzim == doctest::Approx(30.f));
}

TEST_CASE("args --probe sets probeSet and the three coordinates")
{
    const Args a = parse({ "voxelforge", "--probe", "1.5", "2.5", "3.5" });
    REQUIRE(a.probeSet);
    CHECK(a.probe.x == doctest::Approx(1.5f));
    CHECK(a.probe.y == doctest::Approx(2.5f));
    CHECK(a.probe.z == doctest::Approx(3.5f));
}

TEST_CASE("args --shotlist loads one view per line")
{
    // "path x y z tx ty tz" per line; the app loads the world once and walks
    // the list, so a test with N views pays the ~13 s load a single time.
    const char* list = "/tmp/opencode/shotlist_ok.txt";
    FILE* f = fopen(list, "w");
    REQUIRE(f != nullptr);
    fputs("a.ppm 1 2 3 4 5 6\nb.ppm 7 8 9 10 11 12\n", f);
    fclose(f);
    const Args a = parse({ "voxelforge", "--shotlist", list });
    REQUIRE(a.shots.size() == 2);
    CHECK(a.shots[0].path == "a.ppm");
    CHECK(a.shots[1].camx == doctest::Approx(7.f));
    CHECK(a.shots[1].tz == doctest::Approx(12.f));
    remove(list);
    // A missing file is not a crash: the list stays empty and the app runs on.
    const Args miss = parse({ "voxelforge", "--shotlist", "/tmp/opencode/nope.txt" });
    CHECK(miss.shots.empty());
}

TEST_CASE("args ignores an unknown flag instead of aborting")
{
    const Args a = parse({ "voxelforge", "--definitely-not-a-flag" });
    CHECK(a.width == 1600);
}

// ---------------------------------------------------------------- sidebar ---

TEST_CASE("brush names round-trip through the headless hooks")
{
    // brushFromName is what VF_TEST_EDIT="x,y,z,<name>" parses, and brushName
    // is what the stamp log prints, so a mismatch makes a test assert on a
    // different word than the log emits.
    const EditBrush all[] = { EditBrush::Carve,  EditBrush::Add,
                              EditBrush::Delete, EditBrush::Paint,
                              EditBrush::Smooth, EditBrush::Rotate,
                              EditBrush::Move };
    for (EditBrush b : all)
        CHECK(brushFromName(brushName(b)) == b);
    // "raise" and "relax" are the legacy aliases the older hooks used.
    CHECK(brushFromName("raise") == EditBrush::Add);
    CHECK(brushFromName("relax") == EditBrush::Smooth);
    // Anything unrecognised is a carve, never a silent no-op.
    CHECK(brushFromName(nullptr) == EditBrush::Carve);
    CHECK(brushFromName("") == EditBrush::Carve);
    CHECK(brushFromName("nonsense") == EditBrush::Carve);
}

TEST_CASE("foldCase lowercases for the layer and mesh filters")
{
    CHECK(foldCase("Hamlet_Tower") == "hamlet_tower");
    CHECK(foldCase("") == "");
    CHECK(foldCase("ABC123") == "abc123");
}

TEST_CASE("sidebar width is clamped to keep a usable pane on a desktop window")
{
    // 1600 px wide: the responsive default is rail 46 + pane 300.
    const float def = sidebarWidthFor(0.f, 1600.f);
    CHECK(def == doctest::Approx(46.f + 300.f));
    // A user request is honoured when it fits.
    CHECK(sidebarWidthFor(500.f, 1600.f) == doctest::Approx(500.f));
    // ...and clamped when it would not.
    CHECK(sidebarWidthFor(5000.f, 1600.f) <= 1600.f);
    // Never narrower than the rail plus the minimum pane.
    CHECK(sidebarWidthFor(10.f, 1600.f) >= 46.f + 150.f);
}

TEST_CASE("sidebar width on a tiny window fits the display")
{
    // 300 px wide: fitting the display wins, so the docked edge never leaves a
    // scene-coloured strip at the top or bottom.
    CHECK(sidebarWidthFor(0.f, 300.f) <= 300.f);
    CHECK(sidebarWidthFor(0.f, 300.f) >= 46.f);
    // The pane shrinks toward its minimum rather than going to zero.
    CHECK(sidebarWidthFor(0.f, 300.f) < 46.f + 300.f);
}

TEST_CASE("clampSidebarWidth passes a non-positive display width through")
{
    // There is no viewport to clamp against yet (before the first resize), so
    // the width must be returned untouched rather than collapsed to the min.
    CHECK(clampSidebarWidth(400.f, 0.f) == doctest::Approx(400.f));
    CHECK(clampSidebarWidth(400.f, -1.f) == doctest::Approx(400.f));
}

TEST_CASE("the sidebar layout metrics agree with each other")
{
    // The pane minimum plus the rail must be smaller than the shipped default,
    // otherwise the clamp would fire on every normal window.
    CHECK(46.f + 150.f < 46.f + 300.f);
    // The grip sits inside the window so it can never be dragged off-screen.
    CHECK(46.f + 150.f <= 1600.f - 8.f);
    // The rail is narrower than the default pane, so collapsing to the rail is
    // always a visible change.
    CHECK(kRailW < kPaneW);
}

TEST_CASE("the rail table matches the panel enum")
{
    // Order is the rail order AND the Ctrl+1..6 order; kPanels is indexed by
    // Panel, so a mismatch desynchronises the button from its shortcut.
    CHECK(kPanelCount == 6);
    CHECK(kPanels[int(Panel::Edit)].label[0] == 'E');
    CHECK(kPanels[int(Panel::World)].label[0] == 'W');
    CHECK(kPanels[int(Panel::Render)].label[0] == 'R');
    CHECK(kPanels[int(Panel::Textures)].label[0] == 'T');
    CHECK(kPanels[int(Panel::Mesh)].label[0] == 'I');
    CHECK(kPanels[int(Panel::AI)].label[0] == 'A');
    // Every entry is a 2-letter label (the default ImGui font has no glyphs).
    for (int i = 0; i < kPanelCount; ++i)
        CHECK(std::strlen(kPanels[i].label) == 2);
}

TEST_CASE("the material combo names cover the palette")
{
    // kMatNames drives the Edit panel's material combo; index N must read as
    // material N or Paint logs a different material than the user picked.
    CHECK(std::strlen(kMatNames[0]) > 0);
    CHECK(std::strlen(kMatNames[20]) > 0);
    for (int i = 0; i < 21; ++i)
        CHECK(std::strncmp(kMatNames[i], std::to_string(i).c_str(), 1) == 0);
}

// ---------------------------------------------------------------- sun clock -

TEST_CASE("sunAnglesForTime hits the four cardinal clock points")
{
    // 12 h analytic arc: sunrise 06:00 E, noon +60 S, sunset 18:00 W,
    // midnight -60 N. Azimuth closes 360 deg in 24 h at 15 deg/h.
    float e = 0.f, a = 0.f;
    sunAnglesForTime(0.f, e, a);
    CHECK(e == doctest::Approx(-60.f));
    CHECK(a == doctest::Approx(0.f));
    sunAnglesForTime(6.f, e, a);
    CHECK(e == doctest::Approx(0.f));
    CHECK(a == doctest::Approx(90.f));
    sunAnglesForTime(12.f, e, a);
    CHECK(e == doctest::Approx(60.f));
    CHECK(a == doctest::Approx(180.f));
    sunAnglesForTime(18.f, e, a);
    CHECK(e == doctest::Approx(0.f));
    CHECK(a == doctest::Approx(270.f));
}

TEST_CASE("sunAnglesForTime wraps out-of-range hours into the day")
{
    float e = 0.f, a = 0.f, e2 = 0.f, a2 = 0.f;
    sunAnglesForTime(24.f, e, a);
    sunAnglesForTime(0.f, e2, a2);
    CHECK(e == doctest::Approx(e2));
    CHECK(a == doctest::Approx(a2));
    sunAnglesForTime(-1.f, e, a);
    sunAnglesForTime(23.f, e2, a2);
    CHECK(e == doctest::Approx(e2));
    CHECK(a == doctest::Approx(a2));
}

TEST_CASE("parseSunTime accepts HH:MM and rejects garbage")
{
    float h = -1.f;
    CHECK(parseSunTime("00:00", h));
    CHECK(h == doctest::Approx(0.f));
    CHECK(parseSunTime("6:30", h));
    CHECK(h == doctest::Approx(6.5f));
    CHECK(parseSunTime("23:59", h));
    CHECK(h == doctest::Approx(23.f + 59.f / 60.f));
    CHECK_FALSE(parseSunTime("24:00", h));
    CHECK_FALSE(parseSunTime("12:60", h));
    CHECK_FALSE(parseSunTime("noon", h));
    CHECK_FALSE(parseSunTime("", h));
    CHECK_FALSE(parseSunTime(nullptr, h));
    CHECK_FALSE(parseSunTime("12:30 ", h)); // trailing text is not a time
}

TEST_CASE("sunTimeForAngles round-trips on-arc suns exactly")
{
    // Every hour mark lies on the arc by construction, so the inverse must
    // come back without the ambiguity flag.
    for (int hh = 0; hh < 24; ++hh) {
        float e = 0.f, a = 0.f;
        sunAnglesForTime(float(hh), e, a);
        float back = -1.f;
        bool approx = true;
        sunTimeForAngles(e, a, back, approx);
        CHECK_FALSE(approx);
        CHECK(back == doctest::Approx(float(hh)).epsilon(1e-3));
    }
}

TEST_CASE("sunTimeForAngles marks off-arc suns approximate")
{
    // The night preset (-30/96) decodes to 06:24, off by 36.3 deg of
    // elevation: azimuth carries no elevation sign, so a below-horizon sun in
    // the east reads as dawn. The "~" readout must say so, never a bare time.
    float h = -1.f;
    bool approx = false;
    sunTimeForAngles(-30.f, 96.f, h, approx);
    CHECK(approx);
    CHECK(h == doctest::Approx(6.4f).epsilon(1e-3));
    // The day preset (34/238) lands near the arc by luck (15:52, off 2.1 deg).
    sunTimeForAngles(34.f, 238.f, h, approx);
    CHECK_FALSE(approx);
}

TEST_CASE("formatSunTime prints zero-padded HH:MM and wraps")
{
    char buf[8] = "";
    formatSunTime(0.f, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "00:00") == 0);
    formatSunTime(6.5f, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "06:30") == 0);
    formatSunTime(23.f + 59.f / 60.f, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "23:59") == 0);
    formatSunTime(24.f, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "00:00") == 0);
}

// ---------------------------------------------------------------------------
// The two-suns distinction the night gate depends on.
//
// The tree has THREE reachable "nights" and they are not the same frame:
//   1. the Night preset   -> kSunNightElev / kSunNightAzim  (P key + button)
//   2. the clock at 00:00 -> -60 / 0                          (VF_TEST_SUN_TIME)
//   3. anywhere a slider drag lands
// A reference that says "night mean luma = X" is therefore ambiguous unless it
// names which one, so the preset is what the gate pins. These cases assert the
// distinction on ANGLES, which are exact and deterministic -- not on luma, whose
// two measured arms differ by 1.7 (inside the 1.3 spread that made the raw
// means unusable in the first place).

TEST_CASE("clock midnight and the night preset are DIFFERENT suns")
{
    float e = 0.f, a = 0.f;
    sunAnglesForTime(0.f, e, a);
    CHECK(e == doctest::Approx(-60.f).epsilon(1e-3));
    CHECK(a == doctest::Approx(0.f).epsilon(1e-3));

    // 30 deg of elevation and 96 deg of azimuth apart: not the same night
    // spelled differently.
    CHECK(std::fabs(e - kSunNightElev) > 25.f);
    CHECK(std::fabs(a - kSunNightAzim) > 90.f);

    // Both are "night" by the shared threshold, which is exactly what makes
    // this a rendered difference rather than a label difference.
    CHECK(sunIsNight(e));
    CHECK(sunIsNight(kSunNightElev));

    // The moon lifts the anti-solar direction by (-kSunDir.y * 0.75 + 0.30), so
    // it clears the horizon higher at -60 than at -30. Deliberately NOT
    // re-derived here: a second copy of shader constants is free to drift,
    // which is the mistake this whole header exists to avoid.
}

TEST_CASE("sunIsNight is the ONE predicate for the button highlight and the P key")
{
    // The toggle is setSunPhase(!sunIsNight(elev)) and the highlight is
    // sunIsNight(elev), so pressing P from either preset lands on the other.
    // Before these moved into sun_angles.hpp this was a -2.0f literal in three
    // places across two files -- they agreed by convention only.
    CHECK(sunIsNight(kSunNightElev));      // night preset -> P goes to day
    CHECK_FALSE(sunIsNight(kSunDayElev));  // day preset   -> P goes to night
    CHECK(sunIsNight(kSunNightElevThreshold));      // boundary is inclusive
    CHECK_FALSE(sunIsNight(kSunNightElevThreshold + 0.1f));
}

TEST_CASE("the day preset is the CLI default sun, so reference shots do not move")
{
    // 34/238 is what every reference shot was calibrated to. If the preset and
    // the CLI default drift apart, snapping back to Day would no longer restore
    // the frame the gates were measured against -- and nothing would fail,
    // because both are "correct" on their own.
    const Args a;
    CHECK(a.sunElev == doctest::Approx(kSunDayElev).epsilon(1e-6));
    CHECK(a.sunAzim == doctest::Approx(kSunDayAzim).epsilon(1e-6));
}

TEST_CASE("submitting the default sun's read-back moves it less than 3 deg")
{
    // Round-trip is NOT a no-op and the test pins that instead of hiding it:
    // 34/238 decodes to 15:52 (unmarked, off by 2.2 deg), and pressing Enter
    // on that commits the ARC value 31.8. Documented in sun_time.hpp: the
    // display is a projection, not the stored state.
    float h = -1.f;
    bool approx = true;
    sunTimeForAngles(34.f, 238.f, h, approx);
    CHECK_FALSE(approx);
    char buf[8] = "";
    formatSunTime(h, buf, sizeof(buf));
    float back = -1.f;
    REQUIRE(parseSunTime(buf, back));
    float e = 0.f, a = 0.f;
    sunAnglesForTime(back, e, a);
    CHECK(std::fabs(e - 34.f) < 3.f);
    CHECK(e == doctest::Approx(31.8f).epsilon(1e-2));
}
