# Native Linux build: setup, launcher, known issues

This is the **native Linux (ELF) build** of this fork. It is separate from [linux.md](linux.md), which covers the Windows
build run through Proton. Everything below comes from this repo and the measurements in
[nuc-perf-notes.md](nuc-perf-notes.md) and the development log (not published). Anything not checked from the repo is marked
**unverified**.

**Tested on one machine only:** Intel Core Ultra 5 225H mini PC (Arc iGPU, Mesa ANV 25.0.7), Debian 13 container on
Proxmox, streamed with Moonlight through Games on Whales / Wolf. Other GPUs, other distributions and a plain desktop
session (no Wolf) are **unverified**.

You need your own legally obtained copy of NHL Legacy. This repo contains no EA data, and nothing described here
ships any. Do not commit `default.xex`, `game/`, `generated/`, shader storage, database files or exports.

## 1. What you provide

- A game folder `game/` containing `default.xex` and the game's files. The repo builds it with the release builder
  (`nhl-legacy-builder install --iso ... --out ...`, see [../README.md](../README.md) and the Proton flow in
  [linux.md](linux.md)): it checks `default.xex` by SHA-256, extracts the disc image and unpacks the `.big` archives
  into a loose tree (`DEV-README.md`). The builder is a Windows program; on Linux it was run via Proton (the maintainer
  copied a finished install; running the builder natively is **unverified**).
- The port only accepts one vanilla `default.xex`. `tools/nuc/ct105/setup.sh` checks
  `d818eb15775c1fee34b057b0fc494b211535d07123230ee45bd7d40991c55624`. A different region or a title-updated image will
  not pass.
- The native binary finds `game/` next to the executable (`OnConfigurePaths`, `DEV-README.md`). The maintainer's install
  uses a symlink `game -> ../nhl/game`.
- Codegen reads `game/default.xex` inside the repo checkout (copied there by `setup.sh`).

## 2. Toolchain actually used

| Part | Version / source |
|---|---|
| OS | Debian 13 (trixie) container; `setup.sh` installs packages with apt |
| Compiler | **clang-20 / clang++-20** and **lld-20** (from apt.llvm.org, because trixie has no clang-20) |
| Build | CMake + Ninja; game built Release with `-march=x86-64-v3` (needs a CPU with AVX2) |
| SDK | rexglue-sdk at commit `c94f5eb` plus `sdk/rexglue-nhl-nuc-combined.patch`, in `third_party/rexglue-sdk-main` (excluded from git, local branch `nhl-nuc`) |
| Libraries | Vulkan dev, glslang, spirv-tools, snappy, X11/Wayland/ALSA/Pulse/GTK/udev/etc. (full list in `setup.sh`) |
| Movies | a static `ffmpeg` next to the binary (the VP6 movie bridge needs it; without it movies are green) |

The combined patch is checked to apply to a clean `c94f5eb`. The same changes are also kept as an ordered series in
`sdk/series-nhl-nuc/` (39 files) and as single patches in `sdk/`. A few are off-by-default experiments.

## 3. Build

`tools/nuc/ct105/setup.sh` (as root) does the one-time part: apt packages, clang-20, SDK clone at `c94f5eb` with the
combined patch applied, `game/default.xex` check, static ffmpeg download with checksum. It is written for the
maintainer's container: it assumes `/opt/nhl/nhl-legacy-recomp` and a Wolf profile under
`/etc/wolf/profile-data/user/WolfSteam/`. Adapt the paths for another machine (**unverified** elsewhere).

Then:

```sh
tools/nuc/ct105/build.sh                       # SDK preset linux-amd64 + codegen + build
DEST_DIR=nhl-linux-dev tools/nuc/ct105/build.sh --deploy   # copy binaries into an install folder
```

What `build.sh` does: builds `rexglue` with `cmake --preset linux-amd64` (clang-20, Tracy off), runs
`rexglue codegen nhllegacy_manifest.toml` once (creates `generated/`), configures `out/linux-rel` (Ninja, Release,
clang-20, lld-20, `-march=x86-64-v3`) and builds `nhllegacy`. `--deploy` installs `nhllegacy`, `librexruntime.so` and
`librexgpu-xenos.so` into `$DEST` and strips them. `--deploy` to the default `nhl-linux` install kills a running game.

Measured notes from the repo:
- First build about 60 minutes (handoff). Keep `JOBS` at 4 or less while the game runs; the container hung twice from
  memory pressure (16 GB container).
- If the runtime library ABI changes (for example the `XThread` layout), touch the SDK sources so `librexruntime.so`
  is rebuilt as well, or the game crashes at boot.
- Disk was about 90 % full on the maintainer's machine; keep `generated/`, `out/` and the SDK build.

## 4. Install folder and where files go

An install folder (the maintainer uses `nhl-linux` for playing and `nhl-linux-dev` for experiments) holds:
`nhllegacy`, `librexruntime.so`, `librexgpu-xenos.so`, `nhl_play.sh`, `ffmpeg`, `game` (the game folder or a link to
it), `logs/`. Optional: `nhl_pin.sh` (P-core pinning), `nhl_fill.sh`, `nhl_enhancements.ini`, `nhl_pregame.ini`.

| What | Where |
|---|---|
| Logs | `logs/nhllegacy_*.log` next to the executable |
| Shader storage | `<cache root>/shaders/shareable/` (the maintainer's cache root is `~/.local/share/nhllegacy/cache`); `REX_CACHE_ROOT` overrides the cache root |
| Loose-file overlay and served database | `<cache root>/guest/...` (see [mods-and-tweaks.md](mods-and-tweaks.md)) |
| Settings files | beside the executable |

The shader storage is shared by every install that uses the same cache root.

## 5. Launcher `nhl_play.sh`

`tools/nuc/ct105/nhl_play.sh` is the tested way to start the game. It `cd`s to its own folder, exports the defaults
below (an already set variable wins, so `VAR=value ./nhl_play.sh` or Steam launch options `VAR=value %command%`
override them) and executes `./nhllegacy`. Dates are when the number was measured; "run-to-run variance is 10 to 30 %"
(bench README), so single figures are indicative.

| Variable (default) | What it does and the measured effect |
|---|---|
| `NHL_SWAP_INTERVAL=0` | Guest present interval 0: no hard 60 to 30 drops. The game clock is time based (verified 2026-10-30). `2` gives a steady 30 fps lock. |
| `NHL_VK_READBACK_MODE=fast` | Double-buffered readback, one frame stale, no GPU drain. Fixed green garbage on equipment (helmets); the per-destination limit did not (issue 6). `none` is faster, see mods doc. Values: `full`, `fast`, `some`, `none`. |
| `NHL_NO_GUIDE=1` | The Guide button goes to Steam, not the in-game overlay. |
| `SDL_VIDEO_DRIVER=x11`, `DISPLAY=:0` (unless `NHL_WAYLAND=1`) | X11 window through Xwayland, because the Steam overlay hooks X11. `NHL_WAYLAND=1` keeps a native Wayland window. |
| `NHL_DIRECT_PRESENT=1` | Present every guest frame from the output thread. With an always-registered ImGui drawer only about every second frame reached the screen. Costs the F1 overlay and achievement toasts. |
| `REX_CLEAR_MEMORY_PAGE_STATE=false` | Do not re-upload all guest memory each frame. Visible fps median 46 to 61 over 5 interleaved runs each (launcher comment); README states +30 %. Set `true` if stale geometry shows. |
| `REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS=true` | Skip EDRAM ownership transfers into areas the next draw overwrites. Presented fps p5 36 to 60 over 2 interleaved runs each. Set `false` if a pass looks wrong. |
| `REX_NHL_UPLOAD_PREPASS=true` | Record shared-memory uploads in a pre-pass. Frames at or under 17 ms: 92.7/78.9 % to 99.7/91.5 % over 2 interleaved rounds with P-core pinning (2026-11-04). Reaction time unchanged. |
| `REX_NHL_FENCE_POLL=3` | Fence wait pauses 50 us, then sleeps instead of spinning. Saves about 0.8 of a core (cores 3.23 to 2.40, CPU per frame 36.6 to 30.7 ms). On two E-cores median fps 63.8 to 89.9 and 75.7 to 86.4 (2026-11-05). With all cores no fps gain (one A/B each). `0` spins as recompiled. |
| `NHL_QUICK_START=1`, `NHL_BOOT_CLOCK=3`, `NHL_QUICK_MOVIE_FRAMES=12` | Presses through the boot screens to the main menu; guest clock 3x until then; 12-frame EA logo movie. Main menu at 24.2 s without them (no pad: about 44 s because of the language-screen timeout), 14.2 s with the clock only, 10.3 s with the clock and the short movie (menu file; on screen ~12 s) (2026-11-06). `NHL_QUICK_START=0` turns all off. |
| `NHL_VP6_FFMPEG=$PWD/ffmpeg` | Host ffmpeg for the VP6 movies, if `./ffmpeg` is executable. |
| `NHL_HUD=1` | MangoHud fps and frame-time graph top right (needs `mangohud`); `0` off; Shift_R+F12 toggles in game. |
| `NHL_CPU_CLUSTER=1`, `NHL_PIN_PCORES=1` | Default: the game places its own threads on hybrid CPUs (`src/cpu_placement.cpp`, cvar `nhl_cpu_cluster=auto`). `NHL_CPU_CLUSTER=0` uses P-core pinning through `nhl_pin.sh` instead; `NHL_PIN_PCORES=0` leaves it to the scheduler. See mods doc for the numbers. |

Other switches the launcher comments mention: `REX_MAX_FRAMES_IN_FLIGHT=1` (about 1 frame, 19 ms, less latency and
about 25 % lower fps; undated launcher comment) and `REX_PRESENT_THREAD=1` (off: no measurable fps gain and it felt
laggier). `NHL_VK_RT_PATH=fsi` selects the old, correct but slow render-target path; `rtv` is the default and is
about 5x faster on this GPU (README, 2026-10). Any cvar can be set as `REX_<NAME>=value`; the exact variable names for
newer ones are in the SDK patches and nuc-perf-notes.

Other launchers: `nhl_fill.sh` (shader storage fill), `nhl_pin.sh`, `gow-startup-nhl.sh` (Wolf app startup),
`wolf_session.sh` (headless Wolf session for benches).

## 6. Wolf / Moonlight streaming (as recorded)

Recorded only for the maintainer's setup; the Wolf configuration files are not in git (they hold client certificates).

- Games on Whales / Wolf runs in a container; the game is started inside the GoW Steam image. Two Moonlight apps:
  **Steam** (full Steam, runner `NHLSteam`) and **NHL Legacy** (the same image with `/opt/gow/startup-app.sh` replaced by
  `gow-startup-nhl.sh`, runner `NHLDirect`: no Steam, the session ends when the game exits).
- Each paired Moonlight client needs `Steam` and `NHL Legacy` symlinks in `/etc/wolf/<client id>/` pointing to the
  WolfSteam profile folder, otherwise Steam shows a login prompt or the screen stays grey.
- Steam shortcuts take launch options `VAR=value %command%`.
- A Moonlight client with a controller makes the virtual pad player 2 and a bench run stalls on the sign-in dialog.
- Host settings are the user's own business: the maintainer set the iGPU minimum frequency to 1400 MHz
  (`rps_min_freq_mhz`, resets on reboot) because SLPC dropped the GPU to about 800 MHz and made A/B results swing.
  Do not use `slpc_ignore_eff_freq=1`.
- Streaming below 4K is cheaper (issue 18).
- A setup outside Wolf (plain desktop) is **unverified**; `nhl_fill.sh` says it needs "a Wolf session or a desktop".

## 7. Known limitations and open bugs

From roadmap section 10 and `nuc-issues.md` (status as last recorded):

| Issue | State |
|---|---|
| 2 Input feel: 90 to 116 ms press to reacting frame (`NHL_LAT_PROBE`), mostly game-side | measured; 2026-11-05 play test: response very good, low priority |
| 9, 10 Odd sounds when scrolling menus (also in the original); goal horn not heard | open |
| 11 Multi-second pauses in menus and when leaving a match | open |
| 12 Steam overlay unusable with the X11 window; next try `NHL_HUD=0` | open |
| 15 Same improvements on Windows | open, needs a Windows test |
| Screen wipes (replay in/out transitions) showed artifacts; source movies decode clean. Cause: the VP6 bridge looped finished wipes and did not bind a reused ring at once. Fix in `src/vp6_bridge.cpp`, installed 2026-11-06 | fix installed, not yet checked by eye |
| Boot stall (hang before the EA logo movie): cause found (lost wakeup in the game's condition variable), fix `nhl_cv_entry_gate` on by default. One stall seen afterwards on two E-cores (2026-11-06, untraced) | watch |
| First-sight freezes: a pipeline not yet in the shader storage freezes play for 50 to 300 ms (cold worst 322 to 346 ms). Fix: fill the storage (`nhl_fill.sh`) | mitigated |
| Close-ups and replays well under 60 fps (about 45 to 55; about 100 in gameplay). Shadows and ambient occlusion only run outside gameplay | open |
| Overlay (Guide/F1) disabled on Linux; loose `.dds` to `.rx2` conversion is Windows-only in `CMakeLists.txt` | open |
| Black players in team select | likely by design (mannequin model with no texture; same on all paths) |

Performance state on the test machine, 2026-11-06: about 100 fps median in gameplay, about 55 in close-ups and replays,
82 on two E-cores; 30.5 min of gameplay without a gap over 50 ms with a filled shader storage (quick route, 2x main pass MSAA;
newest numbers in the development log (not published)). Older documents may still quote ~62 to 65, which is from an earlier state.

## 8. Benchmark harness

All in `tools/nuc/bench/` (README there) on top of `tools/nuc/nhl_bench.sh`. Run inside the container as the normal
user; the bench needs sudo. The harness assumes the Wolf container setup.

```sh
WOLF_NOSTEAM=1 tools/nuc/ct105/wolf_session.sh start      # headless lobby, no Moonlight client
NHL_DIR=nhl-linux-dev tools/nuc/bench/abrun.sh <tag> "<env>"
tools/nuc/ct105/wolf_session.sh stop
```

- `abrun.sh` runs one bench, samples the GPU clock, prints presented fps median/p5, draws and the present-interval
  histogram. It dumps all thread backtraces if the EA logo movie has not started at 110 s (boot-stall watchdog).
  `ROUTE=quick DUR=165` is the default route (on the ice at about 40 s); use `DUR=285` for anything about the boot.
- In-process A/B: `NHL_AB_TOGGLE="<cvar>=<a>,<b>@<seconds>"` with `DUR=420`, results by `abphase.py`.
- `lat2.py` (press-to-reaction time, needs `NHL_LAT_PROBE=1`), `pres.py`, `bootloop-dev.sh <outdir> <N>`,
  `perfprof.sh`, `stream_cadence.py` (see mods doc).
- Rules the notes keep: interleave A/B runs, at least 2 per configuration; discard runs whose GPU clock differs a lot
  (SLPC downclocking, host load); check `free -m` first (abort under 4 GB available); stop other iGPU users (Frigate,
  Plex transcodes, a Moonlight session) for clean numbers; screenshots of `rtv` runs can look black, verify with other means
  as described in `the launcher notes`.

## 9. Reporting issues

The repo is `erik-131/nhl-legacy-recomp` (fork of puckhead73/nhl-legacy-recomp); whether GitHub Issues is enabled there
is **unverified**. Useful to include, as the existing notes do: git commit and SDK commit, GPU and Mesa/driver version,
the exact launcher variables, the newest `logs/nhllegacy_*.log` (no game data in it), what you were doing at which
second, and the fps/frame-time figures. Do not attach game files, databases or shader storage.
