# Design: Native ARM64 GPU process (option A)

Status: **proposal for decision** (2026-10-11). Reverses founding decision 1 in `CLAUDE.md`
("everything runs as one x86-64 process under Rosetta"), so it needs the owner's go-ahead.
Evidence behind it: [`FINDINGS.md`](FINDINGS.md), Part 2.

## 1. Problem

At a 720p render resolution (MetalFX/FSR Quality), the game runs at **27–30 FPS**, capped by one
CPU thread, `bb:DrawRec`, which turns ~1,600 guest draws per frame into GPU work:

| `bb:DrawRec` at 720p | Share |
|---|---|
| MoltenVK encoding Vulkan → Metal (`vkQueueSubmit`) | ~51% |
| Spin-waiting on host copies | ~11% |
| Renderer per-draw work | ~9% |
| Idle / waits | ~6% |

Moving work to other threads does not help (async submit, MoltenVK prefill on the recorder
thread, the prep pool: all unchanged) because the frame is serialised through sync points. Every
instruction on that path — the renderer and MoltenVK — is **x86-64 code translated by Rosetta**,
because the game's code is x86-64 and a macOS process cannot mix architectures.

## 2. Proposal

Split the program into two processes that share guest memory:

```
 bb-probe  (x86-64, Rosetta)                    bbgpu-host  (arm64, native)
 ┌──────────────────────────────┐               ┌──────────────────────────────────┐
 │ game eboot + libc.prx         │               │ Liverpool command processor       │
 │ PS4 HLE runtime (src/)        │  shared guest │ shadPS4 renderer (Vulkan)         │
 │ audio (CoreAudio)             │◄═══ memory ══►│ MoltenVK → Metal, MetalFX         │
 │ GnmDriver command builders    │  (same VAs)   │ texture/buffer caches, shaders    │
 │ GPU proxy: submit/flip/events │──── rings ───►│ SDL window, overlay, input        │
 │ page-protection + fault relay │◄── messages ──│ presenter                         │
 └──────────────────────────────┘               └──────────────────────────────────┘
```

- **`bb-probe`** stays x86-64 under Rosetta: loader, game, PS4 runtime, audio. It links a thin
  **GPU proxy** instead of `libbbgpu`.
- **`bbgpu-host`** is a new **native arm64** executable containing today's GPU library
  (renderer, MoltenVK, MetalFX, window, overlay). It runs the hot path natively.
- **Guest memory is mapped at the same virtual addresses in both processes**, so the renderer's
  pointers into command buffers, vertex data and textures stay valid unchanged. Apple Silicon has
  unified memory, so both processes (and the GPU) see the same physical pages — no copying.

### Why this is the right cut

The interface is already narrow (measured from the code):

| Direction | Today | After the split |
|---|---|---|
| runtime → GPU library | ~15 `bbgpu_*` calls (init, symbol resolution, fault handling, overlay/text input, stats) | Messages to the host; init at launch |
| GPU library → runtime | ~18 `runtime_*` calls (memory queries, page protection, write-through-backing, clocks, thread bookkeeping) | Mostly replaced by shared-memory state; protection via messages |
| game → GPU library | 327 HLE functions (GnmDriver, VideoOut, event queues, AvPlayer) | Split by kind — see 3.3 |

## 3. Design

### 3.1 Shared guest memory

- Today the guest pool is one anonymous region; guest views are `mach_vm_remap` aliases
  (`src/runtime_memory.c`). It becomes a **Mach named memory entry**
  (`mach_make_memory_entry_64`), whose send right is passed to the host at launch.
- The host reserves the same guest range (`[0x7000000000, 0xfc00000000)`; the 63–448 GiB
  kernel hole is the same in arm64 processes) and maps views at identical addresses with
  `mach_vm_map` of the memory entry. Every guest map/unmap/protect in the runtime is mirrored to
  the host through the existing GPU hooks (`runtime_memory_set_gpu_hooks`), now as messages.
- The guest image and stacks are not GPU-visible and stay private to `bb-probe`.
- This also realises the **zero-copy** backlog item: the host can import guest pages straight
  into Metal (`VK_EXT_external_memory_host`) instead of copying into a 16 GiB arena.

### 3.2 Communication

- **Command rings** in a small shared control block: submit records (DCB/CCB addresses, sizes,
  queue), flip requests, and **event/interrupt records** back from the host (EOP, flips,
  vblank) that the runtime turns into `sceKernel` event-queue wakeups.
- **Doorbells**: Mach semaphores or `os_sync_wait_on_address` on a shared word, so neither side
  spins (today's 11% spin-wait goes away rather than moving).
- **Control messages** (Mach messages over ports exchanged at launch): init, symbol table, page
  protection requests, fault relays, overlay/text input, stats, shutdown.
- **Memory ordering**: Rosetta runs translated threads with Apple Silicon's hardware **TSO** mode;
  the native host does not. All cross-process handoffs use explicit release/acquire atomics; the
  renderer's polling of guest labels (WAIT_REG_MEM etc.) must use acquire loads. This is the main
  correctness risk and gets its own tests.

### 3.3 The 327 HLE functions

| Kind | Examples | Where |
|---|---|---|
| Command builders (write PM4 into guest memory, no GPU state) | `sceGnmDrawIndex`, `sceGnmSetVsShader`, `sceGnmDispatchDirect` … | Compiled into `bb-probe` from the same `gnmdriver` source — no IPC per call |
| Submission / flip | `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceVideoOutSubmitFlip` | Proxy → ring + doorbell |
| GPU state queries and waits | `sceGnmGetGpuCoreClockFrequency`, `sceVideoOutGetFlipStatus`, `sceGnmSubmitDone` … | Shared-memory status block (cheap reads), message for the rest |
| Event queues | `sceKernelCreateEqueue`, `sceKernelWaitEqueue` with GPU events | Runtime side; host posts events through the ring |
| AvPlayer (FFmpeg video) | `sceAvPlayer*` | Decide in M6: keep in `bb-probe` (x86 FFmpeg) or move to the host |

Classifying all 327 is a mechanical early task (M2).

### 3.4 GPU page tracking across processes

Today the renderer write-protects guest pages in-process and catches the SIGBUS when the game
writes them (~150–800 faults/s in gameplay). After the split:

1. The host asks `bb-probe` to protect/unprotect ranges (batched messages; `runtime_memory_gpu_protect`
   already batches).
2. A fault in `bb-probe` relays the address to the host, which invalidates its cache entries and
   replies; `bb-probe` unprotects and resumes. Round-trip cost is a Mach message pair (to be
   measured in M0; tens of microseconds would be fine at these rates).
3. Read faults (the CPU reading data the GPU wrote) need the host to write data back first — the
   same relay, waiting for completion.

The host's own views stay writable: protection only matters where the game runs.

### 3.5 Window, input, overlay

- `bbgpu-host` owns SDL, the Cocoa window (main thread, as today), the overlay and text input.
- Keyboard and gamepad state are read in the host and published to a shared **pad-state block**
  that `runtime_pad.c` reads (it already polls state rather than consuming events).
- Audio stays in `bb-probe` (SDL audio/CoreAudio work without a window).

### 3.6 Lifecycle

- `bb-probe` spawns `bbgpu-host` (`posix_spawn`, arm64 slice), passes the memory-entry and
  control ports, and waits for "ready" before entering the game.
- If either process dies the other exits; `runtime_restart` restarts both.
- A **single-process fallback** (`BB_GPU_PROCESS=0`, today's design) stays until the split is
  proven, so the game keeps working throughout.

### 3.7 Build

- Second dependency set for arm64 (`deps/arm64`): SDL3, Vulkan loader, fmt, xxHash, miniz,
  Zydis, FFmpeg if AvPlayer moves. MoltenVK is already universal.
- `build_macos.sh` builds `bbgpu-host` (arm64) from today's `gpu/` sources plus a host `main`,
  and the GPU proxy (x86-64) into `bb-probe`.

## 4. Milestones

| # | Milestone | Exit criterion |
|---|---|---|
| **M0** | **Feasibility spike (1–2 days) — go/no-go** | (a) x86-64 and arm64 processes map one memory entry at identical VAs and exchange data correctly under TSO/weak ordering; (b) doorbell and message round-trip latency measured; (c) **the same synthetic Vulkan workload** (~1,600 draws/frame, push descriptors, ~200 render passes) built x86-64 (Rosetta) and arm64, CPU time per frame compared — this number decides whether the project is worth it |
| M1 | Shared guest memory in the runtime | Game runs single-process on the memory-entry backing (no behaviour change) |
| M2 | arm64 deps, `bbgpu-host` builds, HLE classification | Host starts, opens the window, maps guest memory |
| M3 | Command path | Title screen renders through the host |
| M4 | Page tracking + events across processes | Gameplay renders correctly; no stale-data glitches |
| M5 | Input, overlay, text input in the host | Fully playable |
| M6 | AvPlayer, restart, crash handling, diagnostics | Feature parity with single-process |
| M7 | Zero-copy textures/buffers from shared memory; remove the 16 GiB arena | Lower memory, fewer copies; benchmark |

## 5. Expected gain — honestly

- Rosetta typically runs compiled C/C++ at roughly **60–80% of native speed**; heavily branchy,
  pointer-chasing code like MoltenVK's encoder and the renderer can fare worse, plus the cost of
  TSO mode. So the bottleneck thread could speed up by roughly **1.3–1.7×**, i.e. from 27–30 FPS
  to **~35–50 FPS** at 720p, before zero-copy savings. The spike (M0c) replaces this estimate
  with a measurement before any real porting.
- New costs: page-fault relays and doorbells. Expected small at the measured rates; M0b checks.
- Native 1080p would remain GPU-bound (~22–25 FPS) — upscaling stays the way to play.

## 6. Risks

| Risk | Mitigation |
|---|---|
| TSO vs weak memory ordering bugs (intermittent, hard to see) | Explicit atomics at every handoff; M0a tests; audit renderer polling of guest memory |
| Synchronous HLE calls that need host state become slow | Classification (3.3); status block in shared memory for queries |
| Fault relay latency under heavy write tracking | Batch protection; measure in M0b; zero-copy (M7) reduces tracking |
| Two dependency sets, two processes to debug | Shared build script; host logs prefixed; fallback mode |
| Engineering size (estimate: **several weeks** after a successful spike) | Milestones each leave the game working (fallback mode) |
| It may not be enough on its own | M7 zero-copy and fewer submissions (66/frame) remain available on top |

## 7. Alternatives considered

| Option | Why not first |
|---|---|
| B. Native Metal backend (replace Vulkan + MoltenVK) | Removes MoltenVK's ~51% but keeps Rosetta on the renderer; rewrite of a ~50k-line backend; long time to first pixel. Still possible later inside `bbgpu-host`. |
| More threading/tuning in one process | Tried three ways, no gain: the frame is serialised through translated code. |
| Zero-copy memory alone | Worth doing, but removes copies/spin-waits (~11–15%), not the MoltenVK encode cost. Folded in as M7. |
| Recompiling the game to arm64 | Out of scope (decision 1): no source, static recompilation of the eboot is a research project. |

## 8. Decision requested

1. Approve **M0 (spike)** only — 1–2 days, produces the measurement that decides the rest.
2. Or approve the whole plan conditional on M0 meeting a threshold (suggested: the arm64 build
   of the synthetic workload is **≥ 1.3× faster** than the Rosetta build).
