---
title: ImGui owns the text of an active field — clearing a caller buffer is not enough
tags: [imgui, ui, input-text, chat-ui, composer, gotcha, regression-test]
sourceRefs: [src/app/chat_ui.cpp, src/app/chat_ui.hpp, tests/test_app_opencode.cpp, CMakeLists.txt]
lastReviewed: 2026-11-22
---

# ImGui owns the text of an active field

**Clearing the caller's buffer of an active `InputText`/`InputTextMultiline`
does nothing.** The widget keeps the live text in its own state
(`ImGuiInputTextState::TextA`), renders *that*, and writes it back into the
caller's buffer. Any code that "empties the field" by writing `buf[0] = '\0'`
after the widget call is silently undone on the next frame.

This is the whole explanation of the AI composer's asymmetry: pressing **Enter**
kept the text on screen, while clicking **Send** appeared to clear it. Enter
leaves the field *active*, so ImGui keeps rendering its own copy. Send
deactivates the field, so the frame after the click displays the (empty) caller
buffer — but the deactivation path carries a second hazard (below), so Send was
never actually safe either, just luckier.

## The two mechanisms

1. **Active state wins.** In the vendored ImGui 1.91.8 `InputTextEx`, while
   `g.ActiveId == id` the working buffer is `state->TextA`; the caller's `buf`
   is not the source of truth. Measured 2026-11-22: after a bare
   `buf[0] = '\0'` the *next* frame reads `buf[0] == 'h'` (104) again and
   `state->TextLen == 6`. Both are rewritten, so neither can be trusted as an
   "is it clear?" probe right after a bare clear.
2. **Deactivation snapshots the old text.** `InputTextDeactivateHook` copies
   the state into `g.InputTextDeactivatedState`. A later frame re-applies that
   snapshot into `buf` when
   `g.InputTextDeactivatedState.ID == id && g.ActiveId != id && IsItemDeactivatedAfterEdit()`
   and the snapshot differs from `buf`. Clicking **Send** is exactly that case:
   the button click deactivates the field and the snapshot is one frame behind
   the clear.

## The fix

`ChatUi::clearInputBuffer(char* buf, size_t bufSize, ImGuiID id)`
(`src/app/chat_ui.cpp`, declared public in `chat_ui.hpp` for the test):

- zero `buf`, then
- `ImGui::GetInputTextState(id)->ReloadUserBufAndMoveToEnd()` (needs
  `imgui_internal.h`), so the next frame reloads from the now-empty buffer, and
- clear `GetCurrentContext()->InputTextDeactivatedState.ID` when it names the
  widget, so the deactivation snapshot cannot come back.

Both submit paths — Enter and the Send button — go through this one function.
Enter is detected *after* the widget call and the newline it inserted is
stripped before sending, so the clear happens in the same frame the text is
captured.

## The id, not the item

The widget id must be taken **before** the widget:
`const ImGuiID inputId = ImGui::GetID("##aiInput");`. Multiline `InputText`
wraps itself in `BeginGroup()`/`EndGroup()`, which leaves `LastItemData.ID` at
the group's `0` unless the field happens to be active — so `GetItemID()` is the
wrong instrument here, and the failure mode is "clears while typing, not while
idle", i.e. it looks like a timing bug rather than an id bug.

## Gate

`tests/test_app_opencode.cpp`, case **"composer buffer clear reaches ImGui's
active input state"**: builds a headless `ImGui::CreateContext()`, feeds
characters, focuses the field, and runs real frames through
`InputTextMultiline`. It asserts the buffer holds the typed text, then calls
`ChatUi::clearInputBuffer`, then asserts on the *following* frame that
`buf[0] == '\0'` **and** that the input state's `TextLen == 0`. Both halves
matter: the buffer alone can be right while the state is still wrong.

It runs in the `unit_app_tests` entry (`vf_tests --source-file=*test_app_*`,
label `app`), i.e. `test-app`, and inside `test-unit`.

**Negative control (measured):** temporarily restoring the pre-fix body
(`buf[0] = '\0'` only) makes the case fail **2 of 5** assertions —
`CHECK( buf[0] == '\0' )` reads `104` and `stateCleared` is false. A regression
test whose failure mode was never demonstrated is decoration; this one was
shown to fail before it was trusted.

Note the doctest constraint met while writing it: `CHECK(a || b)` does not
compile ("Expression Too Complex Please Rewrite As Binary Comparison"), so the
disjunction is folded into a named `bool` first.

## Related

- [[concepts/textbox-input-gate]] — the *other* consequence of an active field:
  while one owns the keyboard, the camera and hotkeys are inert. Same widget
  lifecycle, different rule.
- [[entities/hud-sidebar]] — the AI pane is a section of the sidebar, and the
  composer has no window of its own.
- [[concepts/opencode-request-timeout]] — the other way a submit can appear to
  fail: `Send failed: malformed OpenCode response` is a no-response/timeout in
  the HTTP client, not a cleared field.
