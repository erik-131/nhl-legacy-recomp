# Rosters pipeline, stage 1: the 2026-27 NHL clubs in the 30 stock slots (roadmap 2d, P13)

Tools (all in `tools/rosters/`): `nhl_fetch.py` (API to a cache), `build_roster.py` (stock `nhlng.db` + cache to a new resealed
`nhlng.db`), on top of `tdb.py` / `rosters.py` ([rosters-tool.md](rosters-tool.md)). Tests: `python3 tools/rosters/test_build_roster.py`
(10 tests, synthetic DB and synthetic fake API cache only; no real data anywhere in the repo).
Tags: **[real]** done on the real stock file (2026-11-07), **[synthetic]** unit tests only, **[not tried]** not verified in game.

## Rules
Nothing fetched and nothing generated goes into git: the repo is public. Cache, generated DBs and CSVs live under
`<workdir>/rosters-out/` (`nhl-cache/`, `nhlng_2026.db`, `export2026/`). The tools ship; data does not.

## Steps
```
python3 tools/rosters/nhl_fetch.py [--cache DIR] [--refresh] [--max-age HOURS] [--teams ANA,UTA]
python3 tools/rosters/build_roster.py STOCK_nhlng.db OUT.db [--cache DIR] [--meta nhlng-meta.xml] [--surplus delete|keep] [--teams ANA,..]
python3 tools/rosters/rosters.py check OUT.db --meta nhlng-meta.xml
```
Then serve OUT.db as `cache/guest/db/nhlng.db` from a scratch cache root (the route proven for names, roadmap 2b) **[not tried for this file]**.
The input DB is opened read-only in memory, the output is written to a different path; same inputs give a byte-identical output
(`cmp` of two runs on the real file) **[real]**.

## Data source and terms
- Source: the public NHL web API `https://api-web.nhle.com/v1/`: `roster/<T>/current` (redirects to the season, e.g. 20262027),
  `club-stats/<T>/now` (current season), `club-stats/<T>/<previous season>/2` (previous regular season). 32 clubs x 3 requests = 96.
  Nothing is documented by the league; the endpoints are unofficial and can change.
- Politeness: files cached on disk, never fetched twice unless `--refresh`/`--max-age`, at most 2 requests per second (default 0.6 s
  apart, never below 0.5 s), a User-Agent that names the tool. `manifest.json` in the cache records URL, final URL, UTC time and size per file.
- Terms: **uncertain.** I found no published terms for `api-web.nhle.com` itself (a web search returned only third-party wrappers that say it is
  undocumented, without key or documented rate limit, and refer to "NHL's API terms of service" without quoting them). The NHL.com Terms of
  Service (`nhl.com/info/terms-of-service`, read through an automated page summary, not first-hand verbatim, so check it yourself) do not
  mention APIs or robots.txt, but prohibit "unauthorized spidering, scraping, or harvesting", "unauthorized automated means to compile information",
  and load that is "unreasonable or disproportionately large", and allow use of the Services only for "non-commercial, informational, personal use,
  without modification", and no copying/entering into a database without permission. Whether a handful of cached JSON requests for a private,
  non-commercial local mod falls under this is **not determined**. Hence: the tool is for the user to run on their own machine, small and
  cached; the repo contains no fetched data, no rosters, no ratings; players' names and numbers are facts, but we do not publish them.

## Mapping: 2026-27 clubs onto the 30 stock slots
The stock slots are the 2014-15 league (team index = row in `exhibitionteams`, roster key = team*40 + slot).

| stock slot (abbr) | API club | | stock slot | API club |
|---|---|---|---|---|
| 0 ANA 1 WPG 2 BOS 3 BUF 4 CGY 5 CAR | same | | 15 MTL 16 NSH 18 NYI 19 NYR 20 OTT 21 PHI | same |
| 6 CHI 7 COL 8 CBJ 9 DAL 10 DET 11 EDM 12 FLA | same | | 23 PIT 24 STL 27 TOR 28 VAN 29 WSH | same |
| 13 LA | LAK | | 17 NJ | NJD |
| 25 SJ | SJS | | 26 TB | TBL |
| **22 ARZ** | **UTA (Utah)** | | 14 MIN | MIN |

- Arizona's slot 22 takes Utah. Only the roster is Utah's: the team row (name "Arizona Coyotes", abbreviation ARZ, logo, jersey, arena art) is untouched, a later
  data/art step (2d/2e).
- Seattle (SEA) and Vegas (VGK) have no stock slot (slots 30/31 are the all-star teams RED/BLU). They are fetched into the cache but **not built**; that is stage 2e.
- Teams outside slots 0..29 (AHL, juniors, national, all-star, ...) are not touched. Their rosters share players with the NHL teams (see Limits).

## What build_roster.py does per team
1. **Donors.** All live roster rows of the team are the donor rows; each points at a player (roster -> players.index -> playerid -> bio game_id -> skater or goalie AI).
   Rows are reused; nothing is inserted. A donor also on another NHL team's roster would be reused twice, so it is skipped (treated as surplus).
2. **Slots.** A donor row has a role from the stock data: bio position (C, LW, RW, D, G), for D left/right from its `lNld`/`lNrd` line flag (else the shooting side),
   a line rank (the N of `l1..l4`, 9 = no line) and a count of special-team flags (pp, pk, ot, shootout). Goalies: the `g1` row is the starter's.
3. **Assignment.** Skaters sorted by quality fill slots in order (line rank, then more special-team flags, then roster key): the slot's position first, else any
   forward / any defenseman; for defenseman slots a player with the matching shooting side if his quality is within 0.10 of the best. Goalies sorted by games
   started (current + previous season) fill g1 first. The line flags of a donor row are not touched, so the lines are the stock line structure filled by the best
   fitting players: best C on l1c, wingers on l1lw/l1rw, best pair on l1ld/l1rd, and so on; special teams follow the donor's flags.
4. **Too many players** (API roster larger than the donor rows, 3 to 6 per team): the lowest-quality skaters and the 3rd/4th goalie are dropped (80 players in total).
   **Too few** (only COL 3, WPG 1, OTT 1, UTA 1: 6 rows in total): `--surplus delete` (default) marks the donor roster row deleted with the engine's free flag
   (the player stays in the player tables and on any other roster, like the 208 bio players on no roster in the stock file); `--surplus keep` leaves the old player as a
   scratch (all line flags and `rosterstatus` cleared). Deleted roster rows are **[not tried]** in game; if they misbehave use `keep`.
5. **Bio:** first/last name, birthplace (city only, as in the stock file), birth year-1900 / month-1 / day-1, height (inches-54), weight (lb-123), position,
   handedness (0 L, 1 R; goalies catch side), jersey (bio and roster row). The offsets were checked against known players (e.g. year 85 = 1985, height 22 = 76 in) **[real]**.
   Wiped: `hasportrait`, `artid`, `audioid` = 0 (states that exist in the stock file), `headid` = 60000 (the stock's most common head, 318 players), draft record
   (`draftyear` 255, round/position 0, team -1), `rookie`, games-played counters (`nhlgamesplayedlastseason` and career = games in the cache). Kept from the donor: skin/eye
   colour, contract, `intlcountry`, `state`, growth fields, equipment. AI: all `traitN` = 0, ratings below.
6. **Captain:** all `captain` cleared, then the three best skaters by quality get C, A, A (placeholder; the API roster has no captaincy).
7. **Names:** the stock file stores UTF-8 and already contains letters like Ä Å é ö ø ß. Characters the stock file uses are kept; others are ASCII-folded
   (NFKD, a small table for ø đ ł ß æ ...). Limits: names 24 bytes (25-byte field with terminator), birthplace 39, cut at a character boundary; none of
   today's 747 players needed cutting **[real]**. Whether the game's font draws every kept character is only known for what the stock data already has.

## Ratings (stage 1: from stats only, not copied from any game)
Everything is a pure function of the cached stats and position. Stored 6-bit values (shown = stored, the game adds 36 per the review; stock NHL-team skaters sit at
p0 / p10 / median / p90 / max = 32 / 40 / 46 / 51 / 60 for overall and about 36..58 for single skills **[real]**). `base(x) = round(36 + 22*x)`, x in 0..1, skater range 36..58.
- Stats pooled per player over the current and previous regular season (all cached clubs). Per-game values are shrunk towards the position-group mean with 20 games of prior,
  then turned into percentile ranks (0..1) within the group (forwards, defensemen) over everyone in the cache:
  `off` = points per game, `toi` = average time on ice, `pm` = plus/minus per game, `shots` = shots per game, `pim` = penalty minutes per game.
- Quality `q` = forwards 0.5 off + 0.3 toi + 0.2 pm; defensemen 0.3 off + 0.5 toi + 0.2 pm. Defence `df` = 0.6 toi + 0.4 pm. With few games `q` is pulled towards 0.30
  (weight gp/(gp+20)); no games at all = 0.30. This only orders the lines and sets the ratings; it is not a talent estimate.
- Skills: shot power = `base(0.5 off + 0.5 shots)`; accuracy, deking, puck control, passing, offensive awareness, hand-eye = `base(off)`; poise = `base(q)`; defensive awareness,
  stick checking, shot blocking = `base(df)`; body checking = `base(0.5 df + 0.5 weight rank)`; speed, acceleration, agility, balance, endurance = `base(0.25 + 0.5 q)`
  (compressed: skating is not in the stats); strength = `base(weight rank)`; durability = 0.5; discipline = `base(1 - pim)`, aggression = `base(pim)`,
  fighting skill = 24..50 by `pim`, `c_fighting` 2/1/0 for pim above 0.97 / 0.85; faceoffs for centres `30 + 28 (fo% - 0.40)/0.20`, others 20. `c_overall` = mean of nine key skills
  (the game recomputes it; per the review it never reads it). `c_potential`, growth fields: donor's.
- Goalies: save percentage pooled over both seasons, shrunk with 500 shots of league-average prior; `q = clamp(0.5 + (sv - league)/0.040)` (plus/minus 2 points of save percentage is
  the full range), weighted towards 0.30 by gp/(gp+5). Save skills (glove/stick high/low, five-hole, positioning, rebound control, shot recovery, vision, breakaway, poise)
  = `40 + 16 q`; speed, agility, endurance, puck play, poke check, passing, aggression, durability = stock medians. Starter = most games started.

## Verified on the real file [real]
- `rosters.py check` on the output: **0 errors**, 122 warnings (stock: 157; same 6 layout warnings, 20 free agents on a roster, jersey/team differences). New: `bio.jersey differs from roster jersey`
  47 (stock 9) because a donor who also sits on an all-star/national roster keeps that row's old jersey. 1 deleted row listed per kind of check as INFO; 222 teams, size 20..35, 2..5 goalies each.
- `rosters.py diff`: goalie AI 61 rows changed, bio 667, skater AI 606, roster 658 changed + 6 removed (deleted), nothing else.
- API players 747 on the 30 clubs: **667 written** (reused donors), **80 dropped** (excess), **6 donor rows surplus**. Per team (donors / API / written / dropped / surplus):
  ANA 24/27/24/3/0, WPG 25/25/24/1/1, BOS 21/23/21/2/0, BUF 24/26/24/2/0, CGY 25/26/25/1/0, CAR 20/24/20/4/0, CHI 24/24/24/0/0, COL 25/22/22/0/3, CBJ 23/23/23/0/0, DAL 20/24/20/4/0,
  DET 23/26/23/3/0, EDM 23/29/23/6/0, FLA 21/25/21/4/0, LA(K) 22/25/22/3/0, MIN 23/27/23/4/0, MTL 21/25/21/4/0, NSH 21/24/21/3/0, NJ(D) 20/22/20/2/0, NYI 21/25/21/4/0, NYR 23/26/23/3/0,
  OTT 24/24/23/1/1, PHI 23/24/23/1/0, ARZ(UTA) 24/24/23/1/1, PIT 22/25/22/3/0, STL 24/26/24/2/0, SJ(S) 21/24/21/3/0, TB(L) 23/24/23/1/0, TOR 23/28/23/5/0, VAN 20/25/20/5/0, WSH 20/25/20/5/0.
- Input `nhlng.db` hash unchanged by a run; two runs give identical output.

## Not verified
- Not loaded in the game: line cards, depth chart, deleted roster rows, `headid` 60000 look, blank portraits and audio, ratings feel, Play Now / Team Rosters screens, whether the game recalculates anything.
- Ratings are plausible by construction, not tuned; no comparison with how the game plays.

## Limits and open issues
- **Seattle and Vegas**: no slot (stage 2e: slots 30/31, all-stars moved, team-field widening, art, licence question first).
- **Utah**: roster only; name/logo/jerseys/arena of slot 22 still Arizona.
- **Shared players**: a rewritten donor stays on every other roster that listed him (all-star RED/BLU, national teams, others): they now show the new name there, and bio vs roster jersey differ (47 warnings).
- **Draft class / prospects / AHL**: only the NHL rosters are replaced. AHL, junior and draft pools still hold the 2014 players; the draft class and `exhibitiondraftpick` are untouched.
- **Retirement of old players**: replaced donors are overwritten, not retired; the old players are simply gone from those rows. No free-agent pool for 2026 (the 340 stock free agents are 2014 players
  that were overwritten if they were donors, else unchanged).
- **Contracts, ages' growth, `intlcountry`, `state`, skin/eye colour** are the donor's values (nationality and appearance can be wrong).
- **Faces and portraits**: generic head, no portrait, no audio id (announcer will not say the names). Needs art/audio work (later).
- **Roster size**: a club with more players than donor rows loses its lowest-ranked players (3..6 per team); injured/IR players are what the API's roster omits or includes without a flag.
- **Name limit**: 24 bytes (ours 25 minus the terminator; their tool 24, check); accented letters not in the stock file become ASCII.
- **ROSTER saves**: a saved ROSTER file (and franchise saves) overrides the DB; none in the maintainer's saves today. A newly served DB only shows in modes that read the DB.
- **`draft_roster.db`** is not in step: it holds a copy of 10 tables and is not touched; what the game needs from it is unknown.
- **Stats season**: the current season has few games early in the year; the previous season is pooled to compensate; rookies and new signings sit at the 0.30 default.
- **API instability and terms** as above.
