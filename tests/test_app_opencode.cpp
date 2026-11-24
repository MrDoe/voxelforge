// Unit tests for the OpenCode HTTP/SSE client + AI panel state helpers.
// The mock-service case is opt-in via VF_OPENCODE_URL (tests/mock_opencode.py).
#include "ai/opencode_client.hpp"
#include "ai/tools.hpp"
#include "app/chat_ui.hpp"
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

using namespace vf::ai;

TEST_CASE("opencode client decodes HTTP chunked streams")
{
    // "hi" + "there" in two chunks, with a trail of headers already removed.
    const std::string wire =
        "2\r\nhi\r\n"
        "5\r\nthere\r\n"
        "0\r\n\r\n";
    CHECK(OpenCodeClient::decodeChunked(wire) == "hithere");

    // A partial chunk may arrive with the SSE connection still open: the
    // decoder returns only complete payload bytes.
    const std::string partial = "4\r\nab";
    CHECK(OpenCodeClient::decodeChunked(partial).empty());
    CHECK(OpenCodeClient::decodeChunked("4\r\nabcd\r\n") == "abcd");
}

TEST_CASE("opencode client extracts event types")
{
    CHECK(OpenCodeClient::parseEventType(
              R"({"type":"session.text.delta","data":{"delta":"x"}})") ==
          "session.text.delta");
    CHECK(OpenCodeClient::parseEventType(R"({"data":{}})").empty());
    CHECK(OpenCodeClient::parseEventType(
              R"({"type":"session.tool.called"})") == "session.tool.called");
}

TEST_CASE("opencode client constructs and can probe the local service")
{
    OpenCodeClient client([](const OpenCodeEvent&) {});
    std::string err;
    // May be offline in CI; the contract is "returns false with an error",
    // never a crash.
    bool ok = client.connect(&err);
    if (!ok) CHECK_FALSE(err.empty());
}

TEST_CASE("agent screenshot path is parsed from a render_shot result")
{
    CHECK(extractImagePath("rendered workspace/screenshots/shot_123.ppm (172815 bytes)") ==
          "workspace/screenshots/shot_123.ppm");
    CHECK(extractImagePath("shot: \"/tmp/opencode/x.png\" (12 kB)") ==
          "/tmp/opencode/x.png");
    CHECK(extractImagePath("no image here").empty());
}

TEST_CASE("opencode client guards model/session operations without a session")
{
    OpenCodeClient client([](const OpenCodeEvent&) {});
    std::string err;
    // No session attached: these must fail cleanly, not crash.
    CHECK_FALSE(client.setSessionModel("beam-free", "opencode-go", &err));
    CHECK_FALSE(client.interrupt(&err));
}

TEST_CASE("opencode client against mock service")
{
    // Run with: python3 tests/mock_opencode.py 45999 &
    //           VF_OPENCODE_URL=http://127.0.0.1:45999 vf_tests --test-case=*mock*
    const char* url = std::getenv("VF_OPENCODE_URL");
    if (!url || !*url) return; // opt-in: no network in the default gate
    int events = 0;
    OpenCodeClient client([&](const OpenCodeEvent&) { ++events; });
    std::string err;
    REQUIRE(client.connect(&err));
    REQUIRE(client.ensureMcpConnected("/tmp/mock-project", &err));
    auto models = client.listModels("/tmp/mock-project", &err);
    REQUIRE(models.size() == 2);
    CHECK(models[0].id == "beam-free");
    CHECK(models[0].name == "Beam Free");
    REQUIRE(client.createSession("/tmp/mock-project", "voxelforge",
                                 "beam-free", "opencode-go", &err));
    CHECK(client.sessionId() == "ses_mock00000000000000001");
    CHECK(client.setEnvironment({ { "VF_SCENERY", "mock" } }, &err));
    CHECK(client.setSessionModel("beam-free", "opencode-go", &err));
    CHECK(client.sendPrompt("hello", &err));
    REQUIRE(client.startEventStream(&err));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(events >= 1);
    client.stop();
}

TEST_CASE("app session JSON normalizer fixes trailing-newline append bug")
{
    // First write ends with "}\n"; the old code popped the newline, then
    // appended a second object and produced invalid JSON ("Extra data").
    CHECK(vf::ai::ChatUi::normalizeSessionJson("{\n  \"a\": \"b\"\n}\n") ==
          "{\n  \"a\": \"b\"\n");
    CHECK(vf::ai::ChatUi::normalizeSessionJson(
              "{\n\"a\":\"b\"\n}\n,\n\"c\":\"d\"\n}") == "{");
}

TEST_CASE("app prompt carries the selected voxel to the agent")
{
    std::string p = vf::ai::ChatUi::promptWithSelection("put a crate here",
                                                        { 512, 508, 512 }, true);
    CHECK(p.find("put a crate here") == 0);
    CHECK(p.find("512,508,512") != std::string::npos);
    CHECK(p.find("world position") != std::string::npos);
    CHECK(p.find("bottom-center anchor") != std::string::npos);
    std::string q = vf::ai::ChatUi::promptWithSelection("hello", {}, false);
    CHECK(q.find("no voxel is selected") != std::string::npos);
}

TEST_CASE("app session key writer appends and replaces without breaking JSON")
{
    std::string text = vf::ai::ChatUi::setSessionKey("", "/tmp/sc", "ses_one");
    CHECK(text.find("\"/tmp/sc\": \"ses_one\"") != std::string::npos);
    text = vf::ai::ChatUi::setSessionKey(text, "/tmp/sc", "ses_two");
    CHECK(text.find("ses_one") == std::string::npos);
    CHECK(text.find("\"/tmp/sc\": \"ses_two\"") != std::string::npos);
    text = vf::ai::ChatUi::setSessionKey(text, "/tmp/sc::model", "opencode/beam-free");
    CHECK(text.find("\"opencode/beam-free\"") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '{') == 1);
    CHECK(std::count(text.begin(), text.end(), '}') == 1);
}

TEST_CASE("app session key writer resets a spliced entry instead of extending it")
{
    // Observed in .opencode/voxelforge_sessions.json: two concurrent writers
    // left one line with a key and two concatenated values. Patching that in
    // place would keep the garbage; the writer must start from a clean object.
    const std::string corrupt =
        "{\n  \"/tmp/sc::model\"opencode-go/beam-free\"opencode-go/hy4-flash-free\"\n}\n";
    const std::string text =
        vf::ai::ChatUi::setSessionKey(corrupt, "/tmp/sc::model", "opencode-go/beam-free");
    CHECK(text.find("hy4-flash-free") == std::string::npos);
    CHECK(text.find("\"/tmp/sc::model\": \"opencode-go/beam-free\"") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '{') == 1);
    CHECK(std::count(text.begin(), text.end(), '}') == 1);
}

TEST_CASE("chat transcript decodes JSON newlines from SSE deltas")
{
    using vf::ai::ChatUi;
    // A delta's text arrives JSON-encoded ("\n" = two bytes), not as a
    // literal LF; without this step the wrapped transcript shows "\n".
    CHECK(ChatUi::unescapeJsonText("a\\nb") == "a\nb");
    CHECK(ChatUi::unescapeJsonText("a\\r\\nb") == "a\nb"); // CRLF -> LF
    CHECK(ChatUi::unescapeJsonText("line1\\nline2\\n") == "line1\nline2\n");
    CHECK(ChatUi::unescapeJsonText("quote: \\\"x\\\" slash: \\\\ end") ==
          "quote: \"x\" slash: \\ end");
    CHECK(ChatUi::unescapeJsonText("\\u00e4") == "\xc3\xa4"); // ä -> UTF-8
    CHECK(ChatUi::unescapeJsonText("already\nhere") == "already\nhere");
    CHECK(ChatUi::unescapeJsonText("\\uD83D\\uDE00").empty() == false); // no invalid UTF-8
}

TEST_CASE("opencode client builds HTTP Basic credentials")
{
    CHECK(OpenCodeClient::base64("opencode:secret") == "b3BlbmNvZGU6c2VjcmV0");
    CHECK(OpenCodeClient::base64("") .empty());
}

TEST_CASE("composer buffer clear reaches ImGui's active input state")
{
    // ImGui keeps the live text in its own state while a multiline InputText
    // is active, so clearing the caller's buffer alone is re-applied from
    // state->TextA on the next frame and the field keeps showing the old text.
    // ChatUi::clearInputBuffer must force the reload path.
    struct ContextGuard {
        ContextGuard() { ImGui::CreateContext(); }
        ~ContextGuard() { ImGui::DestroyContext(); }
    } guard;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int atlasW = 0, atlasH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasW, &atlasH);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);

    char buf[64] = {};
    ImGuiID id = 0;
    auto frame = [&](const char* type, bool focus) {
        for (const char* p = type; *p; ++p)
            io.AddInputCharacter((unsigned int)(unsigned char)*p);
        ImGui::NewFrame();
        ImGui::Begin("composer-test");
        id = ImGui::GetID("##input");
        if (focus)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##input", buf, sizeof(buf),
                                  ImVec2(200.0f, 40.0f));
        ImGui::End();
        ImGui::Render();
    };

    frame("", true);   // focus the field
    frame("", false);  // it becomes active
    frame("hello", false);
    CHECK(std::string(buf) == "hello");
    CHECK(ImGui::GetInputTextState(id) != nullptr);

    vf::ai::ChatUi::clearInputBuffer(buf, sizeof(buf), id);
    CHECK(buf[0] == '\0');

    frame("", false); // next frame: ImGui reloads from the caller's buffer
    CHECK(buf[0] == '\0');
    ImGuiInputTextState* st = ImGui::GetInputTextState(id);
    const bool stateCleared = (st == nullptr) || st->TextLen == 0;
    CHECK(stateCleared);
}
