# Handoff — NUC 60 fps work (resume here)

Goal: stable 60 fps in gameplay on the NUC (Core Ultra 5 225H / Arc iGPU, Proxmox CT 115 (was CT 105 before the
unprivileged conversion) = Games on Whales/Wolf, 192.168.0.22, 16 GB RAM) with the Linux build from branch `nuc-perf` of
erik-131/nhl-legacy-recomp. All measurements: `docs/nuc-perf-notes.md`.

## Setup (as of 2026-10-30)
- The Wolf CT is now **CT 115, unprivileged** (uid 0 → 100000), and Claude Code runs **inside it** as user
  `claude` (sudo) via `claude remote-control`; the repo is `/home/claude/nhl-legacy-recomp` with
  `/opt/nhl/nhl-legacy-recomp` symlinked to it. Debian 13 has no clang-20, so it comes from apt.llvm.org
  (`llvm-toolchain-trixie-20`, 20.1.8). The `tools/nuc/ct105/` directory keeps its old name. One-time setup: `bash tools/nuc/ct105/setup.sh` (toolchain, Claude Code, repo in
  `/opt/nhl/nhl-legacy-recomp`, SDK = rexglue c94f5eb + `sdk/rexglue-nhl-nuc-combined.patch`, default.xex
  from the game install, bench script to `/root/nhlbench/`). Agent rules: `tools/nuc/ct105/CLAUDE.md`
  (copied to `CLAUDE.local.md`). Build/deploy: `tools/nuc/ct105/build.sh [--deploy]`.
- Game install (never in git): `/etc/wolf/profile-data/user/WolfSteam/nhl` (Windows install, `game/`);
  Linux build dir `.../WolfSteam/nhl-linux` (`nhllegacy`, `librexruntime.so`, `librexgpu-xenos.so`,
  `game -> ../nhl/game`, `logs/`). Steam shortcut "nhllegacy.exe" lives in the Wolf Steam profile.
- Older paths (still valid as fallback): cloud session + Windows PC bats in `Downloads\nhl`
  (`nhl_cmd.bat` runs `nhl_cmd.sh` over ssh key `nhl_nuc`, `nhl_upload.bat`, `nhl_get.bat`, `nhl_backup.bat`);
  build-CT + gate design in `tools/nuc/buildct/` (option C, superseded by running in CT 105).
- After the conversion check: `/dev/dri/renderD128` + render gid, `/dev/uinput` + `/dev/uhid` (virtual
  gamepad for the bench route), `nesting=1,keyctl=1`, docker works, Wolf session starts.

## Benchmark
`/root/nhlbench/nhl_bench.sh <tag> "<env>"` (= `tools/nuc/nhl_bench.sh`, v9). Time-based vpad route reaches
gameplay ~178 s after the first fps line; window = 60 s of gameplay; screenshots at 230/270 s. Needs a Wolf
Steam session: `tools/nuc/ct105/wolf_session.sh start` creates it headless via the Wolf API
(`/api/v1/lobbies/create`, same as Wolf UI → User → Steam → Start; no Moonlight client needed). Aborts if MemAvailable < 4 GB; memory per run logged in `mem.log`.
The CT hung twice today with memory "used" growing ~0.6–1.7 GB per run — watch `mem.log`.

## Key findings (2026-10-29)
1. Guest game logic costs only ~9 ms/frame; the guest spends 70–92 % waiting on the GPU path.
2. **fsi (correct image) is GPU-bound**: ~70–100 ms GPU per frame, 95 % of it in the draws themselves
   (fragment-shader-interlock EDRAM emulation ~7x the rtv draw cost on Arc). Texture loads, resolves and
   uploads are ~4 ms total. CPU-side work cannot fix fsi.
3. **Guest vsync quantization**: D3D presentation interval 1 makes the GPU stream wait for the next vblank
   in the swap callback `sub_827F0D88` (CP interrupt + WAIT_REG_MEM on scratch1). ~20 ms CP frames got
   quantized to 33 ms. Hook `NHL_SWAP_INTERVAL=0` (src/diag_hooks.cpp) → rtv 33 → 43.8 fps. Game-speed
   side effect not verified yet.
4. rtv (host render targets) + `NHL_VK_READBACK_MODE=some` + interval 0 = 43.8 fps, now GPU-bound (~22–28 ms
   GPU/frame: draws 52 %, RT updates 20 %, resolves 10 %). But **rtv renders the 3D scene black**.
5. rtv black-3D investigation (`REX_RESOLVE_LOG_AT=230`): an rtv frame has 13 resolves vs 24 in fsi; the
   3D pass depth resolve is nearly empty and the final resolve nearly black → the 3D draws do not rasterize
   in rtv. Not caused by native 2x MSAA (`REX_NATIVE_2X_MSAA=false`) or stencil export
   (`REX_NATIVE_STENCIL_VALUE_OUTPUT=false`). Draw-state runs logged in `/root/nhlbench/runs-fsi.txt` and
   `runs-rtv.txt` (lines `draws xN rp=… surf=… depth=… dctl=… cmask=… win_off=… sc_mode=…`) — diff them next.
   3D pass: surf=0A010280 (pitch 640, 2x MSAA), depth base 0x168/0x2E0, predicated tiling in two 640-wide
   halves (window offset 0 and -640).
6. Readback: per-destination limit (`NHL_VK_RB_MAX_PER_DEST=2`) is as fast as `readback=some`; note `some`
   effectively never copies after the first miss.
7. With the vsync cvar on (normal play, no `NHL_VK_NO_VSYNC`), CP WAIT_REG_MEM sleeps ≥1 ms per failed poll.
8. Audio Worker thread burns ~75 % of a core (busy-wait) in every run.

## Diagnostic switches (all env)
`REX_FENCE_WAIT_STATS`, `NHL_FENCE_SPIN_STATS` (guest fence wait + caller histogram), `REX_CP_IDLE_STATS`,
`REX_CP_PROFILE` (per-PM4-opcode CP time), `REX_WRM_TRACE` (WAIT_REG_MEM per address + packet history),
`REX_GPU_PROF` (GPU timestamps per category), `REX_RESOLVE_LOG_AT=<s>` (one frame of resolves + draw state;
forces sync readback for that frame), `NHL_VK_RB_TRACE`, `NHL_VK_RB_MAX_PER_DEST`, `NHL_SWAP_INTERVAL`,
`NHL_VK_RT_PATH=fsi|rtv`, `NHL_VK_READBACK_MODE=full|fast|some|none`, any cvar via `REX_<NAME>=value`.

## Status 2026-10-30
- **rtv renders correctly** (fix: `execute_unclipped_draw_vs_on_cpu=true`, see perf notes) and is the app
  default. Play config `nhl-linux/nhl_play.sh` (= `tools/nuc/ct105/nhl_play.sh`: interval 0,
  `NHL_VK_RB_MAX_PER_DEST=2`, vsync on) measures 38.5 fps (p5 30, median 38.5) in gameplay; bench config
  (no vsync) 40–43 fps. Steam shortcut "NHL Legacy (Linux)" → `nhl_play.sh` (native, no Proton).
- Game clock is time-based: `NHL_SWAP_INTERVAL=0` keeps correct game speed.
- GPU contention: the iGPU runs at 1.1–1.25 GHz with no game running in the CT; absolute numbers today are
  ~15–20 % below 10-29.

## Next steps
1. Erik play-tests rtv (watch for transition artefacts, HUD text, equipment textures).
2. GPU profile (`REX_GPU_PROF`) of the *correct* rtv to see what is left (draws vs RT updates) → < 16.7 ms.
   The Simpsons native host-RT path is the candidate if RT updates/transfers still cost a lot.
3. Vsync-on cost (CP WAIT_REG_MEM sleeps), CP per-draw cost, Audio Worker spin.

## Older next steps (10-29)
1. Diff draw-state runs fsi vs rtv → find why 3D draws don't rasterize in rtv; fix in the Vulkan host-RT path.
   Reference for a working host-RT path: The Simpsons Game recomp's `tools/rexglue-sdk` (`native_*` cvars;
   their notes: `simpsons/re/native_renderer_plan.md`).
2. Once rtv is correct: get GPU time < 16.7 ms (rtv draws ~14 ms, RT updates ~5 ms under the profiler).
3. Verify game speed with `NHL_SWAP_INTERVAL=0`; consider host-paced presentation.
4. Then CP-thread cost (~4–7 µs/draw) and the Audio Worker spin.
