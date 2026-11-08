# Roster choice: original or 2026 rosters (P13)

The player picks the rosters; no file copying by hand. Code: `src/roster_choice.{h,cpp}` (pure logic), the hook in `OnPostSetup`
(`src/nhllegacy_app.h`), the `rosters` row of the pregame menu (`src/pregame_menu/menu_core.cpp`), installer
`tools/rosters/install_roster_set.sh`. Test: `tests/roster_choice/test_roster_choice.cpp`.

## Mechanism
A roster set is a mod directory `mods/roster-<name>/db/nhlng.db` beside the exe ([mod-layers.md](mod-layers.md)). The mod layer
already serves `db/nhlng.db` to the game's `cache:\db\nhlng.db` open ahead of the stock `_compiled/db/nhlng.db`, so nothing in
the game or the DB layer is hooked: the swap happens when the mod index is built (OnPostSetup, before any guest code runs), and
the game just opens a different file. All `roster-*` mods are mutually exclusive: `ModIndex::Scan(..., force_disabled)` marks
every set but the chosen one `disabled (not selected)`.

## Bug found on the way: mods were not served for most opens
The guest opens most files relative to a directory handle, which never goes through `UnionDevice::ResolvePath` where the mod index was
consulted; the first roster runs therefore always got the stock DB (the `[db-open]` line showed `game/_compiled/db/nhlng.db`). Fixed:
`LooseTreeEntry::Open` now consults the mod index too (`LooseTreeDevice::SetMods`, forwarded by `UnionDevice::SetMods`), so every
file of a mod that exists in `_compiled` is served for relative and absolute opens. Mod files that do not exist in `_compiled` are
still served only by absolute path (unchanged).

## What was checked
Quick route, original and 2026 set, plus the menu path:
- The log shows which database was opened: `[db-open] ... <- .../game/_compiled/db/nhlng.db` (original) v `<- .../mods/roster-2026/db/nhlng.db (mod)` (2026).
- Faceoff line cards at 49 s differ between the two sets (original `M. BOEDKER / M. HANZAL / S. DOAN` v `P. MAROON / R. GETZLAF / C. PERRY`; 2026
  `L. CROUSE / N. SCHMALTZ / C. KELLER` v `C. GAUTHIER / L. CARLSSON / T. TERRY`), and #91 Carlsson shows in the faceoff close-up.
- Menu end to end: on the Start tab, 5 x down, right -> the row shows `*2026-27 season` with the "Takes effect when the game
  starts" note; START wrote `rosters=2026` to `nhl_enhancements.ini` and the same run served the mod DB (131 `(mod)` opens).
- Shared memory during the runs stayed flat (52-1616 MB), no growth.

## Where the choice is stored
1. env `NHL_ROSTERS=<name>` (launcher / dev run; wins)
2. else key `rosters=<name>` in `nhl_enhancements.ini` (written by the pregame menu, row "Rosters" on the Start tab; the original
   choice erases the key)
3. else original.

`original` (also `default`, `stock`, empty) = no set, stock DB. Names are case-insensitive.

## Safe fallback
Anything wrong gives the original rosters, with one log line `[nhl-rosters] choice 'x': using the original rosters -- <why>`:
unknown name, no `db/nhlng.db`, a symlink, smaller than 1 MiB, first 16 bytes differ from the stock `nhlng.db`, or any exception.
A rejected set is also disabled in the mod index, so the broken file is never served. Not checked: the TDB table CRCs
(`tools/rosters/tdb.py check` does that offline; the installer does not run it).

## Installing a set
```
tools/rosters/install_roster_set.sh <install dir> 2026 <path to>/nhlng_2026.db "2026-27 rosters, 30 slots"
```
Other sets (e.g. the 32-team DB from `build_32.py`) are just more `roster-<name>` dirs; the menu row has the two fixed choices
Original and 2026 (menu choices are static), other names are reached through the env var or the ini key.

## Caveats
- Restart needed: the DB is read at load; the choice is read once at start. An in-game row (`ui_rows`) was not added for this reason:
  it could only write the ini for the next start. The pregame menu is shown at launch (`NHL_MENU=1`).
- A file `cache/guest/db/nhlng.db` (the old manual route, e.g. `REX_CACHE_ROOT` test roots such as `nhl-rostertest`) sits above the
  mod layer and shadows any set. It is not bypassed (the cache dir is the game's own writable layer); the start now logs
  `[nhl-rosters] <path> exists and shadows the roster choice; remove it`. The normal cache dir
  (`.local/share/nhllegacy/cache/guest`) has no such file. The `[db-open]` log lines now end with `<- <host file served>`, which shows
  which DB the game really got.
- The menu row says "Takes effect when the game starts" (option flag `restart`); the menu only runs before the game, so the note is always true.
- Saves (rosters/franchise) made with one set may look wrong with the other (player ids differ).
- 2026 set = P13 stage 1 (30 stock slots; ratings derived from stats). See [rosters-pipeline.md](rosters-pipeline.md).
