# CLAUDE.md — Bloodborne macOS port (session handoff)

Read this first, then `docs/MACOS_PORT.md` (plan, porting table, progress checklist).

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

## Next steps (in order)

1. **macOS build script** (`build_macos.sh` or extend `build.sh`): configure `gpu/CMakeLists.txt`
   for APPLE (it currently assumes WIN32 or Linux+X11: drop X11, use `-undefined dynamic_lookup`
   instead of `--allow-shlib-undefined`, `VK_USE_PLATFORM_METAL_EXT`, no `-no-pie`/`-rdynamic`
   GNU-isms as needed), `PKG_CONFIG_LIBDIR=deps/x86_64/lib/pkgconfig`, `-arch x86_64`.
2. **Port the runtime (`src/`)** — see the porting table in `docs/MACOS_PORT.md`:
   - Guest TLS: loader rewrites `mov rax, fs:[0]` → `gs:[0]` (`scripts/link_modules.py:79`,
     `scripts/link_libc.py:161`). On macOS GS = pthread TSD array, so reserve a pthread key and
     rewrite to `gs:[key*8]`, store guest TCB in that slot (`src/runtime_thread.c` `set_tls_base`).
     Windows does the same trick with a TEB slot — mirror it.
   - Guest-visible host memory must be < 1 TiB (40-bit packed pointers): `runtime_low_map`
     (`src/runtime_memory.c:593`) uses `MAP_FIXED_NOREPLACE`; Linux also relies on `mallopt`
     brk heap — macOS malloc is far above 1 TiB, so route through `runtime_low_alloc`.
   - Linux-only: `arch_prctl`, `getdents64`/`tgkill` thread dumps, `close_range`, `mallopt`,
     `<malloc.h>`, `<asm/prctl.h>`, futex in `src/host_sync.h`, `ucontext gregs[REG_RIP]`
     (→ `uc_mcontext->__ss.__rip`) in `src/probe.c`.
3. Boot with `--cpu-only` (no GPU) until the game reaches its main loop.
4. Enable GPU via MoltenVK; fix rendering gaps (geometry shaders, D24S8 → D32S8, etc.).
5. Hybrid Metal: MetalFX upscaler, zero-copy guest memory. Then launcher / `.app` bundle.

## Gotchas learned

- zsh: `"$VAR:src/..."` triggers zsh's `:s` history modifier — write `"${VAR}:src/..."`.
- Run Vulkan programs with `VK_DRIVER_FILES=$PWD/deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json`
  and request `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
- Builds must not pick up arm64 libs from `/opt/homebrew` (bootstrap sets
  `CMAKE_IGNORE_PREFIX_PATH` and an isolated `PKG_CONFIG_LIBDIR`; do the same for bbport).
- Existing docs in `docs/` (ROADMAP, history) are the upstream project's, partly in Russian.
