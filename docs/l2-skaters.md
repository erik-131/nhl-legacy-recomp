# Skaters on ice and line changes: hook map for a custom-skaters module (notes, no code yet)

Source: names pass 10 (static reading, nothing run). Guest addresses and our own descriptive labels only. B = game-data block at 0x83C87508, F = flow struct at 0x83C1D220. Labels: [read] / [guess].

## Counts and tests
- Skater counts per side at B+444 / B+448 (see docs/l2-penalty.md). AddSkater 0x8330BEA0 caps at 5; RemoveSkater floor 3 (both hard-coded) [read].
- 0x83384C48 side has fewer skaters than the opponent (shorthanded test), 0x83384CD0 more (power-play test), 0x83384D08 count == 3 [read]. 0x83381B38 sets the count for an object's side [read].
- Consequence: a mode with other team sizes needs the two limits (5, 3) changed in AddSkater/RemoveSkater/OvertimeReduce, and every test above works on counts, so they follow automatically.

## On-ice slot tables [read, role of the 6-per-side split is a guess]
B+216..+260: 12 object pointers; B+264..+308: 12 values initialised to -2. Writer 0x833FF5C0, reader 0x833FF578, membership test 0x833FF738, setup loop 0x833C2170. Likeliest place to hook for a different number of skaters or custom line changes. Player-object table: pass 10 reads it at 0x83C1D278 (40 entries); docs/l2-penalty.md says 0x83C1D2F8, to be reconciled.

## Line changes [read]
0x8331E210 sets flow state 8, then 0x83315EE8 runs the off-play line changes through the LineChange event for both sides (fire function 0x833DCAE8). 0x834DCA18 does the same for Be-A-Pro. Line indices at +40/+41 of the line object (0x8337C620/0x8337C640 set them and reset +44/+48). 0x83393688 sets starting lineups. 0x833B79D8 dispatches shift start/end.

## Goalie
0x83400010 goalie-swap check -> 0x8331E3F8 (side = player index / 20, fires the swap event, enters flow state 10). Pull-goalie request in F+40/+44/+48 (set 0x8330D690, cleared 0x8330D6B0).

## Flow states (table 0x83AA5B20, 14 states)
4 goal announce, 5 post-whistle (update 0x83326AC8, exit 0x8331EDB8 = OvertimeReduce), 8 replay-like, 10 period-end hub (update 0x83328930); 6 goes to 7, 11 goes to 1. SetState 0x8330C890. Details in docs/l2-gameflow.md.

## Penalty call wrappers
0x83488270 / 0x834882E8 / 0x83488360 wrap 0x83485BD0 with type codes 7, 27, 23, gated on rules+4. Lookup by player and type 0x83310230; penalty struct reset 0x833118B0 (2180 bytes); put a player into penalty state 0x834EC1E0.

## Next L2 step
Hook-point ranking: (1) the 5/3 limits in the three skater functions (already hooked by nhl_l2_penalty replace mode: make them cvar-driven), (2) slot tables at B+216..+308 and 0x833C2170, (3) line-change fire path. Needs an in-game verify of the penalty module first.
