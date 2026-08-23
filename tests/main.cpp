#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include "voxel/heightmap.hpp"
#include <spdlog/spdlog.h>

int main(int argc, char** argv)
{
    if (!vf::voxel::sharedHeightmap().loaded()) {
        spdlog::critical("tests require assets/heightmap.png (run heightmap_gen)");
        return 1;
    }
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
