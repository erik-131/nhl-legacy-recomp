# Lua scripting in the recompiled game (file-only, no code change)

Status 2026-11-07, measured on the Linux dev install (scratch cache roots, `REX_CACHE_ROOT`). Scripts: `tools/lua/`.
Background and the full binding table: the development log (2026-11-07 Lua entries); the binding list was lifted from the game's own
registry (`p-lua/lua_bindings.tsv`, not committed). Nothing here modifies the game install: the cache copy overrides the loose file.

## What works (all verified)

| Item | Result |
|---|---|
| Override `scrape/scenedef.lua` from `cache/guest/scrape/` | works; the game reads `cache:` first, no FAILED probe line for it |
| `LoadScript("scripts/x.lua")` from the end of that scenedef | works; path relative to `cache/guest/`; runs at boot (about +1.5 s) |
| `RunEachFrame("code")` | works; runs once per guest frame, ONE string only (a second call replaces the first) |
| `EnterGameplay(home, away, 0, -1)` from the frame hook | **starts an exhibition match without menu input** (below) |
| `EnterShootOutMode()` from the frame hook | **starts a shootout directly** (VAN v VAN, "Shootout Round 1") |
| Log output from Lua | works through a failed `io.open` (below) |
| `SetClearColor(r,g,b,a)` | works (the earlier s3 run tinted the ice) |
| `SetTunableValue(name, value)` | **does nothing in this build** (below) |

## Logging and errors (the pitfalls first)

- `print` prints nothing anywhere. The game replaces the base library `print` with a function that only type-checks its arguments.
- Lua errors are silent: a runtime error or a syntax error aborts that one script only (the next `LoadScript` still runs) and logs nothing.
- Writes do not work (`io.open(..., "w")` fails with 0xc0000022). `os`, `debug`, `package` are nil, so there is no clock: use the host log timestamps.
- **`loadstring()` in a script kills the game** about 1 s after start ("Window closing" in the log; two runs). Do not use it (or `dofile`/`load`, untested).
- **Log channel:** `io.open("LUAMARK_text", "r")` fails, and the host log gets `[NtCreateFile] FAILED: path='LUAMARK_text'` with a timestamp.
  Characters outside `[A-Za-z0-9_.-]` must be replaced (a name with `"`, `:` `[` or a space-less error prefix makes the game reject the name silently).
  `tools/lua/prelude.lua` has `LOG(msg)` (sanitises) and `SAFE(f, ...)` (pcall that logs the error message).
  Verified: `pcall` works and returns the message, e.g. `[string "scripts/b3.lua"]:9: attempt to call local 'x' (a nil value)` comes back as the log line
  `LUAMARK_d3_false__string__scripts_b3.lua___9__attempt_to_call_local__x___a_nil_value_`.
- `io.open("cache:\\scrape\\scenedef.lua", "r")` succeeds (read access goes through the game's file layer with the `cache:` device); a relative `io.open("scripts/a1.lua")` fails.
- Lua is PUC 5.1.3 (`_VERSION` = "Lua 5.1"); `string`, `math`, `table`, `io` (read) and `pcall` behave normally.

## Straight to a match

The bindings are thin wrappers (read from the lifted code): `EnterGameplay(a,b,c,d)` calls `sub_829D7078(0, a, b, c, d, -1)`, `EnterShootOutMode()` calls it with
`(1, 28, 28, 0, -1, -1)`; `a`/`b` are team ids (below 220), `c`/`d` per-side values (0 and -1 as in the shootout call).
`SetDemoTeams(h, a)` only stores two globals; `EnableFEBypass(a,b,c,d)` sets a flag and four globals; `SetAISeed`, `EnableGamespeed`, `SetEnableAttractModeGameplay`
are empty stubs (`lua_tonumber(1)` then return).

Measured recipe (`tools/lua/autostart.lua`): at hook call 5200 (about +11.5 s, after the main menu is up at +10.2 s) call `EnterGameplay(5, 12, 0, -1)` (FLA v CAR).
The scene goes GAMEPLAY/3D, MENU/2D, GAMEPLAY/3D (the game loads), the arena intro needs two Start presses, then the faceoff.

| Run | Result |
|---|---|
| `EnableFEBypass(5,12,0,0)` in scenedef at boot | no effect, stays on the main menu |
| `SetDemoTeams(5,12); EnterGameplay(5,12,0,0)` at call 6000 | scene reaches GAMEPLAY/3D but the screen stays black for 58 s (no Start presses; `0,0` instead of `0,-1` is the other difference) |
| `EnterGameplay(5,12,0,-1)` at call 1500 (+4.6 s, title screen) | too early: never reaches the ice |
| `EnterGameplay(5,12,0,-1)` at call 6000, Start at 23 s and 31 s | **ice (more than 1000 draws/frame) at +34.1 s** from process start (quick route: +40.1 s); teams FLA v CAR as asked |
| the same with a third Start at 39 s | pauses the game (pause overlay): do not press more |
| call 5200 (+11.5 s), Start at pad times 16 s and 18 s (log +17.2, +19.2 s) | **ice at +21.0 s** (best; quick route +40.1 s, about half) |
| call 4400 (+10.6 s, right at "main menu done") | too early again: no scene change at all in 40 s |

The hook runs about 500 calls/s in the menus and 700/s in a match on the bench (swap interval 0), so the call count is only a rough clock.

## Shootout

`EnterShootOutMode()` from the frame hook (call 6000, Start at 23 s and 31 s): the scene switches, the arena loads, ice at +34 s with the
scoreboard "(H) 0 ... (A) 0 ... Shootout Round 1" (both teams are Vancouver, the id 28 is hard-coded in the binding), a skater against the goalie, replays follow.
Screenshot: `<workdir>/p-lua2/shots/S_shot_34s.jpg`. Team ids cannot be chosen: the binding passes 28 and 28 (call `EnterGameplay` with mode 1 is not exposed).

## Per-frame hook

`RunEachFrame("HOOK_TICK()")` (`tools/lua/hook_log.lua`): the string is compiled and run every guest frame. Verified through the log: one `LUAMARK_hook_N`
line per 1200 calls, which came every 0.12 s in the menus before the main menu (~490 calls/s), 0.8 s on the menu and 1.7 s in the match (~700/s).
It runs on the guest main thread, so keep it tiny (it is parsed again every frame). Its rate depends on the host frame rate, there is no clock in Lua:
for "once a second" count calls and calibrate against the log timestamps, or drive time from something else.

## SetTunableValue: no effect in this build, and why

The binding is `SetTunableValue(name, value)`: both arguments are read as strings (`lua_tolstring`), the name is hashed and looked up in a hash map whose
object pointer sits in guest memory at 0x83B6F0FC (`sub_828B0610`). **That pointer is 0 at runtime** (read from the live process in the menu, `p-lua2/T4.mem`),
so the lookup finds nothing and the call silently does nothing. The map is probably filled only by the EA tweak/remote-console server that the retail
build does not start. Evidence, all negative: (1) a host-side scan of 288 MB of guest memory (static data 0x82000000-0x84000000 and 0x40000000-0x50000000) after setting
`gLODShader` to 195948557 and `gPuckGravity` to 1234.5 (also with six name spellings: group path with `/`, `.`, `\`) finds neither value anywhere;
(2) `gShowCPUAwayLineWhenUserHome = 0` (stock 1) leaves the away line box in the faceoff HUD (paired runs, same screenshot seconds); (3) five more gameplay tunables
set at boot and every 10th hook call (puck gravity -40, max speed 400 %, ...) showed nothing in a match.
Note: the `field+8` value in `docs/tunable_values_runtime.txt` is the type code of the registry record (2 int, 1 bool, 4 float), not the live value (read back: unchanged).
**Consequence: the tunables editor (memory scan, `src/tunable_runtime.h`) stays the only route for tunables; Lua cannot replace it.**

### gLODShader re-test (the request)

With a straight-to-match script, `gLODShader` = 2 (stock) / 0 / 5 set at boot and every 10th hook call: match fps median 103.1 / 102.5 / 101.1 (n = 29 one-second samples each, one run each),
draws/frame 1709 / 1685 / 1721, screenshots at the faceoff look identical. Because the call is a no-op (above), this is not a measurement of the LOD shader tiers; it
only confirms again that nothing changes. Values 1 and 3 were not run (no point once the store is shown to be unreachable).

## Not done / open

- `SetDemoTeams` as a team chooser, `EnableFEBypass_*`, `EnterPracticeMode`, `SetInfiniteGameplay`, `EnableWarmupGame`, `SetCamera`, `controller setters`, `Resim*`,
  `SaveInputs_*`, `CreateLeague` were read (argument counts) but not run. Their wrappers: most read their arguments with `lua_tonumber` and call one C++ function.
- A way to skip the two Start presses (arena intro): not found. `SetAISeed`, `EnableGamespeed` and `SetEnableAttractModeGameplay` are empty stubs.
- The earlier s2 test (Crowd render object removed) stays inconclusive.
- Pitfall for runs: other agents' runs wiped `shot_*.png` and killed the game twice at start; `p-lua2/run.sh` picks its log by the cache-root string.

## Reproduce

```
GAME_SCRAPE=<game>/_compiled/scrape tools/lua/stage.sh /path/to/stage autostart.lua   # copy shaders next to guest/ if needed
REX_CACHE_ROOT=<stage dir as seen by the game> ./nhllegacy       # plus the usual quick-start flags; press Start at about 17 s and 19 s after launch
```
