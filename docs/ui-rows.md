# Extra rows in the game's own settings screens (`src/ui_rows.cpp`)

Cvar `nhl_ui_rows` (env `REX_NHL_UI_ROWS`), default empty = off. Comma list: `skaters` (row "Skaters: 5 on 5 / 4 on 4 / 3 on 3"), `skaterslong` (label key from a loc mod, see below), `log` (log every call the settings screens make, 40 per function), `all`. Study that led here: `menu-control.md` (plan B). Addresses are in `ui_rows.cpp` and the private notes, not here.

## Roles (how the settings screens work)
- The screens are a generic renderer; a native settings object answers: page lists, per-setting details, choice labels, current value, set value.
- A setting is a record of a static table (about 222 records of 32 bytes): id, label key, control type (3 = choice list), min, max index, default index, count, pointer to an array of choice-label keys. The visible text is the loc key's text.
- A page lists records by index. Two runtime tables (in game memory, filled at start) hold {pointer to index array, count}: settings per page, and settings per page/sub-page (`page * 47 + sub`). The in-game Rules page, the pre-game Rules pages (with period length and tie-break) and others are arrays that contain both Offsides (11) and Penalties (17).
- Get and set of a value are hand-written switches in two native functions (current value: `this, ?, index, out{0,value}` returns the choice index; set: `this, ?, index, valueObject` with the new choice at `+128`).
- The calls are made through a vtable, so a hook on the function sees them.

## Mechanism (nothing on disk changes)
1. One hidden record that no page shows (a profile flag, label 0) is overwritten with the new row. The table sits in read-only mapped data: its pages are made writable first (`Heap::Protect`).
2. Strings and the choice array are written to a 4 KB block of the guest system heap (the image padding is read-only too).
3. Every page array that holds Offsides and Penalties is copied with the new index appended, and the runtime table entry is repointed (done at the first page query, content-matched, so no page numbers are hard-coded).
4. The hooks on get/set answer for the new record; every other index goes to the stock code.
The value lives in `nhl_skaters_per_side` (cvar, used by `rules_hooks.cpp`): 5 on 5 = 0, 4 on 4 = 4, 3 on 3 = 3. It lasts for the process, not in a save.
Adding another row (manual line changes, arcade preset): another host record, another label and choice list, and handlers for its index.

## Pages
- `skaters`: the rules pages (arrays holding Offsides and Penalties: in-game Rules, other rules pages) and the pre-game **Quick Settings** page (Play Now > Game Settings; after Shootout Camera).
- `lines` ("Line Changes: Auto / Manual", cvar `nhl_manual_lines`, logic in `rules_hooks.cpp`): the pre-game **Advanced Settings** page (after Away Shot Aim). The page is chosen per row by `PageWants` (content match).
- The pre-game screen has only two tabs (Quick, Advanced) and no Offsides/Icing page; the "Rules: Custom" row is a rule-set choice.

## Driving it with the pad script
`NHL_VPAD_SCRIPT` now takes `lt` and `rt` (analog triggers, raw axes 2 and 5). Route used (seconds): `14,16,18,20:a` (Select Jerseys, then the advance menu), `23,25:down`, `27:a` (Game Settings), nine `down` (Skaters), `right` x2 (3 on 3), `rt` (Advanced), five `down`, `right` (Manual), then `a`, `a` (advance menu, Advance to Gameplay), `start` x3 for the intro, `a` (resume if it paused).

## Results (dev install, 2026-11-07, no REX_NHL_* env var for the rules; only `REX_NHL_UI_ROWS`)
- Pause > Settings > Rules shows "Skaters 5 on 5 / 4 on 4 / 3 on 3"; right changes it (log `set row skaters`).
- Pre-game: Play Now > Game Settings > Quick Settings shows "Skaters" (5 on 5, then 4 on 4, 3 on 3) and Advanced Settings shows "Line Changes: Auto/Manual" (`rows-pregame.jpg`; log `set row skaters value 1/2`, `set row lines value 1`).
- Match started after setting: **3 on 3** (`x13-play.jpg`): the faceoff card shows one forward (C) per side instead of three, i.e. the pk3 set (1F + 2D); play shots show 3 skaters + goalie for ANA. **4 on 4** (`x14-play.jpg`): faceoff cards with LW + C (2F + 2D). 20+ s of play in each, no crash.
- Manual line changes: the row sets `nhl_manual_lines` (log); its effect in play was not looked at in this run (see `line-change.md`).
- Label: existing loc key "Skaters" / "LineChanges". "Skaters per side" needs `locdb.py add` (file made, OK by `check`, **not tried in game**), then token `skaterslong` and the file under `mods/<name>/fe/loc/nhl_eng_us.db`.
- The description box under the new rows is empty (no description key).
- The value lives only in the process (cvar), not in a save. Fallback C1 was not needed.
