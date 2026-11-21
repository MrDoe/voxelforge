---
title: OpenCode plugin discovery and relative import paths
tags: [opencode, plugin, config, tooling, gotcha]
sourceRefs: [.opencode/opencode.json, https://opencode.ai/docs/plugins/, https://opencode.ai/docs/troubleshooting]
lastReviewed: 2026-11-21
---

OpenCode discovers server plugins from the `plugins` array in `opencode.json(c)`
**and** from a `plugins/` directory under a config root or a project's
`.opencode/`. The directory form needs no config entry: a direct `.js`/`.ts`
file or an immediate package directory there is loaded for every session in
that Location — loading is per-Location, not at server start. The project's
`.opencode/opencode.json` names only LSP + MCP, so the `opencode-rag` plugin was
being discovered purely by the presence of `.opencode/plugins/opencode-rag/`.
The same plugin is also installed globally at
`~/.config/opencode/plugins/opencode-rag/`. See [[concepts/clang-lsp-setup]] for
the other thing configured in that same project file, and
[[concepts/ui-settings-manifest]] for the in-app panel that opens OpenCode
sessions.

## The failure (`ref=err_b7a04439`)

The matching entry in `~/.local/share/opencode/log/opencode.log` (2026-11-21
21:32:16.824Z, `role=server`) reads:

> failed to load plugin — target `/home/christoph/code/voxelforge/.opencode/plugins/opencode-rag`
> cause `Die(ResolveMessage: Cannot find module '../../node_modules/opencode-rag-plugin/dist/plugin-entry.js' imported from .../opencode-rag/index.js)`

The tracked shim was two lines:

```js
import plugin from "../../node_modules/opencode-rag-plugin/dist/plugin-entry.js";
export default plugin;
```

`tui.js` did the same for `dist/tui.js`. Relative to
`.opencode/plugins/opencode-rag/`, `../..` is `<project>/.opencode/`, so the
import demanded `<project>/.opencode/node_modules/opencode-rag-plugin/dist/`.
That install never existed here: `.opencode/package.json` declares only the V1
`@opencode-ai/plugin` 1.18.21, and although `.opencode/package-lock.json` still
listed a stale `opencode-rag-plugin` 1.21.0, `node_modules/` held no such
directory. There was no environment-variable or credential problem — module
resolution does not read either.

## Why the feature still worked

The global copy's `index.js` uses
`"../../../../.opencode/node_modules/opencode-rag-plugin/dist/plugin-entry.js"`.
Four `..` segments from `<home>/.config/opencode/plugins/opencode-rag/` reach
`<home>`, so it resolves into `~/.opencode/node_modules/opencode-rag-plugin`
v2.13.8, which is installed and loadable. Both copies are discovered in the
same run: the global one logs `loading plugin` (INFO), the project one logs
`failed to load plugin` (WARN), and its `tui.js` separately fails in the
`role=cli` reconcile. Semantic retrieval kept working throughout, so the WARN
was a redundant-duplicate defect, not a RAG outage.

## Rules

- A directory-discovered shim's specifiers resolve **relative to the shim's own
  directory**. `../../node_modules` means "two directories up", a different
  place in every copy of the shim — copying one between projects without
  recomputing the depth silently breaks it.
- One plugin, one install root. Running the same plugin from both a global
  config root and a project `.opencode/plugins/` duplicates event
  subscriptions and transforms. If the global copy already works, delete the
  project copy rather than installing a second one.
- `failed to load plugin` is logged per Location, so a healthy global copy can
  coexist with a failing project copy. Read the `target=` field before assuming
  the feature is down, and match the `ref=` to the exact log entry rather than
  guessing from the error code.

## Fix applied 2026-11-21

Removed the two tracked shim files with `git rm`
(`.opencode/plugins/opencode-rag/{index,tui}.js`), leaving the deletion staged
but uncommitted. The global copy is now the single provider. The underlying
data-loss hazard is the same one described in `AGENTS.md` for shared trees:
these files were swept into an unrelated commit (`45060b5`), which is why a
broken config survived unnoticed until the server logged it.
