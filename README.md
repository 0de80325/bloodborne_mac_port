# bloodborne_mac_port: Bloodborne on macOS (Apple Silicon)

A **work-in-progress macOS port** of the native Bloodborne PC port. It runs the original PS4
executable of *Bloodborne* (CUSA03173, version 1.09) on Apple Silicon Macs: the game's own
x86-64 code executes directly under **Rosetta 2**, PS4 system libraries are replaced by a
lightweight HLE runtime written in C, and GPU command buffers are translated to Vulkan by a
**shadPS4-derived video core** running on **MoltenVK**.

This is not an emulator and contains no game code: the loader, runtime and GPU core are built
as one x86-64 macOS process that loads your own copy of the game.

> **No game files or copyrighted assets are included, and none will ever be distributed.**
> You must provide your own legally dumped and decrypted copy of Bloodborne (CUSA03173 v1.09).
> This project is not affiliated with or endorsed by Sony Interactive Entertainment,
> FromSoftware, or AMD.

## Status (2026-10-11)

**Work in progress.** On an Apple M4 Max the game boots, character creation works and the
first level is playable. Expect rendering gaps, missing features and crashes.

- Plan, design decisions and porting checklist: [docs/MACOS_PORT.md](docs/MACOS_PORT.md)
- Known issues and planned refinements: [docs/MACOS_BACKLOG.md](docs/MACOS_BACKLOG.md)
- Tests run so far and their results: [docs/FINDINGS.md](docs/FINDINGS.md)

## How it works

- **Native execution under Rosetta 2.** The eboot is converted offline into a flat memory
  image; PS4 libc and libSceFios2 functions are linked directly into it. There is no CPU
  emulation layer of our own — Rosetta 2 translates the x86-64 code (game + runtime) once.
  The build targets `x86_64-apple-macos` with deployment target 15.0 (Rosetta AVX2 support).
- **HLE runtime** (`src/`): kernel, memory, threads, audio, input (SDL3), save data and other
  PS4 libraries. macOS specifics: guest TLS via a reserved pthread TSD slot, guest-visible
  memory kept below 1 TiB, Mach-based thread inspection.
- **GPU** (`gpu/`): the shadPS4 video core and shader recompiler, built as
  `libbbgpu.dylib`, rendering through MoltenVK (Vulkan on Metal).
- **Planned:** Metal features from inside the Vulkan renderer (MetalFX upscaling, zero-copy
  guest memory on unified memory), and native Metal only where profiling shows MoltenVK
  overhead is the bottleneck.
- Inherited from upstream: high-frame-rate community patches, synthetic motion vectors and
  temporal upscaling (FSR 3.1), a two-stage multi-threaded draw pipeline, and an in-game
  settings overlay (`Insert` or `L3 + R3`).

## Requirements

- A Mac with **Apple Silicon** running **macOS 15 or newer**
- **Rosetta 2** (`softwareupdate --install-rosetta`)
- **Xcode Command Line Tools** (`xcode-select --install`)
- **Homebrew** (arm64) with build tools:
  `brew install cmake ninja nasm glslang pkgconf boost cryptopp`
- **Python 3** (patch compiler and eboot preparation scripts)
- **Your own decrypted game dump**: a `CUSA03173` folder (v1.09) containing `eboot.bin`,
  `sce_sys/`, `sce_module/` and `dvdroot_ps4/`. `python3 scripts/game_check.py` verifies it.

## Quick start

```sh
git clone --recursive https://github.com/Wooloomooloo2/bloodborne_mac_port.git
cd bloodborne_mac_port

# 1. Build pinned x86-64 dependencies (SDL3, Vulkan loader, FFmpeg, ...) and MoltenVK into deps/
bash scripts/macos/bootstrap_deps.sh

# 2. Build the GPU library and the loader (out/gpu/libbbgpu.dylib, out/bb-probe)
bash build_macos.sh

# 3. Prepare the game image and launch (rebuilds if needed)
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

`run.sh` points the Vulkan loader at the bundled MoltenVK ICD automatically. Saves and shader
caches go to `user/`, settings to `bbport.ini`.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and PS4 HLE runtime |
| `gpu/` | shadPS4-derived Vulkan video core, draw pipeline, upscaler integration |
| `scripts/` | Eboot preparation, linking, patch compiler (`patches.py`); `scripts/macos/` dependency bootstrap |
| `patches/` | Community XML patches for Bloodborne 1.09 |
| `launcher/` | GTK launcher from the Linux port (Linux reference path) |
| `tools/` | Developer utilities, including `pkg_extract` (fpkg unpacker) |
| `docs/` | macOS plan and backlog, plus technical notes inherited from upstream |

## Credits

This port stands entirely on the work of others:

- **deadinside28** — author of `bbport`, the original native Linux port of Bloodborne: the flat memory image approach, the HLE
  runtime, the two-stage Vulkan draw pipeline, synthetic motion vectors and the upscaler
  integration.
- **Mrsuss60** — [bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port),
  the Windows port this repository was forked from, including its stability, audio, memory
  and frame-pacing fixes (documented in that repository's `CHANGES.md`).
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** project — the Vulkan video core and
  shader recompiler (`gpu/shadps4/`, GPL-2.0-or-later).
- **FireBurn** — [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), the native Vulkan
  FSR 3.1 / FSR 4 runtime (MIT; includes AMD FidelityFX SDK sources under MIT).
- **Omar Cornut** — [Dear ImGui](https://github.com/ocornut/imgui), used for the in-game overlay (MIT).
- **Alex Barney (Thealexbarney)** — [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9),
  AT9 audio decoding (MIT).
- **sirit** — SPIR-V emitter used by the shader recompiler (BSD-3-Clause), with Khronos
  **SPIRV-Headers** (MIT-style Khronos license).
- **Christian Rau** — [half](https://half.sourceforge.net/), IEEE 754 half-precision
  library (MIT).
- **AMD** — GCN PM4/register headers in `gpu/third_party/gcn/` (MIT-style AMD license).
- **DejaVu fonts** (Bitstream Vera license) — overlay font.
- **[MoltenVK](https://github.com/KhronosGroup/MoltenVK)** (Apache-2.0), and the dependencies
  built by `scripts/macos/bootstrap_deps.sh`: SDL3, Vulkan-Headers / Vulkan-Loader, fmt,
  xxHash, miniz, Zydis, FFmpeg, magic_enum, robin-map, Vulkan Memory Allocator, xbyak, Boost.
- **Community patch authors** — Kyo, Lance McDonald, illusion, emoose, auser1337 and others
  for the 60 FPS, camera and engine patches in `patches/Bloodborne.xml`.
- **zackcage6** — testing and bug reports for the Windows port.

Each third-party component keeps its own license file in its directory.

## License

GNU General Public License v2.0 or later (GPL-2.0-or-later), because of the shadPS4
components. See [LICENSE](LICENSE).
