# macOS Port — Findings

What it took to get Bloodborne running on Apple Silicon, and what every test since has shown.
Hardware: Apple M4 Max, macOS 26.6. Game: CUSA03173 v1.09. All numbers are from 2026-10-11
unless stated. Plan: [`MACOS_PORT.md`](MACOS_PORT.md); deferred work: [`MACOS_BACKLOG.md`](MACOS_BACKLOG.md).

## Summary

- The game **boots, renders and plays** on macOS: title screen, character creation, saving, and
  the first level, with DualSense input and CoreAudio sound.
- Everything runs as **one x86-64 process under Rosetta 2** (the game's code is PS4 x86-64
  machine code) with the shadPS4-derived Vulkan renderer on **MoltenVK**.
- Performance in a fixed test scene: **22–25 FPS at native 1080p**, **27–31 FPS** with FSR 3.1 or
  **MetalFX** upscaling from 720p. At 720p the limit is one CPU thread recording GPU commands.

## Part 1 — Getting it to run at all

Each item below was a hard stop until fixed, roughly in the order they were hit.

### Toolchain and dependencies

| Problem | Finding / fix |
|---|---|
| Game code cannot be recompiled | It is PS4 machine code with no source. The whole process (game + runtime + GPU library) is built **x86-64** and runs under Rosetta 2; deployment target macOS 15 (Rosetta's AVX2 support). |
| arm64 Homebrew libraries leak into an x86-64 build | All dependencies (SDL3, Vulkan loader, fmt, FFmpeg, Zydis, …) are built x86-64 into `deps/x86_64` by `scripts/macos/bootstrap_deps.sh`, with an isolated `PKG_CONFIG_LIBDIR` and `CMAKE_IGNORE_PREFIX_PATH=/opt/homebrew`. |
| Zydis' installed CMake config needs Zycore, which it doesn't install | Zycore is built and installed separately (same bundled copy). |
| `xcrun --find clang` cannot find the SDK | Use the `/usr/bin/clang` shims, which locate the SDK themselves. |

### Build system and GPU library (`gpu/`)

| Problem | Finding / fix |
|---|---|
| CMake assumed Windows or Linux+X11 | Apple branch: no X11, `-undefined dynamic_lookup` (runtime symbols come from the loader), `VK_USE_PLATFORM_METAL_EXT`, Metal/QuartzCore/MetalFX frameworks. `build_macos.sh` builds everything. |
| Linux-only APIs in the GPU library | `gettid`, CPU affinity, `SCHED_IDLE`, `RUSAGE_THREAD`, `pthread_getcpuclockid`, `ucontext` register names — macOS equivalents (Mach thread ids/times, QoS classes, `uc_mcontext->__ss`). |
| libstdc++-only C++ | `std::bitset::_Find_first/_Find_next`; `atomic_ref<const T>`; `u64` vs `unsigned long` casts. |
| Embedded font in assembly | Mach-O needs `_` symbol prefixes and `__TEXT,__const`. |

### Runtime (`src/`) — the PS4 emulation layer

| Problem | Finding / fix |
|---|---|
| **Guest TLS.** The game reads its thread block through `fs:[0]`; Linux rewrites that to `gs:[0]` and sets the GS base with `arch_prctl`. On macOS, GS points at the pthread TSD array and cannot be moved. | The loader rewrites the 17,127 `fs:[0]` loads to `gs:[0xc00]`; the runtime reserves **pthread key 384** at startup and stores each thread's guest TCB there. First free key under Rosetta is 258, with SDL/MoltenVK loaded ~270. |
| **Address space.** The game packs pointers into 40 bits, so guest memory must sit below 1 TiB. | macOS keeps **`[0xfc0000000, 0x7000000000)` (63–448 GiB) as a permanent no-access hole** in every process. Guest memory now starts at 0x7000000000; the whole range is reserved up front (large host `mmap(NULL)` calls otherwise land in it). |
| No `memfd`, `fallocate`, `MAP_FIXED_NOREPLACE` | One anonymous backing region; guest views are `mach_vm_remap` aliases; freeing = fresh anonymous pages mapped over the range; low maps by hint-and-verify. Page size under Rosetta is 4 KiB. |
| Write faults | A write to a write-protected page arrives as **SIGBUS** on macOS, not SIGSEGV; the GPU's page tracking handles both. |
| Thread dumps, restart, timed locks, clocks | `task_threads` + `pthread_kill`; close-fd loop; trylock + backoff for timed mutex/rwlock; monotonic sleep loop; `pthread_cond_timedwait_relative_np`; `arc4random_buf`. |
| Mach-O assembly | Top-level asm names need the `_` prefix (`SYM()` macro). |
| Windows-fork regressions also broke Linux | A `DWORD` log outside a Windows guard, an undeclared `struct stat`, tests needing `restore_guest_fs`. Fixed; Windows support later purged from our code. |

### First boots

| Stop | Finding / fix |
|---|---|
| `--cpu-only` crashes in graphics setup | Expected on any platform: it skips the GPU library, so `sceGnm*` calls return NULL. The CPU side (TLS, memory, ~30 threads, FMOD audio) works up to graphics init. |
| "No available video device" | **Cocoa only works on the real main thread.** The game now runs on a 64 MiB-stack worker thread and the main thread runs `CFRunLoopRun()`; the GPU library posts its window loop to the main queue. `dispatch_main()` does *not* work (it exits the main thread). |
| `ErrorIncompatibleDriver` | MoltenVK is a portability driver: needs `VK_KHR_portability_enumeration` + the enumerate-portability flag, and `VK_KHR_portability_subset` on the device. shadPS4 also overwrote `VK_DRIVER_FILES`. |
| Device creation: `robustBufferAccess2` | MoltenVK lacks **`robustBufferAccess2`** and **`nullDescriptor`**. Option A chosen: create the device without them, bind a zero-filled stand-in buffer for null bindings. Shader-side bounds checks are in the backlog (Stability). |
| `Failed to get buffer device address` | MoltenVK has **no sparse binding**; the buffer cache mirrored the 40-bit GPU address space with sparse 4 GiB arenas. macOS uses one ordinary 16 GiB arena at the guest base — **Metal commits its memory lazily** (measured: a 16 GiB buffer adds ~0 to the footprint until touched). Zero-copy guest memory is the top Performance backlog item. |
| Corrupted shader cache | A run that crashed mid-write leaves a bad cache; deleting it fixes it. |
| ~520 "Command buffer cannot accept commands before vkBeginCommandBuffer" | **Metal cannot write compressed (BC) textures**, even through uncompressed views; MoltenVK rejected BC images created with storage usage. They are filled by buffer copies anyway, so macOS drops the flag. This cleared the errors and got the game **through character creation into the first level.** |
| Pipelines Metal rejects | Metal is stricter than desktop drivers: a fragment input the vertex stage doesn't write (MoltenVK has **no geometry shaders**, which the renderer skips), clashing buffer indices. Rejected pipelines are now logged and their draws skipped instead of stopping the game. |
| `Unknown data_format=10 num_format=7` | An impossible format (8-bit "float") from a garbage or unused descriptor; macOS now uses a same-size stand-in. |

### False alarms worth knowing about

- **The "job-pool crash"** (SIGBUS at guest 0x53b3260): every instance came right after the
  test-run timeout (`BB_TIMEOUT`) fired. Its stack dump derails a worker thread. Normal play has
  no timeout and never hits it.
- **"Unimplemented PM4 type 0"** (intermittent, often when loading a save): a diagnostic showed the
  command buffer's contents identical through the guest's view and the backing store — *not* a
  macOS memory-aliasing bug. The bad word is the tail of the previous packet: the parser fell one
  word out of step, most likely by reading a packet header while the game was still writing it
  (a CPU/GPU-thread race that Rosetta's timing may expose more often). Under investigation; the
  diagnostic now also re-walks the buffer to confirm.

## Part 2 — Performance investigation

### Starting point

| Test | Result |
|---|---|
| Native 1080p, first level | ~21–23 FPS, frame time 42–57 ms |
| Same game, Intel Core Ultra (358H) with Arc graphics, Windows port | 40–50 FPS at 1080p, no upscaling (owner's reference) |

### What it is *not*

| Hypothesis | Test | Result |
|---|---|---|
| Logging | Line counts per run | ~700 lines/minute, warnings only — negligible |
| Page-protection faults (GPU write tracking) | `BB_FRAME_STATS=1` | ~140–800 faults/s, 0 in the GPU thread — negligible |
| Asynchronous submission | `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` | 21–22 → 23–25 FPS at native; no measurable change at 720p |
| Draw-preparation worker pool | `BB_PREP_WORKERS=0` | 21 → 23 FPS (lock contention; workers ran at background QoS on E-cores) |
| Render-pass count on a tile-based GPU | Metal System Trace + `BB_PASS_STATS=1` | See below — **theory disproved** |

### CPU side (native 1080p, `sample` of the draw-recording thread `bb:DrawRec`)

| Share of thread time | Where |
|---|---|
| ~47% | `vkQueueSubmit` at frame flip — MoltenVK encoding the whole frame into Metal, synchronously |
| ~21% | Renderer per-draw work (resource and texture binding) |
| ~12% | Spin-waiting for host copies (`cthread_yield`) |
| ~9% | Idle |

The thread is busy ~35–40 ms per frame (~1,600 draws, ~95 dispatches, 66 submissions).

### GPU side (Metal System Trace, 3 s in a ~23 FPS scene)

| Measure | Result |
|---|---|
| GPU busy | **84%** of wall time (fragment 69%, vertex 14%, compute 6%) |
| Metal render passes per frame | **~287**, ~32 ms fragment time per frame |
| Pass duration distribution | 62% of passes take < 0.02 ms (3.5% of time); **15% take 0.25–2 ms and hold ~72% of time**; the top 5% hold 49% |
| Needless pass splits (same targets reopened right after a break) | ~18% of passes (~40/frame), 85% of them after mid-frame buffer copies |

**Correction:** the first reading of ~287 passes suggested per-pass load/store overhead on Apple's
tile-based GPU. The duration distribution showed the opposite — most passes are nearly free, and
the cost is real shading work in a minority of heavy passes. Reducing pass count would gain little.

Per-pass GPU timing tools that do **not** work in this setup: the renderer's `BB_GPU_PROFILE=1`
(MoltenVK can't place timestamps between draws — every pass reads 0 ms), `BB_VK_MARKERS=1`
(per-draw labels only, no named passes), attaching Xcode (its process list hangs on this
process), and a Metal frame capture to file (`BB_GPU_CAPTURE`; Apple's capture tool crashes in
`DownloadHeap`).

### Resolution and effects

| Test (same scene) | Result |
|---|---|
| Render at 1280×720 (`BB_RENDER_RES`) | Fastest of the tests — cost scales with pixels |
| Game effects off (SSAO, DoF, motion blur, dynamic shadows, chromatic aberration) | A little faster — no single pathological effect |
| Upscaler presets via the **live** path at 1080p output | No FPS difference between Quality/Balanced/Performance: the live path keeps the guest at 1080p and copies scene proxies back, cancelling the saving on this GPU |

So the GPU was **pixel-shading bound** at native 1080p. On macOS, presets at a 1080p output now
patch the game to render the whole frame at the preset size (a restart per change);
`live_resolution=1` keeps the live path.

### Upscaling benchmark (same scene: characters walking into the distance; start → end FPS)

| Mode | FPS | Image |
|---|---|---|
| Native 1080p, no upscaler | 22 → 25 | Reference |
| FSR 3.1 Quality (720p → 1080p) | 28 → 31 | Softer, some smearing, less shimmer |
| MetalFX Quality (720p → 1080p) | 27 → 30 | Sharper, more distant shimmer |
| FSR 3.1 Quality + async submits | 27 → 31 | Same as without |
| FSR 3.1 Balanced (live path, earlier, different scene) | 38–45 | Not comparable (different scene and path) |

**Profile at 720p (MetalFX Quality, `sample` of `bb:DrawRec`):** ~51% inside `vkQueueSubmit`
(MoltenVK encoding Vulkan → Metal, incl. 8% creating Metal render encoders and ~15% per-draw
state/descriptors), ~11% spin-waiting on host copies, ~6% idle, and only ~9% the renderer's own
per-draw work. The GPU command thread spends ~73% waiting for `bb:DrawRec`; the Vulkan recorder
thread is ~76% idle.

| Experiment at 720p | Result |
|---|---|
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` (encode on the recorder thread as recorded) | 27 → 30 FPS, unchanged |

The frame is effectively serialised through Rosetta-translated code: the draw thread regularly
syncs with the recorder thread (direct command-buffer access), so moving MoltenVK's encoding to
another thread moves the wait, not the work.

Rendering 44% of the pixels gains only ~25%: at 720p **the GPU is no longer the limit — the
draw-recording CPU thread is** (~33–40 ms of work per frame). MetalFX and FSR are therefore
equally fast.

### Native-process feasibility spike (M0)

See [`DESIGN_NATIVE_GPU_PROCESS.md`](DESIGN_NATIVE_GPU_PROCESS.md#m0-results-2026-10-11--toolsm0_spike):
a game-shaped Vulkan frame costs **1.75× less CPU natively** than under Rosetta (MoltenVK
encoding 1.72×, recording 2.0×; generic C++ only 1.19×); cross-process shared memory, protection,
doorbells (~5 µs) and fault relays (~24 µs) all work.

**Measurement gotcha:** under Rosetta, `CLOCK_THREAD_CPUTIME_ID` is badly wrong (reported 21 ms
for 871 ms of work). Use `thread_info(THREAD_EXTENDED_INFO)` / Mach thread times instead.

## Part 3 — Open issues

| Issue | Status |
|---|---|
| CPU-bound draw recording at 720p | Next: re-profile at 720p to see what dominates now |
| Intermittent "Unimplemented PM4 type 0" on loading a save | Race hypothesis; extended diagnostic in place |
| Character half-rendered after quitting to the menu and reloading | Logged (backlog, Stability) |
| Ghosting halo around the character in character creation | Logged; null image stand-ins not done yet |
| MetalFX distant shimmer | Try `BB_METALFX_JITTER_SIGN=-1`; jitter must be on |
| Menu: disabled controls looked broken; settings not persisting as expected | Root cause: controls under an "Off" upscaler are disabled, with near-invisible styling. Menu redesign in progress |

## How to reproduce

```sh
# Run (data, saves and settings in out/macdata)
BB_DATA_DIR=$PWD/out/macdata BB_GAME_DIR=~/Bloodborne/CUSA03173 bash run.sh
# Benchmarks: fixed settings files
BB_CONFIG=$PWD/out/macdata/bench-{native,fsr3-quality,metalfx-quality}.ini …
# Diagnostics (environment variables)
BB_FRAME_STATS=1   # per-frame stall breakdown
BB_PASS_STATS=1    # why render passes end, split passes
BB_MENU_TRACE=1    # menu clicks and setting changes
BB_TRACE=1         # import calls, unbound imports
# CPU profile while the game runs
sample $(pgrep -x bb-probe) 10 1 -file profile.txt
# GPU timeline while the game runs
xcrun xctrace record --template "Metal System Trace" --attach $(pgrep -x bb-probe) --time-limit 3s --output gpu.trace
```
