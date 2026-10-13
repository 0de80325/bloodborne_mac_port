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

### Native GPU host (M2, 2026-10-12)

`BB_GPU_PROCESS=1` starts `out/bbgpu-host` (native arm64): ready in 350–380 ms, maps the guest
memory entry at the same addresses (0x7000000000–0xfc00000000) and mirrors all 20 guest views
(4.8 GiB); 9 periodic checksum comparisons per run all match. Killing the host leaves the game
running single-process. Without the variable nothing changes (same menu run, same warnings).

### GPU process split (M3, 2026-10-12, without the game)

Shared control block between `bb-probe` (x86-64/Rosetta) and `bbgpu-host` (arm64): a ring
moves ~1.2 M variable-size records/s in order; a request/reply round trip averages 12.8 µs.
With the real HLE functions in remote mode (`tests/test_gpu_split.c`, warm caches): renderer
start in the host 0.23–0.38 s, `sceGnmSubmitDone` → host GpuIdle interrupt → submissions
unlocked in 1.5–3 ms, a flip presented in 13.5–15 ms (≈ one 60 Hz vblank), vblanks at 62 Hz.
Cold (no pipeline cache) the first flip took 3 s, and the present thread then caught up on the
vblanks it missed in a burst (existing behaviour, not split-specific).

**In the game (2026-10-12):** with `BB_GPU_PROCESS=1` the title screen renders through the
native host at 60 FPS (same as single-process). The first run crashed with "Unimplemented PM4
type 0": a GpuIdle from an earlier queue drain released the submission lock after
`sceGnmSubmitDone`, and the game rewrote command buffers the host had not parsed yet (the
diagnostic showed memory consistent but the submitted size cutting a packet). The same race
exists, with a much smaller window, in single-process mode — the likely cause of the
intermittent crash on loading a save. Fixed in both modes by numbering SubmitDone calls.

### GPU time per render pass (2026-10-12, city scene, split mode)

`BB_PASS_LOG=<file> BB_PASS_LABELS=1` + a Metal System Trace of `bbgpu-host` + `tools/pass_trace.py`
attribute GPU time to render passes (MoltenVK names render encoders after their command buffer,
numbered here; tessellated draws split a pass into several encoders, which the tool merges).
Per frame: fragment 25.5 ms, vertex 5.5 ms, compute 2.2 ms (MetalFX ~0.5 ms, tessellation
emulation ~0.3 ms). **Biggest single cost: ~20 passes per frame with no attachments and no pixel
shader (vertex shaders that only write memory) kept the device-maximum render area, 16384×16384:
~0.3 ms of tiling each, ~6 ms per frame.** A 16×16 area for such passes: **27.5 → 33.9 FPS**,
image unchanged (toggle `NoOutputPassArea`, bit 58).

**G-buffer splits** (`BB_PASS_STATS=1`): ~38 per frame reopen the same attachments; 76% came
right after `CopyBuffer` (uploads of CPU-written guest data into the buffer arena, which Vulkan
forbids inside a render pass). Upper bound measured by dropping in-pass copies (wrong image):
43 FPS. **Upload hoisting** (toggle `UploadHoisting`, bit 59): an in-pass upload into arena pages
nothing in the submission has used yet is recorded into an upload command buffer submitted ahead
of the submission (barriers before and after), and the pass stays open: upload-caused splits
983 → 560/s, **33.9 → 34.8 FPS**, image unchanged. The remaining splits follow uploads into pages
already used in the submission (4 KiB tracking) and HLE copy shaders (`MultiCopy`, GPU data,
not hoistable).

### Pipeline cache was wiped every launch (2026-10-13)

Frame stats counted ~500 shader/pipeline compiles in every session in the same area (365 at the
level load, the rest mid-game: the user's "shader stutter when attacking"). `PipelineCache::WarmUp`
cleared the whole cache whenever any one cached pipeline failed to preload, and one always did. Now
it clears only for old-format entries and re-saves the cache profile: next session **preloaded 226
of 227 pipelines, 2 compiles during play** (was 517). The native host also used the wrong cache
directory (`BB_GPU_USER_DIR` is read when the library loads; `run.sh` now exports it).

### "Precise" float math made every pixel shader unoptimizable on Metal (2026-10-13)

`MVK_CONFIG_SHADER_DUMP_DIR=<dir>` (with an empty `BB_GPU_USER_DIR`, so every shader converts)
dumps the SPIR-V and MSL; the pass log names each pass's shaders, and matching the SPIR-V bytes
of `<user>/cache/*.spv` to the dump maps guest hashes to MSL. The hottest G-buffer pixel shader
(1.8 ms/frame) did every float add, multiply and FMA through `spvFAdd`/`spvFMul` helpers marked
`[[clang::optnone]]`: SPIRV-Cross's translation of SPIR-V `NoContraction`, which the recompiler
put on every FP add/sub/mul/fma (`emit_spirv_floating_point.cpp`) — in 223 of 308 shaders.
Now only vertex/tessellation/geometry stages keep it (positions must match bit for bit across
the passes that draw the same geometry: depth prepass, G-buffer, shadows):

| Respawn spot, Central Yharnam (1379 draws/frame) | FPS | GPU busy/frame | fragment | vertex | compute |
|---|---|---|---|---|---|
| precise everywhere (before) | 39.4 | 19.6 ms | 13.4 | 5.3 | 2.3 |
| precise in vertex stages only (now) | 60 (vsync cap) | 12.1 ms | 3.7 | 7.2 | 2.4 |
| precise nowhere + invariant position | 60 (vsync cap) | 12.4 ms | 5.4 | 5.1 | 3.5 |

(Per-stage times overlap on a tile-based GPU; the union "GPU busy" is the comparable number.)
Dropping it from vertex shaders too gains nothing more, so they keep it. Screenshots are the same
(mean difference 3.9/255, MetalFX history and idle animation). Single-process mode: 60 FPS too.
The scene is now held by the 60 Hz present, with the GPU ~72% busy. `BB_SPIRV_PRECISE=all`
(with a fresh shader cache) restores the old code. This corrects the 2026-10-11 conclusion that
the remaining fragment time was "real shading": most of it was this.

### Resolution targets on the M4 Max (2026-10-13, Central Yharnam respawn spot)

Target: a locked 60 FPS (the game's physics run at 30 Hz; more is not wanted), as high a
resolution as that allows. Uncapped for measurement: `BB_PRESENT_MODE=Immediate BB_FPS_LIMIT=0`.

| Output | Scene | Path | FPS uncapped | Limit |
|---|---|---|---|---|
| 1080p MetalFX Quality (default) | 1280x720 | startup patch | 65–77 (60 locked) | — |
| 1080p native | 1920x1080 | — | 61–63 | GPU timeline incl. render-pass overhead (see frame timeline) |
| 4K MetalFX Performance | 1916x1078 | startup patch | ~52 | GPU 69% busy (not yet broken down) |
| 1440p native | 2560x1440 | live | 49 | GPU (~17.5 ms of a 19–20 ms frame) |
| 4K MetalFX Quality | 2560x1440 | live | 43 | GPU |

- **The startup resolution patch breaks scenes above 1080p** (2560x1440: polygons stretched across
  the screen, both process modes; 1916x1078 is fine). `run.sh` now takes the live path above
  1080p, which renders correctly. Earlier 1440p/4K-Quality numbers (33/30 FPS) measured the broken
  image.
- Draw recording thread (`bb:DrawRec`, 1080p native): 43% in `vkQueueSubmit` (MoltenVK encodes
  Metal commands there), 12% beginning Metal render passes, ~4% MoltenVK formatting encoder
  labels from command buffer names (now unnamed). `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`: no
  gain (62 vs 62 FPS). `BB_FRAMES_AHEAD=2`: no gain at 1440p (49.2 vs 49.1).
- Frame times snap to 480 Hz vblank ticks (2.08 ms): 49 FPS is 10 ticks per frame.
- Dropping NoContraction from vertex shaders too ("precise nowhere", with invariant positions)
  broke geometry: some vertex shaders need exact rounding. Vertex stages keep it.

**Frame timeline (2026-10-13, `BB_FRAME_STATS=1` "Frame timeline" line, 1080p native uncapped,
~61 FPS):** per 16.4 ms frame the draw recording thread records for ~5.4 ms, spends 4.8 ms in the
flip's `vkQueueSubmit` (MoltenVK encoding the frame), **waits 4.5 ms for the GPU to finish the
previous frame** (frames-ahead bound) and idles 1.7 ms; the command thread waits ~10 ms for it.
Each flip finishes on the GPU ~20 ms after its submit, one every ~16.4 ms. With
`BB_FRAMES_AHEAD=2` the latency doubles (38 ms) and the rate stays (16.8 ms): **the GPU timeline,
not the CPU, limits 1080p native**, so CPU work (encoding, recording) would not raise it.

**Why the GPU timeline is longer than its busy time:** a Metal System Trace shows the GPU idle
~3.8 ms per frame (18–24% of wall time), in 0.3–2 ms gaps inside one command buffer, almost all
right after a render pass's fragment work, **recurring every ~115 Metal encoders** (every ~85 with
a pass per draw) whatever the passes around them are. The frame has **~408 encoders**: 190 render
passes, 81 buffer-copy blits, 77 compute encoders (one per dispatch), 47 tessellation compute
encoders (MoltenVK's tessellation emulation, each also splitting its render pass). A standalone
Metal test (`tools/metal_encoder_cost/encoder_cost.m`, 1080p, 3 attachments loaded and stored,
same small draw): **a render pass costs ~13–16 µs of GPU time** (8.6 µs each at 50 per command
buffer, 14–16 µs at 100–400) **against ~0.7 µs for the same draw inside one pass**; a compute
encoder costs ~1 µs more than a dispatch in a shared encoder; a blit encoder ~4 µs. So ~190
passes are ~3 ms of fixed GPU cost per frame, independent of resolution — the lever for both
1080p headroom and 1440p (GPU ~17.5 ms; 60 needs ≤ ~15).

**Pipeline preload:** the 217 cached pipelines preload serially in 0.29–0.39 s at start (macOS's
own shader cache warm); pipelines the game has not used before compile on the GPU command thread
when first drawn, ~1–3.5 ms each (more when Metal's cache is cold): the in-game hitches.

**Why render passes end (2026-10-13, `BB_PASS_STATS=1`, 1080p native, ~190 passes/frame):**
~33/frame other attachments, ~45 a target about to be sampled (render-to-texture), ~26 buffer
uploads (`CopyBuffer`), ~15 the game's GPU copies (`MultiCopy`), ~22 dispatches; only ~32/frame
reopen the same attachments right after (mostly after copies). Per frame the game also issues
**~37 tessellated draws and ~32 rect/quad-list draws** (emulated with tessellation), each of which
MoltenVK splits the render pass around (compute for the vertex/control stages). Upper bound at
1440p native (`BB_DROP_TESS`, wrong image): dropping the tessellated draws 49.1 → 51.9 FPS
(~1.1 ms/frame including their own shading). Dropping rect lists is not measurable that way (they
include full-screen post passes; the game stalled at the title). So pass merging is worth roughly
0.5 ms (copy splits) + ≤ 1 ms (tessellation) + ≤ ~1 ms (rect lists) at 1440p, short of the ~3.7 ms
1440p needs for 60 on its own.

**Background pipeline creation (2026-10-13).** With an empty shader cache (macOS's own Metal
cache warm), a new shader cost ~1.2 ms: ~70% creating the pipeline (MoltenVK SPIR-V→MSL and the
Metal compiler), ~30% our translation. `FreePermIndex` (a `stat()` per probe) ran on every program
lookup and cost more than both during loads; now only for new permutations. Pipeline creation now
runs on a 4-thread pool (`BB_PIPELINE_THREADS`): level load from an empty cache, compile time on
the GPU command thread 440 → 130 ms; the render path waited ~260 ms for the pool with
`shader_compile=wait` (worst loading frame 248 → 187 ms) and ~35 ms with `skip` (76 ms; ~2,000
draws skipped during the load, none after). Warm cache: 217 pipelines built in the background,
nothing waited, start-up preload step 290–390 → 110 ms. Gameplay images correct in both modes.

**Pipeline cache permutations (fixed):** Metal rejected 6 pipelines per session once the cache was
kept between sessions ("cannot reserve 'buffer' resource location at index 0"): a new shader
permutation took the index of a stored one the session had not loaded and overwrote it, and
pipelines saved earlier then loaded a shader numbered for other bindings; their draws were
skipped (part of a G-buffer pass). Also, permutations of shaders whose resources were all null
ignored their first binding. Four sessions alternating 720p/1440p: 0 rejected.

Instruments' GPU limiter counters cannot be recorded headless: the Metal System Trace template's
`counterprofile` values 1–3 are "not supported on target device", and the default profile records
only "RT Unit Active".

**Host crash found while profiling (fixed):** hoisted uploads (`Scheduler::RecordHoistedCopy`) were
recorded on the draw thread into a command buffer from the same `VkCommandPool` the recorder thread
was recording into; MoltenVK allocates command objects per pool, so the host died with SIGSEGV in
`MVKCmdCopyBuffer::setContent` (address 0x20). Upload command buffers now come from their own pool.

**Rosetta gotcha:** attaching lldb to a translated process and killing it wedged Rosetta for
hours (new x86 processes hung in state U); it recovered on its own.

## Part 3 — Open issues

| Issue | Status |
|---|---|
| CPU-bound draw recording at 720p | Next: re-profile at 720p to see what dominates now |
| Intermittent "Unimplemented PM4 type 0" / bad fence packets on loading a save | **Root cause found 2026-10-12:** the game rewrites command buffers after the submit returns, before the (asynchronous) parser reads them. Split mode (default) now copies DCB/CCB at submit into a shared arena: 0 crashes in 5 stressed save loads (was 2 in 3). Single-process mode still parses the game's memory (smaller window): backlog. **Seen again 2026-10-13 in split mode** (1 in ~8 runs, at load): not fully fixed |
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
