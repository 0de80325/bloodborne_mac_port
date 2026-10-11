# macOS Port Plan (Apple Silicon)

Status: **work in progress — does not run yet.**

## Approach

The game's code is the original PS4 `eboot.bin` (x86-64 machine code); there is no game
source, so it cannot be recompiled for ARM64. The loader, HLE runtime and GPU core are
therefore built as an **x86-64 macOS binary** and the whole process runs under **Rosetta 2**
(AVX2 is supported by Rosetta on macOS 15+).

Graphics use a staged hybrid plan:

1. **MoltenVK** (Vulkan on Metal) with the existing shadPS4-derived Vulkan renderer.
2. **Metal from inside the Vulkan renderer** via `VK_EXT_metal_objects`: MetalFX
   upscaling, zero-copy guest memory (`newBufferWithBytesNoCopy` on unified memory).
3. **Native Metal backend** only where profiling (Instruments / Metal System Trace) shows
   MoltenVK translation overhead is the bottleneck.

## Porting work

| Area | Linux/Windows today | macOS plan |
|---|---|---|
| Guest TLS (`fs:[0]`) | rewritten to `gs:[0]` + `arch_prctl` (Linux), TEB slot (Windows) | rewrite to `gs:[key*8]`: a reserved pthread TSD slot holding the guest TCB |
| Low (<1 TiB) host memory | `MAP_FIXED_NOREPLACE`, brk heap via `mallopt` | `mmap` with hint + verify; route guest-visible host allocations through `runtime_low_alloc` |
| Fault handling | `ucontext gregs[REG_RIP]` | `uc_mcontext->__ss.__rip` |
| Thread dumps | `getdents64` + `tgkill` | `task_threads` / `pthread_kill` |
| Restart | `close_range` | close loop / `posix_spawn` |
| Futex waits | `futex` syscall | `os_sync_wait_on_address` / `__ulock_wait` |
| Upscaling | FSR 3.1 / FSR 4 | FSR 3.1, then MetalFX (FSR 4 not expected to work) |

## Toolchain

- Xcode command line tools (Apple clang, targeting `x86_64-apple-macos`)
- x86-64 Homebrew under `/usr/local` (Rosetta) for x86-64 SDL3 / MoltenVK / Vulkan loader
- CMake, Ninja, Python 3.10+

## Unpacking the game (`tools/pkg_extract`)

A fake-signed `.pkg` dump can be unpacked natively with the bundled extractor (shadPS4's
PKG/PFS code from before its removal, with 64-bit file sizes):

```sh
cmake -S tools/pkg_extract -B out/pkg_extract -G Ninja && ninja -C out/pkg_extract   # needs: brew install cryptopp pkgconf
out/pkg_extract/pkg_extract <game.pkg>   <dir>                   # -> <dir>/CUSA03173
out/pkg_extract/pkg_extract <update.pkg> <dir2>/CUSA03173-patch  # then copy its contents over the game folder
python3 -c "import sys; sys.path.insert(0,'scripts'); import game_check; print(game_check.problem('<dir>/CUSA03173') or 'OK')"
```
