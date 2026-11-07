# Mods and tweaks available today (Linux build)

What a user can change now, how, what it was measured to do, and the risks. All numbers come from
[nuc-perf-notes.md](nuc-perf-notes.md) and the development log (not published) (dates are 2026, month-day). They were measured on
one machine (Intel Core Ultra 5 225H, Arc iGPU, 1280x720) with 2 or more interleaved runs where stated; run-to-run
variance is 10 to 30 %, so treat small figures as indicative. Setup and the launcher are in [linux-setup.md](linux-setup.md).

This repo contains no EA data. Everything below works on files you own; never commit game files, databases, exports or
shader storage.

## Speed and boot options

Set them as environment variables for `nhl_play.sh` (or Steam launch options `VAR=value %command%`). A variable that is
already set wins over the launcher default.

### Quick start
- **Does:** presses through the language screen, title and autosave notice up to the main menu. A real button press
  from you ends it (ignored during the first 3 s, and for 2 s while the short EA movie plays). Time limit 90 s.
- **Set:** on by default in `nhl_play.sh`; `NHL_QUICK_START=0` off (cvar `nhl_quick_start`, `src/quick_start.cpp`).
- **Measured (11-06):** main menu at 24.2 to 24.6 s (11/11 boots) with presses alone, instead of waiting out the language-screen
  timeout (about 44 s to the EA logo without a pad). With the boot clock and short movie below the menu file opens at 10.3 s
  and is on screen at ~12 s. Then play: 100.4 median fps, 1 gap over 50 ms.
- **Risks:** tested with scripted presses; the maintainer reported "quick start ok" after a Moonlight play session (11-06); a worklog line still lists a real-pad check as pending. The language screen
  waits for A; quick start stops at the main menu, where A would pick Play Now.

### Boot clock
- **Does:** runs the game's clock faster during the boot only, because the boot screens are timed.
- **Set:** `NHL_BOOT_CLOCK=3` (default with quick start); `1` is the normal clock.
- **Measured (11-06):** menu at 14.2 s instead of 24.2 s at 3x (11/11 within 14.2 to 14.5 s; 14.5 s on two E-cores);
  8x gave 14.3 s, no better. The part after the title (about 8.7 s) does not shrink with the clock.
- **Risks:** sound during the boot was not checked in benches. The clock returns to 1 when quick start ends.

### Short EA movie
- **Does:** serves a 12-frame version of the EA logo movie.
- **Set:** `NHL_QUICK_MOVIE_FRAMES=12` (default with quick start); `365` plays the whole movie.
- **Measured (11-06):** menu file at 10.3 s with the 12-frame movie (boot clock 3x; 13/13 on 14 CPUs, 5/5 on two E-cores at 10.3-10.6 s),
  on screen ~12 s; 14.2 s with the clock only.
- **Risks:** the movie's cue file has to be moved to the first frames; valid movies of 2, 60, 80 and 100 frames froze
  the boot and 150 ran as normal, so do not try other values below 150 unless you know why. The legal text screen
  cannot be replaced or skipped (replacing it hung the boot).

### 1x multisampling of the main pass
- **Does:** draws the 3D main pass at 1 sample instead of the game's 2x.
- **Set:** cvar `nhl_force_host_msaa_1x` (env `REX_NHL_FORCE_HOST_MSAA_1X` by the `REX_<cvar>` pattern; also a row in the
  pre-game menu). It is read once when the render target cache starts, so it cannot be toggled in a match.
- **Measured (11-06, 2 rounds each, gameplay median / p5):** 2x 103.4 / 51.8; 1x 118.8 / 61.6 and 117.1 / 62.6. About
  +14 % median, +10 fps p5. An earlier GPU-time reading (10-31) showed 15.7 against 16.5 ms per frame.
- **Looks:** at 1:1 no visible difference in matched faceoff, overhead and arena scenes; zoomed, 1x has slightly
  stair-stepped edges on skates, jersey numbers and the circle line.
- **Risks:** jagged edges; off by default; not tested on every scene. Needs D24S8 render targets (the code logs a
  warning otherwise).

### Readback mode `none`
- **Does:** `NHL_VK_READBACK_MODE` selects how GPU results are read back: `full`, `fast` (default, one frame stale),
  `some`, `none`.
- **Measured (11-06):** about **+3-5 % (7 runs, fast v none pairs and combined)**. Pairs, quick route: median 109.8 and 106.7
  against 104.9 and 103.4 (+4.7 % and +3.2 %); the earlier 5 runs 105.2 to 108.3 against 101.0 to 105.7 (median of medians
  107.1 v 101.0).
- **Risks:** **not visually fully checked.** The faceoff scenes differ no more than two runs of the same mode do; close-ups
  of helmets, gloves and sticks (replays, celebrations, intros) were not compared and are where stale composites
  could show. The per-destination limit variant gave green garbage on equipment (issue 6), so look at replays before
  keeping it. Not promoted to the default.

### Fence poll
- **Does:** how the fence wait behaves. `0` spins as recompiled, `1` pause, `2` yield, `3` pause 50 us then sleep.
- **Set:** `REX_NHL_FENCE_POLL=3` (default in `nhl_play.sh`; cvar `nhl_fence_poll`, `nhl_fence_poll_spin_us`).
- **Measured (11-05):** only mode 3 saves CPU: cores 3.23 to 2.40, main thread 0.95 to 0.30 cores, CPU per frame
  36.6 to 30.7 ms. On two E-cores median fps 63.8 to 89.9 and 75.7 to 86.4. On all cores no fps gain (GPU clock
  1711-1693 against 1715-1740 MHz, one A/B each).
- **Risks:** in one in-process A/B mode 3 had more low-fps segments (9 against 2 over three runs, about 5 fps lower), so
  close-ups may lose a little.

### Thread placement
- **Does:** on hybrid Intel CPUs with an integrated GPU, the game puts its threads on one E-core cluster and the 10
  pipeline-compile threads elsewhere (`src/cpu_placement.cpp`).
- **Set:** automatic (`nhl_cpu_cluster=auto`); `NHL_CPU_CLUSTER=0` in the launcher gives P-core pinning (`nhl_pin.sh`)
  instead, `NHL_PIN_PCORES=0` leaves it to the scheduler.
- **Measured (11-05/06):** game threads on `4-7`: median 97.2 to 106.4, CPU per frame 36.1 to 24.9 ms (one run each); app
  against launcher-script version 93.2, 100.2 against 88.6, 100.0. P-core pinning against none: no clear difference
  (97.9 against 95.7). Compile threads off the game cluster lowered the sum of frame gaps over 50 ms in cold runs
  (3058, 6675 ms down to 2115, 2374 ms).
- **Risks:** specific to this CPU layout; effect on other CPUs **unverified**. The CPU numbering changed between boots
  of the test container; `nhl_pin.sh` reads it at start.

### Shader storage fill (`nhl_fill.sh`)
- **Does:** plays the game by itself for N minutes with random button presses and no sound, so the pipeline storage is
  filled before you play. A pipeline seen for the first time otherwise freezes play for 50 to 300 ms
  (cause: translated pixel shaders are big, about 1.5 ms of driver compile per KB).
- **Set:** `tools/nuc/ct105/nhl_fill.sh [minutes]` (default 30); `NHL_FILL_REPLAY=1` adds a replay-camera block every
  ~100 s (did not add pipelines).
- **Measured (11-06):** a 30 min random run reaches 288 pipelines (255 in the first minute); a real long-time storage
  holds about 400. Check runs with a filled storage: gaps over 50 ms 1, 1, 0 (cold: 11 to 16); worst gap 88, 100, 0 ms
  (cold: 321 to 347 ms). A 50 min soak with stored shaders: 0 gaps over 50 ms in 30.5 min of gameplay.
- **Risks:** it presses buttons in menus on **your profile and saves** (run on a fresh install or a scratch profile).
  Replays, goals, penalties and fights are not reached; normal play adds the remaining ~110. The storage files hold
  shader code derived from the game: keep them private. The storage is shared by all installs with the same cache root.

## Pre-game menu (in development)

- **Does:** a small window before the game starts, for pad or keyboard: five tabs (Start, Performance, Look, Display,
  Advanced). Start saves and plays with one press, B skips changes. Options write the same environment variables and
  ini keys as above (list and table: [menu-design.md](menu-design.md)). Settings are stored in `nhl_pregame.ini` beside
  the executable (only non-default choices); some also in `nhl_enhancements.ini`.
- **Set:** `NHL_MENU=1` (off by default). An environment variable that is already set wins over the menu.
  Test helpers: `NHL_MENU_FORCE=1`, `NHL_MENU_AUTOSTART_S`, `NHL_MENU_SCRIPT`, `NHL_MENU_DUMP`, `NHL_MENU_DUMP_ALL`.
- **Measured (11-06):** window about 85 ms after process start, first frame 1 ms later; the game booted normally ~7 s
  later in the test; a discard, timeout or unchanged Start left `nhl_enhancements.ini` byte-identical.
- **Not done:** a real pad or keyboard, delivery of pad events through Moonlight/Wolf, and the "Prepare shaders" item
  are untested or not wired.

## Audio statistics and stream cadence

- **Audio stats:** the log line `[nhl-audio-stats] played=... silent=... max_cb_gap=...ms queued=...` appears once a second
  (on by default in the current build). Use it to see whether stutter comes from the game.
  Measured in one streamed play session (11-06): 0 silence frames in play and no callback gap over 17 ms after the boot second.
- **`tools/nuc/bench/stream_cadence.py`:** `sudo python3 stream_cadence.py -i eth0 -o file.log [--video-port 48100]
  [--audio-port 48200] [--seconds N]` sniffs Wolf's RTP packets leaving the host (headers and arrival times only, no
  payload) and logs per second the video frames, longest gap, audio packets, longest gap and RTP sequence gaps. Even
  cadence on the host while the client stutters means the cause is after the host. In the same session the audio left
  the host evenly (300 packets/s including FEC, longest gap 7.3 ms).
  Defaults assume Wolf's ports and interface `eth0`; **unverified** elsewhere.

## Rosters: names and numbers (`tools/rosters/`)

The game accepts a modified, resealed league database. **Proven (11-07):** every last name in the stock `nhlng.db` was set
to a test string, resealed (all CRCs valid, same size) and served as `<cache root>/guest/db/nhlng.db`; the game booted,
quick start worked, a match played, and line cards and jerseys showed the new text. Ratings, jersey, team and position
edits pass the tool's checks but are **not yet tried in the game**.

- **`tdb.py`** reads and writes the EA TDB v8 format: `verify`, `check`, `tables`, `export`, `live`, `set`, `delete`,
  `add`, `widen`, `maxrows`, `rebuild`. A rewrite of the stock files is byte-identical and every output is resealed.
- **`rosters.py`** (tests: `python3 tools/rosters/test_rosters.py`): `export DB OUTDIR`, `import DB CSVDIR OUT`,
  `check DB [--draft PATH]`, `diff DB_A DB_B`. Export, then import without edits, is byte-identical (checked on the
  stock file). Import changes existing rows only, matched by key; any rejected value (unknown key or column, value out
  of range, string of 25 bytes or more) stops the whole import and nothing is written. Details:
  [rosters-tool.md](rosters-tool.md).
- **Workflow:** export from your own `db/nhlng.db`, edit the CSVs outside the repo, import to a new file, run `check`
  and `diff`, then copy the result to `<cache root>/guest/db/nhlng.db`. The original is never touched; delete the cache
  file to go back.
- **Limits and caveats:** names 25 bytes (field size minus one); ratings 6 bits; player ids 14 bits; 40 players per team
  (roadmap section 2). No inserting, deleting or reviving of rows and no cascade (changing a position does not move the AI
  row). A saved ROSTER file overrides the database (none in the maintainer's saves); franchise saves carry their own
  copy (roadmap). `draft_roster.db` differs from `nhlng.db` in the stock install and what the game needs from it is
  unknown. Position codes (0 C, 1 LW, 2 RW, 3 D, 4 G) are verified, handedness (0 L, 1 R) and the rating offset
  (+36) are guesses. Roadmap: jersey numbers come from an atlas, not per player.
- 2026 rosters, new teams and art are planned, not available.

## Loose-file overlay (mods)

- **Does:** the game looks for `cache:\<path>` before the `.big` archive. The port mounts the unpacked tree
  `game/_compiled` read-only beneath the writable `<cache root>/guest`, so a loose file in either place replaces the
  archive version with no repacking. The log line `[loose-overlay] mounted ...` confirms it.
- **Works (verified in the code's notes):** `.rx2` rendering files, attribdb `.vlt/.bin`, FE `.bin`, the league database
  (above). The `.big` archives themselves cannot be repacked.
- **Does not work on Linux yet:** loose `.dds` textures (`game/_loose_tex`, converted to `.rx2` at load) are wired for
  Windows only in `CMakeLists.txt` (roadmap section 2). A mod manager (`mods/<name>/` folders, load order) and a
  custom music folder are planned, not built.
- **Risks:** a bad replacement file can crash or hang the boot. Keep originals; the overlay never modifies the dump.
  Do not share mod files that contain EA data.

## Does not work yet or was tried without gain

- In-game overlay (Guide/F1) on Linux: off, it halves visible fps. Settings only via variables, ini files or the menu.
- Straight to a match, rosters for 2026, 3v3 mode, mod manager, music folder, higher internal resolution: planned only.
- Tried, no gain, off: dedicated present thread, synchronous input pump, frames-in-flight cap (lower latency, about
  25 % lower fps), clear-through, `occlusion_query_enable=false` (retracted), `gpu_stream_skip_unchanged`,
  `robustBufferAccess` off, fast vertex/pixel math, SPIR-V optimizer, `WaitMultiple` notify, PGO (within noise).
- Not wired: the shader fill as a menu item; resolve without the EDRAM dump (design only, ceiling about +10 %).
