# bloodborne_mac_port: Bloodborne on macOS (Apple Silicon)

A **work-in-progress macOS port** of the native Bloodborne PC port. It runs the original PS4
executable of *Bloodborne* (CUSA03173, version 1.09) on Apple Silicon Macs: the game's own
x86-64 code executes directly under **Rosetta 2**, PS4 system libraries are replaced by a
lightweight HLE runtime written in C, and GPU command buffers are translated to Vulkan by a
**shadPS4-derived video core** running on **MoltenVK**.

This is not an emulator and contains no game code: the loader and runtime run your own copy of
the game as an x86-64 process under Rosetta 2, and rendering happens in a separate native arm64
process.

> **No game files or copyrighted assets are included, and none will ever be distributed.**
> You must provide your own legally dumped and decrypted copy of Bloodborne (CUSA03173 v1.09).
> This project is not affiliated with or endorsed by Sony Interactive Entertainment,
> FromSoftware, or AMD.

## Status (2026-10-13)

**Work in progress, playable.** On an Apple M4 Max the game boots, character creation works,
saves load, and Central Yharnam runs at a **locked 60 FPS** at 1080p — natively or with MetalFX
upscaling. Expect occasional rendering glitches and crashes (see the backlog).

The target is a locked 60 FPS (the game's physics run at 30 Hz, so more is not wanted), at the
highest resolution each Mac allows. Measured on the M4 Max, frame rate uncapped:

| Output | Renders at | FPS | Limited by |
|---|---|---|---|
| 1080p, MetalFX Quality | 1280×720 | 65–77 | — (60 locked) |
| 1080p native | 1920×1080 | 61–63 | GPU timeline (render-pass overhead) |
| 4K, MetalFX Performance | 1916×1078 | ~52 | GPU / pacing |
| 1440p native | 2560×1440 | 49 | GPU (~17.5 ms/frame; 60 needs ≤ 15) |
| 4K, MetalFX Quality | 2560×1440 | 43 | GPU |

Goals: M4 Max 1440p native and 4K upscaled at 60; base M5 1080p native; smaller Macs 1080p
upscaled.

### From 22 to 60 FPS

The first playable build (2026-10-11) ran the first level at **~22 FPS** at native 1080p. What
got it to 60, roughly in order:

| Change | Effect |
|---|---|
| **MetalFX temporal upscaling** (via `VK_EXT_metal_objects`) alongside FSR 3.1 | Render at 720p, upscale to 1080p: 22 → ~30 FPS at the time |
| **Native arm64 GPU process.** The game still runs as x86-64 under Rosetta 2, but all rendering moved into a separate native `bbgpu-host` process sharing the game's memory (Mach memory entry, ring buffers, cross-process page-protection relays) | MoltenVK's Vulkan→Metal encoding costs **1.75× less CPU** natively; now the default |
| **Tiny render area for passes with no output.** ~20 vertex-only passes per frame were tiled over a 16384×16384 area | 27.5 → 33.9 FPS in the city |
| **Upload hoisting.** Buffer uploads recorded ahead of the submission instead of splitting render passes | 33.9 → 34.8 FPS, fewer G-buffer pass splits |
| **Pipeline cache kept between sessions.** It was wiped every launch (~500 shader compiles each session; the "stutter when attacking") | 2 compiles per session instead of ~500 |
| **No "precise" float math in pixel/compute shaders.** SPIR-V `NoContraction` became `[[clang::optnone]]` helper calls for every float operation in Metal (223 of 308 shaders) | City respawn spot **39 → 60 FPS**, GPU 19.6 → 12.1 ms per frame |
| Shader permutation fix (Metal rejected ~6 pipelines per session, their draws vanished); unnamed pooled command buffers (MoltenVK labelled every encoder, ~4% CPU) | Correct image; less CPU per frame |
| **Shaders compile on background threads** (4 low-priority threads). New option `shader_compile=wait` (brief hitch, complete image) or `skip` (no hitch, an object may pop in) in `bbport.ini` and the in-game menu | Level load with an empty cache: render thread compile time 440 → 130 ms; worst frame 248 → 76 ms with `skip` |

Also fixed along the way: a MoltenVK crash from two threads sharing one command pool, and
geometry corruption at 1440p (scenes above 1080p now use live resolution changes).

### What's next

- **Fewer Metal render passes.** Profiling found ~408 Metal encoders per frame (190 render
  passes). Each extra render pass costs ~13–16 µs of GPU time on Apple GPUs regardless of
  resolution — about 3 ms per frame — and leaves the GPU idle in periodic gaps. Merging passes
  is the main lever for 1440p and 4K at 60.
- Remaining stability issues: an intermittent "PM4 type 0" crash while loading, and a
  self-closing window (both instrumented).
- A launcher and a `.app` bundle.

Details:
- Plan, design decisions and porting checklist: [docs/MACOS_PORT.md](docs/MACOS_PORT.md)
- Known issues and planned refinements: [docs/MACOS_BACKLOG.md](docs/MACOS_BACKLOG.md)
- Every test and measurement: [docs/FINDINGS.md](docs/FINDINGS.md)
- The two-process design: [docs/DESIGN_NATIVE_GPU_PROCESS.md](docs/DESIGN_NATIVE_GPU_PROCESS.md)

## How it works

- **Native execution under Rosetta 2.** The eboot is converted offline into a flat memory
  image; PS4 libc and libSceFios2 functions are linked directly into it. There is no CPU
  emulation layer of our own — Rosetta 2 translates the x86-64 code (game + runtime) once.
  The build targets `x86_64-apple-macos` with deployment target 15.0 (Rosetta AVX2 support).
- **HLE runtime** (`src/`): kernel, memory, threads, audio, input (SDL3), save data and other
  PS4 libraries. macOS specifics: guest TLS via a reserved pthread TSD slot, guest-visible
  memory kept below 1 TiB, Mach-based thread inspection.
- **GPU** (`gpu/`): the shadPS4 video core and shader recompiler, built as
  `libbbgpu.dylib`, rendering through MoltenVK (Vulkan on Metal). By default it runs in
  `bbgpu-host`, a **native arm64 process** that maps the game's memory at the same addresses
  (`host/`, `docs/DESIGN_NATIVE_GPU_PROCESS.md`); `BB_GPU_PROCESS=0` runs everything in one
  process.
- **Metal features from inside the Vulkan renderer:** MetalFX temporal upscaling. Planned:
  zero-copy guest memory on unified memory; native Metal only where profiling shows MoltenVK
  overhead is the bottleneck.
- Inherited from upstream: high-frame-rate community patches, synthetic motion vectors and
  temporal upscaling (FSR 3.1), a two-stage multi-threaded draw pipeline, and an in-game
  settings overlay (`⌘,` or `L3 + R3`).

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

# 1. Build pinned dependencies (SDL3, Vulkan loader, FFmpeg, ...) and MoltenVK into deps/:
#    x86-64 for the game process, arm64 for the native GPU host
bash scripts/macos/bootstrap_deps.sh
BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh

# 2. Build the loader, the GPU library and the GPU host
#    (out/bb-probe, out/gpu/libbbgpu.dylib, out/bbgpu-host)
bash build_macos.sh

# 3. Prepare the game image and launch (rebuilds if needed)
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

`run.sh` points the Vulkan loader at the bundled MoltenVK ICD automatically. Saves and shader
caches go to `user/`, settings to `bbport.ini` (also editable in the in-game menu, `⌘,`). The
first launch, and the first visit to each area, compiles shaders; later sessions load them at
start.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and PS4 HLE runtime |
| `gpu/` | shadPS4-derived Vulkan video core, draw pipeline, upscaler integration |
| `host/` | Native arm64 GPU host process (`bbgpu-host`) and the shared rings |
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
