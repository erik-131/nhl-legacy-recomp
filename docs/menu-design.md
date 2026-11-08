# Pre-game settings menu: design (P21, roadmap 4e)

Status 2026-11-07: logic written and unit-tested (372 checks); the SDL layer `menu_sdl.cpp` is written, wired into `main()` and **runs on dev with `NHL_MENU=1`** (window ~85 ms after start, draws into the window surface because the bundled SDL3 has no renderer; word wrap, ini keys untouched unless changed, scripted presses and page dumps done). Default off. **Scripted-pad run on dev 2026-11-07 (worklog 2026-11-07, menu test); not yet tested with a real pad or keyboard or under Moonlight/Wolf; not in the playable launcher.** The text below is the original design; steps 3 and 4 of the list near the end are done.
Code: `src/pregame_menu/` (logic only, no SDK/SDL/ImGui includes). Tests: `tools/pregame_menu/run_tests.sh` (258 checks, ASan+UBSan).
Layout preview without a window: `g++ -std=c++17 src/pregame_menu/menu_core.cpp tests/pregame_menu/demo.cpp -o demo && ./demo "]r"`
(keys `u d l r a b y [ ] s k`; `--table` prints the option table).

## Goal and the maintainer's condition
A compact screen before the game starts, driven by a gamepad, replacing launcher env vars (`tools/nuc/ct105/nhl_play.sh`) and the
messy F1 overlay as the place for settings. **Ok if it shows fast and closes with one Start press.** Nothing here may slow the normal
start, so: one window, no assets, no fonts to load, no shader compile, no game memory needed.

## Where it hooks in (recommended: A)
| | What | For | Against |
|---|---|---|---|
| **A** | Own SDL3 window + `SDL_Renderer` + `SDL_RenderDebugText` (built-in 8x8 font) + SDL gamepad API, run from `main()` before the app/runtime is created, same style as `src/game_setup.cpp` (SDL window + renderer, no ImGui) | Env vars and ini keys are set before any cvar is read, so every option works including restart-only ones (draw scale, vsync, fullscreen, monitor); no font/asset; no presenter, so fast | Text is plain 8x8 (scale 2x for 720p); a second window flashes before the game's own (see Unverified) |
| B | ImGui in the game's presenter after the window exists | Same look as the overlay, `NavEnableGamepad` already used in `nhl_overlay.cpp` | Window and cvars already set up: restart-only options too late; presenter must exist and then be paused; ImGui painting halves fps (measured) but irrelevant here |
| C | Separate launcher program | Cleanest separation | Another binary to ship and sync; no gain over A |

A is the design. B stays an option for re-using the same `Menu` as an in-game page later (4d); the logic does not change, only the renderer.

### Flow
1. `main()` (before `ReXApp` setup; if that is not possible, at the top of `OnPreSetup`): `Settings::Load("nhl_pregame.ini")` next to the exe.
2. If `menu.show=hold` and Back is not held, or `NHL_MENU=0`, or no pad and no keyboard attached: skip to step 5 (**the menu is never a blocker**).
3. Open window (about 720x405 is enough; if the SDK later wants the same SDL video init, keep it initialised), loop at 60 Hz: poll pad → `Menu::Update(bits, ms)` → draw `RenderLines(BuildView(menu))`.
4. On `StartGame`: `Settings::Save`. On `Discard` (B/Back) or `TimedOut`: do not save. Destroy window.
5. `ApplyAll(DefaultOptions(), settings, applier)`; then the existing `OnPreSetup` reads env/ini as today. Applier: `EnvIsSet` = `getenv`, `SetEnv` = `nhl::compat::SetEnv`, `SetIni` = `nhl::SaveSetting` (empty value erases the key; needs a small `EraseSetting` in `renderer/core/nhl_settings.h`).
6. The game starts. The quick-start pressing already ignores real input for 3 s after launch, which covers the Start press that closed the menu; it also needs the pad bits not to stay "held" (see tests: `TestDiscardAndHeldStart` covers the menu side only).

**Env wins**: an env var already set (Steam launch options, `bench` scripts, `NHL_*` dev overrides) is never overwritten, so benches stay reproducible. When the NUC session has verified the menu, the matching `export` defaults in `nhl_play.sh` can go (the menu's defaults equal them; unit-tested in `TestApply`).

## Screens
One screen, five tabs (LB/RB), about nine rows visible, a hint panel under the list that follows the focused row, a one-line legend.
```
NHL Legacy - Settings
 Start  [Performance]  Look   Display   Advanced
----------------------------------------------------------------
> Main pass multisampling                              *Off (1x)
  Render resolution                                    1x (720p)
  Waiting for the GPU                          Sleep (saves CPU)
  Frame limit                                           No limit
  Thread placement                           Game places threads
----------------------------------------------------------------
Draw the game's main pass at 1 sample instead of the game's 2x M
! Edges look more jagged.
Takes effect when the game starts.
A change  Y default  LB/RB page  START play  B skip changes
```
`*` = differs from default; `!` line = quality warning of the focused choice; "Takes effect when the game starts" = restart-type option (for the day the menu is also shown in game).
Landing tab is **Start**, so the common case is: open, press Start, play.

## Controller map
| Button | Does |
|---|---|
| D-pad / left stick up, down | Move; wraps; hold repeats after 400 ms, then every 120 ms |
| D-pad left, right | Previous / next value (wraps) |
| A | Next value; runs an action row (Reset to defaults) |
| Y | Focused option back to default |
| LB, RB | Previous / next tab |
| **Start** | **Save, close, start the game. One press.** |
| B or Back | Close without saving this session's changes; game starts with the saved settings |
| (hold Back at launch) | Shows the menu when `Show this menu at launch` is set to "only when Back is held" |
A button already held when the menu opens (the press that launched the game) is ignored until released. Optional auto-start timer (`MenuConfig::auto_start_ms`, default off): counts down visibly, any input restarts it.

## Options
Defaults equal what `nhl_play.sh` exports today. "Restart" = cannot change while the game runs; in this menu everything applies at the next game start. Exhaustive list and exact assignments: `demo --table`.

| Tab | Option (id) | Choices (default first) | Sets | Restart | Warning / evidence |
|---|---|---|---|---|---|
| Start | Boot screens (`boot`) | Quick start / Quick + full intro movie / Normal | `REX_NHL_QUICK_START`, `NHL_BOOT_CLOCK` (3 or 1), `NHL_QUICK_MOVIE_FRAMES` (12 or 365) | yes | Measured 11-06: menu file at 10.3 s (on screen ~12 s) with the 3x clock and 12-frame movie, 14.2 s with the clock only, v ~24-26 s |
| Start | Pre-compile shaders (`fill`, **one-shot**) | No / Yes next start 10 min / 30 min | `NHL_VPAD_SCRIPT` (generated, `FillScript`), `NHL_VPAD_EXIT_AFTER_SEC`, `REX_AUDIO_MUTE` | yes | The shader storage fill of `nhl_fill.sh` run inside the game process: see "Shader fill row" below |
| Start | Legal screen (`legal`) | Shown / Hidden | `REX_NHL_SKIP_LEGAL` | yes | Blank clouds instead of the legal text; boot no shorter |
| Start | Language screen (`lang`) | Shown / Hidden | `REX_NHL_SKIP_LANGUAGE` | yes | Keeps the default language; boot no shorter |
| Start | Show this menu (`show`) | Always / only when Back held | menu only | no | warns that it is hidden next time |
| Start | Reset to defaults | action | | | |
| Performance | Main pass multisampling (`msaa`) | Game 2x / Off 1x | `REX_NHL_FORCE_HOST_MSAA_1X` (cvar `nhl_force_host_msaa_1x`) | yes | Edges more jagged. **Measured 2026-11-06 17:25 (worklog): +14 % median** (2x 101.3/103.4, 1x 118.8/117.1 fps; p5 +10 fps), quick route; the cvar tested is this one. At 1:1 no visible difference, at 2x zoom slightly stair-stepped edges on skates, numbers and the circle line. (The earlier "+23 %" had no source; a 12:01 frame-script round saw +21 % in single runs.) the maintainer agreed it as a menu choice |
| Performance | Render resolution (`ss`) | 1x / 2x | ini `supersampling` | yes | 2x: heavier, cost on the NUC **guess**; 2x is the cap |
| Performance | Waiting for the GPU (`fence`) | Sleep / Spin | `REX_NHL_FENCE_POLL` 3 / 0 | yes | Measured: ~0.8 core saved; E-cores median 63.8→89.9 |
| Performance | Frame limit (`cap`) | None / 30 fps | `NHL_SWAP_INTERVAL` 0 / 2 | yes | From launcher comments, not re-measured |
| Performance | Thread placement (`cluster`) | Game / OS | `NHL_CPU_CLUSTER` 1 / 0 | yes | Intel hybrid only |
| Look | Edge AA (`edge_aa`) | Auto / On / Off | ini `edge_aa_mode` | yes | Off: jagged |
| Look | Post AA (`post_aa`) | Off / FXAA / extreme | ini `swap_post_effect` | yes | GPU cost **guess** |
| Look | Soften shadows (`shadow_soft`) | Off / On | ini `shadow_filter_linear` | yes | |
| Look | FPS graph (`hud`) | On / Off | `NHL_HUD` | yes | MangoHud |
| Display | Window (`fullscreen`), Monitor (`monitor`) | window / borderless; default, 1–3 | ini `fullscreen`, `monitor` | yes | |
| Advanced | Line changes (`manual_lines`) | Game option / Always manual | `REX_NHL_MANUAL_LINES` | yes | Every match starts with manual line changes |
| Look | Shadows in replays (`replay_shadows`) | On (engine default) / Off | `REX_NHL_NO_REPLAY_SHADOWS=1` | yes | Measured p25 69 -> 101 fps in replays; the replay picture loses shadows |
| Advanced | Skip pointless RT copies (`transfers`) | On / Off | `REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS` | yes | Off is slower (p5 36→60 measured) |
| Advanced | Upload pre-pass (`prepass`) | On / Off | `REX_NHL_UPLOAD_PREPASS` | yes | Off is slower |
| Advanced | Re-upload all guest memory (`pagestate`) | Off / On | `REX_CLEAR_MEMORY_PAGE_STATE` | yes | On is slower; for stale geometry only |
| Advanced | New shaders (`placeholder`) | Wait / Placeholder | `REX_NHL_ASYNC_NO_PLACEHOLDER` true / false | yes | Placeholder: more freezes (P17) |

**Not in the menu yet** (and why): engine tunables (`nhl_tunables.json`) and the input map need the running game or a long list; they stay in the F1 overlay until 4d/3c decide. Colour grade, FidelityFX, shadow softness and vsync exist in the overlay/ini and can be added as rows (one `add()` each, no logic change). Shadows/AO only run outside gameplay (replays, cutscenes) so a "shadow quality" row would not change gameplay fps; do not add one without the frame-script work (1l). Team choice / straight to a match (6b) would be a `Choice` list filled at run time: not built.

## Shader fill row (`fill`)
The fill route (`tools/nuc/ct105/nhl_fill.sh`) is a normal game run in which the scripted virtual pad (`src/vpad_script.cpp`,
`NHL_VPAD_SCRIPT`, started in the game process) presses random buttons for N minutes, so the pipeline storage fills. The menu
runs in the same process before the game starts, so it needs no launcher support: choosing "Yes, next start: 30 min" makes
`ApplyAll` export the generated press script (`FillScript(minutes, seed)`, same shape as the awk in `nhl_fill.sh`: quick-start
presses at 14 to 28 s, then a random press every 0.8 to 2.4 s), `REX_AUDIO_MUTE=true` and `NHL_VPAD_EXIT_AFTER_SEC=<minutes*60>`.
That last variable is new (`StartVpadScript`): the process ends itself with `_exit(0)` after that time (what `timeout` does in
the shell version; the storage is flushed as it grows, so nothing is lost).
- **One-shot:** the option has `Option::oneshot`; `menu_sdl` calls `Settings::ResetOneShots()` before saving, so `nhl_pregame.ini`
  never keeps it and the next start is a normal one. Discard (B) applies nothing of it.
- **Env wins:** a `NHL_VPAD_SCRIPT` already in the environment (bench scripts, `nhl_fill.sh`) is kept; the exit timer and mute still apply.
- **No progress hint in the menu:** the window is gone when the fill runs. Progress is visible in the game log
  (`[nhl-vpad] will exit after N s`, pipeline lines) and by the shader files growing; the run ends with the game closing
  (in the Wolf session the stream returns to Steam). Needs "Boot screens = Quick start" because the press timing assumes the 3x boot clock.
- Runs on the player's profile and saves (it presses buttons in menus); see `docs/mods-and-tweaks.md`.

## Persistence
`nhl_pregame.ini` next to the exe (same folder as `nhl_enhancements.ini`, `nhl_tunables.json`). Plain `key=value`, `#` comments, `version=1`. **Only non-default choices are stored**, so a later change of a default reaches everyone who never touched the option. Unknown keys are kept on save; a bad value falls back to the default for that one option; a missing file means defaults; write is `path.tmp` then rename. Options that the overlay already persists (`supersampling`, `fullscreen`, `monitor`, `edge_aa_mode`, `swap_post_effect`, `shadow_filter_linear`) are written to `nhl_enhancements.ini` by `Applier::SetIni` at every start, so the overlay and the existing `OnPreSetup` code keep working unchanged and **the menu is the owner of those keys** (a change made in the overlay is overwritten at the next start; either hide the overlay rows or make the overlay write `nhl_pregame.ini`, to decide).

## What the NUC session must do
1. Read this doc and `src/pregame_menu/menu_core.h`; run `tools/pregame_menu/run_tests.sh` (needs only g++).
2. **Check the cvar names** against the build: env `REX_<UPPERCASE CVAR>` is assumed from `REX_NHL_FENCE_POLL` ↔ `nhl_fence_poll`; `REX_NHL_FORCE_HOST_MSAA_1X` was used for the 2026-11-06 1x tests and works (cvar read once at render target cache start). A wrong name is silently ignored.
3. Write the thin SDL renderer `src/pregame_menu/menu_sdl.cpp` (~150 lines): `SDL_Init(VIDEO|GAMEPAD)`, window + renderer, open pad(s), map `SDL_GAMEPAD_BUTTON_*` and the left stick (`StickToPad`) to the `Pad` bits, `SDL_SetRenderScale(r, 2, 2)` and `SDL_RenderDebugText` per line of `RenderLines(..., cols)` (colour: selected row, `*` rows, `!` line), keyboard fallback (arrows, Enter = A, Esc = B, F5 = Start). Add `EraseSetting` to `nhl_settings.h`, the `Applier`, and call it from `main()`.
4. **Measure**: time from process start to first menu frame, and from Start to the game's first log line (budget: noticeable cost should be under ~1 s; this is a guess, no number exists). Check the Start press does not leak into quick start or the title screen.
5. Test over **Moonlight with a real pad**: Start closes it with one press; the Guide button (Steam menu) is not needed; B discards; hold-Back path; with the Wolf vpad the menu must still get pad events (see 'A client with a controller makes the vpad player 2' in the CT CLAUDE.md).
6. Then remove the overlapping `export`s from `nhl_play.sh`, update `the launcher notes`, log in the worklog, and decide the overlay overlap (above).
7. A/B once: all-defaults menu v current launcher (same env after `ApplyAll`; print the resulting env in the log to diff).

## Unverified (as written 2026-11-06 morning; the SDL window, timing and SDL_RenderDebugText points are resolved since, see Status)
- Whether the repo's SDL3 has `SDL_RenderDebugText` (added in SDL 3.2.0); fallback: ImGui (B) or a tiny bitmap font. The container has no SDL3.
- Whether a second SDL window before the game's window works under Xwayland in the Wolf session, and how fast it appears (`game_setup.cpp` does this for the missing-data case, but that path has not been timed).
- SDL gamepad events under Moonlight/Wolf before the game's window has focus; Steam Input remapping of Start/Back.
- Whether cvars read `REX_*` env at the point `main()` runs (the launcher works because env is set before exec; setting env inside the process later may be too late for some cvars, especially `kInitOnly` ones). If so, call before CLI/cvar parsing or re-exec once.
- ImGui gamepad nav: used by the overlay (`FeedGamepadNav`, `NavEnableGamepad`) but not tested by me; not needed for design A.
- The 1x main pass numbers are measured now (+14 %, worklog 2026-11-06 17:25); the 30 fps and thread-placement rows' interplay with vsync.
- Snowboard Kids (1 and 2): see the section below (looked at 2026-11-06, read-only; README, launcher and config source only; the menu library itself, RecompFrontend, was not read).

## Tests (container, no game)
`tools/pregame_menu/run_tests.sh`: option table sanity (unique ids, valid defaults); save/load round trip, default normalisation, unknown keys kept, junk/binary input, missing file, failing write, temp file gone; `ApplyAll` equals the launcher defaults, env wins, ini erase, menu-only keys not exported; navigation (move, wrap, change, pages, reset, **one Start press ends**, B/Back discard, held-at-open button ignored, repeat timing, stick dead zone, scrolling, auto-start timer and its reset); view (changed marker, warning, width limit down to 20 columns).

## Snowboard Kids recomp's menu (cdlewis/snowboardkids-recomp = SK1, and ...kids2-recomp = SK2; GPL-3.0: ideas only, nothing copied)
Read 2026-11-06 (shallow clones; submodules not fetched, so `lib/RecompFrontend` was not read). SK1 (the one the maintainer meant) and SK2 have the same structure; SK1 read: README, `src/main/sk1_launcher.cpp` (462 lines), `src/game/config.cpp`.
- It is **not a pre-window screen**. A launcher runs inside the app on the render backend (RT64), built with the RmlUi-based `RecompFrontend` library (themed, SVG/PNG art): buttons, in this order: Start game (becomes Select ROM until a valid ROM is chosen) / Change ROM / Controls / Settings / Mods / Exit (SK1: `sk1_launcher.cpp`); `Start` calls `recomp::start_game(...)` and hides the UI contexts.
- Settings are a tabbed config menu (gameplay, graphics, sound, controls, mods) usable with mouse, controller or keyboard; framerate slider, mod toggles with per-mod options; saves/config in the user config dir, `portable.txt` for portable mode.
- Compared with ours: same shape (launcher -> Start; tabs; pad-first), but theirs needs a UI toolkit and a running renderer, which is why they can use rich widgets. Our design A trades looks for starting before any cvar is read and for speed; it is the right choice while options are env/init-only. If a richer look is wanted later, the `Menu`/`View` split keeps the logic and only the renderer changes (design B, ImGui).
- Ideas worth taking: a visible **Start** as the first, default-focused action (ours: landing tab Start, Start button); tab per topic; mods page later (4d); portable mode flag if we ever ship outside the NUC.
- **Settings are declared as a schema** (SK1 `src/game/config.cpp`): per tab, options added with `add_enum_option(id, ..., options, default)` / number options, each with description, optional dependencies and a `hidden` flag (e.g. developer mode hidden by default), values read back by id with a default (`get_general_enum_or_default`). That is the same idea as our `Option`/`Choice` table, so the table approach is sound; their schema also has option **dependencies** and **hidden** options, which ours lacks (could be added if a row should only show when another is on, e.g. a quick-start sub-option).
- Game-specific rows live in the game's own `config.cpp` (split-screen direction, rumble, per-channel volume); the generic tabs (graphics, controls, mods) come from the library. For us: generic = Performance/Display, game-specific = Start/Advanced.
