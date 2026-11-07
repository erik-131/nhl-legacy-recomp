# Merge description (draft)

No `.github/pull_request_template.md` exists in the repository, so this uses a plain Summary / Changes / Risks / Testing layout.

## Summary
Brings the fork's work into `master`: a native Linux build, Vulkan performance work for integrated GPUs (about 8-10 -> about 100 fps median on an Arc iGPU), quick start, a pre-game menu, mod layers, database and roster tools, rule hooks and first source-level (L1/L2) work. A rewritten README separates this fork from the original project.

## What it contains
- `src/`: quick start, VP6 bridge changes, pre-game menu (SDL), mod layers, rule hooks, `src/l2/` modules, diagnostics.
- `gpu/`, `renderer/`: render-target and shader-translation changes behind cvars.
- `sdk/`: patch series against rexglue-sdk `c94f5eb` (derivative; attribution in `sdk/README.md`).
- `tools/`: bench harness, Linux setup/build/launcher, rosters/loc-db/art/Lua/study tools, tests.

## What is default off
Pre-game menu (`NHL_MENU=1`), rule hooks (`REX_NHL_SKATERS_PER_SIDE`, `REX_NHL_PERIOD_LENGTH`), L2 modules (`nhl_l2_*`), fused resolve, texture specialisation, implicit LOD, clear-through, PCF gather, 1x main pass, shader fill. Default behaviour changes on: quick start with boot clock and short EA movie in the Linux launcher, direct presentation, skip of overwritten RT transfers, pre-pass and thread placement (Linux launcher only).

## Risks
- Large diff (about 190 files vs master); SDK patches are big (the combined patch is 3.3 MB).
- Only one hardware configuration measured; Windows and macOS builds not re-tested with these changes.
- Shared SDK changes (unclipped-draw default, shm unlink, audio wait) apply to all platforms of the patched SDK.

## How to verify
1. Linux: `tools/nuc/ct105/setup.sh`, `tools/nuc/ct105/build.sh`, start with `nhl_play.sh`; play a match; main menu in about 12 s.
2. Unit tests: `tests/pregame_menu`, `tests/mod_layers`, `tests/l2_*` (CMake targets / `run_tests.sh`), `python3 tools/rosters/test_*.py`.
3. A/B: `tools/nuc/bench/abrun.sh` with `NHL_AB_TOGGLE` for any switch.
4. Check that no game data is tracked: `git ls-files | grep -Ei '\.(xex|big|iso)$'` is empty.
