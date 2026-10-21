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
debug` = 0), so gdb stops at `free()` and cannot name the member. What the
disassembly does give: the destructor frees the pointer at `this + 0x2ae0`,
a `std::vector`'s `_M_start`, and that pointer is a **stack** address - so
the vector's header was overwritten (heap corruption), not merely dangling.
The next step is a build with debug info (or an ASan build) to find the
writer.

## A reproduction that touches neither the mouse nor the focus

The crash needs the *interactive* path (the headless `--shot` / `--smoke` /
`--selftest` paths all exit 0), and the interactive app grabs the cursor.
Run it on a virtual display with a window manager, and close the window
programmatically:

```bash
Xvfb :78 -screen 0 1600x900x24 &
DISPLAY=:78 metacity --replace &
DISPLAY=:78 ./build/voxelforge > /tmp/crash.log 2>&1 &
sleep 30                       # the world load is ~20 s
DISPLAY=:78 wmctrl -c "Voxelforge"   # the title is set in rhi/surface.cpp
wait $!; echo "exit=$?"        # 134 (SIGABRT) with "free(): invalid size"
```

`wmctrl -l` needs a window manager (metacity) on the display, otherwise it
reports "Cannot get client list properties". The core lands in the system
coredump store: `coredumpctl dump <pid> --output=/tmp/core`.

## A latent use-after-free found on the way

`LayeredWorld::reloadIfChanged()` called `parseAndComputeDirty()`, which
**clears and refills** the shared record arrays (`m_records`, `m_colTop`,
`m_objCells`, ...) that the async rebuild thread from `kick()` is *reading*
inside `buildInto()`. The `m_rebuildRunning` guard lived in `kick()` - i.e.
*after* the arrays had already been swapped under the running thread's feet.
`requestReload()` had the guard in the right place; `reloadIfChanged()` did
not. Fixed by moving the guard to the top of `reloadIfChanged()` (2026-10-21).

## See also

- [[concepts/headless-render-mouse-safety]] - how to run the app at all
