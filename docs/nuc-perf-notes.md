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
