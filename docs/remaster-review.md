# Review of megarohas/nhl-legacy-remaster (2026-11-06)

Read by four read-only agents; nothing from it was run or copied. **No licence file**, so nothing may be copied into this repo
without the author's permission; ideas are fine. Same game and xex (SHA-256 `d818eb15…`), same SDK, Windows only
(RTX 3060, D3D12 or Vulkan), content focus. Their performance work is two days on NHL 13 (10-07/08), ported to Legacy.
Raw reports are in the scratchpad (not kept in git). Coverage was not complete everywhere: some art, kit and editor tools were
skimmed, and `fe-menu-legacy.md` and parts of `legacy-port.md` were read by heading only.

## What it gives us
| Lead | Where | Status |
|---|---|---|
| **Frame script** `scrape/default.scrape` is text, looked up in `cache:` first. Shadows and AO only run outside gameplay (replays, cutscenes); ice and glass reflections redraw characters every frame | roadmap 1l | Confirmed in our Legacy files; variants being benched |
| **`EndSubmission` waits for the compile workers** every submission, so "async" compile isn't async; they proposed a ~10 line SDK patch with `async_skip_incomplete_frames=false`, never done. Our SDK still has it (`pipeline_cache.cpp` ~750) | 1n | Untried here; could explain our 1 s gaps with that cvar off |
| Cheap cvar tests nobody measured: `occlusion_query_enable=false`, `readback_resolve=none` (+ full only during composites, `836214A8`/`8361F3E0`), `readback_memexport=false`; tunables `gLODShader`, `gEnablePostFX`, `sEnableZPrepass` | 1l | To bench. Note `NHL_VK_READBACK_MODE` overrides the cvar |
| Fence-wait detection by state pointer and timestamp instead of our 20 µs gap; may explain `nhl_fence_poll=3` looking worse in close-ups | 1m | CPU saving, A/B later |
| LOD far meshes never appeared at Ice/True Broadcast cameras or zoomed-out replays; LOD selector untraced | idea | Possible lever for weak CPUs |
| Exit hang: process lived on with music; fix is `_Exit` plus a 5 s watchdog | bugs | Check on Linux |
| Standings tie-buffer bug (records 26+ garbage after a triple tie), marketplace stub trapping a menu, new-career factory settings | bugs | Need codegen or testing |
| VFS misses: a `cache:` miss probed 11 folders under a global lock (stalls up to 14 ms); an in-memory index removed them | 4a | Design note for mod layers |
| Shipping the stored shader list: 552 pipelines, 24–27 s cold, 0.15 s warm | 1n | Their list may hold game data; conflicts with our rule |
| Settings menu patterns (always-on list, hint panel that follows focus, restart labels, per-option warnings, off-screen shot for tests) | P21 | Design input |

## Rosters (P13, roadmap 2)
- Chain: Python tools write a new `nhlng.db`, loc DB and art as loose `cache:` files and merge into the newest ROSTER save. Sources: NHL API, lines sites, Wikipedia, ratings sites. Rows are never inserted: new players reuse donor rows.
- **Corrections to our notes:** the newest ROSTER save overrides the DB and a wrong table `max` gives "damaged" even with good CRCs; `draft_roster.db` must hold exactly the 10 EA tables (a full copy corrupted the heap); stadium art for slots 30/31 is not a blocker (point `arenaartid` at a neutral arena or clone one); `.rx2` is prebuilt, only DDS conversion is Windows-gated in our CMake; Legacy `artid` is 16 bits; their name limit is 24 bytes (ours 25, check); the game computes OVR from attributes (`worth2.brk`) and never reads overall.
- **Widening `proteam`/`draftteam` for team 31** must touch the DB headers and meta; the franchise tables are built from the DB's own `league*` headers (otherwise the sim hangs at `rand() % 0`). The last bit of a record is the engine's "free" flag.
- Play Now with 32 teams is data only; Season/GM/franchise on 32 teams and the year need ~500 hook sites (275 year), tied to the exact xex, plus codegen.
- Our `tdb.py` lacks: deleted-row free list, the free bit, widen, resize, save wrapper. Fixed today: range offset (`proteam` low is −1). **Our 2c/2d estimates were light.**
- Their rating model is fitted to NHL 27 ratings, which conflicts with our rule of not copying EA's; art from other games needs dumps the maintainer would have to own.
- Data-only, no other game needed: names, attributes, rosters, contracts, FA, 32 teams and Utah in Play Now, schedule rows, portraits and logos from public sources.

## Shootout camera (P2)
No work on it there. Their code logs `ShootoutCamera` at the same address as our `0x83BEF66C`. They list the setting as id 106, we found 213: undetermined. Reports of "Ice saved but Action shown" may be sticky GoalieMode, relevant if we force a preset.

## Their mistakes to avoid
Junctions in temp plus a cleaner wiped game data and saves; GPU fixes ranked and left untested for a month; a wrong SDK default became "RTV is black"; the working SDK tree drifted from the published patch series.
