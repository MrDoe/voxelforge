---
title: "Send failed: malformed OpenCode response" is a no-response, not a parse error
tags: [chat-ui, opencode, http, timeout, gotcha, regression-test]
sourceRefs: [src/ai/opencode_client.cpp, src/app/chat_ui.cpp, tests/test_app_opencode.cpp]
lastReviewed: 2026-11-24
---

# "malformed OpenCode response" — what it actually means

The string appears in exactly one place: `OpenCodeClient::request()` in
`src/ai/opencode_client.cpp`, when `readAll(fd)` returns bytes with no
`\r\n\r\n` header terminator. `ChatUi::sendPrompt()` prefixes it with
`Send failed: ` when `sendPrompt()` gets an empty body back. So the message
does **not** mean a JSON parse failed — it means the service sent **no HTTP
response at all** (or closed without one) before the client's receive timeout.

## Why "all chat messages" can fail while the service is healthy

The request path sets `SO_RCVTIMEO`/`SO_SNDTIMEO` to **1 second for every
method** (`timeval tv { 1, 0 }`). A `GET /api/info` health probe answers in
sub-milliseconds, but a `POST /api/session/<id>/prompt` has to enter the
service's session/event loop. Under load, a 1 s ceiling makes every send fail
with the misleading "malformed" text while the service is in fact reachable.

Measured 2026-11-24 on the live service (`http://127.0.0.1:49374`, Basic auth
from `~/.local/state/opencode/service.json`):

| probe | result |
|---|---|
| authenticated `GET /api/info` (curl) | 200 in 0.6 ms |
| `POST /api/session/ses_doesnotexist/prompt` (curl) | 404 in 8 ms |
| shipping client `connect()` / `interrupt()` | 3 ms / 7 ms, no error |
| shipping client `createSession()` + `sendPrompt("…")` | 10 ms + **68 ms**, no error |
| silent TCP server (accept, never reply), old error | `malformed OpenCode response`, ~1020 ms |

So the transport and the prompt route were healthy at diagnosis time; the
original failure was not reproducible on demand. The fix below addresses the
mechanism that makes a transient slowdown a *total* chat failure and makes the
next occurrence diagnosable instead of guessing.

## Fix (2026-11-24)

- **`request()` uses a method-dependent timeout**: 1 s for `GET` (startup
  probes, model/session lists — must never stall startup), **5 s for writes**
  (`POST`/`PUT`). Bounded, so a dead service still cannot hang a UI frame
  indefinitely.
- **The error now carries evidence**: method, path, byte count and the
  captured `errno` + `strerror` (`errno` is read *before* `close(fd)` overwrites
  it). A timeout reports `errno 11 - Resource temporarily unavailable`; a
  closed connection reports `errno 0 - Success`. The word "malformed" is kept
  as a prefix so existing log searches still match.
- **No retry on `POST`**: `/prompt` and `/interrupt` are not idempotent, so an
  automatic retry could submit a prompt twice. A timeout now surfaces as a
  precise error the user can act on (retry manually / check the service).

## Adjacent defect found while diagnosing

`.opencode/voxelforge_sessions.json` (gitignored app state) was invalid JSON:

```
"/…/LighthouseIsle::model"opencode-go/beam-free"opencode-go/hy4-flash-free"
```

Two processes each staging into the *same* `file + ".tmp"` path can rename one
another's half-written file. `saveSessionId()` / `saveModelChoice()` now use
`file + ".tmp." + getpid()`; `setSessionKey()` validates that the input is a
flat string→string object before patching an entry in place (a spliced entry
is reset to `{}` instead of extended), and the observed file was repaired from
a labelled backup in `/tmp/opencode/`. Pinned by
`tests/test_app_opencode.cpp` ("app session key writer resets a spliced entry
instead of extending it").

## Validation / gotchas

- `ninja -C build`, `ninja -C build test-app` and `test-unit` green.
- A scratch server that accepts and stays silent reproduces both branches:
  timeout (1 s GET, 0 bytes, errno 11) and immediate-close (0 bytes, errno 0).
- Do not "fix" this by pointing `VF_OPENCODE_URL` at a different port: the
  service registration file's `url` is the authority, and a wrong port fails
  with `cannot connect to …`, not this error.
- The SSE worker uses its own long-lived socket; `startEventStream()` is
  idempotent, and a live stream only proves the service was reachable when it
  connected, not that a later POST will answer inside the timeout.

See [[concepts/imgui-active-input-text]] for the composer-side clear bug that
can be confused with a failed send, and [[concepts/ui-settings-manifest]] for
the AI panel's session/model persistence.
