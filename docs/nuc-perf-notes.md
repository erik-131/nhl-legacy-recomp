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

## Guest fence-wait share (NHL_FENCE_SPIN_STATS, the container at 16 GB) — 2026-10-29
| run | ms/frame | guest in sub_827EDAB0 | guest real work/frame | CP thread |
|---|---|---|---|---|
| fsi | 117 | 92 % | ~9 ms | 20 % CPU, 72 % blocked |
| rtv | 44.5 | 80–83 % (~35 fence waits/frame) | ~9 ms | 42 % busy, 30 % blocked (drains), ~28 % idle |
Conclusion: the recompiled game itself costs only ~9 ms/frame. The limit is the GPU path (fsi) and then
the CP thread: per-draw cost (5.8 µs vs 2 µs on M4), readback drains, and guest↔CP ping-pong bubbles.
Order of work: lazy readback write-back → Simpsons host-RT correctness → CP per-draw cost.
Also seen: "Audio Worker" thread ~75 % of a core in every run (looks like a busy-wait). the container needed 16 GB
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

## First runs in the container (unprivileged), fresh build — 2026-10-30
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

## rtv black 3D — root cause and fix (2026-10-30)
New diag (`sdk/rexglue-reslog-ownership-diag.patch`, active in the `REX_RESOLVE_LOG_AT` frame):
`[nhl-resdump]` = host RTs each resolve dumps, `[nhl-own]` = every EDRAM ownership change,
`rt_d=/rt_c0=` RT keys per draw run.
- Resolves #0–#6 identical in fsi and rtv; #6 (`copy_ctl=00000004`, depth) fine. #7 (`00100140`:
  colour RT0, 2x→k01, colour clear) was black: tiles 0–720 were owned by `003042E0` = the **4x MSAA
  depth RT @736** at that moment, not the 2x colour RT `00084000` the 3D pass drew into.
- The single draw between #6 and #7 (`rp=6 surf=05020140 modectl=5 dctl=8777`, XDK depth-only 4x
  clear quad, clipping disabled) claimed ownership with **len 2048** (whole EDRAM, wrapping onto the
  colour tiles): `execute_unclipped_draw_vs_on_cpu` defaulted to **false** in this SDK (upstream Xenia:
  true), so unclipped draws get full-scissor (8192) height. On the host-RT path that transfers the 3D
  colour into a depth RT and back → black. fsi only uses ownership for barriers, so it was unaffected.
- Fix: default true (`sdk/rexglue-rtv-unclipped-extent-fix.patch`). App default path is now `rtv`.

| run | env | fps | ms/frame | draws | notes |
|---|---|---|---|---|---|
| fix-rtvsome-int0 | rtv, some, int0, cvar via env | 42.22 | 23.7 | 1785 | correct image incl. HUD; per-second min 37.5 / p5 38.0 / median 42.4 / max 44.9 |
| fix-rtv-default | rtv, some, int0 (new default) | 39.67 | 25.2 | 1731 | correct image |
| fix-fsi | fsi | 6.97 | 143.4 | 1632 | lower than 7.4–8.7 earlier today; not investigated |
| speed-int0 | rtv, some, int0 | 43.01 | 23.2 | 1705 | |
| speed-int1 | rtv, some | 31.28 | 32.0 | 1755 | |
| speed-int1-rb2 | rtv, `RB_MAX_PER_DEST=2` | 32.65 | 30.6 | 1671 | |
| speed-fsi-int0 | fsi, int0 | 9.87 | 101.3 | 1715 | |
| play-vsync | rtv, int0, `RB_MAX_PER_DEST=2`, **vsync on** (`VSYNC=1`, bench v10) | 38.53 | 26.0 | 1605 | per-second min 29.1 / p5 30.2 / median 38.5 / max 47.1 |

Fixing ownership also made rtv faster: 37.3 fps (broken, same env) → 42.2 fps.

**Game speed with `NHL_SWAP_INTERVAL=0` is correct**: screenshots every 5–10 s, period clock advances
6.0x real time at 9.9 fps (fsi) and 6.1x at 43 fps (rtv) → the game clock is time-based, not frame-locked.
Screenshots at guest interval 1 (default) show the scoreboard without text in both paths, with interval 0
the text is always there — likely a capture-timing artefact of the deferred flip (unverified in play).
One play-vsync shot at 230 s (goal celebration) was a grey frame with green garbage; the 270 s shot is clean.

## Play-test feedback → readback, movies, Guide (2026-10-30 evening)
the maintainer's play session (`nhllegacy_054.log`, rtv + `RB_MAX_PER_DEST=2`, vsync on, Frigate + 4K Moonlight on):
gameplay fps min 7.4 / p5 26.7 / p25 34.1 / median 42.9 / max 52.8 — not smooth. Green fields in movies,
transitions, arena TV and on equipment; black players in team select (also on the Windows build);
Guide button fought with the Steam menu.
- **`NHL_VK_RB_MAX_PER_DEST` is a lifetime cap per destination** — after 2 readbacks a resolve address is
  never read back again, so recomposited equipment goes stale (green helmet in `rb-rb2` 260 s). Do not use.
- Readback comparison (bench v11, vsync on, int0, shots every 10 s; Frigate on for rb2/full):
  rb2 38.96 fps / full 29.15 / **fast 42.38**; equipment correct in full and fast.
- **Green movies = no ffmpeg** in the GoW Steam image: `vp6_bridge.txt` `spawn FAILED rc=2`, so the movie
  YUV planes stay zero. Static ffmpeg (BtbN n8.1, sha256-checked) in `nhl-linux/ffmpeg`, `NHL_VP6_FFMPEG`
  set by `nhl_play.sh`; `setup.sh` installs it. Movies, intro and arena TV now clean.
- `NHL_NO_GUIDE=1` (launcher default) leaves the Guide button to Steam.
- Bench v11 hides every controller except the vpad (`SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`); a lingering
  Moonlight Xbox controller made all three first runs stall on the sign-in dialog.
- With Frigate stopped and no Moonlight session the iGPU idles at 0 MHz (1.1–1.25 GHz before).

| run | env | fps | ms/frame | per-second min / p5 / median / max |
|---|---|---|---|---|
| clean-play (Frigate off, no client) | rtv, int0, readback fast, vsync on, ffmpeg | **44.54** | 22.4 | 32.7 / 33.8 / 46.6 / 49.1 |

## Presented frames ≠ guest frames; /dev/shm leak (2026-10-30 night)
the maintainer saw < 30 fps in MangoHud while the log said ~50. New counter `[nhl-present]` at `vkQueuePresentKHR`
(`sdk/rexglue-present-diag.patch`, with `[nhl-paintmode]`) shows the presenter dropping half the frames:

| run (1080p session unless noted, vsync on, int0, readback fast) | guest fps p5 / median | **presented** fps p5 / median | gaps > 50 ms per min |
|---|---|---|---|
| pres2-1920x1080 | 36.2 / 56.0 | 18.2 / 28.5 | 159 |
| pres2-3840x2160 (4K session) | 34.0 / 47.8 | 16.8 / 23.9 | 229 |
| pres3 (enhancements overlay not registered) | 54.9 / 58.0 | 27.9 / 29.6 | 0 |
| **pres5 `NHL_DIRECT_PRESENT=1`** | 30.3 / 45.8 | **30.1 / 46.2** | 1 |

- Cause: any registered ImGui UI drawer forces `PaintMode::kUIThreadOnRequest`; UI-thread paints compete with
  the game's GPU work and show ~every second frame. The app's enhancements overlay (registered while hidden, only
  to poll Guide) and the SDK's achievement toast dialog (created at launch) both did that. `NHL_DIRECT_PRESENT=1`
  (launcher default) registers neither → `kGuestOutputThreadImmediately`; costs the F1/Guide overlay and toasts.
  Guest fps drops (presenting now happens on the guest output thread) but visible fps goes 28 → 46.
- 4K streaming costs a further ~15 % — keep the Moonlight stream at 1080p.
- **/dev/shm leak**: every killed run left its `xenia_memory_*` guest mapping (4.8 GB virtual) in /dev/shm —
  15 GB resident after today's runs, the bench memory guard aborted. Likely the 10-29 CT hangs. Fix:
  `shm_unlink` right after `shm_open` (`sdk/rexglue-shm-unlink-fix.patch`); /dev/shm now stays at 700 MB.
- Intermittent boot stall: 3 bench runs today sat on the cloud screen (draws 60, ~267 fps, vpad presses ignored)
  before the EA logo movie; 6 short boots + the next full run were fine; not the saves (moved aside: still OK and
  stuck both happened). Not yet understood — the bench should get a stall watchdog with a gdb dump.

## Dedicated present thread (2026-10-30 night)
`REX_PRESENT_THREAD=1` (`sdk/rexglue-present-thread.patch`): in `kGuestOutputThreadImmediately` mode the guest
output thread only signals a present thread, which paints + presents under the same `paint_mode_mutex_`
protocol (frames signalled while painting collapse to the latest). Stopped before the Vulkan presenter is torn down.

| run (1080p session, no client, vsync on, `NHL_DIRECT_PRESENT=1`, readback fast) | guest p5 / median | presented p5 / median / min | gaps > 50 ms per min |
|---|---|---|---|
| pres5 (present on guest output thread), int0 | 30.3 / 45.8 | 30.1 / 46.2 / 29.9 | 1 |
| **pt-int0** present thread, int0 | 35.7 / 54.4 | **37.0 / 54.7 / 33.9** | 0 |
| pt-int2 present thread, `NHL_SWAP_INTERVAL=2` (30 lock) | 28.5 / 29.9 | 28.2 / 29.9 / 27.2 | 9 |

Launcher default: present thread on, interval 0. 30 lock available via Steam launch options.
the maintainer's play feedback (rtv + direct present, before the present thread): "absolutely playable", hitches on fast
camera pans; slight green at the very start of the intro movie; broken (blocky) transition wipes — same on the
Windows build; Steam overlay stutters; UI scroll sound glitches (also in the original).

## Input latency check — present thread reverted (2026-10-30 late)
the maintainer: present-thread build has higher fps but feels sluggish (input lag); Steam overlay still stutters.
New: `REX_MAX_FRAMES_IN_FLIGHT=1|2` (cap below kMaxFramesInFlight=3) and `[nhl-latency]` = frames the CPU is
ahead of the GPU when opening a frame (`sdk/rexglue-frames-in-flight.patch`). Same session, back to back:

| run (int0, direct present, readback fast, vsync on) | presented median / p5 | frames ahead avg |
|---|---|---|
| lat-A no present thread | **53.4 / 37.9** | 1.00 |
| lat-B present thread | 47.0 / 35.6 | 1.05 |
| lat-C present thread, cap 2 | 46.6 / 34.9 | 1.00 |
| lat-D present thread, cap 1 | 39.4 / 24.4 | 0.00 |

- The earlier pt-int0 vs pres5 gain (55 vs 46) was run-to-run variance (scene/matchup, ±10 %); here the order is
  reversed. **Present thread is off again** in the launcher.
- The CPU only runs ~1 frame ahead in every config, so the "3 frames queued" theory was wrong. Cap 1 removes that
  frame (~19 ms) at ~25 % fps — offered as a launch option, not default.
- Lesson: single-run comparisons need ≥2 runs per config before concluding.

## GPU profile of correct rtv + `clear_memory_page_state=false` (2026-10-30/31 night)
`REX_GPU_PROF` on the play config (2 runs): GPU busy 96–97 %, 20.9 / 28.3 ms per frame; split stable:
draw 54–57 %, rt 13–17 %, resolve 10–12 %, upload 8–9 %, tex 5 %. CP: DRAW_INDX 6.9 µs/draw (34–44 % of CP time).
GPU clock under load ~1600 MHz (max 2200) — the NUC is power/thermally limited, part of the run-to-run variance.

`clear_memory_page_state=false` (SDK default true: every guest buffer re-uploaded each frame; the Simpsons recomp
turned it off first). Interleaved A/B, GPU clock logged (`abrun.sh`), play config, vsync on:

| | presented median per run | median | p5 range |
|---|---|---|---|
| A default | 47.2, 46.6, 45.1, 46.2 (1 run boot-stalled) | **46.4** | 26–41 |
| B `REX_CLEAR_MEMORY_PAGE_STATE=false` | 64.7, 45.1, 59.7, 65.3, 61.2 | **61.2** | 35–37 |

Images correct (gameplay, close-up with equipment/crowd). Now the launcher default.

Research (other X360 recomps): Tekken 6 recomp hit the same unclipped-clear EDRAM bug and uses
`execute_unclipped_draw_vs_on_cpu=true`; Free Riders Recompiled (own renderer) cut GPU 39 → 22 ms with one host RT
per guest surface + deferred/aliased resolves (same idea as Simpsons' `native_rt_size_by_use` + copy-free
resolves) — the next big candidate for our rt+resolve share (~25 %). Tekken 6's `vulkan_present_pacing`
(VK_GOOGLE_display_timing) only helps at the display rate — revisit at 60 fps.
Boot stall seen once more (cmps-A1); the A/B script now gdb-dumps all threads if the EA logo movie hasn't started at 110 s.

## Input latency probe, RT transfer skipping, transitions (2026-10-30 late evening)
- **Latency probe** (`NHL_LAT_PROBE=1`, `[nhl-lat]`): scripted START/A presses in gameplay every 3 s; the first frame
  whose draw count changes ≥40 is the reaction. Quick reaction (resume/pause) ~90–116 ms from press to the frame
  being swapped/presented (~6 frames at ~60 fps); GPU + display come on top. `REX_SYNC_INPUT_PUMP=1`
  (synchronous SDL pump before XInput reads) made no measurable difference (A 104/92 ms vs B 116/108 ms) — the
  rest looks game-side (logic/animation). the maintainer saw no difference with `REX_MAX_FRAMES_IN_FLIGHT=1` or the direct tile.
- **RT transfers** (`REX_NHL_RT_TRANSFER_STATS=true`, `[nhl-xfer]`): ~0.8–0.9 M tiles/s, almost all 1x↔4x round
  trips around the XDK 4x MSAA clear quads. `REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS=true` (ported from the Simpsons
  recomp, CPU vertex-shader overwrite proof) drops the 1x→4x colour+depth copies (~0.7 M tiles/s); images identical
  with `REX_NHL_RT_DEBUG_POISON_OVERWRITES=true`. Interleaved A/B: presented median 62.3/62.1 → 64.4/65.3,
  **p5 36.3/36.8 → 60.4/58.9**. Launcher default. Next: the remaining 4x→1x copy-back (design step 2, clear-through).
- **Transitions / intro green**: VP6 bridge now one ffmpeg stream per movie player ring (bind lines per ring),
  holds the last frame on shortfall, waits up to 200 ms for the first frame. Intro clean in screenshots.
- Boot stall pattern: every stall so far was the first game start after a fresh Steam container (lat1 again);
  stalls during loading right after `gps.big`, before `realai/*.def`.

## Night of 2026-10-31 (autonomous, experiments in nhl-linux-dev)
Playable build tagged `playable-2026-10-31`, backup `<backups>/stable-20261031`; experiments deploy to
`nhl-linux-dev` (`DEST_DIR`, bench `NHL_DIR`).
- `[nhl-present]` now has a present-interval histogram (≤17/≤20/≤25/≤34/>34 ms).
- **vsync-locked 60** (guest interval 1) vs interval 0, 2 runs each: interval 0 has ~71 % of frames ≤17 ms; the lock
  turns the 20–34 ms frames into 33 ms drops (12–25 % at 25–34 ms, 2–7 % >34) → presented median 47–54 vs 63.6.
  Not yet — needs GPU headroom first. Reaction time unchanged (~100 ms).
- **Resolve use stats** (`NHL_RESOLVE_USE_STATS=1`, whole run): no resolve is safely skippable — the depth resolve
  1A7D9000 is sampled in some scenes (3122 fetches), the right halves (1A7ED000, 1AF1D000) belong to textures that
  start at the left half, 1AB71000 is fetched 23k times. Needs lazy resolve-on-first-use (Simpsons) — parked.
- **RT clear-through** (`nhl_rt_clear_through`, NHL clear shaders VS 1E6883FCCDE1F688 / PS A4A965C189287B99 via
  `nhl_rt_clear_vertex_shader_hash`/`..._pixel_shader_hash`): `[nhl-xfer] transfers=0 tiles=0`, cleared_through
  ~720k tiles/s; images identical with poison (menus, gameplay, replay, close-up). A/B running.
- Boot stall caught once more (vs60-A1): all guest threads blocked in NtWaitForSingleObjectEx / KeWaitFor*,
  nobody runnable → lost wakeup; backtrace depth raised to 40 frames to see the guest functions next time.
- Clear-through A/B (3 runs each) + GPU profile: no gain (rt 1.61 ms without vs 1.93 ms with — compute clears cost as much
  as the copies). Stays off.
- Alertable poll slice 1 vs 4 ms (`REX_ALERTABLE_POLL_MS`): no change in process CPU (267–282 %), GPU clock or fps.
- 1x host MSAA (`nhl_force_host_msaa_1x`): GPU 15.7 vs 16.5 ms/frame, draws unchanged (10.15 vs 10.37) — MSAA is not the
  draw cost. Stays off. **Superseded 2026-11-06 (worklog 17:25):** measured on fps, 1x is about +14 % median (2x 103.4 v 1x 118.8 and
  117.1; p5 +10 fps; draws ~1800 → 1209-1758) with matched screenshots: no visible difference at 1:1, slightly stair-stepped edges at 2x zoom.
  Still off by default; offered as a menu choice.
- GPU profile now (play config): 16.5 ms/frame — draw 10.4 (63 %), resolve 2.4, rt 1.6, tex 1.4, upload 0.23.
- Per shader pair (`REX_GPU_PROF_SHADERS=1`, `[nhl-gpushader]`): flat — top pair 4.6 % of draw time, top 10 ~30 %.
- `REX_CMD_STATS=1` (`[nhl-cmdstats]`): per gameplay frame 75–110 render pass begins, 440–590 pipeline binds,
  150–265 barriers for ~1650 draws → fixed per-pass/barrier cost is the prime suspect for the draw time on the iGPU.
- INTEL_MEASURE only captured the presenter's command buffers, not the game's — not usable here.
- Boot stall: likely cause found by code reading — the SDK never dispatched DPCs (`KeInsertQueueDpc` only queued them;
  release builds compile out the "going to hang" assert); NHL's D3D wakes its per-core workers via a DPC that calls
  KeSetEvent. Fix: run queued DPCs on the kernel dispatch thread (`[nhl-dpc]` logs). 10/10 boots OK with the fix, but no
  DPC was queued in those boots or in a full run, so the cause is not yet confirmed.
- **Render pass breaks by source** (`[nhl-rpends]`/`[nhl-barriers]`, Steam-free lobby): per frame fb_switch ~16, resolve 13,
  upload ~23, draw ~12, tex ~8, rt 6, prep ~6; barriers pushed mostly by resolve (51 buffer + 18 image), rt (42 image),
  tex (~39 b + 27 i) and upload (~62 b).
- **`gpu_stream_skip_unchanged`** (adapted from the Simpsons recomp, default off): ~60 % of upload requests fully skipped;
  render passes 84 → 67, barriers 169 → 126, upload breaks 22.7 → 12 per frame. fps A/B not done — see thermals.
- **Thermals / host**: the idle Steam Big Picture UI in the bench lobby held the iGPU at ~600 MHz and heated the CPU during
  all earlier benches → new `WOLF_NOSTEAM=1` lobby (sway only). Even with the CT idle, the CPU package sat at 77–101 °C
  and the GPU fell to 800–1300 MHz under load later in the night (host load or cooling, outside the CT); GPU
  `slpc_power_profile` = power_saving. fps numbers after ~03:00 are not comparable; nothing was promoted.
- **Boot stall**: DPC theory disproved (no DPC is ever queued; stalls continue 2/10, 1/12 with the DPC fix). Main
  XThread sleeps forever by design (sub_828B2550 → Sleep(INFINITE)); the stall is a lost wakeup between the end of boot
  loading (gps.big) and the EA logo movie, object unknown. gdb can't unwind recompiled code. An on-demand guest
  thread dump (wait handles + r1 back-chain) broke the build (XThread layout change → guest access violations) and was
  reverted.
- **GPU frequency policy (morning)**: `slpc_power_profile` is `[base]` (earlier note misread it), but `rps_RP1_freq_mhz=800`
  (efficient frequency) with `slpc_ignore_eff_freq=0`: SLPC drops the GPU to ~800–900 MHz when it isn't fully busy.
  Both runs with `gpu_stream_skip_unchanged` (fewer barrier stalls) ran at 812/889 MHz vs 1300–1650 MHz for the
  neighbouring A runs → efficiency gains get eaten by downclocking (the night backup job on the host also ran then).
  CORRECTION: `slpc_ignore_eff_freq=1` lets SLPC go *below* RPe (more power saving) and does nothing here (RPe = RPn =
  800 MHz) - do not use it. The floor to raise is the minimum frequency, on the host:
  `echo 1400 > /sys/class/drm/card0/gt/gt0/rps_min_freq_mhz` (resets on reboot; 800 restores). Re-run the A/B after that.
- The night's heat (CPU 77–101 °C with the CT idle) was a scheduled backup job on the host.
- Skip-unchanged A/B: A 64.6/67.3 median (GPU 1645/1569 MHz), B1 48.3 at 889 MHz, B2 boot stall → invalid, not promoted.

## Render pass breaks — mechanism and fix design (agent analysis, 2026-10-31)
- Any queued barrier ends the render pass: `SubmitBarriers()` calls `EndRenderPass()` when `pending_barriers_` is not
  empty, and `SubmitBarriersAndEnterRenderTargetCacheRenderPass()` calls `SubmitBarriers(false)` before every draw. On ANV
  each break + `vkCmdPipelineBarrier` is a CS stall with cache flushes.
- Sources (estimates per frame): shared-memory uploads (`UploadRanges` from `RequestRange` for vertex fetch and
  index/DMA in the primitive processor: `Use(kTransferDestination)` → `SubmitBarriers(true)` → copy → `Use(kRead)` with a
  transfer→read barrier on the whole buffer) 30–45 breaks, likely the largest; texture loads 10–15 (resolve targets
  that are sampled get reloaded from shared memory every frame); resolves 15–25 (dump/compute ends the pass,
  `Use(kComputeWrite)` leaves a barrier for the next draw); framebuffer switches 15–25 (legitimate); RT transfers ~0
  with `nhl_rt_skip_overwritten_transfers`.
- Fix plan: (1) `gpu_stream_skip_unchanged` (done, default off, SDK patch 0021); (2) **upload pre-pass**: an extra
  `DeferredCommandBuffer` executed before the main stream with one transfer→read barrier; `UploadRanges` records into it
  when the range doesn't overlap anything read/written earlier in the same submission (RequestRange ranges, resolve and
  memexport writes via `Use(..., range)`, textures loaded from shared memory), otherwise the current path. Risk: the
  tracking must cover all GPU access to shared memory; flush the pre-pass when a submission splits mid-frame.
  Est. ~150–200 LOC, 2–4 ms/frame; (3) textures: the break is unavoidable when a draw loads a texture.

## Evening of 2026-11-04 — GPU floor set, skip-unchanged verdict, P-core pinning
Host: `rps_min_freq_mhz=1400` (the maintainer), Frigate stopped, Steam-free lobby, dev binaries from 2026-10-31 06:26.
Play env as in `nhl_play.sh` (`REX_CLEAR_MEMORY_PAGE_STATE=false`, `REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS=true`,
readback fast, direct present, swap interval 0), `abrun.sh`, 60 s gameplay window.
- **Upstream check**: puckhead73 `master` and `perf/cpu-draw-submission` are at `0852183` (v0.6.0, 2026-07-21) —
  already in `nuc-perf`. rexglue-sdk `development` has 24 commits after our base `c94f5eb`; the relevant ones:
  `bd833a2` (emit fences for PPC `sync`/`lwsync`/`eieio` — in our base they are complete no-ops, not even a compiler
  barrier, so StoreLoad reordering is possible: a lost-wakeup candidate for the boot stall), `7f7c92e` (stwcx. via
  `std::atomic` CAS), `0c7b01a` (leaked kernel event/socket objects), input/UI/logging features. The game has 21
  `sync`, 465 `lwsync`, 18 `eieio` (one `sync; stw; eieio; sync` pair writes the CP ring write pointer).
- **`gpu_stream_skip_unchanged`, 3 interleaved pairs** (presented median / p5 / % ≤17 ms / draws / GPU MHz):
  A 74.7/54.3/86.6/1552/1683, 72.7/47.9/88.1/1892/1658, 58.4/45.7/60.4/1565/1733;
  B 60.9/38.8/69.0/1802/1272, 59.9/48.2/58.1/1543/1644, 61.6/40.3/71.3/1815/1463.
  B is never better; the GPU sat in RC6 (0–100 MHz samples) inside the gameplay window in B1/B3 → it waits on the
  CPU. The per-page `MakeRangeValid` (mprotect) + 4 KB `memcmp` costs more CPU on the CP thread than the saved render
  pass breaks gain. **Stays off.** (The GPU-clock average below the floor in B runs = RC6 samples, not throttling.)
- **Run-to-run variance** (A3 58 vs A1 75 with the same draws/clock) traced to thread placement: the CT gets CPUs
  `1-2,4-7,10-11`; only 1-2 are P-cores (`/sys/devices/cpu_core/cpus` = 0-3). Unpinned, the main guest XThread
  (~85 % of a core) stays on P-cores (118/120 s) but the **GPU command processor thread (~65–70 %) runs on E-cores
  99/120 s**. CPU package 64–88 °C during benches.
- **P-core pinning A/B** (main XThread + "GPU Commands" → `taskset -c 1,2` at 150 s):
  unpinned 73.0/47.2/74.7 %, 75.4/51.1/86.0 %; **pinned 86.8/46.0/88.6 %, 87.5/82.0/99.7 %** (draws 1787/1679,
  GPU ~1700 MHz). Two boot stalls in this set (pin-A2, pin-P3; before the pin point) — dumps
  stall dumps (lost in the 23:39 freeze, /tmp): same picture as before, every thread blocked.
  → `tools/nuc/ct105/nhl_pin.sh` (started by `nhl_play.sh`, `NHL_PIN_PCORES=0` disables).
- **Build with upstream sync fences (`bd833a2`) + `nhl_upload_prepass`** (SDK patches 0024/0025): codegen rewrote
  184 of 725 files; stripped binary +3 KB.
- **Boot stall is NOT fixed by the fences**: bootloop 3 OK, then boot 4 stuck (stopped there). This dump
  (lost in the 23:39 freeze, /tmp) has guest call chains (bt depth 60) and a different picture from the
  "everyone blocked" notes: the game keeps presenting a 60-draw loading screen at ~280 fps, the GPU CP + one XThread
  are running, the EA logo movie is never started. A guest thread polls with Sleep in a loop:
  `sub_826FBC00 → … sub_825AC9D0 → sub_8249DF68 → sub_8249D548 → sub_8249C950 → sub_82499C90 → sub_825AF7C0 →
  sub_825B1BD8 → sub_825B3828 → sub_825B3978 → sub_8277BC00 → Sleep (sub_83067720)`; six workers wait in
  `sub_8289D7D8` (job wait, via sub_826FED18/sub_827001D0 or sub_82671168), main XThread in `sub_828B2550`
  (Sleep(INFINITE), by design). Last log lines before it: `[db-open] db\nhlng.db` opened dozens of times in a burst.
  Next step for 1g: find what sub_8277BC00's loop waits for (flag/state written by a job that never ran?).
- **O/N/Q A/B, all P-core pinned** (O = pre-fence binary, N = fence build, Q = fence build +
  `REX_NHL_UPLOAD_PREPASS=true`), presented median / p5 / % ≤17 ms, 2 interleaved rounds:
  O 80.3/44.2/73.4, 71.9/48.8/70.6; N 87.4/53.0/92.7, 84.6/47.0/78.9; **Q 85.8/82.2/99.7, 88.0/49.2/91.5**.
  Same order in both rounds. `[nhl-prepass]` in gameplay: ~3100–3800 uploads/s through the pre-pass, ~90/s inline
  (~97 % of uploads no longer end a render pass); screenshots correct, no Vulkan errors.
- **Latency probe** (`NHL_LAT_PROBE`, 2 interleaved pairs; old = playable config, old binary, unpinned; new = fence
  build + pre-pass + P-core pin): fast presses old 97/84 ms median (min 73–75), new 96/90 ms (min 74–76); menu-opening
  presses old 254/461, new 207/452 ms → unchanged. fps in the same runs: old 70.6/76.9 median, 74.1/85.8 % ≤17 ms;
  new 87.0/86.6, 91.3/99.6 %. Menu/lineup/gameplay screenshots correct.
  **Promoted to `nhl-linux` 2026-11-04 ~23:00** (backup `<backups>/stable-20261104-pre-prepass/`);
  launcher smoke test: boots, movies play, `[nhl-prepass]` active, both threads pinned inside the container.
- **Research (other X360 recomps)**: mcla-recomp issue #48 lists ports from the FH1 recomp (pinyon-shift, "ShiftGlue"
  SDK fork) and SpeedBreaker: Mesa ANV push descriptors off (30→60 fps there; our SDK doesn't use push descriptors),
  `Usage::kComputeWrite` access mask SHADER_READ→SHADER_WRITE (same bug in ours), texture upload WAW barrier, skip
  unchanged fetch constants (CPU), VBlank-driven WAIT_REG_MEM instead of sleep, `vulkan_max_frames_in_flight`,
  `VK_EXT_present_mode_fifo_latest_ready`, PPC fences (= upstream bd833a2). Native renderers: FH1 recomp ~4× over its
  Xenos path (29 → 120 fps capped, RTX 4080); Skate 3 recomp (EA, same era) native D3D12/Vulkan path ">2× fps at ~¼
  GPU power" with the emulated path as fallback.
- **In-process A/B** (`NHL_AB_TOGGLE`, `src/ab_toggle.cpp`, `tools/nuc/bench/abphase.py`, abrun `DUR=420`): one run,
  20 s phases, first 2 s after a switch dropped. Validation `nhl_upload_prepass=false,true@20`: A (off) 105 s,
  84.5 median, 82.1 % ≤17 ms; B (on) 107 s, 86.8, 90.2 % — same direction as the separate runs, in 7 min instead of
  ~20. Segment medians B87 A91 B88 A86 B86 A86 B81 A55 B87 A57 B59 A71 (scene changes hit both phases).
  abrun now also kills a boot-stalled run at 110 s and retries once.
- **60 fps limiter (MangoHud `fps_limit=60,fps_limit_method=late`) vs none**, dev build (+ `kComputeWrite` fix),
  pre-pass, pinned, latency route, 2 interleaved pairs: none 90.9/95.9 median, 99.8/90.0 % ≤17 ms, fast presses
  80/90 ms (min 70/65); limiter 60.0/60.0, but 38–42 % of frames in the 17–20 ms bucket (sleep-based, not vblank
  aligned) and fast presses 94/97 ms (min 76/83) → +7–14 ms latency. **Rejected.** If pacing is wanted later, try
  presentation-based pacing (`VK_EXT_present_mode_fifo_latest_ready`) instead.

## Host freeze 2026-11-04 23:39 and corrections to the notes above
- **Whole NUC froze; the maintainer had to cut power** (back up 02:54). Last trace 23:39:08, about 125 s into `fast-prof1`
  (new `ROUTE=fast` + `REX_GPU_PROF=1` + `REX_CMD_STATS=1`, dev binary with pre-pass and `kComputeWrite` fix), on the
  title/autosave screen at ~470 fps (that route's early START missed), not on the way into a match. The game log ends mid-write, the CT journal has no shutdown, 15.6 GB memory was free. **Cause not
  established**: the host journal is needed. A GPU/driver hang triggered by the run is a suspicion, not a finding. The
  floor reset to 800, the CT CPUs changed to `1,3,6-10,13`, /tmp was wiped. Bisect one change at a time, attended only.
- **Corrections**: (1) the histogram bucket is ≤17 ms, not ≤16.7 ms. (2) Pre-pass: same binary without/with is
  92.7/78.9 → 99.7/91.5 %, one in-process run 82.1 → 90.2 %; the earlier "70–79 → 91–99 %" left out N1. (3) Pinning:
  the same binary pinned gave 86.8/87.5 (taskset at 150 s) and 80.3/71.9 (`nhl_pin.sh` from boot), so the size of the
  gain is uncertain, and that gap is as large as the pre-pass effect. (4) "Fence build better than old binary" is not
  shown. (5) Limiter: the 38–42 % in the 17–20 ms bucket may be partly the bucket edge just above 16.67 ms; only the
  latency result (+7–14 ms, 2 pairs) supports the rejection, uneven pacing is not shown.
- `X` runs with the `kComputeWrite` fix: one pair only, no measured effect either way; not in `nhl-linux`.

## Boot stall: code reading of the stuck4 stacks (2026-11-05, no run, one stack sample)
- Correction: `sub_8277BC00` is not an endless poll. It is the movie player's per-tick update, called from the FE tick
  `sub_825B3978` (`lwz r3,3136(r31)`). It calls `sub_8277CB20` (state check with a zero timeout at object +312); on
  failure it does one `Sleep(1)` and carries on.
- Handshake between it and the movie frame driver (`sub_8277ABB8` → `sub_8277A248`, field +140 of the object at +312):
  driver sets state=1 and signals, then loops `while (state != 2 && !flag@136) cv_wait` (`loc_8277A3BC`). The update
  takes state==1, then sets state=2 (`stw r11,140(r30)`) and signals (`sub_82670BC0`).
- `sub_82670A78`/`sub_82670BC0` are a guest condition variable built from atomics and semaphores (waiter count at +0,
  gate at +32, mutex at +56); `sub_82671168` → `sub_8289A418` is the kernel wait.
- In stuck4 the driver sits in that wait and the update is failing its check, i.e. state is neither 2 nor 1 **if both
  refer to the same player**. Not established: there are several players, and this is one sample. Candidates: lost CV
  signal, state written to another player, our hook `Vp6BridgeOnMovieSetup` at `sub_8277ABB8`.
- Next test (attended): extend the existing `NHL_VP6_SWEEP_HOOK(8277A248)` in `src/diag_hooks.cpp` to log the object
  address and state at the wait and at `sub_8277BC00`, then bootloop until a stall. Also try `NHL_VP6_BRIDGE=0` in a
  bootloop to rule the bridge in or out. No `XThread` layout change needed.

## After the freeze, night of 2026-11-05 (hot host: fps values below are not comparable)
- **Freeze not reproduced**: `bis-fast`, `bis-prof-r` and `bis-both` (exact fast-prof1 setup) ran without a freeze. One
  attempt each, so nothing is ruled out.
- **Host is hot with the game off**: CPU package 81–99 °C, 3500 rpm fan, ~12 % `ni` load outside the CT, a GPU clock sample
  of 1250 MHz with no game running, `package_throttle_count`=32 after ~40 min uptime. During runs: peaks 92–97 °C and GPU
  averages 1209–1317 MHz despite the 1400 floor. Not yet explained, needs a look at the host. Heat as a freeze cause is
  neither shown nor excluded.
- **`ROUTE=fast` gains almost nothing**: the lineups screen ignores START at 118–126 s and the match starts at ~175 s with
  no further presses, against ~178 s on the normal route. That wait is real loading, not wasted margin. Fixed route: START
  at 66+70 s (58 s was too early). It also changes the matchup (Columbus-Anaheim), so don't compare it with the normal route.
- **Uncapped menus are not the heat or load-time cause**: `NHL_SWAP_INTERVAL=1` (menus at 60 fps, GPU 349 MHz before the
  match): match at 179 s vs 178 s, pre-match 91 °C avg / 97 °C max vs 89 / 97. Hypothesis rejected (one run each).
- GPU profile `bis-prof-r` (hot, 1236 MHz, so ms are inflated): draw 11.39 (65 %), resolve 2.67, rt 1.82, tex 1.19,
  upload 0.06 ms/frame; 59–64 render passes, 108–123 barriers, 504–652 pipeline binds per frame.
- Second boot-stall dump (`<workdir>/stall-bis-prof.bt`) shows the same driver wait; the tick is in
  `sub_8277BA20` (sibling of `sub_8277BC00`, same state check). A healthy-movie control sample is still missing.
- 2026-11-05 06:46, game off, floor still 1400: CPU package 52 °C, 0 % `ni`, `package_throttle_count` still 32. The 03:30
  heat was therefore a passing load on the host, not a permanent cooling fault; what ran is unknown. No work was done
  between 03:38 and 06:45: the session idled, the promised code reading and research did not happen.

## Step 0 and first items of the plan, 2026-11-05 (14 CPUs in the CT, perf available, GPU floor 1400, Frigate and Plex off per the maintainer; no throttling unless marked)
Tools: `perfprof.sh` + `perfrep.py` (perf per thread, hybrid events merged, symbols via `<workdir>/symfs`),
`coreab.sh` (limits the container's cpuset through the cgroup, because dockerd keeps the CPU list from when it started),
`buildab.sh`, `abq.sh`, `lowfps.py`, `abphase.py` prints GPU clock per phase.
- **Where the CPU goes in gameplay** (30 s at ~74 fps): main XThread 84 % of a core, about half of it the fence spin
  (`sub_827E2BC8`, `sub_827EDAB0`, save/restore); ~6 % `/proc/self/maps` in the kernel. GPU Commands 68 %: 25 % register
  writes (`WriteRegister` x2, `GetRegisterInfo`, `ExecutePacketType0`), 19 % ANV, 16 % libc. Four job XThreads 28–35 % each,
  three of them with 7–10 % in `/proc/self/maps`. TimerQueue spins ~17 % of a core doing nothing.
- **Lineups screen**: main thread 77 % fence spin, GPU Commands 40 %, nothing else busy. The ~60 s wait isn't CPU work, so
  plan item 7 (memcpy, LZX, `pow`) drops down.
- **CPU count**, same binary, 2 interleaved rounds, latency route: old 8 (`1,3,6-10,13`, pinned) median 78.2/84.3, p5
  41.6/41.8, ≤17 ms 88.2/85.2 %; all 14 pinned 86.8/79.8, 46.4/54.7, 86.6/87.8 %; all 14 unpinned 76.3/68.9, 42.1/48.5,
  81.4/73.1 %. **More CPUs: no clear gain**; the better p5 came with 15–20 % fewer draws. **Pinning: ~+10 fps median**
  (83 v 73), latency unchanged.
- **Low stretches are scenes**: seconds at ~55 fps come in runs of 10–60 s at equal draw counts and the GPU clock is not
  lower (1772 v 1654 MHz). Screenshots every 5 s with `REX_GPU_PROF`: they are close-ups and stoppages, 22.5 v 13.3 ms GPU per
  frame (resolve 5.3 v 2.2, draw 13.4 v 8.5). Hence A/A control `nhl_fence_poll=0,0`: 92.4 v 100 % ≤17 ms with identical
  settings. **In-process ≤17 ms differences of up to ~8 points are scene noise; compare gameplay segment medians.**
- **`PROCMAP_QUERY`** (SDK patch 0027, `nhl_procmap_query`; off = new allocation-free scan, not the old ifstream code): on v
  off 94.4 v 58.1 and 94.1 v 87.2 median (order swapped), typical gameplay segments ~96 v ~87. Build against the previous
  playable binary (includes `nhl_type0_bulk` and `kComputeWrite`), 2 interleaved rounds, latency route: 92.3/95.0 v 77.4/84.7
  median, ≤17 ms 87.5/100 v 82.5/91.0 %, fast presses 94/85 v 93/96 ms. Plain route on the new build: 94.9 and 96.9 median,
  100 % ≤17 ms in the window. **Promoted to `nhl-linux`**; old binaries in `<backups>/stable-20261105-pre-procmap`.
- **`nhl_type0_bulk`** (same patch): no fps effect (94.7 v 95.0); GPU Commands samples per frame 468 → 419, −11 % (one pair).
  Stays on: CPU headroom, no visible change.
- **`nhl_fence_poll`** 1 (pause), 2 (yield), 3 (pause then sleep): no fps or GPU clock gain in one A/B each (GPU 1711–1693 MHz v
  1715–1740). The idea that less spinning leaves the iGPU more power isn't supported here. Default stays 0. CPU saving and
  power on other machines not measured. **Superseded (later entries, same day 2026-11-05):** mode 3 saves CPU (next entries) and was
  promoted; it is the launcher default (`REX_NHL_FENCE_POLL=3`; `0` spins as recompiled). The gain is CPU, not fps: fps only
  improves on two E-cores (63.8 → 89.9, 75.7 → 86.4 median); with all cores there is none.
- **TimerQueue blocking** (SDK patch 0028): 2 interleaved rounds + earlier pair, no throttling in the last two: fps same (96.8/97.9 v 99.2/95.6), fast presses 79/97 v 95/92 ms, timer thread ~25 % → ~6 % of a core. **Promoted**; previous binaries in `stable-20261105-procmap`.
- **`nhl_fence_poll` CPU** (`cpuab.py`): only mode 3 saves CPU: cores 3.23 → 2.40, main thread 0.95 → 0.30, CPU per frame 36.6 → 30.7 ms; modes 1 and 2 save none. Separate rounds with mode 3: 97.7/96.4 median, fast presses 91/100 v 79/97 ms on the same build. In-process it had more low segments (9 v 2 over three runs) and those look ~5 fps lower, so close-ups may lose. ~~Not enabled.~~ **Superseded:** mode 3 was enabled in the launcher for the maintainer's play test 2026-11-05 20:20 (see "Promoted" below).
- **Thread placement** (`pinab.py`, in-process): `clean` 97.9 v 95.7, `ded` 92.3 v 94.2, `noP` 96.1 v 95.8 (throttled): no clear difference. Bug in `nhl_pin.sh`: threads created by a pinned thread inherit the P-core mask, including Pulse and SDL audio threads. For the in-app version use `clean`.

## Evening 2026-11-05: wakeups, clusters, boot stall (throttling from ~19:30, see below)
- **POSIX event fix** (SDK patch 0029, written from the description of rexglue PR 531, without nesting locks): waiters queue on the
  event. `tools/nuc/tests/event_wakeups.cpp`, old v new library: manual set+reset missed 2000/2000 v 0, `SignalAndWait` signal stolen
  199664/200000 v 0. No effect on the boot stall: 3/25 stalls on both builds, interleaved.
- **Boot stall is a lost wakeup in the guest condition variable** (`NHL_MOVIE_TRACE=1`, `bootab.sh`, 1 stall in 25): driver-in state=0, then
  the update's check returns 1 with state=1 and sets state=2, then no driver-out, and the driver stays in `sub_82670A78` with state=2.
  Same object, and +489 isn't involved (the stream pointer at +216 is still 0). Healthy boot: the same sequence, ending in driver-out.
  The CV (`sub_82670A78`/`sub_82670BC0`) is the pthreads-win32 algorithm over a counting semaphore wrapper (`sub_82671168`/`sub_82671280`), and
  reads as correct. Next: the trace now logs the CV and semaphore counters. Occurs at boot only so far. #386/#387 (thread start race) is in our base.
- **Thread placement, in-process**: all on one E cluster `4-7`: 97.2 → 106.4 median, typical segments 95–103 → 103–108, GPU 1723 → 1819 MHz, CPU per
  frame 36.1 → 24.9 ms. Two clusters `4,5,8,9`: 95.6 → 97.7, GPU 1714 → 1706. All eight E-cores: typical segments unchanged. So sharing an L2 matters,
  not the core count; why the GPU clock rises only there is unexplained. One run each.
- **`nhl_fence_poll=3` on two E-cores**: 63.8 → 89.9 and (order swapped) 75.7 → 86.4 median, CPU per frame 27.3 → 19.2 and 25.6 → 21.0 ms.
- **`4-7` + mode 3** v P pinning + mode 3: 93.5 → 100.0, 1.90 cores used, CPU per frame 31.8 → 21.0 ms. Throttled run.
- **Latency, `4-7` v P pinning**: fast presses 89/80 v 101/95 ms, median 99.6/97.7 v 94.2/94.7. All four runs throttled (`package_throttle_count`
  37 → 115 during the evening, cause unknown), so not valid as proof; repeat when clean.
- **Promoted for the maintainer's play test 2026-11-05 20:20** (launcher: mode 3 + one cluster, `NHL_CPU_CLUSTER=0` / `REX_NHL_FENCE_POLL=0` turn them off). Previous binaries
  and launcher in `<backups>/stable-20261105-timer`.
- Blind alleys: push descriptors aren't used here; hot pages (rexglue PR 549) would save at most ~2 % of samples here.

## Hitches during play (the maintainer, Moonlight, 2026-11-05): new shaders, and my cluster setup made them worse
- the maintainer: smooth pans, good response, but sudden short stutters, sometimes a second or more lost from the match; replays well under 60 fps.
- Log: the worst gaps (587, 222 ms) follow `Creating graphics pipeline state`. With a cold cache (`REX_CACHE_ROOT`, `MESA_SHADER_CACHE_DIR`) it reproduces without a client, ~480 pipelines per
  run, `hitches.py`. Sum of present gaps > 50 ms / worst: all on `4-7` 3058, 6675 / 1026, 3101 ms; P pinning 1869, 1907 / 420, 464; game on `4-7` with `Vulkan Pipelines` threads elsewhere 2115, 2374 / 454, 450
  (`nhl_pin.sh` with `NHL_CPU_CLUSTER=1`, promoted). Skipped presents are not the main cause: `vulkan_async_skip_incomplete_frames=false` leaves gaps as long (976–1012 ms) and shows blank frames, so it stays on.
- Frames stall on the CP thread, which creates a placeholder pipeline per new draw synchronously (~230 per run, 2.5 s in total, up to 64 ms each); the real pipelines take 34 ms median, ~190 ms p95, ~14.5 s total on the compile threads.
- `build.sh --deploy` stopped silently with no lobby running (fixed), so cold-* except cold-pin ran the 20:00 dev binaries without `[nhl-pso]` counters.
- **Soak with stored shaders** (`DUR=3560`, random buttons through menus and matches, 36 min of gameplay, game on `4-7`, compile threads elsewhere): 384 pipelines loaded from storage in 0.3 s (Mesa cache warm), 12 new in the hour (7 at 15 min, 2 at 20, 3 at 25; none after), 3 gaps > 50 ms, worst 71 ms, none > 100. 101.2 median, p5 98.4. So once the storage has a game's shaders the freezes are gone; the maintainer's logs `156` and `157` (4 min in total) got 9 new pipelines and 809 ms of its 994 ms gaps at 19:33:20–22, right on them.
- **No placeholders on the CP thread** (`nhl_async_no_placeholder`; cold storage, 2 runs each, same build): gaps in gameplay sum 2310, 2182 → 1976, 1465 ms, worst 486, 492 → 322, 346; all gaps in the run (with loading) 6326, 5852 → 5218, 4630 ms, worst 2059, 1723 → 1456, 1308. 0 incomplete frames either way. What's left are frames waiting for the real pipelines, mostly at menu → intro → gameplay.
- Boot stall: 23 traced boots with CV counters had 0 stalls. Boots 18 and 19 are not stalls: I ran `abrun.sh` without arguments by mistake at 23:18, which killed the game (no `nhl-movie` lines in their logs). 0/23 against 12 % has ~5 % chance, not enough to call it fixed; 25 more follow.
- Boot stall: another 25 traced boots, 0 stalls, so 0/48 against 3/25 this afternoon. Reason unknown, and the trace reads guest memory under a mutex which could change the timing, so 25 untraced boots of the same build follow.
- GPU idle: 87–88 % with no game because Wolf itself takes 11.9 % of the render engine (`gpuwho.sh`, pid `wolf`), no client connected. Nothing else in this CT uses it; abrun warns below 85 % now.
- Compile placement (cold, no placeholder, 2 runs each): rest of the cores 5356, 4607 ms all gaps; without LP-E (`0-3,8-11`) 4467, 4741; P only 5158, 5442, compile sum 17 → 24 s. No clear difference between the first two, so unchanged. Translation on the CP isn't it either: 0.9 ms median, ~240 ms per cold run.
- **Close-ups** (`closeup.py`, `REX_GPU_PROF_SHADERS`, 234 s): slowest quarter of gameplay seconds 51.9 fps, 19.6 ms GPU per frame (draw 11.8, resolve 4.4 with 20 resolves, rt 1.7, tex 1.5, 1939 draws); fastest half 80.2 fps, 11.9 ms (7.3, 2.1 with 13, 1.4, 0.8, 1541 draws). Flat by shader: the largest pair gains 0.88 ms per frame, the next 0.70, so more pixels and more resolves rather than one shader.
- **Format list on render targets: nothing to gain.** The idea was that `MUTABLE_FORMAT` without a list turns off CCS in ANV. But NHL's targets are `R8G8B8A8_UNORM` with the same transfer format and D24S8, so they aren't mutable and the list is never used (`[nhl-rt] ... list false`). The 6 runs differ by noise only. Removed. Main target 1280 wide at 1x; the others seen are 640x2048 2x, 640x1024 4x, 320x2048 4x, 800x824 4x. `REX_GPU_PROF` costs a lot here (~70 fps median v ~100), so its ms are relative figures.
- Boot stall: 25 untraced boots of the same build, 0 stalls. 0/73 tonight against 3/25 this afternoon, cause of the change unknown (afternoon runs had 8 → 14 CPUs and other load). Parked, keeps being counted.
- **Where the slow scenes go** (`rtprof.py`, `REX_GPU_PROF_SHADERS=2` tags draw time by render target; `[nhl-gpuresolve]` splits resolves into dump and copy): the main pass is 1280x720 at 2x MSAA split in two 640 pitch tiles, ms per frame slow v fast 6.35 v 4.23. Slow scenes add three 768x768 4x depth resolves (light shadow maps, pitch 800, 45 segments, drawing them 0.26 ms): dump 1.27 + copy 0.62 ms, ~0 in fast scenes. Total slow v fast 19.2 v 12.3: draw 11.3 v 7.6, resolve 2.1 v 1.1, dump 2.4 v 1.0, rt 1.7 v 1.4, tex 1.5 v 0.9.
- **Half the pixels** (`nhl_test_half_scissor`, toggled in one match): main pass 5.2 → 3.8 ms, draw 9.0 → 6.9, busy 15.6 → 13.1. So at most about half of it is pixels. **Vertices** (`[nhl-cmdstats]`): 1.08 M per frame in the fast half, 1.81 M in the slow quarter, 2.44 M in the worst 10 s (busy 24.3 ms, 2125 draws, 697 pipeline binds, 78 render passes). Draw time follows vertices; the seconds below 40 fps come in bursts of 2–3 s at up to 4300 draws.
- Vertex fetch in the translator: every `vfetch` loads dwords from an SSBO with `robustBufferAccess` on, then swaps endianness behind two `if`s on a fetch constant. Testing robustness off next, and reading the driver's output (`INTEL_DEBUG=vs`).
- `robustBufferAccess` off: 4 pairs, medians 102.1, 87.1, 85.2, 85.5 with v 101.6, 85.4, 85.5, 87.1 without. Nothing, removed.
- **GPU clock fell at 02:00**: 1700–1800 MHz in gameplay before (rb-norob1, 01:56), ~1500 since (rb-rob2, 02:01), with `throttle_reason_pl4` set and `rps_act` 1350–1550 at a request of 2200, and the same config dropped ~15 % in fps (102 → 85 median). `/proc/stat` shows 11–13 % nice CPU (~1.7 cores) with nothing niced in this CT and no game running, so something outside it (host job, other CT) takes power; `gt0` idle unchanged, `gt1` (media) 100 % idle. Comparisons after 02:00 are only valid against each other.
- **Fast vertex math** (`nhl_fast_vertex_math`, no `0 * x` check in vertex shaders until the first `rcp`, `rsq` or `log`): driver dump (`INTEL_DEBUG=vs`) has skinning shaders of 1322 instructions and 24 sends per 8 vertices. Draw GPU cycles per frame by vertex band over 3 pairs (off v on, Mcycles): 14.2 v 13.6, 15.5 v 15.1, 15.0 v 15.1, 27.3 v 26.1. At most 2–4 %, around the noise, left off.
- **One-sample dump** (`nhl_dump_only_sample`): fast resolve copies read one sample per pixel, so the dump can skip the others. Validated by writing a colour into the skipped samples (`REX_NHL_DUMP_POISON=1`, 35 screenshots clean) and by the negative control keeping the wrong sample (`=wrong`, a bench close-up goes entirely out of focus, since depth of field reads the resolved depth). A/B in one match: scenes with 4x shadow resolves dump 3.12 → 2.66 ms; others 1.10 → 1.07. Too little, because the invocations still run. Left off.
- **Promoted to `nhl-linux` 2026-11-06 ~04:10**: `nhl_async_no_placeholder` on (backup `stable-20261106-pre-noph`). The other test switches in this build default off. A 50 min soak on that install follows.
- Second soak (`soak2`, 21 min of gameplay, under the 02:00 power limit): 3 gaps > 50 ms, worst 188 ms, 2 near a pipeline; storage 25950 → 26148 bytes.
- **Soak on `nhl-linux` with stored shaders** (`soak3`, 50 min, random buttons, 30.5 min of gameplay, through a full match to the result screens, under the 02:00 power limit): **0 gaps > 50 ms in gameplay**, 86.5 median, 99.5 % ≤17 ms, 1 skipped frame, 3 new pipelines. All four gaps in the log (387, 187, 74, 123 ms) came in the first 3 minutes, before gameplay. Storage now holds 399 pipelines. Screenshots right (play, replay camera, result screens).
- CPU in gameplay (`perf -g`, 30 s): 2.0 cores; GPU Commands 0.65 core, 27 % ANV, 18 % libc, the rest flat; `global_critical_region::mutex` 0.06 % and `WaitMultiple` 0.01 % of samples. Item 3 and the `WaitMultiple` idea are small here unless the wake delay matters (`REX_WAIT_STATS` counts them next).
- The 02:00 limit ended around 05:05: nice CPU back to 0 %, GPU 1835–1855 MHz, 100 fps median. A window of 02:00–05:00 looks like a scheduled host or media server job; nothing in this CT.
- **Thread placement inside the game** (`src/cpu_placement.cpp`, `nhl_cpu_cluster=auto`: hybrid CPU and no non-Intel GPU): `/proc` after start shows game threads on `4-7` and the 10 `Vulkan Pipeline` threads on `0-3,8-13`. App v launcher script: 93.2, 100.2 v 88.6, 100.0 median (the first pair under the power limit). Any launcher gets it now; `NHL_CPU_CLUSTER=0` in `nhl_play.sh` still gives P-core pinning.
- **`WaitMultiple` with notifying instead of 1 ms polling** (`nhl_wait_multiple_notify`, ~570 calls/s and ~1800 sleeps/s in gameplay): in one match A/B no fps or CPU change (101.4, 103.9 v 102.3, 100.8 median; CPU per frame 20.5, 19.5 v 20.5, 21.4 ms). GPU-limited here; a run on two E-cores follows.
- `WaitMultiple` notify on two E-cores (`4-5`): 81.6 v 81.9 median, 78.5 v 84.1 % ≤17 ms, scene noise. No gain, stays off. Note the weak setup itself: 82 fps median on two E-cores.
- **Promoted to `nhl-linux` ~05:50**: placement inside the game, launcher without the script for it (backup `stable-20261106-pre-placement`). Launcher smoke test: boots, threads placed by the game.
- **PGO of the recompiled code** (`NHLLEGACY_PGO`, profile from one 8 min run, ~15 min in all): CPU per frame base v PGO 19.3, 21.8 v 20.6, 19.7 ms on all cores, 20.3 v 19.4, 19.5 on two E-cores; gameplay median 105.3, 82.0 v 102.0, 104.0 and 85.9, 98.6 v 94.6, 89.3 (one weak base run was a restart). Within noise, at most ~4 % less CPU on the weak setup. Not promoted; build removed, profile in `out/nhllegacy.profdata`.
- **Boot stall isn't gone**: `pgw-base2` at 06:39 stalled on two E-cores (`4-5`), untraced, restarted by `abrun`. That's 1 stall since the 0/73 count; dump in `nhl-work/stall-pgw-base2.*`. It may need few cores or a tight schedule to show. Next: traced boots on `4-5` should reproduce it much faster and read the CV and semaphore counters.

## Boot stall: cause and fix (2026-11-06 morning)
- **Cause**: the game's condition variable (`sub_82670A78` wait, `sub_82670BC0` signal, pthreads-win32 style, over the semaphore wrapper `sub_82671168`/`sub_82671280`) has the gate at +32 on the signal side but not on wait entry, so a waiter arriving after a signal can take the token. The movie update (`sub_8277CB20`) polls with a zero-timeout wait on each tick, also after it set state 2. If its next tick comes before the driver has woken, it takes the driver's token and the driver sleeps forever.
- **Model** (`tools/nuc/tests/guest_cv_model.py`, all interleavings, sequentially consistent): 5 of 15 end states lose the wakeup at 3 ticks, 9 of 23 at 5. With an entry gate 0 of 10 and 0 of 14; with a poll instead of a wait 0 of 2. My first model stopped the update after it set 2 and found nothing.
- **In the game**, forced race `NHL_MOVIE_RACE=1` (the update's zero-timeout wait right after its signal), two E-cores: without fix STUCK 4/4 at 70 s, `state=2 sem=-1`, blocked minus gone = 1, the end state the model predicts; with fix OK 8/8, driver-out as in a healthy boot.
- **Limits**: the unforced stall is rare now: 0/60 on two E-cores just before the fix, plus 0/3 under CPU bursts. The stall of 06:39 has the same driver stack but no counters, so it being the same race is very likely, not shown. The gate applies to all of the game's condition variables.
- **Fix**: `nhl_cv_entry_gate` (default on, `src/diag_hooks.cpp`) passes the gate on entry.
- **Cost**, 2 + 2 full runs on 14 CPUs, interleaved (`gate-on1`, `off1`, `off2`, `on2`), no throttling: presented median 99.9, 102.0 with v 93.8, 95.8 without; gameplay segments 101.1, 101.9 v 102.5, 91.3; gaps > 50 ms 1 (56 ms), 1 (104 ms) v 1 (52 ms), 0. The 104 ms gap isn't near a pipeline compile and its cause is unknown; one sample. Promoted to `nhl-linux` 2026-11-06 08:30; previous binaries in `<backups>/stable-20261106-pre-cvgate`.

## Boot map and quick start (2026-11-06 morning)
- Boots without a pad wait ~30 s on the language screen for its timeout (~37 s), which is where the "~44 s to the EA logo" came from. Almost no file reads in that time and one thread at 100 % on the screen at ~580 fps.
- From screenshots and FE file opens, seconds from the first log line (launch is ~1.5 s earlier): FE files 1–6.3, at a steady 0.1 s per file from ~2; language 6.3; `01_legal` 7.4; EA movie 12.6; `03_pressstart` 12.9; Start accepted ~17.7; `nhlmainmenubackground` 24.1. Start is ignored before ~18 s and on the notice, A on the title. Mid-boot presses speed up the title animation a little.
- Pressing on from the menu reached the ice ~40 s after launch (Play Now with the menu's matchup): "Loading" on the What's New screen for ≤ 10 s. The "~60 s on the lineups screen" (2026-11-05 notes) was the route's fixed press times.
- `nhl_quick_start`: 11/11 boots to the menu, 24.2–24.6 s. Setting the movie's stop flag (+448) didn't shorten the boot, removed.
- Guest clock during the boot (`NHL_BOOT_CLOCK`, `Clock::set_guest_time_scalar`, back to 1 when quick start ends): menu file at 14.2 s at 3× with the clock only (11/11 within 14.2–14.5 s; with the short movie below: 10.3 s, on screen ~12 s), 14.3 s at 8×. Language 3.7, title 5.5, then 8.7 s to the menu whatever the clock. Match afterwards: 100.4 median, gameplay 100.8, 1 gap of 55 ms. Sound not checked (no audio output in benches).
- Short EA movie (`NHL_QUICK_MOVIE_FRAMES`, `src/quick_start.cpp`): `ealogo.vp6` is little-endian chunks (MVhd, SCHl, MV0K/MV0F + SCDl, SCEl). A cut file froze, and so did valid movies of 2, 60, 80 and 100 frames, while 150 ran as before: `ealogo.sub` has `MARKER_ENABLE_START` at frame 131. With 12 frames and the cues at frames 0–3: menu file at 10.3 s. Serving `blank.big` or `00_ealogo.big` for `01_legal.big` hung the boot, once each. Entry sizes go back to the real file when quick start is over.


## Non-draw GPU work: where the 10 ms of the paused replay go (2026-11-07, `p-nondraw`)
Profiler on (`REX_GPU_PROF=1 REX_CMD_STATS=1`), night power cap (absolute ms inflated, shares are the point), paused instant replay
(`NHL_VPAD_SCRIPT` route), per frame at ~39 fps (busy 25 ms), unfused (`nhl_direct_resolve_fused=false`) vs fused. New diagnostics (default
neutral, all under `REX_GPU_PROF`): categories `xfer` (RT ownership transfers), `clear` (resolve clears), `texres` (texture loads from memory a
resolve wrote), `[nhl-gpubar]`/`[nhl-gpubarsig]` (pass end + `vkCmdPipelineBarrier` time by phase and by signature), `[nhl-texloads]`
(`REX_NHL_TEX_STATS=1` or the profiler: loads, bytes, resolved vs CPU memory), `[nhl-rbbatch]`. The old `rt` (2.0 ms) was **transfers and
resolve clears**, the old `tex` (2.1 ms) was **texture loads of resolve output** (not CPU uploads).
| cause (ms/frame, replay) | unfused | fused | notes |
|---|---|---|---|
| draw | 14.3 | 14.5 | 2000 draws, not this item |
| EDRAM dump + its barriers | 2.76 + 0.26 | 0.33 + 0.01 | fused eligible = fast32 single owner; left: full32 2x colour (`shader` reason, 17k of 87k resolves), 64bpp, multi-owner |
| resolve copy + resolve-phase pass end/barrier | 1.50 + 0.87 | 2.49 + 0.94 | of this the **CPU-readback copies** (fast mode, 17/frame, 18 pass-end+barrier ~46 us each) are the biggest part |
| RT ownership transfers + their barriers | 1.58 + 0.23 | same | ~22 transfers/frame ~70 us each; XDK 4x clear-quad aliasing: 4x>2x depth/colour copy-backs of *constant* data |
| resolve clears | 0.23 | same | |
| texture loads of resolve output (22/frame, 35 MB) | 1.31 + 0.28 | same | untile + copy, bandwidth bound; 1280x720 colour 3/frame, 1280x720 depth 2, shadow 2, 640x360 7, 320x184 6 |
| texture loads of CPU memory (VTF bone textures) | 0.34 + 0.11 | same | each ends the pass |
| draw-phase barriers (pass re-entry) | 0.81 | 0.71 | 41/frame |
| rt, swap, prep, upload | 0.3 | 0.3 | |
Live play (106 fps, profiler on, 8.7 ms busy): draw 5.0, xfer 0.79 (+0.15), dump 0.64, resolve 0.43, texres 0.40, rt 0.15, tex 0.11, clear 0.10,
barriers ~0.9: non-draw ~3.7 ms = 43 % of busy; transfers are the largest non-draw item there (11/frame, 4x>2x depth 166k tiles/s).
Render passes per frame 78 (replay): framebuffer switch 18, resolve ends 23 (dump phase; with fused 21 + 5), texture loads 15, transfers 6.8,
pre-draw barriers 12, other 4. One frame of the replay (`REX_RESOLVE_LOG_AT`): 24 resolves, 46 ownership claims; the scene is drawn twice (two
640-wide predicated tiles of 1280x720 2x MSAA); resolves of the same destination repeat (4 x 1A6E9000, 3 x 1A10D000, 3 x 1A0D1000).
**All 18 resolve destinations are GPU-sampled, none is dead** (`NHL_RESOLVE_USE_STATS` "dead" entries are the right halves of textures that start in the
left half).
Added (all default off, hot reload, `NHL_AB_TOGGLE`-able; off path identical):
- `nhl_readback_batch`: with `readback_resolve=fast/some` the GPU copy to the readback buffer is recorded once per destination and frame (final
  contents) and all of them at the swap behind one barrier (17 requests -> 10 copies, resolve-phase barriers 41 -> 24 per frame).
- `nhl_readback_skip_depth`: no CPU readback of resolves that copy depth (3 shadow maps of 2.4 MB per frame stay under the 3 MB size gate;
  only colour render-to-texture results are composited by the guest CPU). Saves copy, barrier and the memcpy into guest memory.
In-process A/B (`NHL_AB_TOGGLE`, 10 s phases, replay after 80 s, presented median fps A off -> B on, paused-replay frames only): batch 59.3 -> 60.8,
55.5 -> 56.9, 58.7 -> 60.1 (-0.4 ms); skip_depth 57.3 -> 59.6, 55.2 -> 57.0 (with batch on: 59.2 -> 60.9, 61.8 -> 64.1); both 54.0 -> 57.0 and
53.0 -> 55.8 (+5 %, -1.0 ms), both on top of the fused resolve (`nhl_direct_resolve_fused=true` in both phases) 65.1 -> 69.4 (+6.6 %, -0.95 ms), live play (gameplay samples only) both 100.3 -> 104.0, p25 99.3 -> 102.5, p5 48.1 -> 60.4 (one run; the shadow maps are
not drawn in play, so live gains come from the batch). Picture: A and B shots of the same paused replay (08.76) differ like A and A do (temporal noise,
mean abs diff 1.15 per channel; boxed 16x16 differences <= 1 level), no missing shadows or helmets. Caveat: several of my runs overlapped other agents'
runs (my wrapper removed the bench lock twice, fixed 07:00) - the repeats above are consistent in sign and size, the broken ones are not listed.
Next in this area: `ClearThroughAliases` for depth + stencil (then the whole XDK clear quad is skipped and all 4x<->2x transfers go away: ~1.8 ms replay,
0.9 ms live), fusing full32 2x colour, resolve-to-texture (removes dump + copy + readback + texture load for ~7 ms of the replay frame).

### Depth + stencil clear-through and fused full32 2x colour (2026-11-07, agent `p-clear`)
Both default off, hot reload, off path identical (build deployed to `nhl-linux-dev` only; no commits).
**A. `nhl_rt_clear_through_depth`** (needs `nhl_rt_clear_through=true` and the NHL clear shader hashes
`REX_NHL_RT_CLEAR_VERTEX_SHADER_HASH=1E6883FCCDE1F688`, `..._PIXEL_SHADER_HASH=A4A965C189287B99`; the cvar defaults are the Simpsons hashes).
`ClearThroughAliases` now also handles the depth/stencil part of the XDK clear quad: when the quad replaces depth+stencil (z always + write,
stencil always/REPLACE/mask FF), the viewport-transformed z is constant and within half a 24-bit step of 0 or 1 (the quad gives -3.7e-9, hence the
tolerance), no polygon offset, D24S8 with native unorm24 host depth, front/back stencil ref equal, and every colour output of the draw was cleared
through too, the depth/stencil tiles are cleared in their single D24S8 owner (`GetSingleDepthOwner`, new in the common cache) with the reference value
and the draw is skipped: no 4x alias render target is bound, no transfers into or out of it. Anything else falls back to the old path
(`[nhl-xfer]` shows `refused N: z / depth_owner / color_no_owner` counters; `REX_NHL_RT_TRANSFER_STATS` logs the first reasons as `[nhl-clearthrough]`,
`REX_RESOLVE_LOG_AT` logs `clear-through color/depth` in the `[nhl-own]` stream). Poison: `nhl_rt_debug_poison_overwrites` fills the area with garbage first.
Per replay frame: ~10 quads, 6-7 are cleared through (colour + depth), the rest are refused: at frame start the range is owned by a 1x *colour* render
target (00008000, post-processing) so there is no D24S8 owner (`depth_owner`), or the 4x colour range spans a colour and a depth owner (`color_no_owner`).
Result (paused replay, in-process A/B, `NHL_AB_TOGGLE=nhl_rt_clear_through=false,true@10`, 3 repeats, presented): median A -> B
56.0 -> 57.0, 53.1 -> 54.2, 57.1 -> 58.1 (+1.8 %, about -0.33 ms), p5 54.8 -> 56.3, 50.7 -> 53.1, 56.2 -> 57.4, min 54.7 -> 55.9, 49.7 -> 52.9, 55.9 -> 57.4.
Live play (gameplay samples >= 90 fps, 2 runs): median 101.9 -> 107.5 and 102.7 -> 106.0, p25 100.4 -> 104.7 and 100.9 -> 104.5, min 95.9 -> 96.2 and 96.7 -> 98.8
(the ~60 fps dips of those runs are scene changes and occur in both phases). GPU profile (`REX_GPU_PROF=1`, replay, per frame): xfer 1.50 -> 1.22 ms,
clear 0.23 -> 0.48 ms, draw 13.78 -> 13.59, busy 23.8 -> 23.5 ms: **the transfers removed are paid back almost 1:1 by the clears** (each
`vkCmdClearAttachments` enters the owner's render pass; same finding as 2026-10-31 for colour only). The fps gain comes from the skipped quad draw
and 4x render passes, not from the transfers. To gain more: a lazy clear (loadOp CLEAR the next time the owner is bound, no extra pass) and handling the
two refused cases (clear the 1x colour owner with the raw word, reinterpret on the later claim; split the 720-tile range over colour + depth owner).
Pictures (A, B, A shots of the paused replay at 08.76, both features toggled): mean abs diff A-A 1.161, A-B 1.164, A2-B 1.174 per channel, 0.11 % pixels > 8 (temporal
noise); a run with `REX_NHL_RT_DEBUG_POISON_OVERWRITES=1` + `REX_NHL_DUMP_POISON=1` and both features on shows a clean picture (no ice/shadow/helmet
corruption), but at 42 fps the replay stood at 08.80 so no numeric diff against the others.
**B. `nhl_direct_resolve_fused_full`** (with `nhl_direct_resolve_fused=true`): the full (non-fast) 32bpp resolve copy shader, 2x MSAA colour, is patched
like the fast ones (`src/graphics/shaders/fused_resolve/gen.py <sdk shader dir> <outdir> full` -> `resolve_fused_full_32bpp_cs.h`; the full shader loads single
words, so each scalar load becomes `LoadEdramV4(word >> 2, mask 1 << (word & 3))` + `OpVectorExtractDynamic`; 96 loads, 88k words per variant, 2 variants
(host 1x / 2x), spirv-val clean). Verified with `REX_NHL_DIRECT_VERIFY=1 REX_NHL_DIRECT_VERIFY_FULL_ONLY=1` (new: spends the verify slots and never gives up before
the first full copy): 200 full32 resolves compared, 0 mismatching dwords; all 47k full32 2x resolves of a run are eligible (4x full32, ~5 %, not generated).
Pipeline creation 1.8 + 2.2 s uncached (0.7 ms with the driver cache; created at the first fused resolve). **No gain**: A/B x3 presented median 63.6 -> 62.9,
64.1 -> 63.6, 61.0 -> 60.6 (p5 62.3 -> 61.3, 61.1 -> 62.6, 59.7 -> 59.6): the 96 inlined per-word image fetches with div/mod addressing cost about what the dump saved.
Stays off; not worth more unless the loads are batched per tile row.

## Predicated tiling: the scene is drawn twice, identical streams, no tile merge possible on the EDRAM-keyed host targets (2026-11-07, agent `p-tiles`)
**Structure** (new diagnostic `REX_NHL_TILE_LOG=<every N frames>`, `[nhl-tiles]` lines; PM4 level: no draw is predicated, `bin_select & bin_mask` never
rejects anything, the game just repeats the command list with `PA_SC_WINDOW_OFFSET.x` 0 then -640 and the resolve rect/dest offset (+0x14000 = 20 tiles) moved).
Main pass = 1280x720 2x MSAA as two 640-pitch tiles (depth base 0x2E0 / colour 0, 720 EDRAM tiles each, colour + depth = 1440 of 2048 tiles). Per frame (replay):
tile A 530 draws, tile B 434; **every tile-B draw also exists in tile A** (multiset of hash(vertex shader, pixel shader, prim type, index count, host vertex count):
only-in-A 96, only-in-B 0, shared 434; vertices A 648950 / B 648854, i.e. the 96 are the 24 four-vertex clear quads of tile A). Same order for the first 349-357 draws,
the rest differs only by where those clear quads sit. Raw guest scissors differ in all draws (shifted by the window offset), but **0 draws have an empty scissor in
either tile** (the game does not cull per tile), so `nhl_draw_skip_noop` has nothing to win here and a "skip draws outside the tile" rule skips nothing.
The tile-B draws are not redundant: they write the right half (disjoint pixels), only their vertex/state work is duplicated (1.53 M vertices per tile in the heavy replay).
**Ceiling** (`nhl_test_skip_tile_b`, measurement only, hot reload, picture wrong): skip all tile-B draws; in-process A/B `nhl_test_skip_tile_b=false,true@10`, paused replay,
3 repeats presented median (p5, min) A -> B: 55.4 (54.4, 54.3) -> 65.2 (64.7, 64.2); 56.2 (55.1, 55.0) -> 67.2 (65.2, 63.3); 59.0 (57.7, 57.5) -> 70.1 (68.8, 68.5) = **-2.7 to -2.9 ms**
(+18-19 %). With profiler: 41.5 -> 52.7, draw 549 -> 444 ms/s (13.2 -> 8.4 ms/frame). Live play (1 run, gameplay): 103.8 (p5 59.3) -> 116.5 (p5 66.1), -1.05 ms.
This includes the right-half pixel work a real merge must still do (earlier half-pixel test: pixels are at most about half of the main pass), so a perfect merge
would gain roughly half of that: ~1.4 ms replay, ~0.5 ms live, +5-8 %, plus 2 of the 4 EDRAM dumps/resolve passes of the main surface.
**Why no `nhl_tile_merge`**: a merged draw needs a 1280x720 2x host target (colour + depth = 2880 EDRAM tiles). The render target cache keys targets, ownership
(`[nhl-own]`), transfers, dumps and resolves by EDRAM tile index (0..2047, here pitch 8 tiles), so the wide target has no address space; tile B would also have to
become a no-op resolve while every consumer still sees EDRAM addresses of the two halves. Rewriting that is days, not hours, and a wrong ownership state corrupts
the picture silently, so nothing was added. Other routes looked at and rejected: draw-level culling for tile B needs per-draw screen bounds (not available without
running the vertex shader; GPU bounds + `VK_EXT_conditional_rendering` would add atomics to tile A and a draw-index mapping, for a fraction of the 1.4 ms),
skipping by last frame's coverage is unsafe (pops), instancing/multiview needs the 1280-wide target again.
Added (default off, off path identical, build in `nhl-linux-dev`): `REX_NHL_TILE_LOG`, `nhl_test_skip_tile_b`.


## Pixel shader cost: ranking, what is heavy, `nhl_ps_implicit_lod` (2026-11-07, `p-pixshader`)
Profiler on (`REX_GPU_PROF=1 REX_GPU_PROF_SHADERS=3`, `[nhl-gpupair]`), base = spec + register vars (`REX_NHL_TEXTURE_UNSIGNED_SPEC=true REX_NHL_REGISTER_VARS=true`), dev build,
Arrow Lake iGPU, Mesa 25.0.7 ANV. SPIR-V dumped with `REX_NHL_SPIRV_DUMP` (the path must be the container's: `/home/retro/nhl-linux-dev/<dir>`, not `~/...`).
Tools in `<workdir>/p-pixshader/`: `rank.py` (PS ranking by hash), `abshader.py` (per-PS ms, phase A v B), `psinfo.py`/`chars.py` (static class), `ab_an.py`/`agg.py`, `pngdiff.py`.
**Ranking (GPU ms per presented frame, draw part only; shares of draw time).** Paused replay (A/A run, 45 fps, draw 10.8 ms, 103 distinct PS):
4C28AE7A 1.34 ms (12.4 %, 88 draws), 50040AB5 0.69, **no pixel shader (depth only, 349 draws) 0.68**, 6BD92F21 0.57, 0B6A7668 0.47, 80165FD0 0.40 (225 draws), CB27BC23 0.38,
788DD540 0.36 and 57746B41 0.33 (both 52 instructions, no fetch, 118 draws each), A4A965C1 0.35, 28E04499 0.32, E43F2463 0.29, 6D1D20B8 0.29, 389E88C9 0.22.
Top 12 = 57 % of draw ms. **Live play** (71 fps, draw 7.7 ms) is flat: 0AE84572 0.42, depth only 0.36, 788DD540 0.34, A4A965C1 0.33, FB6737A0 0.31, 57746B41 0.31,
B48733CD 0.28, B453EDB7 0.27, 4C28AE7A 0.25, 6BD92F21 0.19: the top 10 are 3.1 of 7.7 ms.
**What they are.** Seven of the replay's top 13 (4C28, 5004, 6BD9, CB27, 28E0, 6D1D, 389E = 3.8 ms, 35 % of draw ms, 18 % of the 21 ms busy) are the same kind of shader: a lit
surface with a **16-tap shadow-map filter on texture 15 (one fetch per tap: a 4x4 grid at offsets -1.5/-0.5/0.5/1.5 texel, +1.5/1024) plus ~10 material fetches (2D, 2 cube maps)**:
spec SPIR-V 4100-5200 instructions, 25-30 fetches (52-60 without the spec), 2 uniform branches, ~130 `0*x` checks, ~50 FDiv (two per fetch for the offset/size), one colour
output (RT0), early-Z on (`EarlyFragmentTests`), no discard. Mid ones (0B6A, E43F, 0AE8, FB67: 1200-2100 instr, 7-8 fetches) are skin/jersey-like; 80165FD0 (673 instr, 4 fetches) is
a cheap material drawn 225 times; 788DD540/57746B41/A4A965C1 (46-52 instr, no fetch) are UI/clear-like quads (A4A9 has a discard variant without early-Z, 0.35 ms). Nothing in the top 15
loses early-Z except that one; none writes more than one colour target; none has `discard` otherwise.
**Rewrites tried (all default off, in-process A/B `NHL_AB_TOGGLE`, replay median, 1 run each unless noted; the translation differs per setting, so toggling in process just selects another
cached pipeline):** (a) `nhl_ps_float_relax=1` (no `SignedZeroInfNanPreserve`): 62.7 -> 62.5; `=7` (also no `DenormFlushToZero`/`RoundingModeRTE`): 58.8 -> 58.6;
(b) `nhl_ps_fast_math` (no `0*x` check until an rcp/rsq/log: cuts ~130 of ~440 muls' check per shader): 61.7 -> 61.9: **no effect, so ALU is not the limit; the heavy shaders are sampler bound**;
(c) **`nhl_ps_implicit_lod`**: 63.1 -> 66.8, 61.7 -> 65.7 (4 repeats below); (d) `anisotropic_override` 3 (the default, 4x forced) -> 0 on the old gradient path: 61.0 -> 60.7 (nothing, see below).
**`nhl_ps_implicit_lod` (the one that works).** Every 2D fetch with computed LOD was emitted as `OpImageSampleExplicitLod ... Grad` with `DPdxCoarse/DPdyCoarse` of the coordinate scaled by `2^lod_bias`
(a `sample_d` message plus ~8 ALU ops, a bias decode and an `exp2` per fetch). With the flag the same fetch is `OpImageSampleImplicitLod ... Bias lod` (a plain `sample_b`, hardware derivatives),
only for k2D fetches without register gradients (cube, 3D/stacked, 1D, explicit LOD unchanged). Flag storage: bit 15 of `pixel.interpolators_centroid` (0 unless the shader has interpolator 15, then it is
just not applied); the other experiment flags use the 4 free bits of `param_gen_interpolator` for shaders without ps_param_gen. All read at pipeline selection (`pipeline_cache.cpp`), so they
are hot reload with their own cached translation. SPIR-V 4C28: 5572 (generic) -> 4258 (spec) -> 4114 (spec + implicit), derivative ops 26 -> 2.
**Result, presented fps, A off -> B on, in-process A/B (10 s phases, `ab_an.py`, gameplay seconds after 82 s):** paused replay, 6 repeats (4 with build 2 = ps_param_gen shaders not covered, 2 with build 3 = covered), per-repeat median
63.1 -> 66.8, 61.7 -> 65.7, 62.5 -> 66.3, 60.8 -> 64.6, 65.0 -> 68.5, 63.0 -> 67.1: **pooled median 62.7 -> 66.5 (+6.1 %), p5 60.5 -> 64.2, p1 59.7 -> 63.2, min 58.8 -> 62.9** (every repeat +3.5 to +4.0 fps).
Live play (quick route, 150 s, 5 repeats): 108.0 -> 112.6, 109.8 -> 111.6, 108.9 -> 113.0, 109.6 -> 112.0 (p5 51 -> 63, 62 -> 68, 51 -> 67, 58 -> 67) and **one repeat that does not count (109.4 -> 66.1: the B phases fell on slow-scene seconds, 55-70 fps, which also
hit A phases of the same run at 81-90 s and 120-130 s; live scenes are not repeatable and the 10 s phases alias with them)**: 4 of 5 repeats +1.6 to +4.3 % median (pooled of the four 109.0 -> 112.3), p5 +6 to +16 fps; "seconds >= 100" is not comparable in live. Profiled A/B, GPU ms per frame, replay: draw 11.04 -> 9.32 (-15.6 %), busy 21.4 -> 19.6 (-8.3 %); per shader 4C28 1.33 -> 0.87,
5004 0.70 -> 0.43, CB27 0.41 -> 0.28, E43F 0.45 -> 0.32, 6BD9 0.57 -> 0.49 (-14 %; -0 % before the param_gen coverage), untouched shaders +-3 % (A/A noise of this method: -3 %);
live (build 2): draw 7.70 -> 6.85, busy 14.7 -> 13.5; 4C28 0.25 -> 0.13, FB67 0.31 -> 0.19, B487 0.28 -> 0.22, 0AE8 0.42 -> 0.35, DF2A 0.14 -> 0.05.
**Picture.** Two A/B shot sets (replay frame 08.76, identical content; A = flag off, B = on; A/A = same setting at two times): flag alone, A/A mean abs diff 1.17/255 (2000 pixels > 8),
**A/B 1.87 (37 000 pixels > 8, 88 of 1440 40x40 blocks > 3 where A/A has 0)**, concentrated on oblique ice decals and board text (EA SPORTS logo, rink boards): B is visibly sharper. Cause: the gradient path
(`sample_d`) **ignores the forced 4x anisotropy (`anisotropic_override` 3), the implicit path honours it** (so the earlier A/B of 3 -> 0 showed nothing: 61.0 -> 60.7, and with the flag on it costs nothing either:
64.7 v 65.0). Proof: same A/B with `anisotropic_override=0` in both phases: A/A 1.176, B/B 1.149, **A/B 1.168 = identical picture**, and still faster (55.5 -> 60.5, +9 %). So the change is pixel-identical
when anisotropy is off and "textures sharper as the setting always promised" when it is on; the maintainer's call (default 3 stays).
**Next, not done: a gather for the 16-tap shadow filter.** The taps are 16 point (NEAREST: depth maps are forced to nearest unless `shadow_filter_linear`) fetches of one channel on a 4x4 texel block at fixed
offsets; four `OpImageGather` (each a 2x2 block, base `floor(uv*size - 1.5 + 0.5)`, +2 for the next block) return exactly the same 16 values (address modes per texel), i.e. 16 -> 4 sampler messages in the 7 shaders that are 35 % of the replay's draw ms
(and ~half of those 16 messages' cost in live). Needs: pattern match of the 16 fetches (same register, constant offsets), nearest + single-mip check at pipeline selection (a flag bit; the float-controls
and fast-math flags above are free to give theirs up), results cached in function variables per exec clause. Estimated 0.5-1 ms/frame in the replay, not measured.
Added (default off): `nhl_ps_implicit_lod`, `nhl_ps_float_relax` (bit 0 no SZINP, 1/2 no FTZ+RTE), `nhl_ps_fast_math`; files `spirv_translator.h`, `spirv_translator.cpp`, `spirv_translator_alu.cpp`,
`spirv_translator_fetch.cpp`, `vulkan/pipeline_cache.cpp` (SDK, uncommitted). The last two cvars did nothing measurable and can go.

### `nhl_ps_pcf_gather`: 16 nearest shadow taps -> 4 `OpImageGather` (2026-11-07, `p-gather`): works, picture-identical, but only +2 % alone and ~0 on top of `nhl_ps_implicit_lod`
Default-off cvar `nhl_ps_pcf_gather` (SDK, uncommitted: `spirv_translator_fetch.cpp`, `spirv_translator.cpp/.h`, `vulkan/command_processor.cpp`). **How:** fetch constant 15 only (the shadow map; 4C28 etc. use it for all 16 taps).
In the translator each `tfetch2D` on fc 15 with offsets in {-1.5,-.5,.5,1.5}^2 (k = offset+1.5, lane = k&1, gather shift = -1 for k<2 else +1 texel plus the usual 1.5/1024 epsilon) becomes one lane of a cached 2x2 gather
(`gather coord = u + (shift+eps)/size`, lanes x=(i0,j1) y=(i1,j1) z=(i1,j0) w=(i0,j0)); cache per (register, swizzle, component) in the build block, invalidated by any `StoreResult` to that register (and relative writes).
Conditions in the translator: textures_all_unsigned, view swizzle by image view, no resolution scale, normalised coords, exactly one used component, instruction filters UseFetchConst/point, aniso off. **Flag**: the translation cannot know the sampler,
so `command_processor.cpp` probes `GetSamplerParameters` for fc 15 (UseFetchConst everywhere) and sets bit 2 of `pixel.param_gen_interpolator` (free flags, only shaders without ps_param_gen, so 6BD9-type shaders are not covered) only if mag/min nearest, aniso off, 2D, mip_min==mip_max.
In the replay every draw is eligible (`[nhl-gather]` log: mag_lin=0 min_lin=0 aniso=0 mips 0..0). 4C28: 26 samples -> 10 samples + 4 gathers, spirv-val clean. Not strictly bit-exact only for float rounding of the gather coordinate within 1e-4 texel of a texel edge (the epsilon is 1.5e-3).
**Result**, in-process A/B (`NHL_AB_TOGGLE`), replay, implicit LOD off (the Grad path): median 61.6->62.6, 57.9->59.3, 61.4->62.7, 61.6->62.8 (**+1.6 / +2.4 / +2.1 / +1.9 %**, p5 +1.0, +0.1, +0.9, +1.3); profiled draw ms 11.66 -> 10.78 (one run).
With `nhl_ps_implicit_lod` on in both phases: 62.1->62.3, 63.8->64.0, 61.9->62.1 (+0.3 %), profiled draw 9.28 -> 9.22: **the gain is gone because the taps were only expensive as `sample_d`; as `sample_b` the 16 nearest taps cost little**. Live (150 s, 3 repeats, off): median 109.6->108.7, 110.1->110.7, 109.0->107.6 (noise), p5 +2.7, +0.5, +2.4.
**Picture** (replay frame, 4 shots/run, A/A 126v146 vs A/B 126v136): A/A 1.185, A/B 1.196 / 1.194 / 1.207 mean abs diff, pixels>8 1557 v 1571-2186: identical within noise. **Verdict:** correct and safe but not worth enabling next to implicit LOD; the shadow filter is no longer the bottleneck. Remaining 4C28 cost is the ~10 material fetches and ALU. Cvar stays default off; could be deleted.

## Stacked picture-identical GPU wins (`NHL_FAST`, 2026-11-07/08)

The launcher `tools/nuc/ct105/nhl_play.sh` turns this set on unless `NHL_FAST=0` (each cvar can also be turned off alone):
`nhl_texture_unsigned_spec`, `nhl_register_vars`, `nhl_direct_resolve_fused`, `nhl_readback_batch`, `nhl_readback_skip_depth`,
`nhl_rt_clear_through` (+ depth, with the two clear shader hashes), `nhl_ps_implicit_lod`, `anisotropic_override=0`, and the
vertex-shader pair `nhl_vs_texture_unsigned_spec` + `nhl_fast_vertex_math` (SDK patch 0042). The binary defaults stay off.

- **Dev snapshot, nine cvars on vs off, 3 interleaved repeats** (paused replay pooled): min 50.8 -> 71.4, p5 51.6 -> 72.8, median
  53.8 -> 75.7 fps (**+40 %**). Quick route, full-length runs: min 45.7 -> 69.6, p5 60.6 -> 105.5, median 103.1 -> 128.2, seconds at
  or above 100 fps 76 -> 95 %.
- **Installed build, GPU power-limited (clocks 1350-1600 MHz), interleaved:** quick route median 83.8/83.2/84.4 (p5 44-42) without,
  101.7/104.4/104.3 (p5 58.5/60.8/70.8) with: **+24 % median, p5 about +20 fps**. Paused replay 51.8/47.1/55.1 -> 70.2/72.7/74.3 (**+40 %**).
- **Picture:** in-process A/B of all nine cvars on three paused-replay scenes: mean abs difference per channel 1.18-1.22 between
  A and B, the same as B against B (temporal noise about 1.17). No visible difference.
- **Streaming (Moonlight client attached, synthetic session, 1080p60 encode):** replay median 64 with the set against 48 without; the
  encoder and client cost a few percent (-4 % in an earlier pair).
- **Limits:** one machine, GPU clocks varied 10-15 % between runs, replays and close-ups are still far below 100 fps; 11 of 12 quick
  runs with the set ran to the end with a clean shutdown, one was killed by the harness timeout with no log kept (cause unknown, not
  seen again in two reruns).
