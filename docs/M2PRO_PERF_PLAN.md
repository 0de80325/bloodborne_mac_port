# M2 Pro Performance Plan — Memory first, then GPU

Status: **active** (resumed 2026-10-17 — merged upstream **mac-v0.4.0**, the launcher, into the
fork and rebuilt all three binaries; renderer unchanged, release notes say performance is the
same as 0.3.3, so our Phase 0 work carries over).
Repo: fork `bloodborne_mac_port`, branch `zerocopy-m2` (`origin` has the 0.3.3 merge; the
2026-10-16/17 notes + the 0.4.0 merge are local until pushed).
Machine: Apple M2 Pro, 16 GB RAM (12 GiB GPU heap), macOS 27.0.1.
Engine: upstream merges `mac-v0.3.3` (`5292f9e`, 2026-10-16) and `mac-v0.4.0` (`5521d99`,
2026-10-17: launcher, `BB_FPS_LIMIT`/`BB_FULLSCREEN`/`BB_SAVE_DIR`, settings keep unknown
keys). Our parts kept: prefix reservation, per-page 4 GiB arenas, zero-copy phase 1.

**Resume here → Phase 0, step G1:** reboot, start the baseline run with
`BB_TOGGLE_FILE=$PWD/out/toggles` (file created, currently `0`), stand in the heavy view, turn
the camera (smears expected), then flip bit `16777216` (FSR off) / `33554432` (jitter off) and
re-test — one run, no restarts. `bbport.ini` `fps_cap`/`fullscreen` now feed
`BB_FPS_LIMIT`/`BB_FULLSCREEN` via run.sh (environment wins; our recipes set their own).

**Upstream watch (2026-10-17).** `mac-v0.4.0` (2026-10-16) is the **SwiftUI launcher**
(fullscreen, FPS cap 30–120, upscaler/effects/skip-intro settings, save folder handling) —
release notes say *performance is the same as 0.3.3*; its 15 commits are now merged into our
fork (`5521d99`). The `metal-native` branch is the future **0.5.0 native Metal renderer** (112
commits, still active as of 2026-10-17 06:13): TAA/MetalFX/FSR 3.1 on Metal, motion passes,
compute-emulated geometry-shader draws (the ones MoltenVK skips) — but currently **~11% slower
than 0.4.0**, GPU-bound, the gap diagnosed as Metal hazard tracking on the guest arena + fault
buffer (experiments with those untracked reach ~110–120 FPS). M2-class slowness is upstream
**issue #2** (M2 Max at 2–3 FPS): acknowledged as their bug, they expect ~65–75 FPS at 1080p on
an M2 Max once fixed. Our machine reserves the guest range and runs the split GPU process
normally (no fallback), so if we still see 9–21 FPS in-game it is worth comparing against a
stock 0.4.0 run and/or contributing our `BB_FRAME_STATS` log to issue #2.
Smoke result (no reboot, dirty machine): **menus 60–82 FPS** (was 0.4–1.9), light scene 60,
plaza (1764 draws) 23.9, per-draw recording 20.9–2166 µs (was 3.1k–44k). **The merge was the
fix — MoltenVK argument buffers were the primary pathology, not memory.**
Open problems (ordered):
1. **Visual glitches (confirmed by the user):** the 3D world smears/washes/blacks out while
   the camera **moves**; a stationary camera renders correctly (proven by `bb-still1..6.png`
   vs `bb-bad1..8.png` in the temp dir). Points at the temporal/upscaler path (FSR 3.1 +
   live-res, window resized mid-session) — see **Phase 0**. Zero-copy churn stays suspect #2.
2. Heavy areas: a later area (1542 draws/frame) ran 9–10 FPS — GPU 78% busy *and* recording
   ~63 µs/draw, ~49 ms/frame kernel time under swap → heaviness + memory overcommit (§ B
   stays relevant for long sessions).
Goal: **playable framerates + a correct image on this machine** — ≥ 30 FPS menus,
≥ 24 FPS Dream / ≥ 20 FPS Central Yharnam at 720p-class render (FSR3 → 1080p output).

The plan is ordered so that **cheap configuration experiments come first** (no rebuilds),
instrumentation and code changes only where a measurement says they're needed.

---

## 0. TL;DR of the diagnosis (why we think 3 FPS is not "M2 Pro too weak")

> **Verdict, 2026-10-16 — partially falsified; kept for history.** The merge (MoltenVK
> argument buffers off by default) alone took menus from 0.4–1.9 to 60–82 FPS on this box,
> so the dominant pathology was the argument-buffer path on M2-class GPUs, not the memory
> system. The footprint experiments (Phase B) stay relevant only for the long-session creep
> (swap / kernel time) and are now demoted below the glitch investigation (Phase 0).

Evidence from this machine's logs (`/tmp/bb-zc6.log`, `/tmp/bb-zc7.log`, `/tmp/bb-cap.log`):

| Observation | Value | Meaning |
|---|---|---|
| Menu scenes at **14–160 draws** | 550–2400 ms/frame, 0.4–1.9 FPS | A 14-draw menu is trivial GPU work; half-second frames cannot be shading cost. |
| `GPU thread … us/draw` (CPU time per draw on the recording thread) | **3,100–44,000 µs** | Healthy is tens of µs. 2–3 orders of magnitude slow. |
| `Frame timeline: command thread waiting for recording` | 1,474–2,336 ms/frame | The GPU command thread is starved: it waits over a second for recorded work. |
| `flip submit -> GPU done` | 1,149–1,500 ms | Even the GPU's own execution of one submitted frame takes > 1 s. |
| `presented->display` | 1,179–2,588 ms | The display link path is equally stalled. |
| `Texture cache: memory pressure, 10421 of 9699 MiB (critical 10669)` | recurring | Total Vulkan device usage sits **between** the pressure (9,699 = 80% of budget) and critical (10,669 = 88%) marks of a ≈ 12,124 MiB budget (the 12 GiB GPU heap). **0 images evicted.** |
| Same code on M4 Max | 60 FPS | The renderer itself is fine; this machine's memory system is the difference. |

**Hypothesis (falsifiable):** the machine is overcommitted — guest pool ≈ 5.4 GiB
(5056 MiB + 448 MiB flexible) + Vulkan device usage ≈ 10.4 GiB + macOS/apps ≈ everything
else on a 16 GiB machine — so the GPU heap sits at its ceiling and the OS swaps/compresses,
which turns every driver-level operation into a multi-millisecond stall. The GPU is starved,
not weak (consistent with the user's observation that the GPU looks unused).

**Falsification test:** reduce the footprint (below) and re-measure. If menu FPS jumps to
≥ 30 with no long stalls, the hypothesis holds and we tune the GPU. If menu FPS stays ≤ 5,
the hypothesis is wrong and Phase D (profiling) takes over.

### Realistic ceiling for this hardware
M2 Pro GPU ≈ ⅓ of an M4 Max. A sane port should do ~20–25 FPS at 1080p native, more at
720p→FSR3, and menus near the frame cap. 60 FPS at native 1080p is not a target on this box.

---

## 1. Ground rules (read first)

1. **One variable per run.** Never change two knobs between two measured runs.
2. **Reboot before the baseline** (clear swap/compressor state). Between later runs, quit
   all big apps; check `sysctl vm.swapusage` shows `used` near 0.
3. **Warm cache:** the first run after a change may build pipeline caches; measure the second.
4. **Same play script every run** so numbers compare:
   1. Boot to the title menu, idle 30 s (record menu FPS).
   2. Continue → Hunter's Dream lamp; walk a fixed loop for 60 s.
   3. (If reachable) Central Yharnam plaza, 60 s.
   4. Quit cleanly.
5. **Capture everything**: log via `tee`, memory samples via the script in Phase A.
6. A run ending in `Device lost` or `outside the macOS guest arena` is **invalid** — record
   it as a failure of that configuration, restore the previous knob, move on.
7. Record every run in the results table (§ 4). No numbers in the table → the run didn't happen.

---

## 2. Knobs and where they live (cheat sheet)

| Knob | Default | What it does | Where |
|---|---|---|---|
| `BB_ARENA_GIB` | 16 (4 × 4 GiB pages) | Size of the fallback device-local arenas; any multiple of 4, then clamped to the largest Metal buffer (on this 16 GB Mac that is 2 pages = **8 GiB**, so 8/16/24 all give 8; use 4 for one page). Current runs use 8 = the default here. | `gpu/.../buffer_cache/buffer_cache.cpp:107-133`, `buffer_cache.h:45-53` |
| `BB_ZERO_COPY` | off | Serves buffer binds by importing pool pages (`VK_EXT_external_memory_host`) instead of copying into an arena. Import caps: 256 entries / 2 GiB / single import ≤ 1 GiB. | `gpu/.../buffer_cache/zerocopy.cpp:28-40` |
| `BB_DMEM_MB` | 5056 | Guest direct memory ("pool"). Range 5056–16384. `run.sh` exports **9152** for the patch path unless overridden. | `src/runtime_memory.c:25-35`, `run.sh:130` |
| `BB_LIVE_RES=1` | (ini: 0) | Live path: guest renders 1080p, host scales at runtime. Leave unset (=ini 0) → patch path: whole frame renders at preset size (1280x720 at preset 1) and the upscaler fills 1080p. | `run.sh:96-132`, `scripts/patches.py:136-157` |
| `BB_GC_BUDGET_MB` | 0 (driver budget) | Forces the texture cache's GC budget. Marks: trigger 70%, pressure 80%, critical 88% of it. | `gpu/.../texture_cache/texture_cache.cpp:1017-1042` |
| `BB_GC_IDLE_SECONDS` | 20 | Age before an image below the pressure mark is evictable (idle eviction). | `texture_cache.cpp:1062-1065` |
| `BB_GC_TRIGGER/PRESSURE/CRITICAL` | 70/80/88 | Percentages of the budget for the three marks. | `texture_cache.cpp:1027-1038` |
| `BB_HIGH_DPI=0` | (macOS default 1) | Window/swapchain at 1x pixels instead of the display's 2x. | host |
| `BB_SHADER_COMPILE=skip` | balanced | Skip draws while background compiles run (no mid-frame compile stalls). | `bbport_settings.cpp` |
| `BB_FRAME_STATS=1` | off | Per-frame stall lines + 5 s summaries in the log. | `bbport_toggles.h:105-135` |
| `BB_DROP_TESS=1` | off | Diagnostic: drops tessellated draws (visual cost). | FINDINGS 2026-10-15 |
| `iogpu.wired_limit_mb` | 0 (auto ≈ 75% ≈ 12 GiB) | macOS GPU wired-memory cap. Experiment E7 only. | `sysctl` |

Current `bbport.ini`: `upscaler=fsr3`, `preset=1`, `output_res=1920x1080`,
`live_resolution=0`, `shader_compile=skip`.

---

## 3. The plan

### Phase 0 — Glitch investigation (2026-10-16, new top priority)

Evidence (all frames in the temp dir `.../opencode/`):
- `bb-burst4.png` — user-confirmed smear frame (camera turning).
- `bb-scene.png` — window-direct capture, camera still: **correct** interior image, 21 FPS.
- `bb-bad1..8.png` — camera turning: horizontal smears, black frames, a full-screen cream wash.
- `bb-still1..6.png` — same bad view, camera stationary: **correct** (dark, grainy) image,
  8–9 FPS at 1527 draws.

**Finding (2026-10-16):** the wild frames happen while the camera **moves**; a stationary
camera renders a correct image. So this is a **temporal/upscaler artifact, not broken
geometry**. The port's upscaler notes list the matching gaps: motion vectors exist only for
static geometry ("missing: motion of animated objects, cloth, foliage; reactive/transparency
masks for particles and fog; render-resolution scaling"). We run FSR 3.1 (720p → 1080p) on
the live path with a mid-session-resized window (2560×1410) — the most suspect combination
(upstream hit a cousin bug: "upscaler not running while jitter stayed on → stretched scene
frame shifted each phase").

Test order (one variable per run; the baseline run should start with `BB_TOGGLE_FILE=$PWD/out/toggles`,
file created with `0` — live A/B, no restarts):
- **G1 (live, one run):** while the user turns the camera in the bad view, flip `1<<24` (FSR 3.1
  off), then `1<<25` (jitter off), capturing frames before/after each flip. Clean image after a
  flip → upscaler/jitter implicated; then pick the config (MetalFX vs FSR vs patch path) and re-test.
- **G2 window hygiene:** all runs at 1920×1080; never resize mid-session.
- **F1 `BB_ZERO_COPY=0`** — suspect #2 (long session, ~4.7M import/evict events; EOP-fence
  correctness open). Test if G1 is inconclusive or smears persist.
- **F2 `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`** — isolates the merge's changed path
  (also repro material for upstream issue #2).
- **F3 `BB_SHADER_COMPILE=wait`** — only if the artifact is missing pieces, not smears.

Heavy-view note: the same view costs 8–9 FPS even stationary (1527 draws, GPU ticks ~78%,
~66 µs/draw recording, ~108 tessellation-emulated draws/frame: 49.5 tessellated + 58.7
rect/quad-list; Mem[vk] pinned at the 12.1 GiB budget, zero-copy churn ended at 5.7M
imports ≈ evicts, swap peak 6.2 GB) — the old B-phase problem, to attack after the smear is
understood (`BB_DROP_TESS=1` will quantify the tessellation share).

### Phase A — Harness, baseline, and the one instrumentation we need

#### A.1 Create the memory sampler (5 min, no rebuild)
Goal: one line every 5 s in a TSV so every run has memory data next to it.

- A.1.1. Create `tools/m2perf/sample_mem.sh`:
  ```sh
  #!/usr/bin/env bash
  # Logs memory state every 5 s while the game runs. Usage: sample_mem.sh OUT.tsv
  out=${1:?usage: sample_mem.sh OUT.tsv}
  printf 'time\tswap_used_MB\twired_MB\tcompressor_MB\tbb_probe_MB\tbbgpu_host_MB\n' > "$out"
  while :; do
      swap=$(sysctl -n vm.swapusage | awk '{gsub(/M/,"",$6); print $6}')
      read -r wired comp < <(vm_stat | awk '
          /Pages wired down/            {gsub(/\./,"",$4); w=$4}
          /Pages occupied by compressor/{gsub(/\./,"",$5); c=$5}
          END {print w, c}')
      # pages are 16 KiB on this machine → /64 = MiB
      wired=$((wired / 64)); comp=$((comp / 64))
      rss() { ps -axo rss=,comm= | awk -v p="$1" '$0 ~ p && !/awk/ {s+=$1} END {printf "%d", s/1024}'; }
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%T)" "$swap" "$wired" "$comp" "$(rss bb-probe)" "$(rss bbgpu-host)" >> "$out"
      sleep 5
  done
  ```
- A.1.2. `chmod +x tools/m2perf/sample_mem.sh`.
- A.1.3. Sanity-check it standalone for 10 s, then Ctrl-C.

#### A.2 Instrument the device-memory composition (30–60 min, rebuild)
Goal: know **what** the ~10.4 GiB of device usage is (textures vs. arenas vs. imports).
Needed before Phase B's GC experiment can be interpreted, and certainly before Phase D.
This is additive (a print), so it cannot change behavior beyond log volume.

- A.2.1. Expose the counters.
  - In `gpu/shadps4/video_core/buffer_cache/zerocopy.h` add public getters returning
    `bytes, hits, imports, fallbacks, evicts` (snapshots of the atomics).
- A.2.2. Add a periodic line. The texture cache already prints every ~5 s under
  `BB_FRAME_STATS` (`texture_cache.cpp` ~line 1161). Extend that block to also print:
  ```
  Mem[vk]: usage <X> MiB of budget <Y> (trigger <T>); zerocopy bytes <B> hits <H>
           imports <I> fallbacks <F> evicts <E>; images used <N>
  ```
  using `instance.GetDeviceMemoryUsage()`, `instance.GetDeviceMemoryBudgetNow()`,
  the new getters, and the texture cache's `total_used_memory`.
- A.2.3. Rebuild the arm64 GPU library (split mode loads this one):
  `bash scripts/macos/build_gpu_arm64.sh`
  (fall back to `BB_LTO=OFF bash scripts/macos/build_gpu_arm64.sh` if the link is slow/broken).
- A.2.4. Quick smoke test: run once, confirm the new line appears, quit.

#### A.3 Baseline run (30 min incl. reboot)
- A.3.1. Quit everything, **reboot**, wait 2 min for the system to settle.
- A.3.2. From the repo root:
  ```sh
  BB_GAME_DIR=/Users/onur/Bloodborne/CUSA03173 \
  BB_GPU_PROCESS=1 BB_ARENA_GIB=8 BB_HIGH_DPI=0 BB_FRAME_STATS=1 \
  BB_SHADER_COMPILE=skip BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=1 \
  BB_ZERO_COPY=1 BB_LIVE_RES=1 BB_TOGGLE_FILE=$PWD/out/toggles \
  bash run.sh 2>&1 | tee /tmp/bb-plan-base.log
  ```
  (create the toggles file first: `mkdir -p out && echo 0 > out/toggles`; Phase 0 G1 flips
  bits in it live: `16777216` = FSR off, `33554432` = jitter off, `50331648` = both.)
  In a second terminal start the sampler:
  `bash tools/m2perf/sample_mem.sh /tmp/bb-plan-base-mem.tsv`
- A.3.3. Follow the § 1.4 play script; quit the game; Ctrl-C the sampler.
- A.3.4. Extract the numbers and fill the results table (§ 4, row "baseline"):
  ```sh
  grep -E "Frame stats:|Frame pacing:|Texture cache:|Stall:|Mem\[vk\]:|ZeroCopy:" /tmp/bb-plan-base.log | tail -60
  grep -cE "outside the macOS guest arena|Device lost" /tmp/bb-plan-base.log
  # Memory samples peak:
  sort -t$'\t' -k3 -nr /tmp/bb-plan-base-mem.tsv | head -3
  ```
- A.3.5. **Gate A (post-merge):** the pre-merge 1–2 FPS expectation is obsolete. A valid
  baseline shows menus ≥ 50 FPS and scene numbers like the smoke row (§ 4). If it is far
  below, check machine state (other apps, swap) and re-run before blaming the engine.
  No window resizing during the run — a resize re-creates the swapchain/live targets
  mid-session (seen 2026-10-16).

### Phase B — Footprint experiments (no code, one variable at a time)

Each experiment: repeat the § A.3.2–A.3.4 procedure with `tee /tmp/bb-plan-E<n>.log` and
its own TSV, one change from the previous best config (start from the baseline, adopt a
change only when the run improves).

#### B.1 E1 — Halve the fallback arena: `BB_ARENA_GIB=4`
- B.1.1. Rationale: the 2 × 4 GiB arenas are the biggest **non-evictable** device
  allocation we control. With zero-copy serving pool-backed buffers, page 0's 4 GiB may be
  enough coverage.
- B.1.2. Command: baseline recipe + `BB_ARENA_GIB=4` (replace 8).
- B.1.3. Watch for, in the log:
  - `outside the macOS guest arena` → coverage was insufficient. Invalid run; retry with
    `BB_ARENA_GIB=12`, then back to 8. Record the smallest that boots.
  - the `Mem[vk]:` usage number: a drop of ≈ 4 GiB proves arenas are counted in usage.
  - menu FPS from the 5 s `Frame stats` lines.
- B.1.4. Decision:
  - Menu ≥ 30 FPS and no `Stall:` lines ≥ 100 ms → **primary success**; adopt E1 config and
    go to B.4 (combine with B.3), skipping B.2.
  - Some improvement but stalls remain → adopt if no crash; continue to B.2.

#### B.2 E2 — Make the texture cache responsive earlier
- B.2.1. Rationale: usage (10.4 GiB) sits above the 80% pressure mark but the cache evicted
  nothing. Force it to act well before the heap ceiling.
- B.2.2. Command: E-best + `BB_GC_BUDGET_MB=6144 BB_GC_IDLE_SECONDS=5`
  (ladder if needed: 8192 → 4096).
- B.2.3. Watch for:
  - `Mem[vk]:` usage dropping to ≲ 7 GiB → textures were a large part of it.
  - `0 images evicted` **still** printed → images cannot be freed (they're GPU-written /
    kept); usage comes from buffers/arenas; record and move on — more texture budget won't
    help; Phase D.1 decides the fix.
  - visible pop-in / re-upload churn (image upload MB/s in the frame stats) — if violent,
    raise the budget one step.
- B.2.4. Decision: keep the smallest budget that lowers usage without heavy re-upload
  churn; otherwise revert and rely on B.1/B.3.

#### B.3 E3 — Render below native: the 720p patch path (biggest GPU-side lever)
- B.3.1. Rationale: `bbport.ini` (preset 1) describes rendering the whole frame at
  1280x720 → FSR3 → 1080p. `BB_LIVE_RES=1` overrides that into the live path, which renders
  the scene at guest 1080p and pays proxy copies. On a pixel-bound low-end GPU, the patch
  path is both **less GPU work and less render-target memory**. It needs a slightly bigger
  pool than the live path's default, but the 9152 MiB that `run.sh` auto-exports is sized
  for *above*-1080p patches; at 720p, start from 5056 and climb if entry fails.
- B.3.2. Command: E-best + `BB_LIVE_RES=0 BB_DMEM_MB=5056` (drop nothing else).
- B.3.3. Verify from the run banner: `Output 1920x1080: scene 1280x720, direct memory 5056 MiB`.
- B.3.4. Failure ladder at entry (`out of memory` guest-side, crash at boot):
  `BB_DMEM_MB=6144`, then `7168`. Validity: must reach the Dream.
- B.3.5. Watch for: FPS at equal draw counts vs. E1/E2; render-target-related device usage
  should shrink (check `Mem[vk]:`).

#### B.4 E4 — Combine the winners
- B.4.1. Take: arena setting from B.1, GC setting from B.2 (if kept), `BB_LIVE_RES=0` +
  `BB_DMEM_MB` from B.3.
- B.4.2. Run, record, compare against the best single-change run.
- B.4.3. If the combination regresses (e.g., pool + arenas + GC interact), drop the last
  adopted change and re-run once.

#### B.5 E5 (optional) — Single-process comparison `BB_GPU_PROCESS=0`
- B.5.1. The split host adds IPC/fault-relay overhead; on a small machine it might not pay.
  Run the B.4 config with `BB_GPU_PROCESS=0` once.
- B.5.2. Only adopt if clearly better; the split is the maintained default.

#### B.6 E6 (optional, last resort) — wired limit
- B.6.1. `sudo sysctl iogpu.wired_limit_mb=14336` before launching (revert after reboot).
  Tests whether the driver could use more wired memory than the default ≈ 12 GiB.
- B.6.2. If system-wide memory pressure worsens, abandon; this machine's total RAM is the
  hard wall.

#### Gate B — choose the story
- **G1 (memory was it):** menu ≥ 30 FPS, Dream ≥ 24 FPS; `Mem[vk]:` below the pressure
  mark; no long stalls → proceed to Phase C (GPU tuning).
- **G2 (partial):** stalled but better; usage still pinned high → run A.2 instrumentation,
  then Phase D.1/D.2 as indicated.
- **G3 (theory wrong):** menu ≤ 5 FPS with usage low and no swap → stop, profile instead:
  `sample bbgpu-host 30 -file /tmp/bb-sample-plan.txt` during a stall, plus
  `sudo powermetrics --samplers gpu_power -i 2000 -n 10` to see GPU busy%; bring the
  stacks back for analysis (Phase D.4 becomes the main line).

### Phase C — GPU-side tuning (only after Gate B/G1)

Fixed protocol: use the best B config; measure at the Dream lamp (repeatable) unless noted.

- C.1 **Resolution ladder.** Preset 2 (≈ 1080/1.7 ≈ 635p) and preset 3 (540p) via
  `BB_UPSCALE_PRESET` (restart per change on the patch path). Record FPS vs preset.
  Pick the smallest loss for the target FPS; expect this to matter more than anything else
  on this GPU.
- C.2 **Tessellation cost** (diagnostic): `BB_DROP_TESS=1` for one run. On M4 Max it was
  +4.6 FPS at 1440p; expect proportionally more here if GPU-bound. If it's a big win,
  file "emulate tessellation ourselves" higher in the backlog (already noted).
- C.3 **Copy-shader / pass-split cost:** already known to be structural ("copy shader" pass
  splits); skip for now unless C.1/C.2 leave the GPU idle.
- C.4 Keep the shader-compile skip; re-check `0 compiles` in the stats lines. If new-area
  stutters appear, that's the background compile pool doing its job — note, don't fix here.

### Phase D — Conditional code work (only what Gate B/G2/G3 triggers)

- **D.1 Usage is buffers/arenas, not textures** (B.2 showed `0 evicted` while usage > mark).
  - D.1.1. Re-read `Mem[vk]:` composition (A.2) and `ZeroCopy:` lines.
  - D.1.2. If arenas dominate: on-demand page arenas (backlog item) or auto-size
    `MacArenaPages` from the driver budget at startup (`instance.GetTotalMemoryBudget()`
    < 16 GiB → fewer pages). Implementation: `buffer_cache.cpp:104-122`.
  - D.1.3. If imports dominate and churn (evicts high): raise/scale `kMaxBytes`
    (`zerocopy.cpp:31`, currently 2 GiB) with the driver budget, or make imports survive
    longer (they're the *saving* mechanism). Measure hits/fallbacks/evicts before/after.
- **D.2 Import fallbacks are high** (`ZeroCopy: no single view covers …` in logs).
  - D.2.1. Inspect the specific addresses; the range crosses mirror views or exceeds
    `kMaxImport` (1 GiB) — decide whether the arena must cover those or the import code
    should handle multiple views per request.
- **D.3 EOP fences signalled at GPU completion** (correctness prerequisite for long
  zero-copy sessions; backlog has it under Fence waits / zero-copy). Implement before
  trusting hour-long sessions; validate with a save/load walk.
- **D.4 Device loss under pressure** (`vk_presenter.cpp:784 GetDrawing… Assertion Failed!
  Device lost` at the end of `/tmp/bb-cap.log`).
  - D.4.1. Re-test after B; if it persists, capture the full stderr block and the MoltenVK
    env in effect; check whether any allocation fails around the loss (add an
    `vkAllocateMemory` failure log if needed).

### Phase E — Validation & wrap-up

- E.1 **Extended validation:** 15-minute session on the final config (menu → Dream →
  Yharnam, some combat); acceptance: no `Device lost`, no `outside arena`, no unforced
  `Stall:` ≥ 200 ms after warm-up; FPS windows recorded.
- E.2 **Regression spot checks** (from the backlog): reload-save legs/foot check, area
  transition, settings persistence (exit with menu open).
- E.3 **Docs & backup:**
  - E.3.1. Update `docs/M2PRO_PERF_PLAN.md` results table + `PROJECT_MAP.md` status line
    + `docs/MACOS_BACKLOG.md` entries that this work resolves.
  - E.3.2. Commit harness + instrumentation + doc. Consider pushing `zerocopy-m2` (7
    commits are currently only on this disk).
- E.4 (optional, separate discussion) upstream/PR strategy for the fork's commits.

---

## 4. Results table (fill as runs complete)

| # | Config delta | Log | Menu FPS | Dream FPS | City FPS | median ms | worst ms | Mem[vk] usage | swap used peak | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| base | recipe (arena 8, live, zero-copy) — **pre-merge** | /tmp/bb-plan-base.log | 1–2 | — | — | ~550 | 2408 | 10421 MiB | | superseded by smoke |
| smoke | merged engine, same recipe, no reboot (dirty) | /tmp/bb-merge-smoke.log | 60–82 | 60 light / 23.9 plaza (1764 draws) | 9–10 heavy area (1542 draws; GPU 78%, kernel ~49 ms/frame) | 105–123 | 3156 (scene load) | 12230 MiB of 12124 budget; swap 6.2 GB | **merge = the fix**; new: smears (Phase 0) + heavy-area cost |
| F1 | smoke + `BB_ZERO_COPY=0` | | | | | | | | pending |
| F2 | smoke + `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` | | | | | | | | pending |
| F3 | smoke + `BB_SHADER_COMPILE=wait` | | | | | | | | pending |
| E1 | + `BB_ARENA_GIB=4` | | | | | | | | | |
| E2 | + `BB_GC_BUDGET_MB=6144 BB_GC_IDLE_SECONDS=5` | | | | | | | | | |
| E3 | + `BB_LIVE_RES=0 BB_DMEM_MB=5056` | | | | | | | | | |
| E4 | combination | | | | | | | | | |

---

## 5. Appendix A — command recipes

Base (Phase A.3, live path):
```sh
BB_GAME_DIR=/Users/onur/Bloodborne/CUSA03173 \
BB_GPU_PROCESS=1 BB_ARENA_GIB=8 BB_HIGH_DPI=0 BB_FRAME_STATS=1 \
BB_SHADER_COMPILE=skip BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=1 \
BB_ZERO_COPY=1 BB_LIVE_RES=1 bash run.sh 2>&1 | tee /tmp/bb-plan-base.log
```

Experiment template (only the deltas change):
```sh
BB_GAME_DIR=/Users/onur/Bloodborne/CUSA03173 \
BB_GPU_PROCESS=1 BB_HIGH_DPI=0 BB_FRAME_STATS=1 BB_SHADER_COMPILE=skip \
BB_UPSCALER=fsr3 BB_UPSCALE_PRESET=1 BB_ZERO_COPY=1 \
<delta env vars> bash run.sh 2>&1 | tee /tmp/bb-plan-<tag>.log
```

Post-run extraction:
```sh
grep -E "Frame stats:|Frame pacing:|Texture cache:|Stall:|Mem\[vk\]:|ZeroCopy:" /tmp/bb-plan-<tag>.log | tail -60
grep -cE "outside the macOS guest arena|Device lost" /tmp/bb-plan-<tag>.log
```

GPU utilization during a stall (optional, needs sudo):
```sh
sudo powermetrics --samplers gpu_power -i 2000 -n 10
```

---

## 6. Appendix B — log line cheat sheet

- `Frame stats: <F> FPS, worst frame <W> ms … GPU thread <X> us/draw, <N> draws/frame` —
  the headline. Healthy: X in the tens–hundreds of µs; N·X well under a frame. Current: 3k–44k µs.
- `Frame timeline (ms/frame): recording thread busy …; command thread waiting for recording …;
  flip submit -> GPU done …` — where the second goes. `waiting for recording` ≫ 1 ms is the
  starvation signature.
- `Stall: <W> ms frame; GPU thread on CPU <C> ms …` — printed for frames > 40 ms.
- `Texture cache: memory pressure, <used> of <pressure MiB> MiB (critical <crit>)` —
  `<used>` is *total Vulkan device usage* (not just textures); `<pressure>` = 80% and
  `<crit>` = 88% of the driver budget (≈ 12,124 MiB here).
- `vblank 480 Hz` — marker of the uncapped present mode (`BB_FPS=uncap`), not a display mode.
- `ZeroCopy: …` — one-shot diagnostics from the import path (e.g. fallback reasons).

---

## 7. Appendix C — code pointers

| Topic | File:line |
|---|---|
| Arena setup | `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp:95-123` |
| Arena page constants | `gpu/shadps4/video_core/buffer_cache/buffer_cache.h:45-53` |
| Zero-copy import path | `gpu/shadps4/video_core/buffer_cache/zerocopy.cpp` (caps at 28-32, gates 34-40, Obtain 60-…, EnforceCaps 259) |
| Zero-copy integration in the cache | `buffer_cache.cpp:~357` |
| Texture GC marks + print | `gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1008-1200` |
| Pool size (`BB_DMEM_MB`) | `src/runtime_memory.c:25-35` |
| Patch/live path selection | `run.sh:96-132`; `scripts/patches.py:136-157` (preset scales at line 24) |
| arm64 GPU lib build | `scripts/macos/build_gpu_arm64.sh` → `out/gpu-arm64/libbbgpu.dylib` |

---

## 8. Open questions (decide when we get there)

1. If 720p-class rendering is required for playable FPS, is that acceptable visually
   (FSR3 → 1080p)? If not, the GPU-time work (pass merging, tessellation) moves up.
2. Push the fork's 7 local commits after Phase B (backup), or keep local for now?
3. Time budget for Phase D code work vs. accepting the Phase B/C result as "playable enough".
