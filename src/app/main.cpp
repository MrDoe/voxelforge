// Voxelforge entry point. Everything else lives in the per-functionality
// translation units under src/app/ - see app/app.hpp for the map.

#include "core/log.hpp"

#include "app/app.hpp"
#include "app/cli/args.hpp"

int main(int argc, char** argv)
{
    vf::initLogging();
    vf::app::Args args = vf::app::parseArgs(argc, argv);
    vf::app::App app;
    return app.run(args);
}
