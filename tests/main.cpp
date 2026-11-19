#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include "voxel/heightmap.hpp"
#include "voxel/scenery.hpp"
#include <spdlog/spdlog.h>

#include <cstdlib>

int main(int argc, char** argv)
{
    // The test world is the SHIPPED default scenery, not the user's last
    // one: without this pin the pointer file would decide which world the
    // suite verifies, and a work-in-progress scenery would fail the content
    // tests for reasons that have nothing to do with the code under test.
    // VF_SCENERY set in the environment still wins (a gate may point the
    // suite at a specific folder on purpose).
    if (!getenv("VF_SCENERY"))
        setenv("VF_SCENERY", vf::voxel::scenery::kDefaultScenery, 1);
    if (!vf::voxel::sharedHeightmap().loaded()) {
        spdlog::critical("tests require baked assets - run 'ninja -C build world' first");
        return 1;
    }
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
