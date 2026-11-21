---
title: describe_image vision configuration
tags: [opencode-rag, vision, describe-image, config, tooling, gotcha]
sourceRefs: [.opencode/opencode-rag.json, /home/christoph/code/OpenCodeRAG/src/chunker/image.ts, /home/christoph/code/OpenCodeRAG/src/core/resolve-api-key.ts, /home/christoph/code/OpenCodeRAG/src/opencode/tools.ts, /home/christoph/code/OpenCodeRAG/src/plugin.ts]
lastReviewed: 2026-11-22
---

`describe_image` is an OpenCodeRAG plugin tool. Its vision backend is **not** the
chat model and is not chosen by OpenCode's model picker: it comes from the
workspace file `.opencode/opencode-rag.json`, section `imageDescription`.
That file is gitignored, so a model swap is workspace state, not a commit (see
[[concepts/opencode-plugin-relative-import]] for the separate plugin-load path).

## How the config resolves

- `imageDescription.provider` accepts `ollama`, `openai`, `anthropic`, `google`,
  `opencode` (Zen) and `opencode-go` (Zen Go). Zen providers use
  `https://opencode.ai/zen/v1` / `https://opencode.ai/zen/go/v1` unless the
  configured `baseUrl` already points at `opencode.ai`.
- An empty `apiKey` (`""`, `"public"` or `"PLACEHOLDER"`) is filled by
  `resolveApiKey()` from, in order, the provider env var, the workspace config,
  then OpenCode's `~/.local/share/opencode/auth.json`. Leave it empty so the
  key is not duplicated into a gitignored-but-shared file.
- `imageDescription.onDemand` is applied **verbatim** on top of the base config
  by `resolveOnDemandImageConfig()` for query-time calls — the CLI, MCP handler
  and plugin tool all use it. A stale `onDemand.baseUrl` or `onDemand.apiKey`
  left pointing at the previous provider overrides the new one, so set only
  `onDemand.model` when swapping backends.
- The tool's footer, `_Generated with <provider>/<model>_`, names the config
  actually in force — read it before believing a swap worked.

## 2026-11-22: switch to Hy4 flash Free

Configured `provider: "opencode-go"`, `model: "hy4-flash-free"`,
`baseUrl: "https://opencode.ai/zen/go/v1"`, `apiKey: ""` and
`onDemand.model: "hy4-flash-free"`. JSON validates, and the CLI
(`node /home/christoph/code/OpenCodeRAG/dist/cli/index.js describe-image <img> -c .opencode/opencode-rag.json`)
reports `Provider: opencode-go / Model: hy4-flash-free`.

**Credential state (same day):** the first call failed HTTP 401
`api_key_expired` (no key at all → `OpenCode Go requires an API key`). After a
key refresh it failed 403 `permission_error: This API key can't call models`
— a syntactically valid key with the wrong scope. After a second refresh the
CLI call **succeeded** and returned a sensible caption, so the config and
`hy4-flash-free` itself are fine. Keys live in the `opencode-go` entry of
`~/.local/share/opencode/auth.json` (or set `imageDescription.apiKey`
explicitly); the two error strings tell you which stage a failure is at.
`GET /models` on Zen Go returns 200 even when chat calls are refused, so it is
not a key-scope test.

## Two traps when changing this file

1. **Config is captured at plugin load.** `plugin.ts` registers the tool with
   `{ config: effectiveCfg }`, so editing `opencode-rag.json` does not affect a
   running session — verify with the CLI, which reads the file fresh. OpenCode
   V2 runs plugins in a shared background service that survives TUI restarts,
   so it takes `opencode service restart` (not just a new TUI) to rebuild the
   tool with the new config. A swap that "does nothing" in-session is usually
   this, not a bad model id.
2. **A free-sounding model id is provider-specific.** The literal
   `hy4-flash-free` exists only on Zen Go. OpenRouter carries
   `tencent/hy4-flash` (same family, image-capable, ~$0.000000035/token, no
   `:free` slug) and it captioned a test screenshot sensibly with the existing
   OpenRouter key — but it is a different id and was **not** substituted
   silently.

Related: [[concepts/ui-settings-manifest]] (the in-app AI panel, a separate
OpenCode session), [[concepts/decision-driven-navigation]] (the local decision
model, a different config section).
