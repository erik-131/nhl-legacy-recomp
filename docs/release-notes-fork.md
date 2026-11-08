# Changes in this fork (by theme)

Dates are 2026. The fork point is upstream `0852183`; the fork has about 230 commits. Numbers are from one Intel Arc iGPU
machine (see [nuc-perf-notes.md](nuc-perf-notes.md)).

## Port to native Linux and first Vulkan work (09-06 .. 10-29)
- macOS arm64 port and MoltenVK FSI/tiling fix (Redth); Post AA control; descriptor-set reuse (09-06..09-08).
- 10-29: native Linux ELF build; diagnostics (GPU profile, resolve and readback logs, present histogram); first clean control runs (interlock path about 70 ms/frame GPU, `rtv` 41 ms).

## Rendering performance (10-30 .. 11-07)
- 10-30: `execute_unclipped_draw_vs_on_cpu` default fixed, so host render targets (`rtv`) render the 3D scene; dedicated present thread; Audio Worker waits non-alertably.
- 11-04: upload pre-pass and P-core pinning (frames within 17 ms 92.7/78.9 -> 99.7/91.5 %); in-process A/B toggle (`NHL_AB_TOGGLE`).
- 11-05: event wakeup fix, fence poll modes (CPU per frame 36.6 -> 30.7 ms), E-core placement.
- 11-06: skip overwritten RT transfers (p5 36 -> 60 fps); boot stall root cause and `nhl_cv_entry_gate`; 1x main pass option (+14 %); readback none vs fast (+3-5 %).
- 11-07: fused direct resolve (+8-9 % replay), unsigned-texture specialisation (+10 %), implicit LOD (+6 %), depth+stencil clear-through (+1.8 / +3-5 %), PCF image gather (+2 %), per-shader-pair GPU attribution. These are switches, default off in the binary; the launcher turns them on from 11-08.
- Result: about 8-10 fps -> about 100 median in play; replays and close-ups still 45-75.

## Startup and reliability
- Quick start, 3x boot clock, 12-frame EA movie: menu at 10.3 s (11-06). First-play freeze cause found (large translated pixel shaders) and a shader-storage fill route (`nhl_fill.sh`).
- VP6 movie bridge: one ffmpeg stream per player ring, hold last frame, wait for the first frame (11-06); shm leak fix; sync input pump and latency probe.

## Tooling
- Bench harness `tools/nuc/bench/` (A/B, boot loops, perf profile, stream cadence), Wolf headless lobbies, sidecar function viewer (P22).

## Pre-game menu, mods, scripting (11-06 .. 11-07)
- SDL pre-game menu (`NHL_MENU=1`), mod layers, `.rx2` art tool (centre-ice logo proof), Lua override path and findings, tunables and settings schema tools.

## Data tools and rosters (11-06 .. 11-07)
- Database reader/writer with CRC reseal; CSV tool; localisation DB decoder; 2026-27 roster pipeline (30 slots; match plays with current players); widened DB, Seattle and Vegas selectable with no hook.

## Rules and source control (11-06 .. 11-07)
- Rule hooks (3v3/4v4, period length), scenario rule data findings; source-replication study (129,934 functions, library share >= ~24 %); L1 naming pipeline; L2 modules for the database layer and penalty logic (verify/replace switches, default off, synthetic tests).

## 11-08: roster choice and fast defaults
- Roster sets (`mods/roster-<name>`, installer script, menu row, ini and env choice); mod layer now serves files for directory-relative opens.
- Pre-game menu waits for a late-plugged pad. Crash fix in the VP6 bridge (reader thread joined before destruction).
- Launcher turns the picture-identical GPU set on (`NHL_FAST`), measured as one build (see nuc-perf-notes); SDK patch 0042 (vertex-shader texture-fetch specialisation).
- New research notes: rules, game flow, line change, boot flow, menu control.

## Documentation
- Roadmap, proposals, user guides, research on other recompilations, stream stutter and Wi-Fi findings (kept in the maintainer's development log, not in this release).
