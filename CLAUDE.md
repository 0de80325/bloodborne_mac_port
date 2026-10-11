# CLAUDE.md — Bloodborne macOS port (session handoff)

Read this first, then `docs/MACOS_PORT.md` (plan, porting table, progress checklist).
Deferred work lives in `docs/MACOS_BACKLOG.md`: when taking a shortcut to get the game running,
log the proper fix there under **1) Stability, 2) Performance, 3) Mac GUI** (priority order).

## What this repo is

A work-in-progress **macOS (Apple Silicon) port** of the native Bloodborne PC port
(`bbport` by deadinside28 → Windows port by Mrsuss60). It is NOT game source code: it is a
loader + PS4 HLE runtime (`src/`) + shadPS4-derived Vulkan GPU core (`gpu/`) that runs the
original PS4 `eboot.bin` (x86-64 machine code) directly on the CPU.

- GitHub: https://github.com/Wooloomooloo2/bloodborne_mac_port (public)
- Remotes: `origin` (this repo), `windows-fork` (Wooloomooloo2/bloodborne_pc_windows_port_arc),
  `upstream` (Mrsuss60/bloodborne_pc_windows_port)
- License GPL-2.0-or-later (shadPS4 code).

## Decisions already made (don't re-litigate)

1. **No recompiling the game.** There is no game source; static x86→ARM recompilation is out
   of scope. Everything (game code + our code) runs as **one x86-64 process under Rosetta 2**.
   Our code is recompiled for `x86_64-apple-macos`, deployment target **15.0** (Rosetta AVX2).
2. **Graphics = staged hybrid:** (1) MoltenVK with the existing Vulkan renderer;
   (2) Metal features from inside it via `VK_EXT_metal_objects` (MetalFX upscaling) and
   `VK_EXT_external_memory_host` (zero-copy guest memory); (3) native Metal only if
   Instruments/Metal System Trace profiling shows MoltenVK overhead is the bottleneck.
3. The Linux code path (`build.sh`, `#ifndef _WIN32` branches) is the starting point for
   macOS, not the Windows one — but borrow Windows' tricks where Linux uses Linux-only APIs.

## Machine & paths

- Host: Apple M4 Max, macOS 26.6, arm64. Rosetta installed. Xcode clang 21. arm64 Homebrew
  at `/opt/homebrew` (has cmake, ninja, nasm, glslang, pkgconf, boost, cryptopp).
- **Game (decrypted, 1.09 merged, verified):** `/Users/markhall/Bloodborne/CUSA03173`
  (`eboot.bin`, `sce_sys/`, `sce_module/`, `dvdroot_ps4/`). `scripts/game_check.py` → OK.
  Original archives/pkgs are in `/Users/markhall/Bloodborne/Bloodborne/` — leave untouched.
  Unpacked update (redundant, kept): `/Users/markhall/Bloodborne/update/`.
- **Never commit game files** (repo is public). `.gitignore` covers `/game/`, `/deps/`, `out/`.

## Done so far (2026-10-11)

- Repo created, plan doc `docs/MACOS_PORT.md`, README header.
- `tools/pkg_extract/` — native fpkg unpacker (shadPS4 PKG/PFS code from before upstream
  removed it in be22674f; 64-bit size fixes; parallel). Built at `out/pkg_extract/pkg_extract`.
- `scripts/macos/bootstrap_deps.sh` — builds pinned **x86-64** deps into `deps/x86_64`
  (SDL3, Vulkan headers+loader 1.4.363, fmt 11, xxHash, miniz, Zydis, FFmpeg 7.1, magic_enum,
  robin-map, VMA, xbyak; boost headers symlinked) + universal MoltenVK 1.4.2 dylib & ICD json.
  Already run successfully; re-running is a no-op (stamps in `deps/x86_64/.built-*`).
- Verified: x86-64 test binary under Rosetta sees "Apple M4 Max, Vulkan 1.3" via MoltenVK.
  MoltenVK lacks `geometryShader` and `logicOp`.
- **`build_macos.sh`** builds everything x86-64 (`--gpu-only`, `--test`): `out/gpu/libbbgpu.dylib`
  (`gpu/CMakeLists.txt` APPLE branch: no X11, `-undefined dynamic_lookup`, Metal surface via
  `SDL_Metal_CreateView`), `out/bb-probe`, `out/bb-gpu-capabilities`. All 5 test suites pass
  under Rosetta; bb-gpu-capabilities finds the M4 Max. PGO is off on macOS (GCC .gcda profiles).
- **Runtime port compiled and unit-tested** (not yet booted with the game):
  - Guest TLS: loader rewrites `fs:[0]` → `gs:[0xc00]` (`--gs-slot`/`BBPORT_GS_SLOT`, default
    384 on darwin); `runtime_thread.c` constructor reserves pthread key **384** (first free key is
    258 under Rosetta) and `set_tls_base` stores the TCB there and verifies the read-back.
  - Memory (`runtime_memory.c`): **[0xfc0000000, 0x7000000000) is a permanent kernel hole** on
    macOS, so guest `USER_MIN` = 0x7000000000 there; whole guest range reserved PROT_NONE up front
    (probe.c `main`), backing = one anonymous region aliased with `mach_vm_remap`, hole-punch =
    fixed anonymous remap. Low maps in [0x800000000, 0xfc0000000). Page size under Rosetta = 4K.
  - Write faults arrive as SIGBUS on macOS (handled). Thread dumps via `task_threads`.
  - No missing `runtime_low_alloc` routing found: guest-visible objects already use it.

## Boot status (2026-10-11)

- `--cpu-only`: guest code runs (TLS, memory, ~30 threads, FMOD audio via CoreAudio) up to
  graphics init, then faults on NULL objects from unbound `sceGnm*` imports (expected on any
  platform: cpu-only skips `bbgpu_init`, which registers the GPU HLE symbols).
- GPU boot (`run.sh`, no flag): window + Vulkan instance OK; stops in `Instance::CreateDevice`
  (`vk_instance.cpp` ~272): MoltenVK lacks **`robustBufferAccess2`** and **`nullDescriptor`**
  (has `robustBufferAccess`, `robustImageAccess2`).
- 2026-10-11 later: device created (Option A: no robustBufferAccess2/nullDescriptor, zero
  stand-in buffer); buffer cache uses one ordinary 16 GiB arena at 0x7000000000 on macOS (no
  sparse binding in MoltenVK; Metal commits memory lazily, measured). The window opens, the
  DualSense connects, the game loads textures, then a worker thread faults: its job-pool loop
  (guest 0x263b246) virtual-calls a malformed job object (one pointer level too many; target in
  .data 0x53b3260). Root cause not found yet — suspects: macOS memory aliasing bug in
  flexible memory (0x7109…), or an HLE call behaving differently. Fault handler now prints
  rax/rbx/rdi/rsi/rsp; `BB_TRACE=1` adds import logs and thread snapshots.
- MoltenVK cannot create BC1/BC7 images with Storage usage (`image.cpp:185` errors) — next
  rendering gap after the crash.
- A stale shader cache from a crashed run fails `serdes.h:109`; delete it. Test runs without
  `<data>/user` existing put GPU caches in `~/Library/Application Support/shadPS4` (path_util
  reads `BB_GPU_USER_DIR` at static init, before bbgpu_init sets it) — `mkdir out/macdata/user`.
- macOS main thread: Cocoa needs the real main thread, so `probe.c` `main()` runs the game on a
  64 MiB-stack thread and runs `CFRunLoopRun()`; `bbgpu_init` posts the window loop to the main
  dispatch queue. (`dispatch_main()` does NOT work: SDL then reports "No available video device".)
- Run: `BB_DATA_DIR=$PWD/out/macdata BB_GAME_DIR=/Users/markhall/Bloodborne/CUSA03173
  BB_TIMEOUT=60 bash run.sh [--cpu-only]`; `BB_TRACE=1` logs unbound import calls.

## Next steps (in order)

1. Find the worker-thread job fault above (systematic debugging; compare memory aliasing).
2. Null image bindings: 1x1 stand-in images (Option A, not done yet), BC + Storage on MoltenVK.
3. Continue the GPU boot to the title screen; fix rendering gaps (geometry shaders, D24S8 → D32S8).
4. Hybrid Metal: MetalFX upscaler, zero-copy guest memory. Then launcher / `.app` bundle.

## Gotchas learned

- zsh: `"$VAR:src/..."` triggers zsh's `:s` history modifier — write `"${VAR}:src/..."`.
- Run Vulkan programs with `VK_DRIVER_FILES=$PWD/deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json`
  and request `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
- Builds must not pick up arm64 libs from `/opt/homebrew` (bootstrap sets
  `CMAKE_IGNORE_PREFIX_PATH` and an isolated `PKG_CONFIG_LIBDIR`; do the same for bbport).
- Existing docs in `docs/` (ROADMAP, history) are the upstream project's, partly in Russian.
- **Rosetta + aliased memory:** code written through one alias and executed from another keeps
  the stale translation until a protection change (RW→RX) on the executing alias.
- macOS timed mutex/rwlock waits are trylock+backoff (up to ~1 ms late); `CLOCK_MONOTONIC`
  keeps counting during system sleep.
- Several Windows-fork regressions also broke Linux builds (`runtime_file.c` `DWORD` log,
  `runtime_pad.c` missing `struct stat`, tests needing `restore_guest_fs`); fixed here.
