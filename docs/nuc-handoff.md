# Handoff — NUC 60 fps work (resume here)

Goal: stable 60 fps in gameplay on the NUC (Core Ultra 5 225H / Arc iGPU, Proxmox CT 105 =
Games on Whales, 192.168.0.22) with the Linux build from branch `nuc-perf` of erik-131/nhl-legacy-recomp.

## Where things are
- Build env (cloud session): clang-20, `third_party/rexglue-sdk-main` = rexglue c94f5eb + `sdk/rexglue-macos-fsi-tiling-fixes.patch`
  + `sdk/rexglue-rptr-incremental.patch` + `sdk/rexglue-fence-wait-stats.patch` (local SDK branch `nhl-nuc`).
  Codegen needs the user's vanilla `default.xex` in `game/` (sha256 d818eb15…, copy from the user's PC `Downloads\nhl\game`).
  Configure: `cmake -S . -B out/linux-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DREXSDK_DIR=$PWD/third_party/rexglue-sdk-main -DREXGLUE_ENABLE_TRACY=OFF
  -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20` (needs libsnappy-dev, X11/xcb dev pkgs).
  Full build ~37 min on 2 cores. SDK-only changes rebuild `librexgpu-xenos.so` (runtime-loaded plugin) without relinking the game.
- Package: `nhl-linux/` = stripped `nhllegacy` + `librexruntime.so` + `librexgpu-xenos.so` + `libsnappy.so.1` + ini, tar.xz (~26 MB).
- Deploy: put `nhl-linux.tar.xz` in the user's `Downloads\nhl`, run `nhl_auto_install.bat` (ssh key `%USERPROFILE%\.ssh\nhl_nuc`
  → root@192.168.0.22, from="192.168.0.85"). Installs to `/etc/wolf/profile-data/user/WolfSteam/nhl-linux` (game -> ../nhl/game).
- Remote commands: write `nhl_cmd.sh` into `Downloads\nhl` (stage under a NEW unique filename each time — the commit tool caches by
  staged path), double-click `nhl_cmd.bat` in Explorer (computer use), read `nhl_cmd.log`. `nhl_get.bat` pulls `shot_230s/270s.png`.
- Benchmark: `/root/nhlbench/nhl_bench.sh <tag> [env]` (= `tools/nuc/nhl_bench.sh`). Needs a running Wolf Steam session with NO
  Moonlight client attached (a client's controller makes the vpad player 2 → stuck on sign-in dialog). ~5 min/run, results in
  `/root/nhlbench/last_run.out`. Verify gameplay via screenshots.

## Findings so far (clean runs, see docs/nuc-perf-notes.md)
fsi 96.6 ms (10.4 fps, GPU ~70 ms/frame) · fsi+readback=some 72.5 ms (GPU-bound) · rtv 40.9 ms (3D black; CP thread ~30 ms/frame limits).

## Next steps
1. `NHL_DRAW_TIMING=1` runs (timing-base / timing-rtv) → where the ~30 ms CP time goes (Redth: 2 µs/draw on M4).
2. Port The Simpsons Game recomp's Vulkan host-RT ("native_*") path (YesterMester/TheSimpsonsGameRecomp tools/rexglue-sdk)
   onto our SDK so rtv renders correctly; then lazy readback write-back.
3. CP-thread cost reduction to < ~14 ms/frame.
