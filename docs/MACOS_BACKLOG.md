# macOS Port Backlog (refinements)

Work deliberately deferred while getting the game running. When a shortcut is taken, the
proper solution goes here under one of three categories, in priority order:

1. **Stability** — correctness and robustness gaps left by shortcuts.
2. **Performance** — speed and efficiency work.
3. **Mac GUI** — making it look and feel like a polished Mac app.

Each item: what was done for now, what the refinement is, and where it lives.

## 1. Stability

- **PARKED (2026-10-13): the game window sometimes closes by itself in split mode** (grey screen,
  then the host exits with status 0 and no error). Seen 3 times: after turning subpixel jitter on
  in the menu (user), after the same via `BB_TOGGLE_FILE` (bit 25), and during a profiling run with
  an Instruments attach; NOT reproduced in 3 later runs (toggle, screenshot, plain play), so
  intermittent. Instrumented: the window loop now prints which SDL event closed it ("Window: …"),
  the host prints when its run loop returns, and SDL no longer turns SIGINT/SIGTERM into a quit
  event in the host (a signal now shows as a signal). Next occurrence: read those lines.
- **Shader/pipeline compile stutter.** *Fixed 2026-10-13 (main cause):* the pipeline cache was
  wiped at every launch (one entry that failed to preload cleared everything), so ~500 pipelines
  compiled again each session, many mid-game (attacks, effects). Now only old-format entries clear
  it; the next session preloaded 226 of 227 and compiled 2 during play (was 517). Both processes
  use `<data>/user` (`run.sh` exports `BB_GPU_USER_DIR`; the host used to fall back to
  `~/Library/Application Support/shadPS4`). *Remaining:* a pipeline never seen before still
  compiles on first sight and blocks the frame — compile in the background with a fallback.

- **"Unimplemented PM4 type 0" seen again in split mode (2026-10-13).** Once in ~8 runs, while
  loading into the level (`liverpool.cpp:820`, base reg 530), despite the command-buffer copy
  arena. Find which buffer is still read from guest memory after the submit (CCB? indirect
  buffers / `IndirectBuffer` packets pointing into game memory are not copied).
  *2026-10-13 evening, 2 more (of ~10 runs, right after "Continue" on the title):* the copied DCB
  equals the game's buffer at the fault (0 words differ), so the game submitted those bytes; not
  a late rewrite. Both times stray words sit in a compute-shader setup sequence (`SET_SH_REG`
  0x212 / 0x216–0x217 nearby); in one, `70d1b005 00000000` is a COMPUTE_PGM_LO/HI value
  (program at 0x70d1b00500) with its packet header missing. Suspect a packet writer that emits a
  different size than the game reserves (our HLE `sceGnmSetCsShader*`/dispatch functions, or a
  patch). The diagnostic's guest-buffer comparison crashed the host before (a declaration inside
  `namespace AmdGpu` resolved to a null call through `-undefined dynamic_lookup`): fixed.
  *2026-10-14, hunted:* not reproduced in **62 cold-cache save loads** since (42 with a ring of
  every `libSceGnmDriver` call, 20 without), so the recorder's timing does not hide it. Left in
  place, cheap: each DCB/CCB is walked at submit in the game process ("Gnm check: bad packet …",
  with the library calls that wrote near the bad word; `BB_GNM_TRACE=0` turns the call ring
  off), and the host records each arena copy's sequence number and hash on arrival, so the next
  "Bad PM4 packet" says whether its copy changed after it arrived (arena reused mid-parse) or
  was already bad at submit. Next occurrence: read those two lines first.
  *2026-10-14, caught at submit (load into the level from the title):* the game submitted a
  506-word DCB with only 58 words written; the rest were zeros (still zeros 50 ms later, so no
  writer was racing the submit). The last packets were a correct 25-word
  `sceGnmSetCsShader` (from another thread than the submit); the arena copy was unchanged.
  **Now:** the CP skips type-0 packets as register writes, as the hardware does (a zero tail is
  one write to register 0 per two words), and logs once per buffer ("PM4 type-0 packet …
  skipped"); tested with an injected 448-word zero tail. *Remaining:* why the game leaves the
  tail unwritten (the "Gnm check" now also prints where the writing thread's next library calls
  went: another buffer = a chunk switch the size does not reflect).
- **Fixed 2026-10-14: `MapNumberConversion` UNREACHABLE (data_fmt 0, SnormNz) killed the GPU
  host during a level load** (1 in ~60 loads). A descriptor with that format reads as zeros on
  the GPU; it now logs once ("SnormNz descriptor with data format …") and converts nothing.
  *Remaining:* find which descriptor carries it (stale table read, or a legitimately unused
  slot).
- **Gameplay judder with no compiles (2026-10-13).** The player sees occasional judder at a
  steady 60/61 FPS. Automated runs standing in the city show a worst frame of 20–25 ms in every
  5 s window (median 16.67, p99 17–22 ms) with zero shader compiles; the title screen has none.
  Find what those frames wait on (present timing, a periodic CPU job, host-copy waits).
  *2026-10-14, frame spike log* (`BB_FRAME_STATS=1`, "Spike #…" lines, FINDINGS): in 2 min of
  walking (user settings, 1080p MetalFX Native AA) **9 frames reached the display a vsync late**
  (33.3 ms) and **30 game frames took 21–46 ms** while the display stayed at 16.7 ms. With the
  delta-time patch a long game frame moves the world further in one step, so motion judders
  at a steady 60. Two causes seen: GPU hiccups (one GPU frame ~50 ms: frames-ahead wait 34–36 ms,
  then the game waits too) and runs of 4–5 frames where the recording thread is busy 20–25 ms
  (normal ~15) with the same draw count. *Next:* what the GPU does in the 50 ms frames (Metal
  System Trace at the spike), and why recording slows (MoltenVK encoding, CPU contention).
- **Display latency 34–76 ms (mean 51) from `vkQueuePresent` to the screen** (2026-10-14,
  VK_GOOGLE_display_timing). Two to four vsyncs of queued drawables: input lag. Try fewer
  swapchain images / `maximumDrawableCount` 2 and measure judder and FPS with it.
- **Frame timeline counters on M1–M3** (`rec_busy_ns`, `frames_ahead_ns`, … in BbStats) add
  `CpuCycles()` deltas and print them as ns: right on the M4 (1 GHz counter), 41.7x too small
  on 24 MHz Macs. Convert with the counter frequency.
- **Fixed 2026-10-13: pipelines Metal rejected after a cached session** ("cannot reserve
  'buffer' resource location"): shader permutation indices reused across sessions. *Remaining:*
  one pixel shader (`fs_fbe7c4b6`) has two cached permutations that compare equal ("conflicts
  with index 1"), so one pipeline compiles each session.
- **Startup resolution patch above 1080p breaks geometry** (2560x1440). *Now:* `run.sh` uses the
  live path above 1080p. *Proper fix:* find which game buffer the patch outgrows (the live path
  keeps guest targets at 1080p and costs proxy copies).
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
- **Character half-rendered after reloading a save (user report, 2026-10-11).** Repro: quit to the
  main menu, reload the save point; the character loads with the top half drawn and the bottom
  half missing. Suspects: pipelines skipped on Metal (`Graphics pipeline … rejected`, geometry
  stages skipped by MoltenVK) or stale buffer-cache/arena contents for re-used guest memory
  after the reload (macOS arena has no sparse unbind). Also: the ghosting halo in character
  creation (Mac GUI/visual list above).
- **Timed mutex/rwlock waits** (`runtime_mutex.c`, `runtime_rwlock.c`) are trylock + backoff:
  up to ~1 ms late, not fair; error-checking mutexes return `ETIMEDOUT` instead of `EDEADLK`
  on self-relock. *Refinement:* condvar-based timed locks.
- **Rosetta stale translations on aliased memory:** code written through one guest alias and
  executed from another runs the old translation until a protection change on the executing
  alias. *Refinement:* toggle protection (RW→RX) on executable aliases after host writes, if
  the game ever runs code from direct/flexible memory.

- **GPU process split (default since 2026-10-12; `BB_GPU_PROCESS=0` = single process).** *Now:*
  (a) every GPU page protection is a synchronous round trip to bb-probe and every fault on a
  protected page is relayed to the host (~950 faults/s in the city, no FPS loss measured) —
  *proper:* batch protections asynchronously with ordering guarantees; (b) input: the host
  window's keyboard reaches the game through the shared block, gamepads are polled by SDL in
  bb-probe (`SDL_UpdateGamepads`, no window there) — *proper:* read pads in the host and publish
  a pad-state block (design 3.5); (c) losing the host after the renderer started ends the game —
  *proper:* restart both; (d) the settings menu's Restart travels host → game as an event (not
  yet exercised in game); (e) watchdog/hang dumps in bb-probe report the game side only.
- **Command buffers parsed after the game rewrote them (single-process mode).** The game reuses
  DCB/CCB memory soon after `sceGnmSubmitCommandBuffers` returns; the GPU thread parses later
  ("Unimplemented PM4 type 0" on save loads). Split mode copies them at submit (arena in the
  shared block). *Proper for single-process:* copy at submit too (upstream's copy-GPU-buffers
  option, or the same arena); nested indirect buffers are still read from guest memory in both.
- **SRT walker faulting loads stay zeroed for good** (upstream behaviour, kept in the portable
  interpreter). *Proper:* zero only for the walk that faulted, if repeated faults are cheap enough.

## 2. Performance

- **Resolution targets (user, 2026-10-13): locked 60 FPS; M4 Max 1440p native and 4K
  upscaled, base M5 1080p native, smaller Macs 1080p upscaled.** M4 Max today: 1080p native
  61–63, 4K Performance ~52, 1440p 49, 4K Quality 43 (FINDINGS). Two levers: (a) the draw
  recording thread (MoltenVK encoding in `vkQueueSubmit` ~43%, render pass begins ~12%) caps
  1080p-class scenes at ~62–67 FPS; (b) GPU time at 1440p (~17.5 ms; 60 needs ≤ ~15 ms).
- **Render-pass count is a GPU cost after all (measured 2026-10-13, FINDINGS "Frame timeline").**
  At 1080p native the GPU timeline, not the CPU, sets the frame rate; ~408 Metal encoders per
  frame (190 render passes) cost ~13–16 µs of GPU time per render pass and leave the GPU idle
  ~3.8 ms/frame. *Refinement, in order:* (1) merge passes split by uploads/copies (~38/frame) and
  hoist the remaining copies; (2) put consecutive dispatches in one compute encoder; (3) the 47
  tessellated draws per frame each split a pass (MoltenVK's emulation): batch them or emulate
  tessellation ourselves; (4) attachment-less vertex-only passes (~20/frame) folded together.
- **Shader compilation (done 2026-10-13: background pipeline creation, `shader_compile=wait|skip`;
  `balanced`, the default since 2026-10-13 evening, waits up to `BB_COMPILE_BUDGET_MS` (6) per
  frame and then skips).** A true ahead-of-time precompile is not possible: a pipeline's
  translation depends on draw-time state (fetch shader, buffer/image descriptors, render-target
  formats), so the game's `shader/*.shaderbnd` archives alone cannot produce it.
  Pipeline creation (~70% of a new shader's cost) runs on 4 utility-QoS threads; the preload no
  longer blocks start. *Remaining:* our own GCN→SPIR-V translation (~30%) still runs on the GPU
  command thread; the draw-preparation workers could start compiles for pipelines they find
  missing ahead of it; compute dispatches always wait (skipping one could corrupt later frames).
- **Fixed 2026-10-13 (biggest win so far):** `NoContraction` on every float op became
  `[[clang::optnone]]` helper calls in MSL; dropped in fragment/compute shaders: city respawn
  spot 39 → 60 FPS (vsync cap), GPU 19.6 → 12.1 ms/frame. *Remaining:* vertex stages keep it
  (needed for matching positions across passes); building MoltenVK with a SPIRV-Cross that emits
  precise ops without `optnone` (or `invariant` positions) would remove it there too — measured
  no further gain today, so low priority. The game is now limited by the 60 Hz present: try 120 Hz
  displays / `BB_FPS_LIMIT` and re-profile the CPU side.

- **Render-pass breaks on a tile-based GPU (biggest lever, 2026-10-11).** Metal System Trace
  (`xcrun xctrace record --template "Metal System Trace" --attach <pid> --time-limit 3s`) in a
  ~23 FPS scene: GPU busy 84%, fragment 69%; **~287 Metal render passes per frame, ~32 ms
  fragment time, ~0.11 ms each** (load/store of 1080p targets per pass) for ~1,600 draws.
  Desktop GPUs barely pay for pass breaks; Apple TBDR does. *Refinement:* log why rendering
  ends between draws (barriers, layout changes, copies/clears, compute), merge passes with the
  same attachments, use DONT_CARE load/store where contents are dead.
- **Fixed 2026-10-12:** attachment-less passes with no pixel shader rendered over a 16384×16384
  area (~6 ms/frame of tiling); now 16×16 (27.5 → 33.9 FPS in the city). Remaining pass work:
  G-buffer splits (6 MRT + depth reloaded ~15×/frame) — merge or mark contents.
- **Measured 2026-10-11, correcting the pass-count theory:** of ~287 fragment passes per frame,
  62% take <0.02 ms (3.5% of GPU time); 15% take 0.25–2 ms and hold ~72% — the cost is real
  shading in heavy passes (game defaults; port extras are off), not TBDR load/store. Only ~18% of
  passes are needless splits (same attachments reopened, 85% after buffer copies).
  Per-pass timing tools that do NOT work here: `BB_GPU_PROFILE=1` (MoltenVK timestamps read 0),
  `BB_VK_MARKERS=1` (per-draw labels only), Xcode attach (process list hangs), Metal frame
  capture `BB_GPU_CAPTURE=<s>` + `METAL_CAPTURE_ENABLED=1` (Apple's capture crashes in
  `DownloadHeap`, also with `BB_ARENA_GIB=8`).
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
  *Measured 2026-10-13 (FINDINGS "Copy-shader pass splits"):* the pass splits come from the
  game's copy shader, not uploads, so zero-copy mostly saves CPU work (write faults, staging
  copies), not GPU time. It also needs EOP fences signalled at GPU completion (they are
  signalled at record time today), or the GPU reads buffers the game is already reusing.
- **Versioned copy-shader table.** ~24 copies per frame rewrite one table between draws inside a
  pass, ending it. *Refinement:* give each draw group its own version of the table (copies built
  ahead of the submission) so the pass stays open; bound ~1 ms GPU at 1440p.

- **MetalFX with asynchronous submits.** *Now:* MetalFX encodes on MoltenVK's queue right after
  a synchronous `vkQueueSubmit` (commit order). *Refinement:* order it with an MTLSharedEvent
  (exported timeline semaphore) so `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` can be used.
- **PGO on macOS:** profile-guided optimization is off (the profiles are GCC `.gcda`).
  *Refinement:* clang `-fprofile-generate`/`-fprofile-use` with `.profdata`.

## 3. Mac GUI

- **Native Mac settings.** *Now:* the in-game ImGui overlay (`gpu/shim/bbport_overlay.cpp`), in
  English, opens with ⌘, or L3+R3 and shows FPS, render → output resolution and upscaler.
  *Refinement:* a native Mac settings window in the launcher; if SDL's default app menu
  swallows ⌘, (its "Preferences…" item), wire that menu item to the overlay instead.
- **Menu bugs on macOS.** "Subpixel jitter" ticks then unticks itself; upscaler/preset choices
  made in the menu were not what bbport.ini held afterwards (found upscaler=off, preset=0).
- **Launcher and `.app` bundle** (planned in `MACOS_PORT.md`).
