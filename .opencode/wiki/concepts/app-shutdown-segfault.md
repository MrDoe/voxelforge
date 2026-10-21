---
title: The app's shutdown SIGSEGV in App::~App()
tags: [gotcha, crash, shutdown, overlay, debugging]
sourceRefs: [src/app/app.hpp, src/app/main.cpp, src/voxel/layered_world.cpp, src/voxel/chunk_store.cpp]
lastReviewed: 2026-10-21
---

# `App::~App()` frees a stack address (2026-10-21)

The interactive app sometimes dies on the way out with **SIGSEGV (exit
139)**, with no log line - it looks like a mid-session crash but it is not.

```
#0  arena_for_chunk (ptr=0x7ffcba8ba5f0)   ./malloc/arena.c:153
#1  arena_for_chunk (ptr=0x7ffcba8ba5f0)   ./malloc/arena.c:151
#2  __GI___libc_free (mem=0x7ffcba8ba600)   ./malloc/malloc.c:3397
#3  vf::app::App::~App()
#4  main
```

`info threads` shows only the main thread, so this is the teardown, not a
worker. Reproduced twice (coredumpctl PIDs 278653 and 302176).

## What it means

The freed pointer is in the **stack region**, and the `App` object itself is
a stack object (`vf::app::App app;` in `main.cpp`). So a member's owning
pointer was holding an address inside the stack frame. Two families:

- a container's internal pointer was **overwritten** (heap corruption), or
- a member was **moved/assigned from a stack buffer** and kept the old
  address.

The obvious use-after-free candidates are ruled out: `OverlayWriter`'s
destructor flushes and joins its writer thread, `LayeredWorld`'s destructor
joins its rebuild thread, and the declaration order is right
(`m_overlayWriter` at app.hpp:689 is destroyed *before* `m_layers` at
app.hpp:396, so the writer stops before the store dies).

## Consequences to check before blaming anything else

- The overlay file is **not** damaged by the crash (md5 unchanged after
  both occurrences), but the crash can happen *before*
  `OverlayWriter::flush()` persists the last stroke, so a user who closes
  the window right after a stroke can lose it.
- A SIGSEGV in a **background** run is not evidence that the thing under
  test crashed. Read the backtrace before assuming the render broke.

## How to investigate further

The shipped binary has **no DWARF** (`readelf -S build/voxelforge | grep -c
debug` = 0), so gdb stops at `free()` and cannot name the member. The next
step is a build with debug info (or a dump of the members' owning pointers
before destruction) to find which member's pointer points into the stack.

## See also

- [[concepts/headless-render-mouse-safety]] - how to run the app at all
