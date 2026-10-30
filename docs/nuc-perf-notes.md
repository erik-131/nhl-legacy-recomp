# NUC (Core Ultra 5 225H / Arc iGPU) performance notes — branch `nuc-perf`

Base: Redth/nhl-legacy-recomp-mac (SDK v0.10.0 + `sdk/rexglue-macos-fsi-tiling-fixes.patch`,
which includes the batched end-of-frame readback, 72d560f).

## Baseline (Windows build via Proton in Games on Whales, 2026-10-29)
- Vulkan: Intel(R) Graphics (ARL) 0x7D51, Mesa 25.0.7 ANV, FSI path.
- Gameplay 10–12 fps, 85–100 ms/frame at ~1800 draws/frame (RTX 3070: ~10 ms at the same draw count).

## What the guest actually waits on (verified in generated code)
`sub_827EDAB0` / `sub_827E2BC8` wait until `*(*(device+11024))` reaches a fence value
(`device+11036` = last issued fence). `device+11024` is a 128-byte block allocated in
`nhllegacy_recomp.148.cpp`; the ring-buffer RPTR writeback is pointed at **block+60**
(`VdEnableRingBufferRPtrWriteBack`, `nhllegacy_recomp.24.cpp`), but the poll reads
**block+0**, i.e. the CP SCRATCH register writeback. The SDK writes scratch registers
when the CP thread *processes* the packet (`command_processor.cpp` WriteRegister).

=> The "batch-granular RPTR writeback" hypothesis from docs/macos-perf-where-the-time-goes.md
does not apply to this poll. The guest waits for the **CP thread** to reach the fence
packet; the CP thread in turn blocks on GPU fences. `sdk/rexglue-rptr-incremental.patch`
(`REX_RPTR_INCREMENTAL=1`) is kept for completeness but is expected to be a null result.

## Diagnostics added
- `REX_FENCE_WAIT_STATS=1` (`sdk/rexglue-fence-wait-stats.patch`): once per second logs
  `[nhl-fencewait]` with CP-thread time blocked in `vkWaitForFences`, split into
  frame-slot waits (frames in flight full => GPU-bound) and other waits (readback drains,
  resource reuse). High frame-slot wait share on the NUC => GPU-bound => next step is the
  Simpsons-style host-RT path (drop FSI).

## Linux-native build (cloud)
- clang-20, Ninja, `-march=x86-64-v3`, `REXSDK_DIR=third_party/rexglue-sdk-main`
  (rexglue c94f5eb + the patches above), libsnappy-dev.
- `rexglue codegen nhllegacy_manifest.toml` with the vanilla xex
  (sha256 d818eb15775c1fee34b057b0fc494b211535d07123230ee45bd7d40991c55624) in `game/`: 12 min, 725 files, 384 MB.
- Full game build on 2 cores: ~37 min compile + ~1.5 min lld link (`--ld-path=/usr/bin/ld.lld-20`).
  Output: `nhllegacy` (212 MB, 114 MB stripped) + `librexruntime.so` + `librexgpu-xenos.so`
  (from `third_party/rexglue-sdk-main/out/linux-amd64/`), RUNPATH `$ORIGIN`, needs `libsnappy.so.1`.
- Smoke test under Xvfb + llvmpipe: boots, Vulkan device up, presents frames, stops on missing game data (expected).
- Deploy to NUC: `nhl-linux/` next to the Windows install in the Wolf Steam home, `game -> ../nhl/game`,
  added as a native (no Proton) non-Steam shortcut.

## Measured on the NUC, Linux build in the Wolf Steam session (2026-10-29)
Benchmark: `tools/nuc/nhl_bench.sh` — runs inside the live Wolf Steam container (gamescope
`--backend headless` asserts in wlr_linux_dmabuf feedback on this setup), scripted vpad route
(A every 90 frames 2572–6352, START at 6600/6750/6900), window = guest frames 9000–10500,
validated with `NHL_SHOT_AFTER_FRAME` captures (in-play + replay). `NHL_VK_NO_VSYNC=1`.
Caveat: a Moonlight session at 3840x2160 and Frigate were active on the same iGPU.

| run | result | window draws/frame | CP thread |
|---|---|---|---|
| baseline-linux-r3 | **9.08 fps, 110 ms/frame** | ~1650 | blocked 57–81 %, 0 frame-slot waits, 6–11 readback drains/s of 55–120 ms |
| menus (r2, lineups) | 60 fps | ~230 | blocked 4–6 %, frame-slot waits only |
| jersey select (r1) | 30 fps cap | ~169 | blocked ~60 %, 30 drains/s × ~20 ms |

Interpretation: every gameplay frame ends in the batched readback flush, which waits for the GPU to
finish the frame (~55–120 ms) — so the GPU needs roughly 60+ ms per gameplay frame, and CP work
(~35–40 ms) cannot overlap it because of the drain. To reach 16.7 ms the GPU side must get ~4x
cheaper (FSI/EDRAM emulation → host render targets, cf. The Simpsons Game recomp), and the drain
must stop serialising CP and GPU (lazy write-back).

## Clean control runs (Frigate stopped, Wolf session 1920x1080, no client attached) — bench v4
| run | ms/frame | fps | CP thread |
|---|---|---|---|
| fsi baseline | 96.6 | 10.4 | 67 % blocked, 11–13 readback drains/s |
| fsi + `NHL_VK_READBACK_MODE=some` | 72.5 | 13.8 | 69 % blocked, all frame-slot waits (GPU-bound) |
| `NHL_VK_RT_PATH=rtv` (3D renders black) | 40.9 | 24.4 | 27 % blocked, ~25 short drains/s |

Contention from Frigate + the old 4K session was only ~10 %. On fsi the GPU needs ~70 ms per gameplay
frame. With host RTs the GPU cost drops sharply and the CP thread (~30 ms/frame of non-blocked work at
~1800 draws) becomes the limit. 60 fps therefore needs both a *correct* host-RT path (Simpsons-style)
and roughly halving CP-thread cost. Note: a Moonlight client with a real controller makes the vpad
player 2 and the run stalls on the gamer-profile sign-in dialog — run benchmarks with no client attached.

## Guest fence-wait share (NHL_FENCE_SPIN_STATS, CT 105 at 16 GB) — 2026-10-29
| run | ms/frame | guest in sub_827EDAB0 | guest real work/frame | CP thread |
|---|---|---|---|---|
| fsi | 117 | 92 % | ~9 ms | 20 % CPU, 72 % blocked |
| rtv | 44.5 | 80–83 % (~35 fence waits/frame) | ~9 ms | 42 % busy, 30 % blocked (drains), ~28 % idle |
Conclusion: the recompiled game itself costs only ~9 ms/frame. The limit is the GPU path (fsi) and then
the CP thread: per-draw cost (5.8 µs vs 2 µs on M4), readback drains, and guest↔CP ping-pong bubbles.
Order of work: lazy readback write-back → Simpsons host-RT correctness → CP per-draw cost.
Also seen: "Audio Worker" thread ~75 % of a core in every run (looks like a busy-wait). CT 105 needed 16 GB
(8 GB thrashed swap and hung a run).

## CP profile + guest vsync finding — 2026-10-29 afternoon (bench v8, Frigate on)
New diag switches: `REX_CP_IDLE_STATS`, `REX_CP_PROFILE` (exclusive wall time per PM4 type-3 opcode +
CP-thread CPU), `REX_WRM_TRACE` (per-address WAIT_REG_MEM stats + PM4 packet history around long waits),
fence-wait caller histogram in `NHL_FENCE_SPIN_STATS`.

| run | fps | top CP cost |
|---|---|---|
| rtv + readback=some | 33.0 | WAIT_REG_MEM 38 % (~12 ms/frame), DRAW_INDX 35 % (6.7 µs/draw); CP never idle, 1–4 % GPU-blocked |
| rtv + `NHL_VK_RB_MAX_PER_DEST=2` (readback full) | 32.3 | same as above — per-dest limit removes the drain cost |
| fsi (default) | 10.6 | XE_SWAP 71 % (~66 ms/frame waiting for the GPU) — fsi is GPU-bound |
| rtv + some + **`NHL_SWAP_INTERVAL=0`** | **43.8** | DRAW_INDX_2 49 % (frame-slot fence waits inside), CP 47 % GPU-blocked → now GPU-bound |
| fsi + `NHL_SWAP_INTERVAL=0` | 9.7 | XE_SWAP 82 % (~85 ms) — unchanged, GPU-bound |

Root cause of the rtv ~30 fps ceiling: the guest D3D uses presentation interval 1. The GPU stream sets
scratch1=1, raises a CP interrupt that runs the swap callback `sub_827F0D88(arg=0x100)`, then
WAIT_REG_MEM until scratch1==0. With interval 1 the callback defers the flip (and so the CP) to the next
vblank, so ~20 ms CP frames quantize to 33 ms. The guest's once-per-frame wait (caller 827F2394 in
Present `sub_827F1C88`, ~23 ms) waits for the previous frame's fence. Hook `NHL_SWAP_INTERVAL=0`
(src/diag_hooks.cpp) forces an immediate flip. Game-speed effect not yet verified.
Note: with the vsync cvar on (normal play) WAIT_REG_MEM sleeps wait/0x100 ms per failed poll.

Conclusion: both paths are now GPU-bound on the Arc iGPU (rtv ~22 ms, fsi ~70–85 ms per frame). Next:
GPU timestamp profiling to see which GPU work dominates (draw shaders, texture loads, resolves), and the
Simpsons-style host-RT port for a correct, cheap EDRAM path.

## GPU timestamp profile (REX_GPU_PROF, interval 0) — 2026-10-29 16:00
Timestamps at every category switch (~5 per draw) serialize the GPU somewhat (rtv 43.8 → 35.4 fps,
fsi 9.7 → 8.1 fps under the profiler), so read the split as relative, not absolute.

| path | GPU busy/frame | draw | rt update | resolve | upload | tex |
|---|---|---|---|---|---|---|
| rtv | 27.7 ms (98 %) | 14.5 ms (52 %) | 5.6 ms | 2.9 ms | 2.3 ms | 1.3 ms |
| fsi | 108 ms (87 %) | 102 ms (95 %) | 0.05 ms | 1.2 ms | 2.1 ms | 1.1 ms |

Conclusion: the fsi cost is the draws themselves — the fragment-shader-interlock EDRAM emulation in the
pixel shaders is ~7x the rtv draw cost on Arc. Texture loads, resolves and uploads are small. The way to
60 fps on this iGPU is a correct host-RT path (find why rtv renders the 3D scene black, or port the
Simpsons-style native host-RT path), not CPU-side work.

## First runs in CT 115 (unprivileged), fresh build — 2026-10-30
Build: clang-20.1.8 (apt.llvm.org), rexglue c94f5eb + `sdk/rexglue-nhl-nuc-combined.patch`, `JOBS=6`
(codegen runs twice: once in `build.sh`, again as a ninja step; ~60 min total). Wolf Steam session started
headless via `tools/nuc/ct105/wolf_session.sh start` (no Moonlight client). Bench v9, A/B against the
2026-10-29 binaries (sha 080720bf…) in the same session, back to back:

| run | binary | fps | ms/frame | draws/frame | CP blocked |
|---|---|---|---|---|---|
| ct115-fsi | new (1be6e15c…) | 7.43 | 134.5 | 1916 | 77 % (other_waits) |
| ct115-OLD-fsi | 10-29 | 8.72 | 114.7 | 1607 | 74 % |
| ct115-rtvsome-int0 (`rtv`, `some`, `NHL_SWAP_INTERVAL=0`) | new | 37.28 | 26.8 | 1712 | 53 % (frame-slot) |
| ct115-OLD-rtvsome-int0 | 10-29 | 35.06 | 28.5 | 1651 | 57 % |

- New build = old build: per draw fsi 70.2 vs 71.4 µs, rtv 15.7 vs 17.3 µs. No regression from the rebuild.
- Both binaries are ~15–20 % slower than on 10-29 (old binary: fsi 10.35 → 8.72, rtv 43.8 → 35.1).
  CT CPU is ~90 % idle between runs, but the iGPU runs at 1.1–1.25 GHz with no game running in the CT
  (`gt_act_freq_mhz`) → another GPU client (Frigate on the host, or the Steam Big Picture UI in the
  session) competes. Check outside the CT before comparing absolute numbers.
- The matchup is not fixed: 10-29 fsi shots show ANA @ STL, today NSH vs ANA (another arena) →
  draws/frame vary 1600–1916 between runs. Compare ms per draw, or A/B in the same session.
- Audio Worker still ~75 % of a core.
