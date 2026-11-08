# Forced scenes for benchmarking (proposal P25)

Status 2026-11-07. Goal: start the game in a match without a timed pad route and reach a named scene on demand. Everything here was
measured on the NUC dev install (Lua route, no code change needed). The native route (`src/scene_force.cpp`) is written but **not built or run yet**.
Scene-by-scene results: tables below; screenshots in `<workdir>/p-scenes/<run>/` (contact sheets `sheet.jpg`).

## State (2026-11-08, end of the forcing round)

| Scene | Status | Time | Marker in the log |
|---|---|---|---|
| live, shootout, flyover, closeup | done, reproducible | 22-25 s | ICE (>1000 draws/frame), Lua mark `SC_CAM_*`, `[nhl-scene] reached play` |
| goal, replay | natural wait only (fallback) | goal 107 s, replay 90 s (typical 60-110) | `[nhl-scene] reached goal / replay` (flow-state transitions) |
| goal / replay forced | NOT solved: forced stoppages (any reason) restart without score or replay | - | `reached goal` is logged for a forced goal-whistle too: **check `gdiff` in the state line**, a forced one keeps gdiff 0 |
| wipe | not available | - | - |

A timing without its marker line is invalid; `scene_report.py` prints `scene NOT reached` then. Research tools (default off): `NHL_SCENE_TRACE` build, `NHL_SCENE_FINDPUCK=1`,
`NHL_SCENE_DUMPWORLD`. Entity objects (players, probably the puck) keep their world position at **object+112** as floats (x across, y up, z along the rink; seen in traces of
`0x833E5E48` pickup, `0x83420B50` pass start, `0x83328180` block shot: for a blocked shot the puck object `A9EF40A0` and the blocker share the position).
`NHL_SCENE_FINDPUCK` (heap float-triple diff at faceoff and 3 times in play) finds no clean puck: only noise and one object with a vtable (`0x8239B218`) that did not behave like a puck;
**Puck found (final round).** The global table at `0x83C1D084..0x83C1D118` holds pointers to entity objects; `*0x83C1D0C8` (and `*0x83C1D118`) point at the puck: its position at object+112 follows play exactly
(faceoff drop y 0.93 -> 0.03 on the ice, then x/z moving, z up to 27 in 25 s; **units are metres, goal line at z = +-27.1**, y up). `NHL_SCENE_PUCKPTR=0x83C1D0C8` + `NHL_SCENE_LOGTABLE=1` show it.
Poking that +112 position (`NHL_SCENE_PUCKPOKE=x:y:z:hold`, 400 frames, once per frame) has **no effect**: the game rewrites it every frame, it is only a render/derived copy (gdiff stayed 0).
`NHL_SCENE_PUCKPOKE` now first scans the heap for every exact copy of the puck triple and writes all of them; that run could not be completed because the bench hung (below), so it is **untested**.
**Result of the heap-copies poke (final):** `NHL_SCENE_PUCKPOKE=0:0.1:27.3:400` and `0:0.1:-27.3:400` (two native runs, jobs 00268/00274): 105 and 117 exact copies of the puck triple were found at the draw
(the puck is at rest, so many other records hold the same numbers) and all were overwritten each frame for 400 frames; **gdiff stayed 0 in both runs, no goal, no stoppage from the poke**. So the puck position is not
poked into the sim this way (the sim's authoritative state is not a plain float triple at that moment, or is recomputed from a different representation). Forced goal, replay and wipe are **closed as not feasible
with this method**; the presets `goal` and `replay` stay natural waits (107 s, 90 s), `wipe` unavailable. Anything further needs the physics body of the puck (the `puck.cpp` code at `0x833676B8`/`0x83367xxx`) read from the lifted code.

**Infrastructure note:** two jobs (00256, 00262) hung for 10-22 minutes inside `wolf_session.sh start` waiting for `flock` on `nhl-work/wolf-start.lock`; the lock holder is not a visible
process (fd inherited by a long-lived child?). Nothing in scene_force is involved. Cancel the job to free the bench; whoever owns wolf_session.sh should close fd 9 in children (`9>&-`).

## Tool

`tools/nuc/bench/nhl_scene.sh <live|shootout> [tag]` (goes through the bench queue `bq.sh`, priority 3, owner `scenes`; env is handed to the job):

| Env | Meaning |
|---|---|
| `SCENE_HOME`, `SCENE_AWAY` | team ids (default 5, 12 = FLA v CAR; ids below 220) |
| `SCENE_C`, `SCENE_D` | the two per-side values of `EnterGameplay` (default 0, -1 = user controls home; **-1, -1 = CPU v CPU, see below**) |
| `SCENE_AT` | hook call that enters the match (default 5200 = +11.6 s) |
| `SCENE_LUA` | file with a timeline `SC_TL = { {calls_after_entry, "label", function() ... end}, ... }` (examples `tools/lua/exp/`) |
| `PADS`, `SHOTS`, `DUR`, `XENV` | pad script (default Start at 16 s and 18 s), screenshot seconds, run length, extra game env |

Output `<workdir>/p-scenes/<tag>/`: `game.log`, `shots/`, `result.txt` (seconds from process start to the Lua marks, scene switches, ice).
`tools/nuc/bench/scene_report.py` makes the report, `contact_sheet.py` the JPEG grid, `memdump.py` dumps guest memory (`DUMP=82000000:2000000`).
How it works: `tools/lua/scene.lua` is loaded from a scratch cache root (`REX_CACHE_ROOT`, copy of the game's scenedef plus our scripts, see
`docs/lua-scripting.md`) and calls `EnterGameplay(home, away, c, d)` from the per-frame hook at call 5200. Nothing in the install is modified.

## Measured (seconds from process start, quick start on, dev binary)

| Scene | How | Time | Status |
|---|---|---|---|
| live match, faceoff | `nhl_scene.sh live` (Start at pad 16 s and 18 s) | on the ice at **+22.0 s** (log, >1000 draws), faceoff camera at +24 s, play at +28 s | works, reproducible (3 runs: 22.0, 22.0, 22.0) |
| arena intro (team splash, flyover, scoreboard, tunnel/bench close-ups, goalie close-ups, arena light show, player close-up) | `SCENE_C=-1 SCENE_D=-1 PADS=1:b:100 nhl_scene.sh live` | splash +20 s, flyover banners +24, scoreboard +28, tunnel close-ups +32..48, goalie in net +52, light show +56..60, player close-up +64 | works with **no pad at all**; the whole broadcast intro plays, so close-ups and flyover are on demand by time |
| CPU v CPU intro, second half | same, longer run (`p-scenes/cpucpu2/sheet.jpg`) | goalie cards (Luongo, Ward) +70..78 s, line-up cards +82, faceoff top view +86..90 s, **live play +94 s**; no goal in 20 s of play | works; no pad needed, but live play comes only at +94 s (use `live` with Start presses for +22 s) |
| shootout (VAN v VAN, id 28 fixed by the binding) | `nhl_scene.sh shootout` | ice at **+25.0 s**; shooter, goalie close-up +34 s, waiting shooter +38..58 s (no input) | works. A **"Replay" banner scene at +30 s** (side view of the attempt, shoot-out intro replay) and again at +62 s after the first attempt, then round 2 at +66 s (`p-scenes/shootout1/sheet.jpg`) |
| camera by name | `SetCamera("<name>")` from a timeline (`tools/lua/exp/x2_cams.lua`, first at call 9500 = +23.9 s, 1500 calls = 2.1 s apart) | each camera is live within one frame of the call | **works**: CloseOnPlayer, WideStadium_Broadcast (scoreboard + whole rink), ReplayBroadcastClose, ReplaySlashISO (low sideline), InsideNet (goalie from the net), PostWhistle_Corner (referee close-up), AboveNet (top view of the crease), GAMEPLAY_TRUE_BROADCAST, PLAYER_VIEW (top, follows a player), FACEOFF_SIDE_VIEW (`p-scenes/x2b/sheet.jpg`) |
| replay, goal, celebration, wipe | not forced yet | - | see "Open" |

### Camera names (guest table at 0x83AD03A4, 100 entries, read from a memory dump)
`SetCamera` and `SetAutoreplayCamera` take one of these names (the name list is data, not code): CAMERA_NONE, ACTION, GAMEPLAY_ICE, CLASSIC, OVERHEAD, BROADCAST,
BE_A_PRO, PROTOTYPE, CUSTOM, FACEOFF_TOP_VIEW, FACEOFF_SIDE_VIEW, GAMEPLAY_TRUE_BROADCAST, PLAYER_VIEW(2), SHOOTOUT1..3, SHOOTOUT_GOALIE, PRACTICE, WARMUP, FIGHT,
FIGHT_FIRST_PERSON, PostWhistle_* (Corner, Broadcast, EndZone..., OverheadCrease, PenaltyBoxLong, InsideNet, AboveNet), WideNetAndPowerRing_*, WideStadium_*, AboveNet,
Autozoom_Behind, TopGlass_BehindNet, Far_BehindNet, CornerNearNet, Replay* (BroadcastISO, BroadcastClose, SlashISO, EndZone..., ~20), CloseSideview, FocusOnPlayer,
VideoReplay_* (Dasher, Broadcast, Corner, HighNet, Net, Overhead, InsideNet), CREATE_PLAYER, ShotOnNet, CloseOnPlayer(Low), 3rdPersonReplayView, Penalty, Hit, InsideNet, Save,
UserReplay, Transition, NIS, NIS_SPLIT_SCREEN.

## Findings that matter
- The Lua per-frame hook runs about **760 calls/s** in a match and 400-500/s while loading: the call count is a rough clock only. Time offsets from the entry call:
  +8000 calls = +21.8 s from process start, +10500 = +25.1 s.
- Environment variables do not reach a bq job unless the command passes them (`nhl_scene.sh` does this for its own variables).
- Pause (Start) in a match entered through Lua shows a front-end-like menu (main menu list next to a stats pane), not the in-game pause menu with Instant Replay:
  the pad route "pause, down, A" lands in Community / Game Stats (shots in `p-scenes/replay1`). So a replay through the pause menu is not usable after `EnterGameplay`.
- `StartLoopingReplayHighlights()` sets one byte (0x83C9280C = 1) that the replay state reads; with nothing recorded it shows no replay (x1 run).
- Replay system pointers (read offline from the lifted code and a memory dump): the replay controller is the class that registers "CameraReplayInterface"
  (vtable at 0x822323E0, slots 3/4 = 0x83463DB0 begin replay, 0x834646A8, 0x83464940 sub-start via 0x83468858/0x834687E0). Which object calls them was not found yet.

## Presets (`nhl_scene.sh <preset> [tag]`), result.txt ends with `PRESET <name>: X s from process start`

| Preset | What you get | Seconds to scene (measured) | How |
|---|---|---|---|
| `live` | exhibition match, faceoff on the ice | Lua route 22.0 s (3 runs); native route 23.1 s | Lua `EnterGameplay` at call 5200, Start at pad 16/18 s; native: pads 17.4/19.4 s |
| `shootout` | shootout | 25.0 s | `EnterShootOutMode` |
| `flyover` | arena flyover / broadcast intro (CPU v CPU, no pad needed) | arena on screen 24.0-25.1 s (banner flyover), scoreboard 28, tunnel close-ups 32-48 | `SCENE_C=-1 SCENE_D=-1` |
| `closeup` | `CloseOnPlayer` camera on the faceoff | mark at about 23 s | Lua `SetCamera` at call 8800 |
| `goal` | natural goal (the user team idles; the CPU scores) | 106.9 s in the verification run (play at about 31 s; goals came 24-60 s after play start); a goal appeared in about 60 % of the long sessions (4 of 4 CPU v CPU, 1 of 3 user v CPU idle team, 2 of 2 later) | natural, no forcing |
| `replay` | first stoppage replay (5->10 flow transition, then the replay camera) | 8-14 s after a stoppage, first stoppage 20-60 s after play starts: typically 60-110 s from process start | natural, no forcing |
| `wipe` | not available | - | no forcing found |

`replay` and `goal` need the native hook (`NATIVE=1`, a binary containing `src/scene_force.cpp`, `SRC=<install dir>`). They report from the
log marks `[nhl-scene] reached play|goal|replay` (flow-state transitions read from guest memory, so the plain build needs no trace hooks).
Pads are the weak part of the native user route: in 2 of 12 sessions the Start presses missed and play began at +70 s instead of +31 s.

## Native route (`src/scene_force.cpp`, built and working)
Env: `NHL_SCENE=live|shootout`, `NHL_SCENE_TEAMS=H,A`, `NHL_SCENE_PADS=a,b` (0,-1 user v CPU; -1,-1 CPU v CPU), `NHL_SCENE_ENTER_FRAME` (default 2500 frames
after the main menu = +13.0 s; **800 hangs the load** because the main menu is not ready, the game freezes in a transition), `NHL_SCENE_REF=enter|play`
(what the frame offsets of CALLS/POKE count from; `play` = first frame of flow state 4), `NHL_SCENE_CALLS=f:addr:r3[:r4..r8],...` (arguments may be
`*0xADDR` = word at that address, `@ctrl` = view controller object, `@replay`), `NHL_SCENE_POKE=f:addr:value[:bytes]`, `NHL_SCENE_TRACE=1` (needs
`cmake -DNHL_SCENE_TRACE=ON`: ~20 game functions get logging wrappers; a research build, off by default).
Once a second `[nhl-scene] +Nf flow= lastreason= clock= gdiff= gp= w0= ctrl= replay=` and on every flow-state change `flow a->b`.
Calls run on the thread's own context at the end of the frame function (a copy of the context hung the game; the frame function must not be the
caller either, see the early-entry hang above). Build: `tools/nuc/dev/wt-new.sh scenes2 scenes` (about 25 s), then ninja (ccache hits, 7 s).

### Findings (from traces of natural matches, CPU v CPU and user v CPU)
- **Stoppage hub `0x83326280(reason, 0x13, 1, 0x258, 0, 0)`**: reason 2 = goal, 3, 6, 9, 13 other stoppages (icing/offside/...). Called from `gameplay_reset 0x83481038(obj, reason, x)`, which the
  player-stop-play handler `0x83484320` (caller 0x83502D20) calls. It sets `lastreason` (flow struct 0x83C1D220 + 12), plays the whistle and enters flow state 5.
- **Flow states** (0x83C1D220): 0 load, 10 intro / celebration-replay state, 2 faceoff setup, 3 faceoff, 4 live play, 5 stoppage wait. State 5 is handled by `0x83326AC8`: after
  about 8 s it goes to 2 (restart) or to 10 (replay) depending on conditions in `0x833D9578` and the timer; **forced stoppages (reasons 2, 3, 6, 9, 13) always went 5->2: no replay**.
  The replay needs queued material that only the real play event creates (the view controller `0x834D2A00`, `this` = per-frame update object, walks a list at +220 and
  calls vtable slot 3 of each clip = `0x83463DB0`; the list is empty after a forced stoppage).
- **Forcing a goal by call crashes**: reason 2 runs the referee goal call `0x83505AC8`, which calls a vtable slot of the object stored at `0x83C87508` (word 0 = 0 until a real goal sets it): null call.
  Poking a fake object was not tried. Poke 0x83C1D236 (flow struct + 22) = 1 skips the stoppage wait (5->2 within 25 ms): a fast-forward, not a replay.
- A byte-for-byte natural goal sequence: `stopplay_a` -> `gameplay_reset(reason 2)` -> `stoppage` -> flow 5 for 8.4 s (goalie close-up camera at +4 s) -> flow 10 -> `replay_a`/`replay_f`... replay cameras.
- Crashes seen at about 10 s after any stoppage were the VP6 bridge `std::terminate` (dev/nuc-perf 1cdaf51), not scene forcing: merge dev/nuc-perf or later.
- The run length is capped by the bench window (about 165 s of play); a goal appears in roughly 60 % of the long sessions.

### Forcing a goal: what was learned (second round)
- `0x833FB7C0(x)` stores x in the word at **0x83C87508** (getter `0x833FB7D0`). It is called by the player-stop-play entry `0x83484320` (referee `0x83502C40` calls it with the player that stopped play).
  The referee goal call `0x83505AC8` calls vtable slot 60 of that word, so a forced reason 2 needs a polymorphic object there.
  Result: `833FB7C0(*(gp+164))` then `83326280(2,0x13,1,0x258,0,0)` **survives** (gp = word at 0x83C1E89C; flow 4->5, lastreason 2, goal mark, restart 8.4 s later) but scores nothing and queues no replay.
  With the "user player" (`vtable[60]` of `*(gp+168)`, a heap object at 0xA9...) as the word it crashes (the object's first word is not a vtable).
- Score = `goals_of_side 0x833FAD58`; the event firer for Goal/GoalAwarded is `0x83480248(scorer, ...)`, called from `0x83484320`, which has the natural argument pattern
  `(player, 0x82010000, 0x701EF4F0, 0x701EF4E0, 0x701EF4F0, 0x701EF4F0)` for goals: the next try is calling it with a real player object and those scratch pointers.
- The puck.cpp function `0x833676B8` is only called during the CPU-v-CPU intro (its r3 = AB240570, 44 entries of 144 bytes at +3120), not in user-v-CPU play: it is not the live puck object.
  `NHL_SCENE_DUMPWORLD` logs those entries (research; empty in a normal match).

## Open
Forced goal/replay still open: call `0x83484320` with a real player; or locate the live puck (e.g. snapshot the guest heap twice during play and diff float triples).
