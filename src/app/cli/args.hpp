#pragma once

// The command line: the render mode, the headless capture modes, and the
// view list a --shotlist run walks under a single world load.

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace vf {
namespace app {

// One camera view of a headless render. A --shotlist file holds one per line
// ("path x y z tx ty tz"); the app loads the world once and walks the list,
// so a test with N views pays the ~13 s world load a single time.
struct ShotSpec {
    std::string path;
    float camx=0,camy=0,camz=0,tx=0,ty=0,tz=0;
};

struct Args {
    bool selftest = false;
    int smokeFrames = 0;
    int width = 1600, height = 900;
    std::string shot;    // dump one frame to PPM and exit
    std::vector<ShotSpec> shots; // --shotlist: one load, N cameras
    float camx=0,camy=0,camz=0,tx=0,ty=0,tz=0;
    bool camSet=false;
    float sunElev=34.0f, sunAzim=238.0f; // golden-hour: long visible shadows
    bool sunSet=false;
    float animTime=0.0f;
    int tonemap = 2;    // AgX look: 0=Default 1=Golden 2=Punchy
    std::string mode = "splat"; // primary renderer: "splat" | "svo" (reference)
    bool probeSet=false;
    glm::vec3 probe { 0.f };
    std::string llmUrl = "http://127.0.0.1:11434";
    std::string llmModel = "gemma4:12b";
};

Args parseArgs(int argc, char** argv);

} // namespace app
} // namespace vf
