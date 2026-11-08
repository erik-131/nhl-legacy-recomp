# Taking control of the game's own menus (Game Settings, mode lists, pause)

Static study, 2026-11-07 . Evidence tags: [meas] decoded from files/image here, [read] read in lifted code or earlier docs, [guess] not verified.
Addresses and names are kept in private notes, not here. No game run was needed or made.

## 1. What renders the front end, where screens live

- [meas] The front end is a **Flash-derived "Apt" UI with ActionScript 2 style bytecode** (a movie-clip tree, `_root`, `_level0`, `onEnterFrame`, `loadMovie`, `gotoAndPlay`-style API), not Lua and not Scaleform GFx/.swf at run time (scripts still call the paths `*.swf`; the files on disk are `.big`).
- [meas] Screens are under `_compiled/fe/ion/` (27,263 `.big`, 99 % are art in `artassets/`). Logic screens: `game/screens/<area>/<screen>.big`
  (areas: `settings`, `playnow`, `pause`, `gamemode`, `classicmode`, `scenariomode`, `practice`, `shootout`, `pro`, `dynasty`, `tournament`, `online`, `mud`...),
  shared widgets `game/components`, `game/custom/*`, helper classes `game/pods/*.big` (flow, menu control, UI handlers), shared framework `external/ion_sports07/*`.
- [meas] File format: every `.big` is a **BIGF** archive (magic `BIGF`, size LE, entry count BE, header size BE, then `offset BE, size BE, name` per entry).
  Entries seen: `Apt Data:1:7:4` (the movie: tags + bytecode), `Apt constant file` (string/number pool of the bytecode), `XPR2` (Xenon textures), small geometry blobs.
  Apt Data and the constant file are **RefPack** (`10 FB` + 3-byte size) per entry; some entries are raw. A decoder is ~40 lines (not included here); all 1,402 non-art `.big` unpack, 2,804 Apt/const entries extracted.
- Readable: yes (strings and the bytecode constant pool are plain). Rewritable: **not yet**: needs a RefPack encoder (a literal-only encoder is valid and trivial) and an Apt bytecode patcher (action records, jump offsets, constant indices; no assembler exists for it here). Delivery of a changed `.big` would be the normal loose-file override (`cache/guest/fe/ion/...` or a `mods/` layer, see `mod-layers.md`).
- Screens relevant here [meas]: `settings/gamesettingdata` (the Game Settings pages; it loads on top of the generic `external/.../misc/gamesettings` shell), `settings/customcamerascreen`, `playnow/modeentry` + `01_selectteams08` + `03_startinglineups` (Play Now), `pause/pausemenuscreen` + `limitedpausemenu`, `gamemode/*`, `scenariomode/*`, `practice/scenariosettingsscreen`, `mud/mudsettingsscreen` (league settings), `shootout/*`, `online/screens/settings/settingsscreen`. The mode list entry is `playnow/modeentry` / `classicmode/modeentry` (no per-mode data file seen; the mode ids are constants in `game/custom/customlibrary`).

## 2. Where the Game Settings rows come from

Rows are **not** in a UI file. `gamesettingdata` (ActionScript) asks a native settings object (script class `ION_Settings`) for everything:
categories (`GetCategoryIDs`, `GetCategoryName`, `GetSubCategoryIDs`, `GetCategorySettingsIDs`), and per setting `GetSettingDetails` (control type, labels, values, dependencies),
`GetChoiceListDisplayStrings`, `GetChoiceListIDs`, `GetDependentSettingIDs`, `GetSettingDescription`, `GetCurrentSettingValue`, `SetCurrentSettingValue` [meas: names in the constant pool; bindings registered in the native binding table, read].
So the screen is a generic renderer; the model is native C++ data + code.

- [meas] **Setting table** in the executable's writable data: 126 records of 32 bytes (indexed by "setting index"): `{internal id, label key ptr, control type (3 = choice list, 0/1 = slider), min?, max index, default index, count, ptr to array of label-key string pointers}`.
  Example rows (index: key, choices): 3 `PeriodLength` (3..20 minutes, 18 entries, default entry 7 min), 11 `Offsides` (OFF/ON/DELAYED), 12 `Icing` (OFF/ON/NoTouch/Hybrid), 13 two-line offside, 14 `Injuries`, 15 **`TieBreak`** (ContinuousOT, Shootout, 5MinThenShootout, SingleOt4on4, SingleOt5on5, NoOvertime; default 2), 17 `Penalties`, 18 `Fighting` (sliders 0..6), plus the 20 sim sliders, camera, audio, and the league/season/tournament rows (`SeasonTieBreak`, `PlayoffTieBreak`...).
  The choice arrays are pointers to **loc keys** (`TXT_7_MINUTES`, `SingleOt4on4`...); the visible text is the loc DB entry (`loc-db.md`). So wording is data; the number and order of choices, the category membership and the mapping to the game are code/static data.
- [meas] Category membership/order: static index arrays in the same data area (e.g. a 21-entry order `0..14, 18, 19, 20, 15, 17, 16`); [guess] these are the Rules/Gameplay pages.
- [read] **Get/set are hand-written switches** on the record's internal id (large getter/setter functions; ids 9..23 read a generic attribute array of the game-setup object by `id-9`, other ids read profile/league/online objects). A new id has no case, so a new row needs a hook, not just a table edit.
- [meas] **Reaching the game:** the game-setup object owns a rules struct that is exposed through a reflection descriptor (names = the scenario JSON keys) with plain 32-bit ints: `skill +0`(guess), `penaltylevel +4`, `fightinglevel +8`, `offside +12`, `icing +16`, `twolinesoffside +20`, `injury +24`, `refereetype +28`, `rinktype +32`, `tiebreaker +36`, `minorpenalty +40`, `hero +44`, `goldenhelmet +48`, `createdplays +52`, `beaprobehaviour +56`, `nhlgamestyle +60`, `tuningversion +64`, `dynamicratings +68`, `periodlength +72` (matches `rule-hooks.md`, which reads `+72`), shootout rules as bytes at `+384..386`. Scenario `gamerules` JSON uses the same keys, so JSON and UI feed one struct.
- [meas, consistent] The UI choice **index is stored directly**: `tiebreaker` is 0 or 2 in the stock scenarios = index into the TieBreak list (0 ContinuousOT in playoff scenarios, 2 = NHL default 5 min 4-on-4 then shootout). `periodlength` is the minutes value itself (scenarios use 5/7/8/10/12/15/20) while the UI list index is minutes-3 (two different conversions in the get/set code).

## 3. Is there a data-only skater option?

No. [meas] The rules struct and the scenario `gamerules` chunk have **no skater-count field**; the only OT-related fields are `tiebreaker` (index above) and the shootout bytes. The TieBreak list offers `SingleOt4on4` (4v4 OT) and 5v5 variants but **no 3v3**; the per-team `ot3lines` data exists but the game never selects it (`rule-hooks.md`: only `pk3` is used for 3 skaters). So the UI can expose OT 5v5/4v4 by data (already does), but a 3v3 value needs code (our `nhl_skaters_per_side` clamp) behind it. Loc keys for "3 on 3" exist (`PRACTICE_3_3`, "3 ON 3"), "4 on 4" (`4on4`), "5 on 5" (`5on5`).

## 4. Ways to add "Skaters: 5 / 4 / 3" (ranked)

| | Approach | Effort | Risk | Needs |
|---|---|---|---|---|
| **A** | **Data only** (edit a `.big` or loc, deliver by override) | not possible for a new row; possible only to relabel | low | The setting table and category arrays are in the executable's data, not in files. Loc edit can relabel an existing choice (e.g. `SingleOt4on4` text) but cannot add a row or a value, and a relabel does not change behaviour. A screen `.big` patch (AS) could add a row only if it stores into an existing native setting (see C2), and needs the missing RefPack encoder + Apt patcher. |
| **B** | **Runtime table patch + get/set hook** (host code in `src/`, opt-in like `rules_hooks.cpp`) | medium: 1 to 2 days | medium | At boot, overwrite one table record that is already shown in the Rules page with: new label key (needs a loc key; `locdb.py add` is untested in-game, or reuse a visible-but-unused row's key and change its text with `locdb.py set`, proven), a 3-entry choice array in our own memory (loc keys for "5 on 5"/"4 on 4"/"3 on 3"), max=2, default=0, and a **new internal id**; hook the get and set functions for that id to read/write our cvar `nhl_skaters_per_side` (5->0, 4->4, 3->3) and, for persistence across screens, mirror it in the game-setup object. Candidate rows to give up: ones with 2 or 3 choices not needed in Play Now (`TWOLINESOFFSIDE`, `GoalieMode`, `AssistantCoachEditLines` (3 choices, BAP/GM only: not in the Rules page, so also needs a category array edit)). Unknowns to settle in one dev run: which page lists which record, and whether the setter is called on value change (log the getter/setter ids). |
| **C1** | Reuse the TieBreak choice as the switch ("Single OT (4 on 4)" -> 3v3 in OT only) | small: hours | low | Relabel the loc text of `SingleOt4on4` to "Single OT (3 on 3)" (`locdb.py set`, proven channel) and have the OT start hook (`rules_hooks`, OT line-set pick) clamp to 3 when `tiebreaker==3`. Gives 3v3 overtime from the stock menu with zero UI code, but not a whole-game 3v3. Matches the NHL's real 3-on-3 OT. |
| **C2** | Piggy-back a whole-game value on an existing, visible 3-choice native setting | small to medium | medium | e.g. repurpose `Offsides` (OFF/ON/DELAYED) is a bad idea (breaks offside); only an unused row is acceptable. Same hook as B without table edit. Not recommended over B. |
| **D** | Our own menu (exists: `NHL_MENU=1`, `docs/menu-design.md`) | done | none | Already has the skaters and period rows; runs before the game starts, not inside the game's screens. This is the working answer until B is built. |

Recommendation: keep **D** as the user-facing path, add **C1** (cheap, honest 3v3 OT from the stock screen), and build **B** only if the maintainer wants the row inside the game's own Rules page. B is a hook plus a table patch; it is not a data change, so there is no data-only prototype to test and none was made.

## 5. Pause menu and mode lists

- [meas] Pause: `pause/pausemenuscreen` (largest screen, 64 KB entry set) is an Apt screen like the rest; its rows come from AS plus native flow state (`gScreenFlowManager`, `FROM_*_PAUSEMENU` contexts, and the same `Game Settings` screen reached from pause with context `FROM_OFFLINE_PAUSEMENU`). So **rules edited from the pause menu go through the same table and setters**; B would show there too.
- [read] Mode lists: Play Now teams come from DB rows through a native "get ids" call and the UI resolves names by loc keys (number and abbreviation) (`team-hooks.md`); that is data-driven and already exploited (32 teams). Game *modes* (Play Now, Be a Pro, Dynasty, Shootout...) are constants in AS (`customlibrary`) mapped to flow states; adding a mode needs an AS patch plus a flow state, so not pursued.

## 6. Next steps if B is wanted

1. Dev run with a logging hook on the settings get/set functions (log record id and index) while opening Game Settings once through the F1/quick route; confirms category membership. One run, bench lock `menustudy`.
2. Decide the donor row and loc key (`locdb.py set`, deliver as `cache/guest/fe/loc/nhl_eng_us.db`).
3. Implement the table patch in `src/rules_hooks.cpp` (after the image is loaded) plus get/set hooks; reuse the existing cvar.
4. For anything needing AS changes: write the RefPack encoder + a minimal Apt constant-pool patcher first (strings only, same length) and test on one string in `gamesettingdata`.
