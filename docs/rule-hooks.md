# Rule hooks (src/rules_hooks.cpp)

Opt-in guest hooks that override game rules. Everything is off by default (`0`). Tags: [read] disassembly, [meas] seen in a run, [guess].

## Switches (cvar, env `REX_NHL_<NAME>`)
- `nhl_skaters_per_side` 0 = stock, 3 = 3v3 for the whole game, 4 = 4v4. Example: `REX_NHL_SKATERS_PER_SIDE=3`.
- `nhl_period_length` 0 = stock, N = the game's `periodlength` rule value. Stock scenarios use 5/7/8/10/12/15/20.
  A period is always 20 virtual minutes (sub_83475D50); the value only scales the clock (sub_83480C10 returns a rate from
  rules+72, sub_8337FF90 = (20-len)*x/20), so a period of N minutes runs the 20:00 clock at 20/N. The value is written into the
  runtime rules struct (pointer word 0x83C1D218, +72), re-applied on every period start and line-set call.

## Addresses (image layout VA, confirmed by disassembly; do not use p22_common's reader for .text)
- Skater counts: int[2] at 0x83C876C4 (home) / 0x83C876C8 (away). Getters sub_83381AE8(side), sub_83381B18(team obj); setters
  sub_83381B00(side,val), sub_83381B38(team obj,val) [read]. All writers (game init sub_8331E470, resets sub_83311BA8/sub_833163D0,
  AddSkater sub_8330BEA0, RemoveSkater sub_83311140) go through the setters, so the hook clamps value > N to N in the setters and the
  getters. RemoveSkater keeps its floor of 3 and queues the overflow penalty (byte +2171), so at 3v3 a penalty is queued, never served.
- Line sets: sub_833C25B8(side, reqLine, int* lineOut, int* setOut). Stock: 0-3 even strength, 4-5 pp5, 6-7 pp4, 8-9 pk4, 10-11 pk3,
  12-14 ot4. A team at 3 against 3 gets no set (returns 0, set -1). Hook: own count 3 -> pk3 set 10+min(line,1), count 4 vs 4 -> ot4 set 12+min(line,2), returns 1.
  pk3lines hold 3 skaters (1 forward + 2 defence) in two lines [meas, scenario JSON]; ot3lines (3 lines, same shape) is never selected by the game.
- Not hooked (stock behaviour kept): sub_8331E470 did NOT fire in the quick route (the hook on it never logged), counts are set via the clamp instead.

## Results (quick route, dev install, 2026-11-07)
- `SKATERS_PER_SIDE=3 PERIOD_LENGTH=5`: game reaches the faceoff and plays 60 s without crash or "Title terminated" before exit; wide shots at 60-64 s show
  3 skaters + goalie for SJ (3 visible) and 2 visible + goalie for ANA (third off screen), not 5; hooks fire (SetSkaters, line set, period start).
  Scoreboard clock: 20:00 at 56 s, 19:13 at 64 s, 19:01 at 70 s, 18:46 at 80 s, 18:08 at 100 s (play in between stops the clock).
- Clock calibration (period 20 / stock default / 10) and 4v4: see the results table at the end of this file when filled in.

## Risks / not done
- Penalties at 3v3: queued, so no power play and the penalized player may not serve a visible penalty [guess]; real 3on3 rules (4v3 after a penalty) would need AddSkater for the other side.
- AI formations, PP/PK strategy and the MyTeamHas3/4SkatersOnIce conditions assume stock manpower; with 3v3 the AI uses what the pk3 set gives [not tested over a full period].
- Goalie pull (sub_833DCFD8), faceoff slots (5 slots in the stock circles) and line fatigue with only 2 lines: not checked beyond the opening faceoff.
- Overtime: sub_8331B508 reduces 5v5 -> 4v4 only if min > 3, so it is a no-op at 3v3.
- periodlength N is stored as an int; very small N makes penalties and the period clock fly: untested below 5.

## Open
- Calibration runs (period 20 / stock / 10, and 4v4) were queued but starved on bench.lock; the period-length effect is therefore unverified. The log gives no
  "periodlength a -> b" line in the 5 run, so the quick route's stock value is probably already 5 (the line only logs a change). Re-run with
  `REX_NHL_PERIOD_LENGTH=20` and compare the clock with `5`.
