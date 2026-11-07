# 32 teams in Play Now: no code hook needed (2026-11-07)

Goal: Seattle and Vegas (team ids 31 and 30 of the widened DB, `tools/rosters/build_32.py`) selectable in Play Now.

## Result [measured in game, dev build, scratch cache root]
- **The SELECT TEAMS list is data driven.** With the `sea-vgk` DB + its widened meta XML (docs/rosters-32-teams.md) the NHL list has **32
  entries** without any hook: the table the front end asks for holds the 30 stock clubs in alphabetical city order and then Seattle (31) and Vegas (30).
  The earlier "still 30 teams" reading was wrong: the two new teams sit at the very end of the Down cycle (after Winnipeg), and the first
  screenshots never got that far. Traced with hooks on the front end's native calls: the UI native that returns the id list returned 32 ids, the
  per-team attribute native was then called for all 32 ids, and a run with 31 Down presses visited ids ... 29, 1, 31, 30 (one attribute query per press).
- The old hook on the league-0 loader (`sub_82A1E910`, "append ids 30/31") was **removed**: that function is not in the Play Now path at all (it runs later,
  with no out list, while a match loads). `src/team_hooks.cpp` is now an empty placeholder; cvar `nhl_teams32` / env `REX_NHL_TEAMS32` are gone.
- **Names need loc keys by abbreviation, not just by number.** With only the number keys changed (`NHLTeamName_30`, `NHLCityName_30`, ...) the select
  screen and the lineup bar showed no nickname (blank). The game looks up `NHLTeamName_<abbr>`, `NHLCityName_<abbr>`, `TEAMLINE1/2_<abbr>`,
  `NHLTeamName_Abbr3_<abbr>`, `NICKLINE2_<abbr>`, `TXT_NICKNAME_<abbr>`, `TXT_NICKNAME_ALT_<abbr>`, `X_XLA_TEAM_<abbr>`, `CITYLINE1_<abbr>` with the
  abbreviation from the DB (`abbrname`: VGK, SEA). Recipe (test file only, never committed): start from the installed `nhl_eng_us.db`, `locdb.py set`
  the number keys 30/31 (and the old all-star abbreviation keys EAS/WES, harmless) and `locdb.py add` the 2 x 10 abbreviation keys; result 25,802 keys,
  1,762,464 bytes, `locdb.py check` clean; served as `cache/guest/fe/loc/nhl_eng_us.db`. The game accepted the added keys (the `add` risk in
  docs/loc-db.md is now tested: max rows +20 is fine).
- Vegas (id 30) played a match (away vs San Jose): lineup screen "Vegas VS San Jose", bar "Golden Knights", Vegas skaters (Eichel, Stone, Barbashev,
  Theodore, Andersson, Howden), scoreboard "VGK 0 - 0 SJ", 77 s of in-match rendering, no crash. Seattle (id 31): see the worklog entry.
- **Still placeholder: art.** Vegas uses artid 23 (shows the Penguins logo and jersey), Seattle artid 28 (Vancouver logo). The game has no Seattle or Vegas
  art; real logos/jerseys need new DDS assets (separate job).
- Not covered (unchanged): divisions/conferences for 30/31 (franchise/standings), 32-team schedules, container flag byte for id 31 in franchise paths.
  Play Now ignores them.

## How the list is built (no addresses in git; the address notes are in the private work dir `p-teamhook2/findings.md`)
1. The front end's id-list native -> the data object's list builder -> the league object's team table (a 16-byte object: count, array of 200-byte team
   records) loaded from the DB (`exhibitionteams`, active teams) and sorted by city; count 32 with the widened DB.
2. The UI then fetches attributes per id (attribute native, one call per team at screen load and one per Down press); texts come from the loc DB.
3. Because the count and the ids come from the DB rows, no `< 30` literal sits between the DB and the list for the Play Now NHL league.

## Test recipe
`NHL_VPAD_SCRIPT=14:a:150,16:a:150,22:down:300,...` (integer seconds, one Down per second; Select Teams is up at ~18-20 s; Down x30 = Seattle,
x31 = Vegas), the sea-vgk DB + meta + the patched loc DB in a scratch `REX_CACHE_ROOT`; then `a` every 3 s to the lineup screen and `start`.
Presses are processed one per second; the UI answers ~0.25 s before the pad log line prints the press.
