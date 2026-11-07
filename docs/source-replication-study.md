# Study: from recompilation towards replicated source code (P22)

2026-11-06. Written in the cloud container: only the git repo, no game files, no generated code, nothing run. Every
statement is tagged **read** (from a doc or code in this repo, not verified by running), **measured** (a number from
`nuc-perf-notes.md`/the development log, not re-measured here), **estimate** (my reasoning, no data) or **not known**.

## 1. Checking the two claims

### "The CPU part is decompiled today": half right, and the word matters

- **What it is: static recompilation, not decompilation.** The PowerPC machine code of `default.xex` is translated
  function by function into C++ that works on a register file (`PPCContext`) and a flat guest-memory pointer, with the
  original instruction kept as a comment above each statement. It is behaviourally the game but it is not source
  (**read**: `docs/nhl12-decomp-reference/README.md` §1, which describes the sibling NHL 12 project, same toolchain and
  same engine family).
- **No names, no types, no structure.** All functions are `sub_<address>`; the retail build is stripped. For NHL 12
  that is 103,714 functions (**read**). The Legacy count is **not known** from this repo (`generated/` is not in git, and
  must not be); `nhllegacy_functions.toml` lists 327 manually declared functions the scanner missed (**measured** by counting lines).
- **It is not pure translation.** The recompiler needed hand-found metadata: setjmp/longjmp addresses, jump tables,
  functions the scanner missed (NHL 12 needed 505 jump tables and 860 gap functions, **read**; Legacy's manifest has the
  same kind of fixes, e.g. the setjmp body the recompiler dropped, **read** in `nhllegacy_manifest.toml`).
- **"No emulation" is true for the CPU only.** There is no interpreter or JIT for game code. But the rest of the Xbox 360
  is still emulated by the SDK: kernel (threads, events, file system, virtual memory), XAM (profile, input), XMA audio,
  the content/save layer (**read**: `rexglue-runtime.md`: 317 imports, 200 xboxkrnl + 117 xam, dispositions stub /
  SDK / override). The project also patches the game at run time (VP6 movie decode moved to host ffmpeg, boot
  condition-variable fix, quick start).

Verdict: the CPU side is **recompiled**, which is why it runs at full speed (~20 ms CPU per frame, **measured**, and
GPU-bound at ~100 fps), but it is **not decompiled**: nobody can read it as C++.

### "The GPU still has some emulations": true, but "some" understates it

The whole GPU path is emulation of the Xenos chip, in the Xenia-derived SDK (**read**: `rendering-pipeline.md`,
`current-status.md`):

| Layer | What emulates it | Why it matters |
|---|---|---|
| Command stream | The game's own D3D9 runtime (statically linked XDK, itself recompiled) writes PM4 packets; the SDK's command processor decodes them | Draws are written **inline** into the command buffer by game code, so they cannot be hooked at API level (**read**: `highcut-hybrid-kickoff.md`, `P7`) |
| Shaders | Xenos microcode → SPIR-V translator at run time, pipelines compiled on first sight | First-sight freezes (roadmap 1n); 552 stored pipelines ≈ 24–27 s cold (**read**, in remaster-review) |
| Render targets | 10 MB EDRAM modelled, `rtv` (host render targets) with transfers; `fsi` is the faithful but slow path | Resolve/EDRAM dump costs ≤ 10 % overall, ~19 % in shadow scenes (**measured** 11-06) |
| Textures | Untile, format conversion, endian swap | Part of the "tex" cost, 1.5 ms per frame in close-ups (**measured**) |
| Present | Swap interpreted from `VdSwap` | Works |

What is **not** emulated away: nothing in the game's render code itself. `rw::` (RenderWare), `nhlrender` and the XDK D3D9
library all run as recompiled PPC and still believe they talk to a 360.

The one attempt to remove the emulated layer, a native "high-cut" renderer that reimplements the D3D9 API on the host, reached
~3 fps and was dropped for the SDK's Vulkan path at 66–84 fps at the time, ~100 now (**read**, `vulkan-rov-backend-spike-findings.md`;
the cause of the 3 fps is not stated in the docs I read: **not known**, it may have been immaturity rather than a
principled limit).

## 2. What "replicating the source code" can mean

Four levels, each a separate project with a different payoff.

| Level | Meaning | What it gives | Feasible? |
|---|---|---|---|
| L0 | Today: recompiled `sub_XXXX` | Runs natively | done |
| L1 | **Annotated recomp**: names, struct layouts, module map, comments, in a side database that codegen can use (rename map, typed fields) | Readable stack traces, safe hooks (replacing 275 year-sites by hand is the roster pain today), quicker bug work | Yes, incremental, no end state |
| L2 | **Readable C++ per subsystem** (decompiler output cleaned up: rosters/GM, camera, rules, UI flow), compiled and swapped in for the recompiled functions, one module at a time | Real mods (3v3, 32 teams in all modes, cameras) without hook archaeology | Yes for chosen modules, hard for tight float/vector code |
| L3 | **Whole-game functionally equivalent source** that builds without the PPC lift | Full native port, own renderer, any platform | In principle; not a realistic goal here |
| L4 | **Byte-matching decomp** (rebuilds the exact xex) | Decomp-project purity | Needs the exact XDK compiler (Feb 2011 PPC cl.exe) which is not freely available; not needed for anything above |

### Evidence that L1/L2 have a head start (read)
- Retail assert strings carry Perforce source paths: 86 distinct `.cpp` paths survived as `__FILE__`, plus RTTI/namespace
  strings (`rw::core`, `eastl::hash_map`, etc.). Gives module names and file boundaries (`codebase-map.md`).
- 12,098 runtime tunables dumped with names (`tunable_values_runtime.txt`, `tunables_schema.*`): a symbol source for the AI,
  camera and physics constants.
- The database schema is fully known (134 tables, meta XML) which fixes the data side of franchise/GM code.
- 863 lines of subsystem notes under `docs/nhl12-decomp-reference/` (gameplay, animation, rendering, UI), written for NHL 12.
  **Caveat:** NHL 12 and NHL Legacy are different executables (different addresses; the roster-mod project says Legacy is
  a port of its NHL 13 work). How much transfers is **not known**: needs a function-matching pass.
- Large parts of the binary are not EA game code at all: XDK, EASTL, Speex, Lynx, BlazeSDK, RenderWare base. Those can be
  identified by signature against known libraries and need no reverse engineering (estimate: the game proper is a minority of the
  functions; no count exists yet).

### Evidence that L3 is a different class of effort
- ~100k functions (NHL 12 figure) in a 27.3 MB image (**read**; the image includes data). Even if 70 % is library, tens of thousands of game
  functions remain (estimate).
- Hand-recovered decomp projects of this size (N64/PS2-era titles) took teams years; AI-assisted decompilation changes the rate,
  but nobody has shown full-game results at this size with correct float and vector (VMX128) behaviour. VMX128 and the
  game's tight floating-point code are where machine-translated C++ is hardest to turn into readable, equal-behaviour source
  (estimate).
- The GPU stays emulated at L3 unless the renderer is also rewritten: the D3D9 layer is XDK code writing Xenos packets.
  Replicating game source does **not** by itself remove the GPU emulation. Removing it needs L3 for `nhlrender`/RenderWare **plus**
  a host renderer, which is the high-cut project again.

## 3. Time estimates (all **estimate**; person = the maintainer + AI sessions, an evening ≈ 3 h)

Basis: the roster tool (tdb.py, one format, synthetic tests) took about a day of session time for read/write plus a second
for widening; hook-based work against the xex has been "days" per feature in the roadmap. Nothing below was measured.

| Work | Effort | Output / exit test |
|---|---|---|
| A. Library identification (EASTL, XDK, Speex, Lynx, Blaze, RW base) by signature against public builds | 1–2 weeks | Share of functions that are library; shrinks the problem. Needs the dumped image on the NUC |
| B. Name the game's functions from asserts, RTTI, string xrefs, tunables, DB table tags | 2–4 weeks for the first few thousand, long tail afterwards | `names.toml` feeding codegen, so crash logs and traces read well |
| C. Port NHL 12 notes to Legacy: match functions across the two images (instruction-pattern hashes) | 1–2 weeks | % of NHL 12 names that carry over; decides how much of B is free |
| D. Struct recovery for the roster/GM/franchise stack | 3–6 weeks | Opens roadmap 2e/2f (32 teams in every mode) without ~500 manual hook sites |
| E. First L2 module compiled and swapped in (suggest: camera or rules, small and testable) | 2–4 weeks after B/D | Same behaviour in A/B replay of inputs |
| F. L2 for sim and AI (skating, puck, goalie, checking) | months | Needs determinism check against the recomp |
| G. L3, whole game | 1–3 years (estimate, wide error) | Not recommended as a goal in itself |
| H. Native renderer replacing Xenos emulation | open-ended; prior attempt 3 fps | Only after L2 on nhlrender/RW; GPU-bound win bounded: resolve+EDRAM ≈ 10–19 %, draw cost is vertices (**measured**) and does not disappear with a different API |

## 4. Where the gain actually is (relative to the maintainer's priorities)

1. fps in replays and close-ups: **no source level helps directly.** Slow scenes are GPU-bound on vertex count
   (1–2.4 M/frame) and the frame script (`default.scrape`, a data file) is the lead (**measured/read**). A native renderer
   would still draw the same vertices. Source work does not move this.
2. Shader freezes: caused by the Xenos→SPIR-V compile at first sight. Source replication does not remove them; a stored
   pipeline cache or the SDK wait fix does (roadmap 1n).
3. Rosters/teams to 2026: this is where L1/L2 pays. The roster plan today needs ~500 hook sites for 32 teams in season/GM
   (**read**, remaster-review), exact-xex-bound. Struct recovery (item D) shrinks that to typed edits.
4. Camera, 3v3, rules mods: L2 on small modules (item E).

Honest summary: replicating the **whole** source buys modding freedom and portability, not speed, and the speed
problem is on the GPU side that source replication barely touches.

## 5. Constraints

- **Repo is public.** Decompiled or lifted EA code is a derivative of copyrighted code; like `generated/` it must never be
  committed. Only tools, name/offset maps without code, and our own hand-written replacement modules (under our own licence,
  written from behaviour, not pasted decompiler output) can live in git. Name maps listing addresses and symbols are a grey zone
  (they describe EA's binary): suggest keeping them in the private NUC tree and committing only the tooling.
- All work needs the game image, so it runs on the NUC, not in this container.
- `megarohas/nhl-legacy-remaster` has overlapping reverse engineering (same xex). No licence: ask before using anything.

## 6. Recommendation

1. Do **not** set "replicate the whole source" as the goal. State the goal as "L1 now, L2 where a feature needs it".
2. Start with items A and C on the NUC (cheap, produce numbers that settle the estimates above): how many functions Legacy has, what share is
   library, how much of the NHL 12 knowledge matches. About one week; the result replaces my guesses in section 3.
3. Then B and D driven by roadmap 2 (rosters): every function named is one that a feature needs.
4. Keep the GPU priorities (1l, 1n) separate; they do not depend on any of this.

## 7. Open questions for the maintainer

- Is the goal modding freedom (L1/L2), a different platform, or a faster renderer? The answer changes the order above.
- Can the NUC session run the A/C measurements (function count, library share, NHL 12 match rate)? They need `default.xex` and
  the NHL 12 image, which this container does not have.

## 9. Measured on the NUC (2026-11-06)

Read-only, cores 4–5, nice and idle I/O, no game running. Tools in `tools/study/p22_*.py` (no game data; the raw dumps and
per-function labels stay on the NUC, not in git).

**Size (measured).** `PPCFuncMappings` has 129,934 entries (129,386 `sub_*`, ~550 others), in 728 generated files and
13.7 M lines (379 MiB). The xex image is PE, big endian, base `0x82000000`: `.text` 22.3 MB (the only code section, 66 % of
the 32.1 MB file), `.rdata` 3.9 MB, `.data` 4.7 MB. 69 % of functions are under 128 bytes; the 2.1 % that are 1 KiB or
larger hold 27 % of the code bytes.

**Library share (estimate, heuristic; corrected 2026-11-07).** Method: 15k of 75k strings classified by library or path
keywords, tied to functions through 408k `lis`+`addi` references (67.5 % of functions have one), spread by `.rdata` adjacency,
address interpolation and callers, plus the D3D9 translation unit as an anchor (it has no strings). Percent of functions / of
code bytes. *Before correction* (the image reader mapped `.text`/`.data` through PE raw pointers, but the decompressed image
is a memory-layout image, so every xref was shifted by 0x5200): Blaze 15.1/14.6, audio 5.1/8.6, EASTL 1.6/2.7, RW 0.9/2.0, XDK 0.7/1.0,
library 23.7/29.5, game 44.6/45.6, unlabelled 31.7/24.9, hold-out 95 % family.

| Family | funcs | bytes |
|---|---|---|
| Blaze/OSDK/online | 16.7 | 15.3 |
| EA audio framework (incl. Speex decoder) | 6.1 | 9.7 |
| EASTL | 2.0 | 3.3 |
| RW base | 1.1 | 2.4 |
| XDK | 0.4 | 0.6 |
| Lynx, ANT, EA base, Speex proper, CRT | ~0.5 | ~0.8 |
| **Library labelled** | **26.9** | **32.0** |
| Game labelled | 48.7 | 50.4 |
| Unlabelled | 24.5 | 17.6 |

A 20 % hold-out gave 95.8 % exact family and 97.9 % library-vs-game accuracy (interpolation covered 88.6 %; before: 95.0 / 97.3 /
86.4), a best case that doesn't test the unlabelled quarter. Library is a lower bound
(EASTL templates, CRT, non-D3D XDK are undercounted): plausible range 27-45 % of functions, 32-50 % of bytes. Game proper is
probably the majority. The split of game code into packages (gameplay, frontend, render) can't be given: only 66 source-path
strings exist.

**NHL 12 match rate: not measurable**, no NHL 12 image or generated tree on this machine. `p22_fnmatch.py` is ready. Self-test
on Legacy alone (corrected reader): of 93k functions with 8+ instructions, 50.0 % have a unique exact masked-pattern hash (48.8 % fuzzy),
the ceiling for 1:1 matching; before correction it read 82 % (the shifted code bytes were wrong).

**Game modes (strings and cross-references only).** Modes go through the frontend league stack (a game-modes class with a league
creator, a tuner set, mode logic and mode data). Rules found by name include offside, icing variants, overtime options,
period length (5/10/15/20), penalty levels, 3-on-3 and 4-on-4 power plays. The tunables list has 55 offside, 41 icing, 58
period, 34 shootout and 41 on-ice entries, none for mode select, none that sets skaters on ice. Many rule names have no
direct code reference: they sit in pointer tables or resource keys, so the rules look data-driven. With the corrected reader the
rule-key strings (`icing`, `shootout`) are referenced from the GameRules descriptor `sub_8316AAB8` (before: from a wrong function
at 0x8316F958); `PeriodLength`, `Offside`, `Icing` (capitalised) still have no direct reference. Corrected category counts (strings / referencing functions): mode_select 45/32, league_logic 637/358, rules_settings 1768/1160, onice_roster 225/218. **Unknown:** which code reads period length, offside/icing enables or skater
count; runtime behaviour.

## 10. Progress and direction (2026-11-07, P22 taken over by the NUC session)

Goal as set by the maintainer: modding freedom including fully custom game modes. So: L1 (names, side database) everywhere it helps, L2 (readable C++ with a verify mode against the original) per module a feature needs; L3 (whole source) is not a goal.

**Numbers (names DB, counts only).** 129,934 functions; 70.5 % have a name from any evidence, 23.3 % of functions (43.2 % of code bytes) at confidence >= 1, 0.6 % at >= 2. The curated passes (6 so far, about 250 functions each, ranked by distinct callers) lift the headline to 74.6 % named and 29 % at confidence >= 1. L2: two modules written (a DB reader and skaters/penalty queue, 4 functions), verify mode pending in-game.

**Codegen and names (investigated, nothing invasive done).** The codegen config accepts `[functions] 0xADDR = { name = "..." }`, which renames the C symbol (breaks hooks that use `sub_XXXX`, makes ~90k entries config authority, and would put the map in git): rejected. Chosen route is a sidecar: `tools/study/p22_view.py` prints a lifted function from `generated/default` with callee names taken at run time from a database outside git (`P22_NAMES`), and `p22_annotate.py` does the same for traces. An optional codegen patch that emits a `// name` comment from an env-pointed file is possible but would touch all 359 generated files and force a full rebuild when the names change; proposed only if the maintainer wants names inside IDE views.
