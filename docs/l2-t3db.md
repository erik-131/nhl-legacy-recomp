# L2 module 1: the T3db table reader as readable C++

Level L2 of docs/source-replication-study.md: readable C++ per subsystem, compiled and swapped in for the recompiled functions, verified against the original. This is the first module. Labels: **[read]** = taken from the disassembly/lifted code, **[measured]** = observed in the game or the unit test, **[guess]** = not confirmed.

Files: `src/l2/t3db.h`, `src/l2/t3db.cpp` (pure functions over a `Memory` interface), `src/l2/t3db_hooks.cpp` (the guest hooks), `tests/l2_t3db/test_t3db.cpp` (unit test with a synthetic in-memory DB), CMake: next to `quick_start.cpp` in both source lists. Base study of the DB layer: p-rosterhooks findings (T3db = "EA::T3db", 0x827A0000-0x827C2000), file format: docs/rosters-tool.md and tools/rosters/tdb.py.

## Structures (all big-endian guest data) [read]
Declared as documented structs with `static_assert(offsetof)` in `t3db.h`; the code reads through `Memory` with `offsetof`.

| struct | size | fields |
|---|---|---|
| `Db` | ? | +28 `tables` (ptr to TableDesc[]), +32 `table_count`. Reached from a cursor as cursor+4 -> session, session+4 -> Db |
| `TableDesc` | 68 | +8 `hash` (sort key, unsigned ascending), +12 `hash2` (linear-search key), +36 `fields` (ptr to FieldDesc[]), +40 `field_count` (byte) |
| `FieldDesc` | 64 | +8 `hash`, +20 `bit_offset` (u16), +22 `depth` (u8, bits), +24 `low` (s32 range low) |
| `Column` | 16 | what ExtractBitField is given, at runtime-table-object +84 + idx*16: +0 `type`, +4 `bit_offset`, +12 `depth` |

Shown value = stored value + `low` (ints, dates, times; not floats). Rows are bit-packed MSB-first.

## Functions
| guest | module | what it does |
|---|---|---|
| `db name hash` 0x827B6708 | `HashName` | reflected CRC32 (poly EDB88320, init 0, no final xor); every byte masked with 0xDF (`rlwinm 27,25`), so letters are upper-cased but e.g. '1' becomes 0x11. The game's table at 0x82304320 is the standard CRC table (the unit test matches the five known table hashes, e.g. EXHIBITIONTEAMS = 0x7228E37B) |
| `db table lookup` 0x827B6FA8 (0x827B5D40 = wrapper with r5=1) | `FindTable` | sorted=1: binary search over TableDesc[count] on +8; otherwise linear on +12. Own variant of the search (last-step compare against the other bound) reproduced exactly |
| `db field lookup` 0x827B6EA0 | `FindField` | count <= 16: linear scan; then (and for count > 16 directly) element 0 compared alone, binary search over 1..count-1. Difference: the original's linear loop always runs 16 iterations and so reads past a short array; the readable version stops at `count` |
| `db bit-field extractor` 0x827A3288 | `ExtractBitField` | numeric columns: 64-bit BE load at rowbase + ((bitoff>>3)&~3), shift left by bitoff&31, shift right by 64-depth (arithmetic for type 2, logical otherwise), store the low 32 bits. Types 0/1 (byte copy) and 13..15 (special, 0x827A3118) stay with the original |
| `db value-conversion stage` 0x827B3058 | `ConvertInt/Date/Time` | only the conversion stage after the row value is fetched: type 1 int = raw+low, 2 float = raw bits (low NOT added), 4 date = day number -> y/m/d (integer Julian algorithm, constants 146097, 1461001/4000, 2447/80, offset 2367729), 5 time = seconds -> h/m/s. Type 3 (string) allocates and is not covered. The row lookup in the middle (0x8279F3D0 / 0x8279ECD8, cursor modes, deleted-row bitmaps) is NOT readable C++ yet |

## Verification
`nhl_l2_t3db` (env `REX_NHL_L2_T3DB`): 0 off (passthrough, default), 1 verify, 2 replace.
- Verify: the original runs, the readable version runs on the same arguments, results are compared (full 64-bit r3 for the lookups, the stored 32-bit value for ExtractBitField, the out buffer bytes for GetFieldValue conversions; the raw value for the latter is captured from the ExtractBitField hook, so the conversion is checked on live data). A difference logs `[nhl-l2] MISMATCH ...` with the arguments (first 25 per function) and every second a line with calls and mismatches per function.
- Replace: HashName, FindTable, FindField, ExtractBitField (numeric columns) use the readable code only. GetFieldValue is never replaced (middle not readable).
- Unit test: `clang++-20 -std=c++20 -I src tests/l2_t3db/test_t3db.cpp src/l2/t3db.cpp -o /tmp/t3db_test && /tmp/t3db_test`. Synthetic DBs (0..134 tables, 0..200 fields) vs naive search, ExtractBitField vs bit-by-bit reference for 20000 random fields (signed and unsigned), the date conversion vs an independent civil-calendar algorithm over 1950..2150 incl. leap rules. [measured: all pass]

## Results
- Unit test: all pass [measured].
- Build: compiles and links into nhl-linux-dev (sha256 be03a0ac...) [measured]; default mode 0 is a pure passthrough.
- In-game VERIFY (mode 1) and REPLACE (mode 2) runs: NOT DONE YET [measured: the bench.lock was held by other jobs for over 1.5 h; the queued run `<workdir>/p-l2/run.sh l2v 1` writes its result line to `<workdir>/p-l2/l2v.out` and the log to `p-l2/l2v.log` when it gets the lock]. Fill in calls/mismatches here once it has run; then `run.sh l2r 2` for replace mode and compare the line-card screenshots.

## Next L2 modules
- Row/cursor layer: 0x8279ECD8 (row lookup, deleted bitmap), 0x8279F3D0 (record reader) and the query objects (FieldRef ctor 0x827B10C8, TableRef 0x827B4818, Execute 0x827B4B20, RowCount 0x827B42E8). Finishing it makes GetFieldValue replaceable and gives a complete typed `Table.get(row, field)`.
- Roster loaders on top of it: player bios 0x82A0A488, team-table loader 0x82AF85A8, the container ctor 0x82A13970 (sizes players = max(rows) x 556 B, teams x 200 B). Those are where team-count/year limits live (see roster notes), so this is where full source control pays off first.
