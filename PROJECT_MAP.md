# Bloodborne macOS Port — Project Map (this machine)

Machine: Apple M2 Pro, 16 GB unified (12 GB GPU heap), macOS 27.0.1.
Date: 2026-10-15. Status: boots, menu navigable when memory allows (~3 FPS);
in-game entry works; needs zero-copy (Phase 1 in progress) for real speed.

## Repositories

| Path | Remote / branch | State |
|---|---|---|
| `/Users/onur/Documents/GitHub/bloodborne_mac_port` | `origin` = `0de80325/bloodborne_mac_port` (your fork), `upstream` = `Wooloomooloo2/bloodborne_mac_port`; branch `zerocopy-m2` | **Active work here.** Unpushed local commits only, no PRs. |
| `/Users/onur/bloodborne_mac_port` | `origin` = `Wooloomooloo2/bloodborne_mac_port`; branch `master` @ `8111495` | Pristine upstream + built binaries. Do not modify. |

## Game data (not in any repo, never commit)

| Path | Contents |
|---|---|
| `/Users/onur/Bloodborne/CUSA03173` | Playable game folder, 30 GB. Base 1.00 + 1.09 update merged. Verified `OK` (`CUSA03173` v `01.09`): `eboot.bin`, `sce_sys/`, `sce_module/`, `dvdroot_ps4/`. Used via `BB_GAME_DIR`. |
| `/Users/onur/Downloads/Bloodborne_GOTY/Bloodborne GOTY.pkg` | 29 GB base-game package (source). |
| `/Users/onur/Downloads/Bloodborne_GOTY/Extra/Bloodborne [UPDATE 1.09][CUSA03173](axekin.com).pkg` | 173 MB update package (source of the merge). |
| `/Users/onur/Downloads/Bloodborne_GOTY/Extra/Bloodborne_Game_of_The_Year_Edition_[DLC](axekin.com).zip` | 128 KB `CUSA00900` license-only pkg — **ignored** (wrong title, no content; GOTY base has the DLC data). |
| `/Users/onur/Downloads/Bloodborne_GOTY/TOOLS_INSTALLED.md` | Log of every tool install for this project (Homebrew etc.), kept updated. |
| `~/Library/Application Support/Bloodborne/` | Saves, `bbport.ini` (currently: `upscaler=fsr3`, `preset=1`, `output_res=1920x1080`, `live_resolution=0`, `shader_compile=skip`), shader/pipeline caches (`user/cache/`), per-run `out/` (boot images). |
| `~/Library/Logs/Bloodborne/` | `game.log` / `game.previous.log` (app launches). |

## Build outputs (per repo, gitignored)

| Path | Contents |
|---|---|
| `<repo>/deps/x86_64` | Pinned x86-64 deps for the game process (SDL3, Vulkan loader, FFmpeg, MoltenVK universal, …). Built by `scripts/macos/bootstrap_deps.sh`. |
| `<repo>/deps/arm64` | Same set native arm64 for `bbgpu-host` (`BB_DEPS_ARCH=arm64 …`). |
| `<repo>/out/bb-probe` | Game binary, x86-64 under Rosetta. |
| `<repo>/out/bbgpu-host` | Native arm64 GPU host. |
| `<repo>/out/gpu/libbbgpu.dylib` | GPU library, x86-64 (single-process fallback). |
| `<repo>/out/gpu-arm64/libbbgpu.dylib` | GPU library, arm64 (native host). |
| `<repo>/out/bb-gpu-capabilities` | GPU capability probe (`--live-resolution`). |
| `<repo>/out/pkg_extract/pkg_extract` | Fake-signed PKG unpacker (built from `tools/pkg_extract`, needs `cryptopp`). |
| `<repo>/Bloodborne.app` | Double-click launcher (built by `scripts/macos/make_app.sh`; needs `launch.conf`). |
| `<repo>/launch.conf` | `BB_GAME_DIR=/Users/onur/Bloodborne/CUSA03173` (not committed). |

Only the **original** repo currently has all of the above built. The **fork** has fresh `deps/` (both archs); its `out/` was built during Phase 1 work.

## Diagnostics (this machine)

| Path | Contents |
|---|---|
| `/tmp/bb-run*.log`, `/tmp/bb-zc*.log`, `/tmp/bb-cap.log`, `/tmp/bb-trace1.log` | Per-run game logs (`BB_FRAME_STATS=1` Spike/frame stats in most). |
| `/tmp/bb-sample*.txt`, `/tmp/bb-sample-probe.txt` | `sample` profiles (`bbgpu-host`, `bb-probe`). |
| `/tmp/bb-frame.gputrace` | 8.8 GB Metal frame capture (present-only frame; timing not exposed in this Xcode's viewer). |
| `/tmp/bb-metal*.trace`, `/tmp/t-validate.trace` | Empty xctrace captures (attach records nothing here even with Developer Mode on). |
| `/tmp/bb-run.log`, `/tmp/bb-run2.log` | Early runs (reference). |
| `/tmp/encoders.xml`, `/tmp/probe-out.txt`, `/tmp/range-probe*` | Scratch from diagnosis (range reservation probes). |

## Docs (in repo)

| Path | Contents |
|---|---|
| `README.md` | User-facing: what it is, perf history (M4 Max numbers), build/run instructions. |
| `CLAUDE.md` | Session handoff: decisions, machine paths (note: references the previous owner's `/Users/markhall` paths — stale for this machine), status. |
| `docs/MACOS_PORT.md` | Port plan, toolchain, PKG unpacking recipe. |
| `docs/DESIGN_NATIVE_GPU_PROCESS.md` | Two-process design (our prefix-reservation note added §3.1). |
| `docs/MACOS_BACKLOG.md` | Stability/perf/GUI backlog (our arena-window note updated). |
| `docs/FINDINGS.md` | Every test/measurement log (upstream). |
| `tools/zerocopy-spike/README.md` | Phase 0 spike build/run (IMPORT-OK on this M2 Pro). |

## Our changes (fork `zerocopy-m2`, unpushed)

1. Native GPU host prefix reservation (`host/main.c`, `host_runtime.c`, `host.h`) + `host_view_span` view lookup + single-process stub (`src/gpu_process.c`).
2. Per-page 4 GiB GPU arenas (`gpu/.../buffer_cache/buffer_cache.cpp`, + backlog doc).
3. Zero-copy Phase 1 (`zerocopy.h/.cpp`, `buffer.h/.cpp`, `buffer_cache.h/.cpp`): `BB_ZERO_COPY=1` imports pool pages via `VK_EXT_external_memory_host`, arena fallback, LRU + GPU-safe teardown, BDA integration.

## Launch recipe (current best, from either repo with built `out/`)

```sh
BB_GAME_DIR=/Users/onur/Bloodborne/CUSA03173 BB_GPU_PROCESS=1 BB_ARENA_GIB=8 \
BB_HIGH_DPI=0 BB_FRAME_STATS=1 BB_SHADER_COMPILE=skip BB_UPSCALER=fsr3 \
BB_UPSCALE_PRESET=1 BB_ZERO_COPY=1 BB_LIVE_RES=1 bash run.sh
```

Keyboard (no controller needed): `WASD` move, `IJKL`/arrows menu + D-pad,
`Space`/`Return` confirm, `Shift`/`Esc` back, `E`/`Q` Square/Triangle,
`F1`/`O` options, `⌘,` overlay (`⌃⌘F` fullscreen).

## Machine-specific facts established here

- Native arm64 processes have pre-existing mappings inside `[0x7000000000, …)` (malloc zones, ASLR-placed) → single 563 GiB reservation impossible; prefix works, sometimes loses the lottery → single-process fallback.
- 12 GiB heap: 16 GiB arena fails at creation, 8 GiB thrashes, 4 GiB fits but game needs ~4.5 GiB (entry) → crash. Pool 6 GiB + arena ≈ working set edge.
- `bbport.ini` (saved by game) drives `run.sh` into the 9152 MiB direct-memory branch; `BB_LIVE_RES=1` keeps the 6 GiB pool.
- Fence waits (200–600 ms, 7 draws) dominate slow frames; uploads/compiles ~0; display lag tracks pressure. No driver shader compiles in traces; xctrace records nothing here.
