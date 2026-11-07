# rosters.py: CSV export/import, check, diff (roadmap 2c)

`tools/rosters/rosters.py` sits on `tdb.py`. Tests: `python3 tools/rosters/test_rosters.py` (20 tests, synthetic databases only) and `test_tdb.py`.
Never put exports in the repo: they hold EA data (use e.g. `<workdir>/rosters-out/`).

Tags below: **[real]** verified on the stock `nhlng.db` / `draft_roster.db` (2026-11-07), **[synthetic]** only in the unit tests, **[guess]** not confirmed.

## Commands
```
rosters.py export DB OUTDIR [--meta PATH]        one CSV per key table + players.csv
rosters.py import DB CSVDIR OUT [--meta PATH]    apply edited CSVs to existing rows, write a resealed copy
rosters.py check  DB [--draft PATH] [--meta PATH]   consistency report, exit 1 on errors
rosters.py diff   DB_A DB_B [--meta PATH] [--verbose]   rows added/removed/changed per table
```
The meta XML defaults to `nhlng-meta.xml` next to the file (also for `draft_roster.db`, which has none of its own).

**export** writes `<table>.csv` for exhibitionteams, exhibitionplayerbiotable, exhibitionskaterai, exhibitiongoalieai,
exhibitionrostertable, exhibitionplayers, exhibitionskaterequipment, exhibitiongoalieequipment, playernames,
exhibitionfreeagents, exhibitiondraftpick (tables missing in the file are skipped). Columns: `row` plus every field by meta name;
integers are the shown values (stored + schema low limit, so `team` can be -1); deleted rows are skipped. **[real]**
`players.csv` is a read-only joined view, one line per player and roster entry: game_id, team index and abbreviation, slot,
roster key, status (roster / free agent / unlisted), names, position, handedness, jersey, some bio fields, all `c_*` ratings by name
(skater and goalie columns side by side). **[real]** Ratings are shown values 0..63; the game's number is probably shown+36 (review
says "value-36"; stock shows range 0..63, mean about 27) **[guess]**.

**import** reads every `<table>.csv` in CSVDIR (not `players.csv`). Rows are found by the meta's key field(s)
(`key="True"`; no key means the `row` number). Only existing live rows can change; unknown keys, duplicate keys, unknown columns,
non-integers, values outside the schema range or width, and strings of 25+ bytes (field holds size-1) are rejected, all
reported together, and then nothing is written. Key columns cannot be edited; rows removed from a CSV stay unchanged. OUT must
differ from the input; the input is never touched. Export then import without edits is byte identical. **[real]** (whole
stock nhlng.db) **[synthetic]** (rejections, edits). A real edit (name + jersey) imported, passes `check`, `diff` shows exactly
those two values. **[real]**

**check** reports ERROR / WARN / INFO. Errors: any CRC or layout problem from `Db.check()`; duplicate keys; roster key not
`team*40+slot`; roster team not in exhibitionteams; roster row -> `exhibitionplayers.index` -> `playerid` -> bio `game_id` ->
(skater or goalie AI by position) + (matching equipment) must all exist; a player twice on one team; more than 40 per team;
AI/equipment/players rows without bio row; free-agent and draft-pick references (and draft-pick teams) dangling;
draft_roster.db not exactly 10 tables, a table not in nhlng.db, or a different field layout. Warnings: team with fewer than 2 goalies,
bio team/jersey differing from the roster's (NHL teams 0..31 only), a free agent also on a roster, rows that differ between
nhlng.db and draft_roster.db (per table: only in draft / only in nhlng / changed). Info: deleted rows, bio players on no roster,
players on several teams. The draft file gets the same cross-table checks. **[synthetic]** for every error rule; stock passes **[real]**.

**diff** compares live rows of tables in both files by key (raw record bytes first, then field values); `--verbose` lists changed values.

## The real links between tables [real]
```
exhibitionrostertable.key         = team*40 + slot  (key//40 == team on all 5532 rows; slot not contiguous)
exhibitionrostertable.playerindex -> exhibitionplayers.index   (all 5532 resolve)
exhibitionplayers.playerid        -> exhibitionplayerbiotable.game_id   (several indices may share one playerid: 393 players)
bio.game_id == exhibitionskaterai.game_id == exhibitionskaterequipment.game_id        (position 0..3)
bio.game_id == exhibitiongoalieai.game_id == exhibitiongoalieequipment.game_id        (position 4)
exhibitionrostertable.team == exhibitionteams.index  (222 teams: 32 NHL, AHL, junior, national, all-star, ...)
exhibitionfreeagents.playerindex  -> exhibitionplayers.index
exhibitiondraftpick.owner/origteamid -> exhibitionteams.index (-1 = none); playerindex -1 on all 1260 stock rows
playernames (4116 rows) is a name bank for audio ids; bio holds first/last name as plain strings and does not reference it
```
`exhibitionplayers.table` is 0 for normal players, 1/2/3 for special ranges (index 16000.., 16200.., 16300..); table 3 rows
(46) have no bio row and are skipped. Position codes 0 C, 1 LW, 2 RW, 3 D, 4 G match the roster line slots (l1c, l1lw, l1ld, g1) **[real]**; handedness 0 = L, 1 = R is a **[guess]**.
Meta lists `maxinsert` for several tables (e.g. bio 1500, players 3000, roster 5000): the engine may allow inserts **[guess]**,
unlike the review's "rows are never inserted".

## Stock-file results [real]
- nhlng.db: 0 errors, 157 warnings. Warnings: 6 tables (swiss/swedish/german/finnish/czech schedule, stockplayerhelmetplacement) have a
  field reaching the last record bit, so they cannot have a free flag there (the "last bit = free" claim is not general; we report it
  as a warning). 49 bio.team / 9 bio.jersey differences vs the roster on NHL teams, 20 free agents also on a roster, plus the draft-file diffs.
- No deleted rows in any key table, no roster team above 40: 222 teams with players, size 20..35, 2..5 goalies each.
- 370 players sit on rosters of more than one team (all-star 30/31, national and other teams), so "player on one roster" is not a rule.
- draft_roster.db: exactly 10 tables, 0 layout problems, same field layout as nhlng.db, but contents differ in all shared tables
  (e.g. roster 5701 vs 5532 rows, bio 5643 vs 5632, free agents 192 vs 340): not "in step" in the stock install, so a rule that
  they must be equal would be wrong; what the game needs from it is unknown.

## Not done / open
- No inserting or reviving rows, no deleting, no team-field widening, no cascade (changing a position does not move the AI row).
- Whether the game accepts every check-clean edit is only known for names (proven earlier); ratings/jersey/team edits not yet tried in game.
