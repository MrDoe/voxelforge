#include "app/cli/args.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

Args parseArgs(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](int def) -> int {
            return i + 1 < argc ? atoi(argv[++i]) : def;
        };
        if (s == "--selftest")
            a.selftest = true;
        else if (s == "--smoke")
            a.smokeFrames = next(240);
        else if (s == "--width")
            a.width = next(a.width);
        else if (s == "--height")
            a.height = next(a.height);
        else if (s == "--shot" && i + 1 < argc)
            a.shot = argv[++i];
        else if (s == "--shotlist" && i + 1 < argc) {
            const std::string listFile = argv[++i];
            std::ifstream lf(listFile);
            if (!lf) {
                spdlog::warn("--shotlist '{}' cannot be opened", listFile);
            } else {
                std::string line;
                while (std::getline(lf, line)) {
                    if (!line.empty() && line[0] == '#')
                        continue;
                    std::istringstream ls(line);
                    ShotSpec sh;
                    if (ls >> sh.path >> sh.camx >> sh.camy >> sh.camz >>
                            sh.tx >> sh.ty >> sh.tz)
                        a.shots.push_back(sh);
                }
                if (a.shots.empty())
                    spdlog::warn("--shotlist '{}' has no valid lines", listFile);
            }
        }
        else if (s == "--cam" && i + 1 < argc) {
            // accept either six argv tokens or one comma-separated token
            // ("x,y,z,tx,ty,tz") - the documented single-token form silently
            // no-ops under a bare argc check
            float* dst[6] = { &a.camx, &a.camy, &a.camz, &a.tx, &a.ty, &a.tz };
            std::string tok = argv[i + 1];
            const bool comma = tok.find(',') != std::string::npos;
            if (comma) {
                std::istringstream ls(tok);
                std::string val;
                for (int c = 0; c < 6 && std::getline(ls, val, ','); ++c)
                    *dst[c] = float(atof(val.c_str()));
                ++i;
            } else if (i + 6 < argc) {
                for (int c = 0; c < 6; ++c)
                    *dst[c] = float(atof(argv[++i]));
            } else {
                spdlog::warn("--cam needs 6 values (x y z tx ty tz)");
                break;
            }
            a.camSet = true;
        } else if (s == "--sun" && i + 2 < argc) {
            a.sunElev = atof(argv[++i]); a.sunAzim = atof(argv[++i]);
            a.sunSet = true;
        }         else if (s == "--animtime" && i + 1 < argc) {
            a.animTime = atof(argv[++i]);
        } else if (s == "--tonemap" && i + 1 < argc) {
            a.tonemap = atoi(argv[++i]);
        } else if (s == "--mode" && i + 1 < argc) {
            a.mode = argv[++i];
        } else if (s == "--probe-surfel" && i + 3 < argc) {
            a.probeSurfel = { int(atof(argv[i + 1])), int(atof(argv[i + 2])),
                              int(atof(argv[i + 3])) };
            i += 3;
            a.probeSurfelSet = true;
        } else if (s == "--probe" && i + 3 < argc) {
            a.probe = { float(atof(argv[i + 1])), float(atof(argv[i + 2])),
                        float(atof(argv[i + 3])) };
            i += 3;
            a.probeSet = true;
        } else if ((s=="--llm-url" || s=="--ollama-url") && i+1<argc) a.llmUrl = argv[++i];
        else if ((s=="--llm-model" || s=="--ollama-model") && i+1<argc) a.llmModel = argv[++i];
    }
    if (const char* e = getenv("VF_LLM_URL")) a.llmUrl = e;
    if (const char* e = getenv("VF_LLM_MODEL")) a.llmModel = e;
    return a;
}

} // namespace app
} // namespace vf
