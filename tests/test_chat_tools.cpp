// Unit tests for the gemma/MCP tool bridge: name normalization, argument
// defaults, stamp-cell parsing, and the shared JSON extractors.
#include "ai/tools.hpp"
#include <doctest/doctest.h>

using namespace vf::ai;

TEST_CASE("tool names normalize onto canonical tools")
{
    CHECK(normalizeToolCall("create_box", "{}").name == "create_box");
    CHECK(normalizeToolCall("rock_1", "{}").name == "create_ellipsoid");
    CHECK(normalizeToolCall("add_rock", "{}").name == "create_ellipsoid");
    CHECK(normalizeToolCall("Place Box", "{}").name == "create_box");
    CHECK(normalizeToolCall("stone_pillar", "{}").name == "create_cylinder");
    CHECK(normalizeToolCall("list_world", "{}").name == "list_world");
    CHECK(normalizeToolCall("LIST_LAYERS", "{}").name == "list_world");
    CHECK(normalizeToolCall("probe", "{}").name == "probe");
}

TEST_CASE("aliased rock calls get boulder defaults and rock material")
{
    NormalizedCall n = normalizeToolCall("rock_1", "{\"name\":\"rock\"}");
    REQUIRE(n.ok);
    CHECK(n.name == "create_ellipsoid");
    // radius + material injected: flat boulder, palette 4
    CHECK(n.argsJson.find("\"radius\":[0.6,0.45,0.6]") != std::string::npos);
    CHECK(n.argsJson.find("\"material\":4") != std::string::npos);
}

TEST_CASE("explicit arguments are never overridden")
{
    NormalizedCall n = normalizeToolCall(
        "add_box", "{\"size\":[8,2,8],\"material\":6,\"anchor\":[10,20,30]}");
    REQUIRE(n.ok);
    CHECK(n.argsJson.find("\"size\":[8,2,8]") != std::string::npos);
    CHECK(n.argsJson.find("\"material\":6") != std::string::npos);
    CHECK(n.argsJson.find("\"anchor\":[10,20,30]") != std::string::npos);
}

TEST_CASE("unknown garbage reports the valid tool list")
{
    NormalizedCall n = normalizeToolCall("teleport_home", "{}");
    CHECK_FALSE(n.ok);
    CHECK(n.error.find("create_ellipsoid") != std::string::npos);
    CHECK(n.error.find("unknown tool") != std::string::npos);
}

TEST_CASE("stamp cells parse from objects array")
{
    std::vector<StampCellLite> cells;
    std::string args =
        R"({"name":"sign","cells":[{"dx":0,"dy":0,"dz":0,"mat":6},{"dx":1,"dy":0,"dz":0,"mat":4}]})";
    REQUIRE(parseStampCells(args, cells));
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].dx == 1);
    CHECK(cells[1].mat == 4);
}

TEST_CASE("json extractors tolerate whitespace and negatives")
{
    int i = 0;
    CHECK(jsonGetInt("{\"material\": -3 }", "material", i));
    CHECK(i == -3);
    float f = 0;
    CHECK(jsonGetFloat("{ \"radius\" : 0.75 }", "radius", f));
    CHECK(f == doctest::Approx(0.75f));
    std::vector<int> v;
    CHECK(jsonGetIntArray("{\"anchor\":[ 12 , -5 , 300 ]}", "anchor", v, 3));
    CHECK(v[0] == 12);
    CHECK(v[1] == -5);
    CHECK(v[2] == 300);
}

// --- response parsing across LLM backends (ollama_client.cpp) ---------------
#include "ai/ollama_client.hpp"

TEST_CASE("parse: ollama shape - object arguments")
{
    std::string body = R"({"model":"gemma4","message":{"role":"assistant",
        "content":"",
        "tool_calls":[{"function":{"name":"create_box",
        "arguments":{"size":[4,4,4],"material":6}}}]}})";
    std::vector<ToolCall> calls; std::string content, thinking;
    REQUIRE(parseToolCallsFromResponse(body, calls, content, thinking));
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "create_box");
    CHECK(calls[0].argumentsJson.find("\"material\":6") != std::string::npos);
}

TEST_CASE("parse: llama.cpp / OpenAI shape - escaped string arguments")
{
    std::string body =
        R"({"choices":[{"finish_reason":"tool_calls","index":0,"message":{)"
        R"("role":"assistant","content":null,)"
        R"("tool_calls":[{"id":"a0","type":"function",)"
        R"("function":{"name":"create_ellipsoid",)"
        R"("arguments":"{\"name\":\"rock\",\"radius\":[0.7,0.5,0.7],\"material\":4}"}}]}}]})";
    std::vector<ToolCall> calls; std::string content, thinking;
    REQUIRE(parseToolCallsFromResponse(body, calls, content, thinking));
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "create_ellipsoid");
    // unescaped into clean JSON that the argument extractors can read
    CHECK(content.empty());
    float rx = 0, ry = 0, rz = 0;
    std::vector<float> rad;
    REQUIRE(jsonGetFloatArray(calls[0].argumentsJson, "radius", rad, 3));
    CHECK(rad[0] == doctest::Approx(0.7f));
    CHECK(rad[2] == doctest::Approx(0.7f));
}

TEST_CASE("parse: content-embedded call (no native function calling)")
{
    std::string body = R"({"choices":[{"message":{"role":"assistant","content":
        "Sure, adding a rock now.\n{\"name\":\"create_ellipsoid\",\"arguments\":{\"radius\":[0.5,0.4,0.5],\"material\":4}}"
        }}]})";
    std::vector<ToolCall> calls; std::string content, thinking;
    REQUIRE(parseToolCallsFromResponse(body, calls, content, thinking));
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "create_ellipsoid");
    CHECK(calls[0].argumentsJson.find("\"material\":4") != std::string::npos);
    // the prose is still shown to the user
    CHECK(content.find("adding a rock") != std::string::npos);
}

TEST_CASE("parse: null content and no tool calls is safe")
{
    std::vector<ToolCall> calls; std::string content, thinking;
    std::string body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}]})";
    REQUIRE(parseToolCallsFromResponse(body, calls, content, thinking));
    CHECK(calls.empty());
    CHECK(content == "hi");
}

TEST_CASE("parse: whitespace after colons (json.dumps style) does not break tool_calls")
{
    std::string body =
        R"({"choices": [{"message": {"role": "assistant", "content": null, )"
        R"("tool_calls": [{"function": {"name": "create_box", )"
        R"("arguments": "{\"size\":[6,6,6],\"material\":4}"}}]}}]})";
    std::vector<ToolCall> calls; std::string content, thinking;
    REQUIRE(parseToolCallsFromResponse(body, calls, content, thinking));
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "create_box");
    std::vector<int> sz;
    CHECK(jsonGetIntArray(calls[0].argumentsJson, "size", sz, 3));
}
