#!/bin/bash
# stage.sh <stage-dir> [script.lua ...]
# Builds <stage-dir>/guest/{scrape,scripts} from the game's own scenedef plus our scripts. Run the game with
# REX_CACHE_ROOT=<stage-dir>. The game's files are never modified: the cache copy overrides them.
# GAME_SCRAPE must point at the game's _compiled/scrape directory (your own copy of the game).
set -e
S=$1; shift
H=$(cd "$(dirname "$0")" && pwd)
: "${GAME_SCRAPE:?set GAME_SCRAPE to the game's _compiled/scrape dir}"
mkdir -p "$S/guest/scrape" "$S/guest/scripts"
cp "$GAME_SCRAPE/bootscenedef.lua" "$GAME_SCRAPE/scenedef.lua" "$S/guest/scrape/"
cat "$H/scenedef_tail.lua" >> "$S/guest/scrape/scenedef.lua"
cp "$H/prelude.lua" "$S/guest/scripts/"
for f in "${@:-$H/autostart.lua}"; do cp "$f" "$S/guest/scripts/"; done
echo "staged $S (copy shaders next to guest/ if your setup needs them)"
