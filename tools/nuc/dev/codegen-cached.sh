#!/bin/bash
# codegen-cached.sh [WORKTREE]   Fill WORKTREE/generated/default from a content-addressed cache, running `rexglue codegen` only on a miss.
# Key = sha256(nhllegacy_manifest.toml + nhllegacy_functions.toml + game/default.xex + rexglue binary).  Measured 2026-11-07: a codegen run
# is single-threaded and takes 16-25 min; 3-4 identical runs were going at once (p-scenes, wt-install, main).  With this script the first
# caller generates (others with the same key wait on the lock and then link 384 MB of generated sources), every later caller needs ~2 s.
# codegen-cached.sh WORKTREE --seed DIR   store an already finished generated/default (DIR) in the cache under WORKTREE's key.
# Always works on the worktree's own generated/ (never through a symlink to another tree: codegen deletes its outputs first).
set -euo pipefail
WT=$(readlink -f "${1:-.}"); W=${NHL_WORK:-$HOME/nhl-work}; C=$W/codegen-cache; mkdir -p "$C"
REX=${REXGLUE:-$(readlink -f "$WT/third_party")/rexglue-sdk-main/out/linux-amd64/rexglue}
KEY=$(cat "$WT/nhllegacy_manifest.toml" "$WT/nhllegacy_functions.toml" "$WT/game/default.xex" "$REX" | sha256sum | cut -c1-16)
E=$C/$KEY
exec 9>"$C/$KEY.lock"; flock 9
if [ "${2:-}" = --seed ]; then rm -rf "$E.tmp"; mkdir -p "$E.tmp"; cp -a "${3:?DIR}" "$E.tmp/default"; touch "$E.tmp/ok"; rm -rf "$E"; mv "$E.tmp" "$E"; echo "seeded $KEY"; fi
if [ ! -f "$E/ok" ]; then
  echo "codegen cache MISS $KEY - running codegen (16-25 min, single core)"
  rm -rf "$WT/generated/default"; ( cd "$WT" && "$REX" codegen nhllegacy_manifest.toml )
  rm -rf "$E.tmp"; mkdir -p "$E.tmp"; cp -a "$WT/generated/default" "$E.tmp/default"; touch "$E.tmp/ok"; rm -rf "$E"; mv "$E.tmp" "$E"
else echo "codegen cache HIT $KEY"; fi
mkdir -p "$WT/generated"; rm -rf "$WT/generated/default"; cp -al "$E/default" "$WT/generated/default"   # hardlinks: 384 MB once, not per worktree (codegen unlinks before writing, so sharing is safe; never touch/edit these files in place)
echo "generated/default ready ($(ls "$WT/generated/default" | wc -l) files)"
