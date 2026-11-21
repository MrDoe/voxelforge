// Unit tests for the vf_mcp v2 tool-name bridge. Native names must pass
// through the fuzzy normalizer untouched, and invented aliases must land on
// the new verbs rather than a destructive neighbour.
#include "ai/tools.hpp"
#include <doctest/doctest.h>

using namespace vf::ai;

TEST_CASE("v2 tools are accepted as native names and never redirected")
{
    CHECK(normalizeToolCall("describe", "{}").name == "describe");
    CHECK(normalizeToolCall("paint", "{}").name == "paint");
    CHECK(normalizeToolCall("snapshot", "{}").name == "snapshot");
    CHECK(normalizeToolCall("restore_snapshot", "{}").name == "restore_snapshot");
}

TEST_CASE("invented v2 aliases land on the right verb, not on delete_object")
{
    CHECK(normalizeToolCall("describe this cell", "{}").name == "describe");
    CHECK(normalizeToolCall("paint roof", "{}").name == "paint");
    CHECK(normalizeToolCall("recolor the barn", "{}").name == "paint");
    CHECK(normalizeToolCall("backup the layer", "{}").name == "snapshot");
    CHECK(normalizeToolCall("restore snapshot", "{}").name == "restore_snapshot");
    // The old surgical-vs-whole-layer split must survive the new rules.
    CHECK(normalizeToolCall("remove voxels", "{}").name == "delete_voxels");
    CHECK(normalizeToolCall("delete layer", "{}").name == "delete_object");
}

TEST_CASE("builder aliases survive the fuzzy bridge")
{
    CHECK(normalizeToolCall("add_path", "{}").name == "add_path");
    CHECK(normalizeToolCall("draw a path", "{}").name == "add_path");
    CHECK(normalizeToolCall("add_tree", "{}").name == "add_tree");
    CHECK(normalizeToolCall("plant a tree", "{}").name == "add_tree");
    // Deletion must still win over the noun rule.
    CHECK(normalizeToolCall("remove tree", "{}").name == "delete_object");
    CHECK(normalizeToolCall("place_copy", "{}").name == "place_copy");
    CHECK(normalizeToolCall("copy this layer here", "{}").name == "place_copy");
    // A plain mesh import must still be import_mesh.
    CHECK(normalizeToolCall("import mesh", "{}").name == "import_mesh");
    CHECK(normalizeToolCall("transform", "{}").name == "transform");
    CHECK(normalizeToolCall("reposition the barn", "{}").name == "transform");
}
