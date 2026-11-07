# 32 teams in Play Now: database side (roadmap 2e)

Tags: **[read]** read from code or docs, **[measured]** measured on the stock `nhlng.db` / `draft_roster.db` (2026-11-07) or on a widened copy under `<workdir>/p-widen/`, **[guess]** not confirmed in the game.
Nothing here was run in the game. The tool is `tools/rosters/widen_teams.py` (tests: `test_widen_teams.py`, 5 tests, synthetic DB).

## 1. Fields that hold a team id [measured]
Team table layout in `exhibitionteams`: rows 0..29 NHL, **30 and 31 = all-star "Red/Blue" (EAS/WES)**, 32..61 AHL, then junior, national, club, 220/221 EASHL 1/2, 222..251 Custom Team 00..29 (table=1). Index equals row.
Encodings found: (A) plain index 0..N, no offset; (B) team+1 with schema low -1 ("none" = -1); (C) 8-bit with low -1 or 0 (wide).
Stored max = largest stored value in live rows; shown = stored + low. The 5-bit "B" fields hold -1..30, so **team 30 already fits, only team 31 (stored 32) does not**. (Correction to the review's wording: the stock data uses only teams 0..29 + none, but the field is a full 32-value field.)

| table.field | bits | low | high | max shown in data | verdict for 32 teams |
|---|---|---|---|---|---|
| exhibitionplayerbiotable.proteam | 5 | -1 | 30 | 29 (31 distinct incl. -1) | **widen** (B) |
| exhibitionplayerbiotable.draftteam | 5 | -1 | 30 | 29 (31 distinct) | **widen** (B) |
| exhibitiondraftpick.owner / origteamid | 5 | -1 | 30 | 29 (30 distinct; 1260 rows, playerindex -1) | **widen** (B) |
| exhibitionplayerbiotable.team | 8 | -1 | 254 | 221 | ok (C) |
| exhibitionplayerbiotable.juniorteam | 8 | -1 | 254 | 213 | ok |
| exhibitionplayerbiotable.juniorleague | 4 | -1 | 14 | 11 | league id, not a team |
| exhibitionrostertable.team | 8 | 0 | 255 | 221 | ok |
| exhibitionteams.rivalteam / rivalteam2 | 8 | -1 | 254 | 208 / -1 | ok |
| exhibitionteams.ahlaffiliate | 6 | -1 | 62 | 61 (AHL rows 32..61) | ok, full for team index 61 |
| stockteamjerseys.teamid / customteamjerseys.teamid | 8 | 0 | 255 | 221 / 251 | ok |
| stockteamequipmenttable.team, stockgoalieequipment.team, customteamequipmenttable.team | 8 | -1 | 254 | 254 / 221 / 251 | ok |
| stockteamcolorstable.team | 7 | 0 | 127 | 61 | ok |
| stockawardstable.team, stockretirednumbertable.team | 8 | 0 | 255 | 211 / 207 | ok |
| stockseasonteamstats.team / stockplayoffteamstats.team | 6 | 0 | 63 | 29 (30 rows) / 61 (32 rows) | ok (franchise defaults, 30 rows) |
| nhlschedule / nhlfutureschedule / nhlscheduledebug / favoriteteamschedule .home .away | 5 | 0 | 31 | 31 | ok (A, no none value); 1231 rows = 30 teams x 82 / 2 + the all-star game (30 v 31) |
| ahl/ohl/qmjhl/whl schedules .home .away | 5 | 0 | 31 | 29 / 19 / 17 / 21 | ok |
| czech/finnish/german/swedish/swiss schedule .home .away | 4 | 0 | 15 | 13 / 14 / 13 / 13 / 11 | ok, separate leagues |
| buildaistrategies.template_team_id | 8 | -1 | 254 | -1 | ok (30 rows, franchise AI) |
| tweetshashtags.owner_id | 13 | 0 | 8191 | 1 | ok |
| **league\* tables (all 0 rows, max rows 0; built at runtime from these headers and the meta's maxinsert, e.g. leagueteams 128)** | | | | | |
| leagueplayerbiotable.proteam / draftteam / tradeblockteam | 5 | -1 | 30 | empty | **widen** |
| leaguedraftpick.owner / origteamid | 5 | -1 | 30 | empty | **widen** |
| leaguefreeagentdata.signedteamid / originalteamid | 5 | -1 | 30 | empty | **widen** |
| leaguecontractoffers.teamnotification | 5 | -1 | 30 | empty | **widen** |
| leaguegamemodesvars.team, leaguescoutvisits.team, leaguerookiedata.draftteam | 5 | -1 | 30 | empty | **widen** |
| leaguecontractoffers.teamid, leaguerfatable.teamid/origteamid, leaguestaffdata.team, leaguetradeblock.teamid, leaguetradeoffers.teamreceive/teampropose, leagueplayerbuyouttable.teamid | 5 | 0 | 31 | empty | ok (A, 0..31) |
| leagueplayerbiotable.team, leagueteams.rivalteam(2), leagueretirementtable.team, leaguetransactiontable.team/team2, leaguecustomteam*.team(id) | 8 | -1 or 0 | 254/255 | empty | ok |
| leagueteams.ahlaffiliate | 6 | -1 | 62 | empty | ok |
| leaguerostertable.team, leaguescheduletable/leaguetempscheduletable .home/.away, leagueteamseasonstats.team_id, leagueteamplayoffsstats.team_id, leagueplayer*stats.team, leagueawardstable.team, leaguevillaintable | 7-8 | | | empty | ok |
| **leaguefreeagentdata.playerinterestinteam0..29** | 7 | 0 | 127 | empty | **30 columns, one per team**: teams 30/31 have none. Franchise-only, cannot be widened (it is a field count, not a width) **[guess]** |

(The survey matched names by pattern `team|owner|home|away|winner|rival|junior|ahl|affil|club|conf`; there are no winner/loser fields. Team-ish names with other meaning: `team_bias1..6`, `teammodmin/max` are strategy/answer values, 5 bits signed, not team ids.)
Tool rule: widen if the name is in {proteam, draftteam, owner, origteamid, teamnotification, signedteamid, originalteamid, tradeblockteam, team}, low is -1, width below 6 and the range is full. That picks exactly the 15 fields marked **widen** in nhlng.db and 4 in `draft_roster.db` (bio proteam/draftteam, draftpick owner/origteamid).

## 2. Record sizes, max rows, file size [measured]
Widening one bit moves later fields up and grows record bits by 1; the free flag stays the last bit. Byte count changes only if the record crosses a multiple of 8 bits.

| table | record bits before -> after (both fields) | record bytes | max rows | extra bytes |
|---|---|---|---|---|
| exhibitionplayerbiotable (proteam, draftteam) | 1055 -> 1057 | 132 -> **133** | 5632 | +5632 |
| exhibitiondraftpick (owner, origteamid) | 63 -> 65 | 8 -> **9** | 1260 | +1260 |
| leaguedraftpick (owner, origteamid) | 63 -> 65 | 8 -> 9 | 0 | 0 (but the runtime table is built 9 bytes wide) |
| leagueplayerbiotable (3 fields) | 1407 -> 1410 | 176 -> 177 | 0 | 0 |
| leaguefreeagentdata (2) | 575 -> 577 | 72 -> 73 | 0 | 0 |
| leaguecontractoffers, leaguegamemodesvars, leaguerookiedata, leaguescoutvisits | +1 | unchanged (16, 8, 8, 8) | 0 | 0 |

- Two tables cross a byte boundary with data rows (bio, draftpick); `Table.widen` handles it: every row (all max rows) is rewritten with the new size, offsets, file size and CRCs are recomputed. Tested on the stock file: values 2,143,729 of 2,143,729 equal, `Db.check` shows only the 6 problems the stock file already has.
- File size 2,901,504 -> 2,908,396 (+6,892 bytes, the sum above). Max rows unchanged everywhere. No table's space is exceeded: `widen` takes the rows from `max rows` and keeps any spare bytes behind them (draft_roster.db bio has max rows 7988, rows 5643, space intact). draft_roster.db: 1,898,964 -> 1,908,212.
- Open: the game may keep its own copy of a table layout (the review says the newest ROSTER save overrides the DB, and a wrong `max` gives "damaged"). A save written with the old 5-bit layout may therefore not load with the widened DB **[guess]**; test with a fresh save first.

## 3. Experiment [measured]
`widen_teams.py nhlng.db OUT --meta nhlng-meta.xml --meta-out OUT.xml` (3.8 s) on a copy in `<workdir>/p-widen/`:
- `tdb.py verify`: rewrite identical, `tdb.py check`: the 6 stock problems only (foreign schedules and `stockplayerhelmetplacement.type` reach the free-flag bit); `rosters.py check` (with the widened `draft_roster.db` beside it): 0 errors, 157 warnings, the same count as stock.
- Every value of every field of all 134 tables compared via the API: 0 differences. `rosters.py export` of the stock file and the widened file: identical, all CSVs.
- Meta: 15 fields changed to `depth="6" rangehigh="62"` (diff shows 15 changed lines; BOM and CRLF kept). The widened file read with the new meta: `proteam` range -1..62. With the old meta the game would reject it **[guess]**, so DB and meta must ship together.
- `set proteam 30` / `31` / `-1` (and draftteam) read back as 30 / 31 / -1, neighbours untouched; the stock file refuses 31 ("outside -1..30").

## 4. What else must change for 32 teams in Play Now
Rows are in `exhibitionteams` (252 rows, max 252). Verify each step with `rosters.py check` (roster/bio/AI/equipment links, team in exhibitionteams, no player twice on a team, <= 40 per team), `tdb.py check`, `tdb.py verify`, `rosters.py diff` against stock (only intended rows changed) and finally the game.

1. **All-star slots move out of 30/31 [guess on where].** Today team 30/31 = Red/Blue (rows 30, 31; roster keys 1200.., 24 and 27 players; rivalteam 31/30; schedule has the 30 v 31 all-star game). Seattle/Vegas take rows 30/31, so the all-star data (exhibitionteams rows, `exhibitionteamstrategies`, roster rows 30*40+slot, `stockteamjerseys/equipment/colors/goalieequipment` rows with team 30/31: 6+6 jerseys, 54+54 equipment, 3+3 colors, 4+5 goalie sets) must go to new indices. Candidates: 222/223 (Custom Team 00/01, `table`=1, but `customteamjerseys/equipment` use teamid 222..251 and the custom-team editor probably addresses them), or two new rows 252/253 (needs `exhibitionteams` max rows 252 -> 254 and the same in `exhibitionteamstrategies`, `tdb.py maxrows` only works if the table space allows; probably it does not, so a table rebuild is needed). 220/221 EASHL stay. Every `rivalteam` of the moved teams (30<->31) and the bio `team` of the all-star players (-1..221 8-bit, ok) must follow. Whether the game hardcodes 30/31 for the all-star mode is unknown **[guess]**; a cheaper first test is to not move them and just overwrite 30/31 (lose all-star).
2. **exhibitionteams rows 30/31 for Seattle and Vegas**: `shortname`, `fullname`, `abbrname` (3 letters), `artabbr`, colors (`primarycolor_*`, `secondarycolor_*`), `audioid` (all-star has 1378/1323: use a real city/anthem id), `audioid2`, `font`, `helmetbrand`, `gloves`, `goaliemask*_starter/backup`, `coachheadid`, `spotlight`, `tunnel`, `jerseystyle/sockstyle/pantsstyle`, `itechvisorcolor`; `league`=0, `leaguegroup`=0, `state`=0, `active`=1, `table`=0; **`divisiongroup`/`conferencegroup`** (stock: Pacific = 3? Vancouver 28 has div 3, conf 1; Seattle -> div 3 conf 1; Vegas -> div 3 conf 1, it cannot fit as the current 30-team realignment unless a division is changed; Play Now ignores standings **[guess]**); `rivalteam` (Seattle <-> Vegas or an existing rival), `ahlaffiliate` (-1 or an unused AHL row 32..61: Coachella Valley / Henderson are not in the data **[guess]**).
3. **Arena and art ids**: `arenaid` is a row of `exhibitionarena` (187 rows; ids 1, 25, 30, 59, 60, 101, 181, 186 are shared by several teams, so sharing is normal, e.g. Blue uses arena 30, Albany 31). `artid` equals the row index for NHL teams (0..31), 15 bits. Art 30/31 is the all-star art: no Seattle/Vegas files exist **[read: review says art can be cloned or pointed at a neutral arena]**. Cheapest: set `arenaid` to an existing arena (e.g. Vancouver 28) and `artid` to an existing team's art (logo wrong), then add art later. Jerseys: `stockteamjerseys.teamartid` (30/31 currently reuse art 30/31, 6 rows each; AHL teams use 141 etc.), `jerseyindex` 0..12, `variant/style/locstring`, plus equipment rows `stockteamequipmenttable` (team 30/31, 54 rows each) and `stockgoalieequipment`, `stockteamcolorstable` (3 rows) must exist for the NHL team or the game has no uniform/colors: simplest is to repoint the existing 30/31 rows (they exist) and change only colors/art, which also keeps row counts and `max` unchanged.
4. **Roster**: `exhibitionrostertable` rows `team*40+slot` for 30/31 (Seattle/Vegas players: set their `proteam` 30/31 in `exhibitionplayerbiotable`, `draftteam` as wanted, and `team` 30/31); every team needs >= 2 goalies (the sim hangs at `rand() % 0` on an empty roster); `exhibitionplayers`, `skaterai/goalieai`, equipment rows must exist per player (rosters.py check covers it). New players cannot be inserted (rows are reused), so the 50 or so expansion players are donor rows from the all-star rosters. `exhibitiondraftpick` owner/origteamid for 30/31 (optional in Play Now). `exhibitionfreeagents` unaffected.
5. **league\* header tables**: widening (above) is all that data-only Play Now needs; the franchise tables are built from these headers at runtime. 32-team franchise also needs `playerinterestinteam30/31` columns, 32-team schedule tables (`nhlschedule` 1231 rows is for 30 teams), `stockseasonteamstats`/`stockteamsalaryextras`/`buildaistrategies`/`invitationsquestionstexts` (30 rows each): out of scope, tied to the ~500 hook sites in the review.
6. **draft_roster.db**: run the same tool on it with `nhlng-meta.xml` so the layouts match (rosters.py check compares layouts and reports differences as an error).
7. **Not covered by the DB**: loc strings for new names (the `loc DB`), logos/jerseys DDS, stadium files, audio banks.

## 5. In-game experiment: SEA/VGK overwriting the all-star slots (2026-11-07) [measured in game unless noted]
Tool: `tools/rosters/build_32.py` (widens, rewrites teams 30/31, rosters from the NHL API cache; VGK = 30, SEA = 31). Experiment files
live outside the repo in `<workdir>/p-32/` (`progress.md` has the log, `run.sh` the runner). The all-star teams are **lost** in that DB.
- Widened stock DB + widened meta served from a scratch `REX_CACHE_ROOT`: boots and plays like stock (run w1, ANA v SJ, no errors).
- `rosters.py check` on the SEA/VGK DB: 0 errors; `tdb.py check`: only the 6 stock problems. Donor rosters: the EASHL 1/2 teams (220/221, 22 players
  each, full lines, nobody shared with an NHL team); their roster rows are copied into the all-star rows (keys 1200.. / 1240..), the other all-star rows are deleted.
- **Play Now team select (virtual pad):** Play Now (A at ~16 s) shows SELECT TEAMS at ~18 s; Left/Right moves the controller cursor between away and home, **Up/Down changes the team on that side** (alphabetical by city, wraps), X random team, Y random league (NHL, AHL, OHL, QMJHL, WHL, SHL, DEL, National ... all work), RB custom AI (a popup that eats input). LB/RB do not change league (LT/RT do; the vpad has no triggers).
- **Result: slots 30/31 are NOT in the NHL list**, with full donor data, `league`=0, `active`=1, `table`=0, and with or without an AHL affiliate. The list shows exactly the 30 stock clubs. So the filter is not in the row data we changed (probably a hard-coded 30 / index < 30, see the code notes: team-table loader loops, byte[31] in the container); 32 teams need a code hook, a DB-only change cannot make them selectable.
- Team names in select and lineups come from the loc DB (stock names for the stock slots); the loc DB (`fe/loc/nhl_eng_us.db`) is a compressed string DB that tdb.py cannot read (127-bit records, strings in a pool), so changing display names is a separate decoder job.

### Update 2026-11-07 (supersedes the two paragraphs above): the list DOES contain slots 30/31
Re-traced in game (docs/team-hooks.md): with the widened DB the SELECT TEAMS list has 32 entries, Seattle (31) and Vegas (30) at the end of the Down
cycle, after Winnipeg; the earlier "exactly 30" conclusion came from screenshots that never reached them. No hook is needed. What was missing was the
display names (loc keys by abbreviation VGK/SEA, see team-hooks.md). Art is still placeholder (Penguins / Vancouver logos).
