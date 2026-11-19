// Voxelforge entry point. Everything else lives in the per-functionality
// translation units under src/app/ - see app/app.hpp for the map.

#include "core/log.hpp"

#include "app/app.hpp"
#include "app/cli/args.hpp"

int main(int argc, char** argv)
{
    vf::initLogging();
    vf::app::Args args = vf::app::parseArgs(argc, argv);
    // The --scenery choice must land BEFORE App is constructed: its member
    // initialisers resolve the scenery paths (ai_edits layer, overlay).
    if (!args.scenery.empty())
        vf::voxel::scenery::setCliScenery(args.scenery);
    vf::app::App app;
    return app.run(args);
}
