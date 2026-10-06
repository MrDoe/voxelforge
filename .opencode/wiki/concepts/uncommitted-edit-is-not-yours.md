---
title: "A leading ` M` is not yours to restore"
tags: [git, attribution, assets, data-loss, world.json, hazard]
sourceRefs:
  - assets/world.json
  - AGENTS.md
lastReviewed: 2026-10-06
---

# A leading ` M` is not yours to restore

## The rule

> A leading ` M` (uncommitted modification) in `git status` means the file is
> **not yours to restore** — whatever you think you know about how it got
> there.

The overlay file is gitignored so its damage is invisible in `git status`
([[concepts/overlay-silent-write-trap]]). This page is the complementary
hazard: a **tracked** file shows as ` M`, and it is equally not yours to
overwrite, revert, `git checkout`, or `git stash`.

## What happened (2026-10-06)

`assets/world.json` was found already ` M` (md5 `dac9f059`) by the shading
session, which was about to inject a temporary `"lights"` block for a render
test. The shading session **did not author that edit** — it was pre-existing,
from a session before it (the file's keys are only `version` / `layers` /
`textures` — no `sun`, no `lights` — so the edit is inside the layer list or
the textures table).

It then ran `git checkout --` on the file to get a clean baseline, which
**discarded the unknown session's edit**. It recovered the file byte-exact from
a scratch copy and restored it, so the content is intact and still ` M`.

**The owner of that edit is unknown.** Do not attribute it to the shading
session — anyone who believes it owns the file may feel free to overwrite it.

## Why the attribution matters more than the incident

The content was recovered exactly, so the cost here was near zero. The
dangerous outcome is the *belief*: if a later session hears "the world.json
edit is George's", it may safely clobber it, and this time there may be no
scratch copy to recover from. Provenance you inferred from "I was here when it
appeared" is not ownership.

## Prevention

1. `git status` first, every session, before touching anything under
   `assets/`.
2. A file that is ` M` on arrival belongs to someone else. Assume so.
3. If you need a clean baseline, **copy it aside first** (`cp` to a named,
   clearly-owned path) and `git checkout` from *that*, never from memory.
4. `assets/` is versioned but its manifests are rewritten by tests and by
   in-app actions (see [[concepts/authored-assets-are-build-outputs]]), so
   ` M` there is common and usually benign — which is exactly why it must be
   treated as someone else's.