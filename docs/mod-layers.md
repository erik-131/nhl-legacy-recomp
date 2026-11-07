# Mod layers (`mods/`)

User-facing mod directory beside the exe. Code: `src/mod_layers.{h,cpp}` (pure logic),
`src/union_device.{h,cpp}` (`SetMods`, the lookup), hook in `OnPostSetup` in `src/nhllegacy_app.h`.
Tests: `tests/mod_layers/test_mod_layers.cpp`.

## Layout

```
mods/
  load_order.txt            optional, one mod per line
  my-roster/                one directory per mod = the mod id
    mod.toml                optional
    db/nhlng.db             game-relative path, same as under _compiled
  hd-ice/
    rendering/...           art mod; same paths as _compiled/rendering
```

Deleting a mod directory removes that mod. Paths match case-insensitively with `/` or `\`.
`mod.toml` (flat `key = value`, `#` comments, `[sections]` ignored): `name`, `description`,
`enabled` (false disables), `priority` (int, default 0, higher wins over lower regardless of
load order). `mod.toml` itself is never served to the game.

`load_order.txt`: one mod id per line, later wins; `#` comments; `-name` disables. Mods not
listed are appended alphabetically (after the listed ones). Final precedence = (priority,
load-order position), low to high.

## Layer order (first hit wins)

1. writable cache dir (`cache/guest`): the game's own writes and caches, and anything the user
   puts there by hand (existing, proven behaviour)
2. **mods** (new, read-only)
3. loose tree `_compiled` (+ `_loose_tex` .dds -> .rx2 synthesis)
4. fall through to the .big archive under `game:\`

Why mods sit below the writable dir: that directory holds files the game created itself
(`809284.ver`, `admanager_cache_images/`), and it is the manual override channel already in
use. Mods above it would let a mod hide a file the game just wrote and then expects to read back
(stale-cache and corrupt-state risk), and would silently defeat a hand-placed test file. The
cost: a stale file in `cache/guest` shadows a mod of the same path; the startup log does not
show this (the mod index does not know the cache dir), so "mod has no effect" means check
`cache/guest/<path>` first. Saves live elsewhere (`.local/share/nhllegacy/.../<TYPE ts>/`)
and are not part of this stack.

## Performance

The index is built once in `OnPostSetup` and is immutable afterwards. `Resolve(path)` is one
lowercase + hash lookup, no filesystem probing, no lock (the earlier 8-14 ms stalls came from
probing folders under a global lock on every `cache:` miss). The only per-call filesystem work
is on a hit (resolving the entry in the winning mod's device).

## Safety

- keys reject `..`, drive/colon paths, control chars; `Resolve("../x")` returns nothing
- symlinks are never followed and never served: symlinked mod dirs, symlinked directories and
  files inside a mod, and symlinked `mod.toml`/`load_order.txt` are skipped (counted as
  `rejected=` in the log). Names starting with `.` are ignored; names with `:` `\` or control
  chars are rejected
- the scan never throws; a bad mods dir yields an empty index

## Delivering mods

- **DB mod** (resealed `nhlng.db` from `tools/rosters`): `mods/roster-2026/db/nhlng.db`. The
  proven mechanism (TDBTEST run) served it from `cache/guest/db/nhlng.db`; the mods layer
  returns the same kind of host-file entry, via a read-only `HostPathDevice` per mod.
- **Art mod**: `mods/<name>/rendering/<cat>/<file>.rx2` (whole replacement files). `.dds` overrides
  still use `_loose_tex` (synthesis lives in `LooseTreeDevice`, not in this layer).

### ROSTER save shadowing caveat

(worklog 2026-11-07 "How the game loads rosters".) Per the remaster docs the game writes
`ROSTER` saves itself and the newest ROSTER save overrides the loose `nhlng.db`; franchise
saves carry their own copy. So a DB mod only takes effect if the profile has no ROSTER save,
and a 2026 database only affects franchises created after it is installed. Before installing a
DB mod check `.local/share/nhllegacy/.../ROSTER*` and warn; never delete saves without asking.
Tests that may create a ROSTER save must use a scratch user-data root.

## Log

`[nhl-mods] N mods, M files, K conflicts`, then one line per mod (`#rank id (name)
priority= files= rejected=`, or `off id disabled by mod.toml|load_order.txt`) and up to 50
`conflict <path>: <winner> wins over <losers>` lines. Printed only if `mods/` has any mod dir.

## Status: what is verified

Verified: the pure logic, by the unit tests (`clang++-20 -std=c++20 -Isrc
tests/mod_layers/test_mod_layers.cpp src/mod_layers.cpp`): precedence, case-insensitivity,
disabled mods, load order, priority, conflicts, traversal rejection, symlink rejection,
empty/missing dir. `src/union_device.cpp` passes a single-file `-fsyntax-only`.

**Not verified**: the integration was not built or run (`nhllegacy_app.h` not even
syntax-checked), nothing was served to the game.

## Known gaps / risks

- **Directory handles**: the guest opens many files relative to a directory handle
  (`Entry::GetChild`), which bypasses `Device::ResolvePath`. The mods layer only answers
  full-path lookups of files. If the game opens e.g. `db\nhlng.db` by a directory handle for
  an already existing `_compiled` directory, the mod file is not seen. (The TDBTEST proof
  went through `cache/guest/db`, a real directory in the upper layer.) Fix if it shows: make
  `LooseTreeEntry::Populate` consult `ModIndex` (substitute host file per child), or add a
  merged directory entry type.
- Mods that add directories absent from `_compiled` are not resolvable as directories.
- Only mounted when `_compiled` exists (inside the UnionDevice branch).
- Mods file contents are read live; editing a mod while the game runs is unsupported.
- The scan is one-time: added/removed mods need a restart.
