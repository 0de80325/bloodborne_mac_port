# macOS Port Backlog (refinements)

Work deliberately deferred while getting the game running. When a shortcut is taken, the
proper solution goes here under one of three categories, in priority order:

1. **Stability** — correctness and robustness gaps left by shortcuts.
2. **Performance** — speed and efficiency work.
3. **Mac GUI** — making it look and feel like a polished Mac app.

Each item: what was done for now, what the refinement is, and where it lives.

## 1. Stability

- **Shader-side buffer bounds checks (MoltenVK has no `robustBufferAccess2`).**
  *Now:* on macOS the device is created without `robustBufferAccess2`/`nullDescriptor`
  (`vk_instance.cpp`), and null buffer bindings use a small zero-filled dummy buffer
  (`vk_rasterizer.cpp`). Out-of-bounds buffer reads are then only guaranteed not to crash, not
  to return 0 as on the PS4 GPU (reads past `num_records` return 0) — possible visual glitches.
  Shader writes through a null binding also land in the stand-in buffer instead of being
  discarded.
  *Refinement:* emit bounds checks in the SPIR-V backend (`shader_recompiler/backend/spirv`) for
  buffer loads/stores/atomics on macOS — targeted first (vertex fetch, storage buffers) where
  glitches trace back to OOB reads, everywhere if a native Metal backend is built.
- **Buffer-cache arena window.** *Now:* one fully backed 16 GiB arena at 0x7000000000
  (`BufferCache::MacArenaBase/MacArenaPages`); a GPU buffer outside it stops with "outside the
  macOS guest arena". *Refinement:* per-page arenas with copy-based migration (rewriting the
  BDA page table), or zero-copy import (Performance) which removes arenas altogether.
- **Host pointers above 1 TiB reach the guest.** `__error` returns the host thread's errno
  address (0x7fc6…), above the game's 40-bit pointer range. Harmless if used immediately; if
  the game ever stores it packed, route errno through a per-thread low (<1 TiB) slot.
- **Watchdog timeout on macOS ends in a guest fault.** When `BB_TIMEOUT` expires, the SIGUSR2
  stack dump knocks a game worker thread into a SIGBUS (guest 0x53b3260, job-pool loop at
  0x263b246) instead of a clean exit. Only test runs with a timeout are affected; normal play
  has none. *Refinement:* find why the dump signal derails the interrupted guest thread
  (signal delivery under Rosetta with the guest stack/TLS) and exit cleanly.
- **Timed mutex/rwlock waits** (`runtime_mutex.c`, `runtime_rwlock.c`) are trylock + backoff:
  up to ~1 ms late, not fair; error-checking mutexes return `ETIMEDOUT` instead of `EDEADLK`
  on self-relock. *Refinement:* condvar-based timed locks.
- **Rosetta stale translations on aliased memory:** code written through one guest alias and
  executed from another runs the old translation until a protection change on the executing
  alias. *Refinement:* toggle protection (RW→RX) on executable aliases after host writes, if
  the game ever runs code from direct/flexible memory.

## 2. Performance

- **Render-pass breaks on a tile-based GPU (biggest lever, 2026-10-11).** Metal System Trace
  (`xcrun xctrace record --template "Metal System Trace" --attach <pid> --time-limit 3s`) in a
  ~23 FPS scene: GPU busy 84%, fragment 69%; **~287 Metal render passes per frame, ~32 ms
  fragment time, ~0.11 ms each** (load/store of 1080p targets per pass) for ~1,600 draws.
  Desktop GPUs barely pay for pass breaks; Apple TBDR does. *Refinement:* log why rendering
  ends between draws (barriers, layout changes, copies/clears, compute), merge passes with the
  same attachments, use DONT_CARE load/store where contents are dead.
- Other measured costs: MoltenVK encodes Metal commands inside `vkQueueSubmit` (~47% of the
  recording thread; `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` moves it off, small gain while
  GPU-bound); draw-prep pool lock contention + background QoS on E-cores
  (`BB_PREP_WORKERS=0`: 21 → 23 FPS); `cthread_yield` spin in `Scheduler::WaitHostCopies`.

- **Zero-copy guest memory for the buffer cache (top item; plan stage 2).**
  *Now:* MoltenVK has no sparse binding, so on macOS each buffer-cache arena is an ordinary,
  fully backed 2 GiB buffer (`buffer_cache.cpp`) instead of a sparse 4 GiB one: guest data is
  still copied into GPU buffers, and every touched 2 GiB of GPU address space costs 2 GiB.
  *Refinement:* import guest memory directly with `VK_EXT_external_memory_host` (4 KiB
  alignment on M4 Max, unified 48 GiB heap) so the GPU reads/writes guest memory in place —
  removes uploads, read-backs and most write tracking. Mind CPU/GPU coherence and the Rosetta
  aliased-code caveat (Stability).

- **PGO on macOS:** profile-guided optimization is off (the profiles are GCC `.gcda`).
  *Refinement:* clang `-fprofile-generate`/`-fprofile-use` with `.profdata`.

## 3. Mac GUI

- **Native Mac settings.** *Now:* the in-game ImGui overlay (`gpu/shim/bbport_overlay.cpp`), in
  English, opens with ⌘, or L3+R3 and shows FPS, render → output resolution and upscaler.
  *Refinement:* a native Mac settings window in the launcher; if SDL's default app menu
  swallows ⌘, (its "Preferences…" item), wire that menu item to the overlay instead.
- **Launcher and `.app` bundle** (planned in `MACOS_PORT.md`).
