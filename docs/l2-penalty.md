# L2 module 2: skaters on ice and the penalty queue as readable C++

Level L2 of docs/source-replication-study.md, same pattern as docs/l2-t3db.md. Labels: **[read]** = from the disassembly, **[measured]** = observed in the unit test, **[guess]** = not confirmed.
Files: `src/l2/penalty.h` (layout constants + API), `src/l2/penalty.cpp` (pure functions over an `Env`), `src/l2/penalty_hooks.cpp` (guest hooks), `tests/l2_penalty/test_penalty.cpp` (synthetic data only), CMake: next to `t3db_hooks.cpp` in both source lists. No EA names or maps are in git; function names below are our own descriptive labels.

## Layout [read]
All big-endian, one global game-data block at guest VA `0x83C87508`:
| field | address | meaning |
|---|---|---|
| ot_mode | block+432 (0x83C876B8) | int; value 2 makes 4v4 reduce to 3v3 in OvertimeReduce [guess on the name] |
| skater_count[2] | block+444 (0x83C876C4) | int per side (0 home, 1 away); accessors 0x83381AE8 get / 0x83381B00 set (setter = plain store at block+(side+111)*4) |
| penalty block | block+504 (0x83C87700) | 2 sides x 20 entries x 48 bytes, entry (s,i) at base+(s*20+i)*48. +149 u8 active (!=0 = holds a penalty), +151 u8 serve ticket (0 = not queued) |
| queue counter[side] | penalty base+2171+side | u8, incremented per queued penalty, wraps at 256 |
| entry detail (re-read from the create path 0x83322870) | penalty base+148 | the 48-byte entry of a roster slot is really 4 records of 12 bytes starting at base+148+(s*20+i)*48: +0 u8 arg, +1 u8 type (the "active" byte, +149 above), +2 u8 in-use (+150, used by PickBestAvailableSlot), +3 u8 serve ticket (+151), +4 u32 duration, +8 u32 timestamp. All our checks so far look at record 0 only [read] |
| list A | base+8, 8-byte records, s8 count at base+2168 | rec: s8 side, s8 slot, u8 flag at +3 (!= 0 blocks the slot in PickBestAvailableSlot) [read; role guess]. Fits in the 140 bytes before the entries (max ~10) |
| list B | base+88, 6-byte records, s8 count at base+2169 | rec: s8 side, s8 slot, s8 type (10/11 special), s8 state (0 / 32 / other), u16 second slot (255 = none). Read by SyncPenaltyPlayerStates [read] |
| player objects | table of 40 u32 at 0x83C1D2F8, index side*20+slot; gate word 0x83C1E89C (0 = object system off) | obj+40 s32, +428 s32 state (sync writes 2 or 3), +556 u8 roster index (>=128 none), +1352 s32 score [read] |
| side struct | `*(0x83C1D590) + side*38520 + 38268`; +36 = 8 x s8 slot list | slots the chooser skips (guess: players on ice / locked) [read] |
| roster records | 0x83C86230 + idx*160, +148 s32 | >= 0 adds 50 to the chooser score [read] |
Rules chain for RuleGate: `*(0x83C1D218)` -> rules struct, +36 = int rule value; `*(0x83BFE99C)`+24 -> object, +16 = int variant.

## Functions
| guest | readable | semantics | confidence |
|---|---|---|---|
| 0x8330BEA0 | `AddSkater(side)` | count < 5 -> count+1 | high [read, 20 insns] |
| 0x83311140 | `RemoveSkater(side)` | count > 3 -> count-1. Else (at the floor of 3): find first entry of that side with ticket==0 && active!=0, ++counter, entry ticket = counter (low byte); then count = 3. So at 3 skaters a penalty is queued instead of served. Quirk kept: a counter wrap to 0 gives ticket 0, which still reads as "not queued" | high [read] |
| 0x8331B508 | `OvertimeReduce()` | lo/hi = min/max of the two counts: if lo > 3 && hi >= 5, or lo == 4 && hi == 4 && ot_mode == 2: RemoveSkater(0) and RemoveSkater(1) | high [read]; role (OT 5v5 -> 4v4, 4v4 -> 3v3) is from the call context [guess] |
| 0x833159B0 | `RuleGate()` | rule==3 true; rule==2: variant <4 true, 4..5 false, 6 true, 7..10 false, >10 true; else rule==7. Used by 3 call sites; meaning (a penalty-rule predicate) not analysed [guess] | logic high, purpose low |
| 0x8330AB40 | `SlotIndex(side, slot)` | side in {0,1} and s8(slot) in 0..19 -> side*20+slot, else -1. Pure | high [read, 20 insns] |
| 0x83310718 | `PickBestAvailableSlot(side, exclude)` | Read-only chooser, returns a roster slot 0..17 or -1. Skips slots whose record-0 in-use byte (+150) is set, slots named by a list-A record with flag != 0, `exclude`, and slots in the side struct's 8-byte list. Score = player object +1352 (100 if no object / gate 0), +50 if the roster record +148 >= 0; picks the strictly lowest score below 100, first wins ties. Called from the create path 0x83322870 (substitute when r27 == 0) and 0x8331A968. Name and purpose (pick the replacement skater who serves/substitutes) are [guess] | logic high [read, ~190 insns; the callees 0x8330AB40, 0x834F5768 (= u32 at +1352), 0x834F20C0 (= table lookup) are trivial and inlined], purpose low |
| 0x833104D0 | `SyncPenaltyPlayerStates()` | For each list-B record (count = s8 at base+2169): obj = player of (side, second slot if u16 != 255 else slot). state 32 -> obj state = 3; state 0 -> nothing; else obj state = 2 unless it is 3. Then with a second slot, the primary player (side*20+slot) gets state 2 unless its +40 == 1 or state is 3. The callees 0x833FA618/0x833FA630 only do `if (obj) obj+428 = 3 / 2`. The original also builds a 4-entry de-dup list on the stack that is never read (dead), omitted. Called once from the driver 0x83324440 [read]. Meaning of states 2/3 (guess: 3 = serving/in box, 2 = pending) is [guess] | logic high [read, ~190 insns], purpose low |
Note: the old assumption that these two were the "entry create/serve paths" was wrong. The create path is 0x83322870 (see skater-management notes below), which is large (~450 insns) and calls timestamp / team-state / message functions, so it is NOT modelled; 0x83310718 and 0x833104D0 are helpers it and the driver call.
Not covered (too big or call into other systems): the driver 0x83324440, 0x83322870, 0x8331A968, 0x8331A180, 0x8331AB60, 0x8331B1B0 (400-500 insns each), 0x8331A8A8 / 0x83311900 (only build a message and call a virtual logger), 0x833109C8 (larger, not read in this pass).

## Verification
`nhl_l2_penalty` (env `REX_NHL_L2_PENALTY`): 0 off (default, pure passthrough), 1 verify, 2 replace.
- Verify (PickBestAvailableSlot, SyncPenaltyPlayerStates added): Pick runs the readable code on real memory (it only reads) and compares the return value r3; Sync buffers its +428 state-word writes (as 4 byte writes) and compares them. Totals line now also has `pick=.. sync=..` and six mismatch counters. For each call the readable code runs first against a shadow overlay (reads real memory, buffers its writes, nothing is written), then the original runs, then every buffered write (count words, ticket bytes, counter bytes) is compared with the real memory; RuleGate compares r3. A difference logs `[nhl-l2] MISMATCH <fn> ...` (first 25 per function), and once a second `[nhl-l2] penalty mode=1 calls: add=.. remove=.. otreduce=.. gate=.. | mismatches a/b/c/d`. Limitation: writes the original makes that the readable version does NOT make are not detected (only ours are checked). Run verify with `nhl_skaters_per_side=0` (the shadow reads the raw count array, the rule hook clamps in the getters).
- Replace: all six use the readable code. Counts go through the guest accessors 0x83381AE8/0x83381B00 (so `nhl_skaters_per_side` still works); other memory is touched directly.
- Unit test (synthetic): `clang++-20 -std=c++20 -I src tests/l2_penalty/test_penalty.cpp src/l2/penalty.cpp -o /tmp/penalty_test && /tmp/penalty_test`. Covers (added: SlotIndex exhaustive over side -3..4 x all 256 slot bytes; PickBestAvailableSlot directed cases + 30000 randomized worlds against an independently written filter-then-min spec; SyncPenaltyPlayerStates full cross product of gate x side x slot x state x type x second slot x object states x field40, plus multi-record/count cases) the cap at 5, decrement above the floor, queueing with empty/already-queued entries, wrap of the counter, per-side blocks, OvertimeReduce exhaustively for counts 0..6 x ot_mode 0..3, RuleGate for every rule/variant. [measured: all pass]
- Compile: hook and module files syntax-checked with the project's compile command (clang++-20, `-fsyntax-only`, t3db_hooks.cpp command with the file swapped) [measured: clean]. A full link/deploy was NOT done (new files only picked up at the next normal build).

## Run recipe (needs a game run, bench.lock)
1. Build + deploy to nhl-linux-dev under build.lock: `tools/nuc/ct105/build.sh --deploy` with `DEST_DIR=nhl-linux-dev`.
2. Verify: `REX_NHL_L2_PENALTY=1` on the quick route, play with penalties on (set the penalty frequency high in the scenario/rules if available) for at least two periods and an overtime; look in the log for `[nhl-l2] penalty` lines. Pass = calls > 0 for add/remove (also try `REX_NHL_SKATERS_PER_SIDE=0`) and zero mismatches. otreduce/gate calls show up only in OT and at rule checks; if remove stays 0, no penalty happened.
3. Replace: `REX_NHL_L2_PENALTY=2`, same scene; compare the skater counts on screen, power-play behaviour and the penalty clock with a mode 0 run. Then 3v3 check: `REX_NHL_SKATERS_PER_SIDE=3 REX_NHL_L2_PENALTY=2` must behave as in docs/rule-hooks.md.
4. Fill the results here: calls and mismatches per function.

## Results
- Unit test passes (incl. the two new functions); compile check clean (hook file and penalty.cpp, `-fsyntax-only`, flags of the t3db_hooks.cpp ninja entry). In-game verify/replace: NOT DONE (bench busy; in-game verify not done for PickBestAvailableSlot and SyncPenaltyPlayerStates either).

## Notes for a "custom number of skaters / line change" module (no code; read-only analysis, partly skimmed)
Who touches the on-ice count (callers found by grepping the lifted code; function-level only, bodies not fully read):
- `AddSkater` (0x8330BEA0): 0x83322870 (penalty create, opponent gets a skater in the 3v3-OT format: gated by `ot_mode == 3` && RuleGate && other side's count < 5), 0x8331A180 and 0x8331AB60 (large, serve/expiry paths; AddSkater after a penalty ends).
- `RemoveSkater` (0x83311140): 0x83322870 (penalty create: type 1 and no override flag -> RemoveSkater(side)), 0x8331A968 (variant for an already-penalized player, also calls PickBestAvailableSlot), and OvertimeReduce.
- `OvertimeReduce` (0x8331B508): 0x8331EDB8 and 0x8331F540, both also call RuleGate (0x833159B0), so these are the OT start / OT period setup paths.
- `PickBestAvailableSlot`: 0x83322870 and 0x8331A968. `SyncPenaltyPlayerStates`: once from the driver 0x83324440, which also calls 0x83322870 eight times (one per penalty class / call site), 0x8331A180 once and reads the counts via 0x83381AE8 five times.
- Count readers (0x83381AE8) are everywhere (about 15 functions), writers (0x83381B00) only 4 call sites in 3 functions (RemoveSkater, AddSkater, and 0x83311BA8 with two sites, probably a reset/init), so the raw count array is effectively written only by Add/Remove plus that one function.
What a custom-skaters module would have to cover:
1. The cap/floor constants are inside Add/Remove (5 and 3). A module that wants 2..6 skaters must replace those two functions (the L2 versions already isolate the constants: `kMaxSkaters`, `kMinSkaters`) and OvertimeReduce (hard-codes 5 -> 4 -> 3 and ot_mode 2/3 logic). The existing `nhl_skaters_per_side` clamps in the getter 0x83381AE8 instead; it does not change Add/Remove behaviour, so power plays still move the count only inside 3..5.
2. At the floor Remove does not remove, it queues a ticket (+151 and counter +2171). With a floor below 3 or a different team size every consumer of the tickets (serve/expiry paths 0x8331A180/0x8331AB60/0x8331A968) must agree; those were not read.
3. The count is a number only; which players are on ice is in the side struct (8-byte list at +36 of the side struct, PickBestAvailableSlot reads it) and in per-player state words +428 (SyncPenaltyPlayerStates). A line-change module needs to write those, not just the count; the lines/formation itself lives elsewhere and was not found in this pass.
4. Hook points, in order of leverage: the count accessors (0x83381AE8/0x83381B00, already hooked for the rule override), Add/Remove/OvertimeReduce (L2 versions available in replace mode), and the penalty create path 0x83322870 (decides Remove vs Add and the 3v3-OT special case via RuleGate). Verify any change in OT and with two simultaneous penalties.
