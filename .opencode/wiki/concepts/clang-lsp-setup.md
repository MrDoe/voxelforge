---
title: clangd LSP for OpenCode (C++ language server)
tags: [tooling, lsp, clangd, editor, diagnostics]
sourceRefs: [.clangd, .opencode/opencode.json, CMakeLists.txt, docs/index.md]
lastReviewed: 2026-09-26
---

# clangd LSP for OpenCode

The C++ language server for this project. Read this before "the LSP is broken"
— and read [[concepts/measurement-discipline]] if you are about to conclude that
from a diagnostic count, because two of the three traps below produce
*misleadingly clean* output rather than an error.

## What is installed, and where

| Binary | Provided by | Note |
|---|---|---|
| `/usr/bin/clangd-18` | `clangd-18` (pre-existing) | the binary `opencode.json` actually invokes |
| `/usr/bin/clangd` | `clangd` metapackage | added 2026-09-26; symlink → `clangd-18`. Needed only by tools that default to the *unversioned* name |
| `/usr/bin/clang-tidy-18` | `clang-tidy-18` | **added 2026-09-26 — see the trap below** |
| `/usr/bin/clang-tidy` | `clang-tidy` metapackage | added 2026-09-26; symlink → `clang-tidy-18` |

All LLVM tools are 18.1.3, matching the system `clang` 18.1.3. The unversioned
metapackages cost ~16 kB each — they ship no code, only the `clangd` /
`clang-tidy` symlinks. `clangd` looks for `clang-tidy` as a **sibling binary**
in `/usr/lib/llvm-18/bin/`, which is where `clang-tidy-18` puts it.

## The four pieces of configuration

1. **`.opencode/opencode.json`** → `lsp.clangd` — the argv OpenCode spawns:
   `clangd-18 --background-index --clang-tidy --completion-style=detailed
   --header-insertion=never --compile-commands-dir=build`, with extensions
   `.c .cpp .cc .cxx .c++ .h .hpp .hh .hxx .h++`.
2. **`.clangd`** (git-tracked, project root) — `CompilationDatabase: build`,
   `-Wno-unknown-warning-option`, and the ClangTidy check list.
3. **`build/compile_commands.json`** — real file, produced by CMake
   (`CMAKE_EXPORT_COMPILE_COMMANDS ON`). ~108 entries, including the FetchContent
   `_deps` (glm, spdlog, glfw, vma, imgui, doctest).
4. **`compile_commands.json`** at the repo root — a **symlink** into `build/`.
   Both the root symlink and `compile_commands.json` are gitignored; `.clangd`
   is tracked. So a fresh clone has the config but must run `cmake` once before
   the LSP has a database.

OpenCode spawns LSP servers **lazily** — no `clangd` process exists until a C++
file is actually opened, so an absent process is not by itself a fault.

> **The `enabled LSP servers` log line is no longer valid evidence (checked
> 2026-10-01, opencode v2.0.21).** This page used to cite it as the positive
> signal that the config block was being read. That string — and `serverIds`
> and `all LSPs are disabled` — **do not occur anywhere in the v2.0.21 binary**
> (204 MB ELF; `bytes.find()` returns −1 for all three). Those 2026-09-20/21 log
> lines were written by an **older build**, so their absence in a current log
> says nothing either way, and their presence was never proof the server
> *spawned*. Use the process table instead: `ps -eo pid,etime,comm | grep clang`.
> Beware a self-matching `grep` — a `clangd` literal in your own command line
> matches, so match on `comm`, not the full command.

Config is merged global → project. `~/.config/opencode/opencode.jsonc` exists
and defines only `mcp` + `plugins`; it has **no `lsp` block**, so it cannot
shadow the project clangd. (The `voxelforge` MCP in the project config proves
the project file is being loaded.)

## Trap 1 — `--clang-tidy` was silently inert (the real find)

Until 2026-09-26 `clang-tidy` was **not installed at all**, so the `--clang-tidy`
flag and the whole `Diagnostics: ClangTidy` block in `.clangd` were dead
configuration. clangd accepts `--clang-tidy` **with no warning and no error** —
it just cannot find the sibling binary and runs no checks.

The tell: `clangd --check` reports diagnostics normally but *never* runs
ClangTidy. **`--check` does not exercise ClangTidy at all**, so it cannot
detect this. Measuring `clang-tidy` directly is also easy to get wrong twice
over — clang-tidy writes its findings to **stderr**, so
`clang-tidy … 2>/dev/null | grep warning:` reports a confident, wrong `0`.

## Trap 2 — the broad check globs buried real errors

With clang-tidy finally able to run, the configured
`Add: [modernize*, performance*, readability*]` produced, over just four files:

| file | diagnostics |
|---|---|
| `src/app/frame/run.cpp` | 1161 |
| `src/voxel/chunk_store.cpp` | 1039 |
| `src/app/ui/sidebar.cpp` | 111 |
| `src/core/camera.cpp` | 32 |

**Zero** were real compiler errors — the `clang` diagnostic source contributed
nothing. Every one was a `readability*` style rule that contradicts conventions
`AGENTS.md` states on purpose:

- `readability-identifier-length` (434) — short names are the convention: `lx/ly/lz`, `cp/sp`
- `readability-uppercase-literal-suffix` (406) — lowercase `f` throughout
- `readability-magic-numbers` (302) — numeric constants *are* the design (`VOXEL=0.1`, `0.62`, `1.26`, `±3`)
- `readability-braces-around-statements` (241) — braces deliberately omitted in hot paths
- `modernize-avoid-c-arrays` (23) — C arrays are load-bearing in Vulkan push blocks / GLM
- `readability-function-cognitive-complexity` + `-function-size` (11) — the frame loop is ~1400 statements by architecture

`.clangd` now carries a commented `Remove:` list for exactly these. The
`modernize*` checks that remain (`use-auto`, `concat-nested-namespaces`,
`use-anyofallof`) are few and actionable.

**Verified result** (real `clangd` LSP session, the exact `opencode.json` argv):

| file | before | after | what remains |
|---|---|---|---|
| `src/core/camera.cpp` | 32 | **0** | — |
| `src/voxel/chunk_store.cpp` | 1039 | **11** | `modernize-use-auto` (7), `readability-use-anyofallof` (2), `unused-includes` (2) |

A ~99 % reduction, and the 11 survivors are real and actionable. The two kept
checks still firing is what proves the config was **parsed and applied** rather
than rejected — a rejected `.clangd` also yields zero, so "quiet" alone proves
nothing. In clangd's `.clangd` the `Remove:` list takes **plain check names**
(no `-` prefix; clangd adds it). That only matters if you reproduce the
measurement by hand: on the `clang-tidy` command line the same suppression
*requires* the `-` prefix, so a spec assembled without it silently re-enables
everything and reports a confident, wrong "no change".

## Trap 3 — `clangd --check`'s error count is not a diagnostic count

`clangd --check=<file>` ends with `All checks completed, N errors`, but N counts
**log lines emitted at ERROR level**, and the `ExtractFunction` refactoring
probes log at ERROR every time they do not apply:

```
E[08:08:26.510]     tweak: ExtractFunction ==> FAIL: Cannot extract break/continue without corresponding loop/switch statement.
```

A perfectly clean file reported `19 errors` this way. Filter with
`grep "tweak:"` before believing the number.

## Verifying it actually works

`--check` is a poor end-to-end probe, and for three reasons it is easy to
conclude "quiet" when nothing ran:

- it never runs ClangTidy, so it cannot see a missing `clang-tidy` binary;
- its "N errors" is a log-line count, not a diagnostic count (Trap 3);
- an LSP client that waits for a **non-empty** `publishDiagnostics` never
  terminates once a config is correctly tuned, because clangd publishes `[]`.

A real LSP handshake with the exact argv from `opencode.json` (`initialize` →
`initialized` → `textDocument/didOpen` → `textDocument/documentSymbol`, reading
`Content-Length` framed messages) is worth the ~40 lines, and it is the only
probe that exercises `--clang-tidy` diagnostics. Two rules make it sound:

1. **Terminate on the `documentSymbol` response, not on a diagnostics publish.**
   clangd publishes an empty array early during the preamble/index phase, so
   "break on the first publish" reports `EMPTY` for a file with a real error.
2. **Always pair it with a positive control** — a scratch file containing
   `undefined_symbol_here()` — to prove the instrument can emit a finding before
   believing it found none. A tuned config reads as zero either way, so zero is
   only meaningful next to a control that is non-zero.

See [[concepts/measurement-discipline]] for the general form.

## Build-dir hazards (shared with the asset pipeline)

**`assets/` is registered as `heightmap_gen` build OUTPUTS, so never run
`ninja -t clean` in this repo** — it deletes the tracked, hand-authored scene
(`heightmap.png`, `world.json`, `landscape.vxw`). That hazard is documented
once, in full, by
[[concepts/authored-assets-are-build-outputs]]; do not restate it elsewhere.

The one fact specific to *this* page: a clean also removes
`build/compile_commands.json` — the clangd database, since the root
`compile_commands.json` is only a symlink into `build/`. So a single
"clean the build dir" reflex costs you the versioned scene **and** the language
server's flags. To force a recompile, `touch` a source file instead.

## Caveats

- **`build/compile_commands.json` only lists TUs CMake knows about.** The
  `voxelforge` source list is explicit (no `GLOB`), so a new `.cpp` is invisible
  to the build *and* to the LSP until it is listed. If a file clangd "cannot
  find flags for", it is almost always missing from the list, not misconfigured.
- A stale database is the usual cause of the "clangd reports *no member named X*
  / out-of-line definition does not match on code that is correct" experience.
  The database is regenerated by any build; **the compiler is the authority** —
  confirm with `ninja -C build` ([[concepts/focused-test-groups]]) rather than
  chasing the squiggles.
- The index cache goes to `~/.cache/clangd` (~7.5 MB after a full
  `--background-index` pass). It does **not** land in the repo, so it cannot
  dirty the working tree.
- **Parsing clean is not a behavioural gate.** Zero diagnostics is a syntactic
  and type-consistency result only; it cannot catch a wrong loop order or a
  changed render. Do not read it as "the refactor is semantically clean".
