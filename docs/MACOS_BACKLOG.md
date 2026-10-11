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
- **Timed mutex/rwlock waits** (`runtime_mutex.c`, `runtime_rwlock.c`) are trylock + backoff:
  up to ~1 ms late, not fair; error-checking mutexes return `ETIMEDOUT` instead of `EDEADLK`
  on self-relock. *Refinement:* condvar-based timed locks.
- **Rosetta stale translations on aliased memory:** code written through one guest alias and
  executed from another runs the old translation until a protection change on the executing
  alias. *Refinement:* toggle protection (RW→RX) on executable aliases after host writes, if
  the game ever runs code from direct/flexible memory.

## 2. Performance

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

- (none yet — launcher and `.app` bundle are planned in `MACOS_PORT.md`)
