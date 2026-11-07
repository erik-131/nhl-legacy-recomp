# Roadmap (Linux/NUC fork, from 2026-11-01)

Effort: **S** ≈ an evening, **M** ≈ a few sessions, **L** ≈ weeks, **XL** = research project.
Rule for every item: experiments go to `nhl-linux-dev`; the playable install (`nhl-linux`) only changes after an A/B
or play test proves it. Never commit game data, EA assets or copyrighted music.

the maintainer's proposals are assessed first in [proposals.md](proposals.md) and reach this file from there. What was done and
what is installed: the development log (not published).

## Order (updated 2026-11-07)

Performance took all the time from 2026-11-04 to the morning of 11-06. Done since: quick start 6a/6d, bench route 1h, boot stall 1g, 1x as an agreed menu choice, 8a closed. In progress or with tools: 4e menu (runs, default off), 1n fill route, 2 rosters (names proven in game, CSV tool written, 2d data stage built). No code yet: 3, 4a–4d, 5, the rest of 9. The current day-to-day record is the development log (not published).

| Phase | Items | Why |
|---|---|---|
| now | the maintainer's play test of the current build (hitches, replays, boot stall fix) | Result decides how much performance work is left |
| 1 | 6b, 6c Rest of quick start (the maintainer decides 6b) · 1k Loading times · 7a Windows: shared fixes (1h bench route is done) | Small, high value, every boot and bench run gets faster |
| 2 | 1l Replay and close-up fps · 10 Open bugs (8a shootout camera is closed) | the maintainer's reports from play |
| 3 | 4 Mod manager · 5 Music folder | Build on the mod/loose-file and audio paths |
| 4 | 2 Rosters 2026 · 3 3v3 arcade · 9 Image quality | Large, or need GPU headroom first |
| ongoing | 7 Windows parity for everything above | Each feature lands on both platforms |

## 1. Performance (GPU side of the recomp)

State 2026-11-07: ~100 fps median in gameplay (quick route, 2x MSAA, all 14 CPUs), ~45–55 in close-ups and replays, 82 on two E-cores. 30.5 min of gameplay
without a gap > 50 ms once the shaders are stored. Details: [nuc-perf-notes.md](nuc-perf-notes.md), next steps:
the development log.

| Step | What | Effort | Expected |
|---|---|---|---|
| 1a | ~~Host: GPU min-frequency floor~~ done 2026-11-04 (1400 MHz) | S (the maintainer) | done |
| 1b | ~~A/B `gpu_stream_skip_unchanged`~~ done: worse (CPU cost on the CP thread), stays off | S | — |
| 1c | ~~**Upload pre-pass**~~ done 2026-11-04 (`nhl_upload_prepass`, promoted) + P-core pinning (`nhl_pin.sh`) | M–L | 92.7/78.9 → 99.7/91.5 % frames ≤17 ms; pinning size uncertain |
| 1d | Lazy / copy-free resolves (Simpsons stage 2) — resolve on first use, drop when overwritten | L | est. 1–1.5 ms; risky (frame-order bugs in Simpsons) |
| 1e | vsync-locked 60 + present pacing (Tekken 6 `vulkan_present_pacing`) once ≥ 95 % of frames are ≤ 16.7 ms | S–M | smooth pans |
| 1f | CP thread cost (6.9 µs/draw) — matters once the GPU is faster | L | CPU headroom |
| 1h | Faster benches: in-process A/B (`NHL_AB_TOGGLE`), early restart on a boot stall, and `ROUTE=quick DUR=165` (on the ice at ~40 s, same numbers within noise in 3 + 3 runs). **Done 11-06** | — | 165 s per run v 285 s |
| 1i | Ideas from other recomps (mcla-recomp issue #48, FH1/pinyon-shift, SpeedBreaker): `kComputeWrite` access mask (in dev, not measured clean), skip unchanged fetch constants, `present_mode_fifo_latest_ready`; WAIT_REG_MEM already sleeps, MangoHud limiter rejected; long term a native renderer (Skate 3 / FH1 recomps report 2–4× over the Xenos path) | S–XL | see nuc-perf-notes |
| 1j | **Partial native: resolve aliasing** — hook the D3D9 Resolve (`sub_827EF8E0`, dest texture is an argument; see highcut docs) and let the destination texture alias the host render target instead of EDRAM → guest memory → re-upload. Draws stay on the PM4/Xenos path (they are inlined, not hookable). Behind a cvar with the current path as fallback, per-scene A/B. The full native renderer ("high-cut", 2026) reached ~3 fps and was abandoned; this is the contained piece with the most GPU time behind it (resolve 15 % + RT 10 %) | L | est. 2–4 ms/frame |
| 1g | **Boot stall**: cause found 2026-11-06, the game's condition variable lets a late waiter take the driver's wakeup. Fix `nhl_cv_entry_gate` in `nhl-linux` since 08:30; forced race stuck 4/4 without, 0/8 with, no cost in gameplay. Watch for new stalls in normal use | M | reliability |
| 1k | **Loading times** (the maintainer: faster start): the match load is ~10 s; the "~60 s on the lineups screen" in older notes was the bench route waiting to press Start. Boot: 6c. Goes with issue 11 (pauses in menus and when leaving a match) | M | shorter wait |
| 1l | **Replay and close-up fps** (the maintainer: well under 60): 1–2.4 M vertices per frame. **New lead 11-06: the frame script `scrape/default.scrape`** is plain text in the game data and can be overridden through `cache:`. Outside gameplay it adds shadow maps and ambient occlusion, every frame ice and two glass reflections, and the main pass is tiled. Variants: `tools/nuc/bench/scrape_variant.py`. **Results 11-06/07:** `occlusion_query_enable=false` and `readback_memexport=false`: nothing. `NHL_VK_READBACK_MODE=none`: **+3-5 % (7 runs, fast v none pairs and combined)**, faceoff scenes no visible effect, helmets/gloves/sticks in close-ups unchecked, not promoted. **1x main pass: +14 % median** (2x 101.3/103.4 v 1x 118.8/117.1, p5 +10; worklog 11-06 17:25; at 2x zoom slightly stair-stepped edges); the maintainer agreed it as a menu choice (a row in the pre-game menu, default off). Frame script: no shadows outside gameplay removes the slow tail (p25 69 → 101) at the same median, glass off ~+3 %, AO nothing; **shadows outside gameplay is the maintainer's call** (visual cost in replays). Resolve without the EDRAM dump: ceiling ~10 % overall, ~19 % in shadow scenes; **fused direct resolve (`nhl_direct_resolve_fused`) built and verified 11-07** (480 resolves, 0 mismatches, poison run 0 stale reads) but the fps gain is small (+7 % mean over 8 noisy runs, not claimed), default off; the full32 2x colour class, 64bpp and multi-owner spans remain | L | steadier replays |
| 1m | **Weaker systems** (the maintainer: CPU savings count even without more fps): keep measuring CPU per frame and on two E-cores (~20 ms per frame now) | ongoing | wider hardware |
| 1n | **First-play freezes**: pipelines not yet stored freeze play when first seen (cold worst 322–346 ms in gameplay). Look for a way to fill the storage before a match, without shipping game-derived data. Lead 11-06: the SDK waits for the compile workers at the end of every submission (`pipeline_cache.cpp` ~750); see [remaster-review.md](remaster-review.md) | **11-06: root cause = translated pixel shaders are large (~1.5 ms/KB in Mesa ANV; fast math and the SPIR-V optimizer don't remove it); filling the storage works (`tools/nuc/ct105/nhl_fill.sh`, 30 min random play: cold 11–16 gaps over 50 ms → 0–1)**; a steered fill (replay block) did not beat random (11-07, off by default); open: a menu item ("Prepare shaders"), smaller translator output (specialisation, 20–30 %, risky) | M | no freezes on a fresh install |
| 1o | GPU floor idle cost: RC6/frequency with only Frigate running (the maintainer, "another time") | S | decides if 1400 MHz stays |

## 2. Rosters, ratings and teams to the 2026 season

Feasible (P13). League data is in `db/nhlng.db` (uncompressed EA TDB v8, 134 tables), with copies of 10 tables in
`db/draft_roster.db`. Tool: `tools/rosters/tdb.py`. Efforts are estimates.

| Step | What | State / effort |
|---|---|---|
| 2a | Reader and writer with CRC reseal, byte-identical rewrite (`tools/rosters/tdb.py`) | **Done** 11-06, all three DB files |
| 2b | Proof in game: a resealed `nhlng.db` served through `cache/guest` | **Done** 11-07: every last name set to `TDBTEST`, the game boots and plays, line cards and jerseys show it, no rejection. Ratings, jersey, team and position edits not yet tried in game |
| 2c | CSV import/export with cross-table checks (bio, skater/goalie AI, roster, equipment, heads), `draft_roster.db` kept in step | **Done** 11-07: `tools/rosters/rosters.py` (export, import, check, diff; [docs/rosters-tool.md](rosters-tool.md)); export → import byte-identical on the stock file; edits other than names not yet tried in game; stock `draft_roster.db` is not in step with `nhlng.db` and no log shows the game opening it |
| 2d | 30 teams from the NHL API, Arizona → Utah in slot 22 (data and art) | **In progress.** Data stage 1 built 11-07 (`nhl_fetch.py`, `build_roster.py`; [docs/rosters-pipeline.md](rosters-pipeline.md)), 0 check errors, not tried in game; art, Seattle/Vegas open |
| 2e | 32 teams in Play Now: Seattle and Vegas in slots 30/31, all-stars moved | 4–8 days + art. Licence question below first. DB widening done and tested: `tools/rosters/widen_teams.py`, findings and the remaining change list in `docs/rosters-32-teams.md` |
| 2f | Season, GM and the year 2026: code hooks | Days with permission to reuse, weeks without |

Limits: names 25 bytes, ratings 6 bits (rating − 36), player ids 14 bits, 40 per team. New players get generic heads.
Numbers come from an atlas, not per player.

Risks: the game can reject inconsistent tables silently or crash; a saved ROSTER file overrides the DB (none in the maintainer's
saves today); franchise saves carry their own copy. Loose `.dds` → `.rx2` is Windows-only in `CMakeLists.txt`, so art on
Linux needs that fixed first.

Efforts for 2c–2f are light: see [remaster-review.md](remaster-review.md) (widening team fields, the free bit, ROSTER saves, hook sites).

Decision for the maintainer before 2e: ask the author of `megarohas/nhl-legacy-remaster` (same xex, no licence file) for permission.

Rules: ship tools and instructions, never EA files or exports. Ratings from stats, not copied from EA's games. NHL API
terms not read first-hand yet.

## 3. 3v3 arcade (like NHL Threes)

Research first; a full Threes-style mode (money pucks, point values, arcade physics) is XL.

| Step | What | Effort |
|---|---|---|
| 3a | Find the game rules/settings tables (offside, icing, penalties, period length, skaters on ice) and how OT 4v4/penalty 5v3/4v3 are driven | M (research). **Partly done 11-07:** rules are data in `_compiled/scenario/scenario_NNN.big` (`gamerules` chunk: penaltylevel, icing, offside, periodlength ...); no skater-count field, 3v3 likely needs a code patch; EB chunk table not decoded; experiments ranked, none run |
| 3b | "3v3 rules preset": hook lineup size to 3 skaters + goalie, offside/icing/penalties off, short periods — as an option in the mod/settings UI | L |
| 3c | Arcade extras (scoring values, bigger hits, speed) via tunables (`src/tunable_runtime.h` already exists) | L–XL |

## 4. In-game mod support

Today: loose files under `game/` override archives; the enhancements overlay (Guide/F1) is disabled on Linux because an
always-registered ImGui drawer forces UI-thread painting (halves visible fps).

| Step | What | Effort |
|---|---|---|
| 4a | `mods/<name>/` folders layered over `game/` with a load order file; mods never modify the dump | M |
| 4b | Overlay that registers its ImGui drawer **only while open** (keeps direct presentation), opened by a key/pad combo that doesn't collide with Steam's Guide | M |
| 4c | Mod manager UI: list, enable/disable, order, per-mod description/preview, "restart required" hints | M |
| 4d | Same UI hosts settings for 5, 6 and 3 | S |
| 4e | **Menu before the game starts** (the maintainer, P21): quick start, GPU tweaks, mods and tweaks from the old overlay; clear and controller-driven; replaces the overlay as the place for settings. Saved settings instead of launcher env vars. Must not slow the normal start. **11-07: runs on dev with `NHL_MENU=1` (default off), first frame ~85 ms, word wrap and ini handling done, 372 checks; not tested with a real pad or under Moonlight, not in the playable launcher** | M |

## 5. Custom music from a user folder

| Step | What | Effort |
|---|---|---|
| 5a | Find how EA Trax / menu music is started and streamed (audio files in `_compiled/audio`, `cacheaudio.big`; XMA via the APU) | M (research) |
| 5b | `music/` folder: host decodes MP3/OGG/FLAC with the bundled ffmpeg and mixes into the SDL output; hooks pause/stop the game's own music and drive playlist play/pause by game state (menus/intermission vs gameplay) | M–L |
| 5c | Shuffle/volume/now-playing toast in the overlay (4b) | S |

User-owned files only; nothing is shipped.

## 6. Quick start (the maintainer's request)

Boot map, seconds from launch: language screen ~7, waits for A and times out at ~37 without one; legal text ~8–13; EA movie to ~17; title takes Start from ~18–23; autosave notice takes A; menu ~26. The ~44 s to the EA logo measured earlier was that timeout, in boots with no pad.

| Step | What | Effort |
|---|---|---|
| 6a | ~~`nhl_quick_start`~~ done 2026-11-06, in `nhl-linux`, on through the launcher (`NHL_QUICK_START=0` off, `NHL_BOOT_CLOCK=1` for the normal clock): presses through to the main menu with the game's clock at 3×, any button takes over. Menu file at 14.2 s with the clock alone, 10.3 s with 6d (on screen ~12 s) | done |
| 6b | Straight to a match. Pressing on reaches the ice at ~40 s. Needs a stop signal on the ice and a choice of teams (today the menu's matchup) | S–M |
| 6d | ~~Short EA movie~~ done 2026-11-06 (P20): `NHL_QUICK_MOVIE_FRAMES=12`, menu file at 10.3 s (on screen ~12 s). Legal text stays: replacing it hangs the boot; needs the boot flow itself, ~2 s to gain | done |
| 6c | What remains of the ~10 s: language 3.6 s, legal ~2 s, then ~4.5 s from the title to the menu, not shortened by a faster clock | M |

## 7. Windows build parity

| Step | What | Effort |
|---|---|---|
| 7a | Shared fixes as upstream PRs (branches ready: `fix/vp6-bridge-per-player`, `feat/swap-interval-option`) — build and test on the maintainer's PC first | S |
| 7b | D3D12: try `rtv` with `execute_unclipped_draw_vs_on_cpu=true` (same black-3D bug as Vulkan, Windows uses slow ROV today), `clear_memory_page_state=false`, readback `fast` | M |
| 7c | Port Vulkan-only perf work to D3D12 where it applies (skip overwritten transfers, direct presentation) | M–L |
| 7d | Features 2–6 are mostly platform-independent (app code); keep them out of Linux-only files | — |
| 7e | SDK fixes to `rexglue/rexglue-sdk` (unclipped default, shm leak, Audio Worker wait) — needs a fork of rexglue-sdk | S |
| 7f | **Vulkan backend on Windows with our SDK series** (`sdk/series-nhl-nuc/` on rexglue `c94f5eb`, `REXGLUE_USE_VULKAN=ON`): the June 2026 spike already ran Vulkan on Windows at parity; brings rtv + the NUC perf work without a D3D12 port. Benchmark against D3D12 on the 3070 PC | M |

Testing on Windows needs the maintainer's PC (the build needs the game's `default.xex` for codegen, so CI can only compile the SDK).

## 8. Gameplay and presentation mods (the maintainer's requests)

| Step | What | Effort |
|---|---|---|
| 8a | ~~Shootout camera higher~~ **Closed 11-06**: the maintainer adjusts it with the game's own setting (P2). No work | — |

## 9. Image quality (the maintainer's question, only with GPU headroom)

| Step | What | Effort |
|---|---|---|
| 9a | (Study done 11-07, `p-iq`: 1x main pass is the measured option, +14 %; supersampling capped at 2, never measured; FXAA exists, SMAA/TAA do not.) Higher internal resolution: try the SDK's scaling. Resolves and EDRAM must follow, UI and post effects are the risk. 1.5× means 2.25× the pixels, so not on this iGPU at today's close-up cost | M–L |
| 9b | Post-process AA or upscaling (see `fidelityfx-enablement-notes.md`); map the game's existing MSAA first | M |

## 10. Open bugs and smaller requests ([nuc-issues.md](nuc-issues.md))

| Issue | What |
|---|---|
| 2 | Input feel: ~90–116 ms press to reacting frame, mostly game-side. the maintainer's play test 2026-11-05: response very good, so low priority |
| 9, 10 | Odd sounds when scrolling menus; goal horn not heard |
| 7 | Screen wipes (transitions inside matches): cause found in the VP6 bridge (finished wipes looped, a reused ring did not bind the next wipe); fix in `src/vp6_bridge.cpp` installed in `nhl-linux` 11-06; the maintainer to check by eye |
| 11 | Multi-second pauses in menus and when leaving a match (with 1k) |
| 12 | Steam overlay unusable. Next try: `NHL_HUD=0` |
| 15 | Same improvements on Windows (section 7). A PR against the main repo: 7a and 7e, only when the maintainer asks |

## Answered, not planned

- **Replicating source code (P22):** study done 11-06 ([source-replication-study.md](source-replication-study.md)): 129,934 functions, library share at least ~24 %; awaiting the maintainer's decision on the goal.
- **Windows 7 (7):** possible in principle on Vulkan only, unproven (worklog 11-07); needs the maintainer's PC.

- **Shield TV Pro / Android:** no Android port, and GPU and CPU look too weak (estimate 15–25 fps at 720p, not measured).
- **iGPU overclocking:** the driver caps at RP0 (2200 MHz).
- **Full native GPU renderer:** earlier attempt reached ~3 fps. 1j is the contained piece.
