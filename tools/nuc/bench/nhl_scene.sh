#!/bin/bash
# nhl_scene.sh <scene> [tag] : start the game straight in a match (Lua EnterGameplay from the per-frame hook, no menu
# route) and jump to a scene. Output: $OUT/<tag>/{shots,log,result.txt}. See docs/bench-scenes.md.
# Env: SCENE_HOME/SCENE_AWAY (team ids, default 5/12), SCENE_AT (hook call that enters the match, default 5200),
#   SCENE_LUA (extra timeline Lua, file path), SHOTS (seconds from process start, csv), DUR, NHL_DIR (default nhl-linux-dev),
#   PADS (NHL_VPAD_SCRIPT, default Start at 16 s and 18 s pad time), XENV (extra game env), NATIVE=1 (use the native NHL_SCENE hook of src/scene_force.cpp instead of Lua), SRC (install dir to snapshot, default nhl-linux-dev), NOLOCK=1 (caller holds the lock)
# Runs through the fair bench queue (see tools/nuc/bench/README.md): the job gets a private snapshot of the dev install in $NHL_DIR.
if [ -z "$BQ_JOB" ] && [ -z "$NOQUEUE" ]; then
  # the queue daemon runs the job with its own environment: hand ours over explicitly
  PASS=(BQ_JOB=1); for v in $(compgen -e | grep -E '^(SCENE_|SHOTS$|DUR$|PADS$|XENV$|DUMP|OUT$|NATIVE$|SRC$|KEEPSHOTS$|KEEP$)'); do PASS+=("$v=${!v}"); done
  exec "$(dirname "$(readlink -f "$0")")/bq.sh" run -p ${BQ_PRIO:-3} -e $(( ${DUR:-70} + 40 )) ${KEEP:+-k} -s ${SRC:-nhl-linux-dev} -o scenes -t $(( ${DUR:-70} + 150 )) -n "scene-${2:-$1}" -- env "${PASS[@]}" "$(readlink -f "$0")" "$@"
fi
N=${NHL_WORK:-$HOME/nhl-work}; W=/etc/wolf/profile-data/user/WolfSteam; ND=${NHL_DIR:-nhl-linux-dev}; D=$W/$ND
H=$(cd "$(dirname "$0")/../../.." && pwd); B=$H/tools/nuc/bench
SC=${1:?scene}; PRESET=$SC; TAG=${2:-$SC};
# Presets (docs/bench-scenes.md): each is an environment for the generic runner; result.txt ends with "PRESET <name>: ... s" = seconds
# from process start to the scene.   live (default)  shootout  flyover (arena flyover/intro, CPU v CPU)  closeup (player close-up camera)
#   replay / goal (native hook, natural wait: seconds to the first replay / goal; need a binary with src/scene_force.cpp)
case $SC in
  flyover) SC=live; SCENE_C=${SCENE_C:--1}; SCENE_D=${SCENE_D:--1}; PADS=${PADS:-1:b:100}; SHOTS=${SHOTS:-20,22,24,26,28,30}; DUR=${DUR:-36};;
  closeup) SC=live; SCENE_LUA=${SCENE_LUA:-$(dirname "$0")/../../lua/presets/closeup.lua}; SHOTS=${SHOTS:-22,24,26,28}; DUR=${DUR:-36};;
  replay|goal) SC=live; NATIVE=1; PADS=${PADS:-17.4:start:150,19.4:start:150}; SHOTS=${SHOTS:-40,60,80,100,110,120,130,140,150}; DUR=${DUR:-160}
    XENV="NHL_SCENE=live $XENV";;   # natural: the user team idles, the first goal/replay comes by itself (see docs/bench-scenes.md)
  wipe) echo "preset wipe: not available yet (no forcing found, docs/bench-scenes.md)"; exit 2;;
esac
 OUT=${OUT:-$N/p-scenes}/$TAG; mkdir -p $OUT
HOME_T=${SCENE_HOME:-5}; AWAY_T=${SCENE_AWAY:-12}; AT=${SCENE_AT:-5200}; DUR=${DUR:-70}; SHOTS=${SHOTS:-24,28,32,36,40,45,50,55,60}
PADS=${PADS:-16:start:150,18:start:150}
S=$W/nhl-scenes/$TAG; G=$W/nhl/game/_compiled/scrape
rm -rf $S; install -d $S/guest/scrape $S/guest/scripts
cp $G/bootscenedef.lua $G/scenedef.lua $S/guest/scrape/
[ -z "$NATIVE" ] && printf 'LoadScript("scripts/prelude.lua");\nLoadScript("scripts/scene_cfg.lua");\nLoadScript("scripts/scene.lua");\n' >> $S/guest/scrape/scenedef.lua
cp $H/tools/lua/prelude.lua $H/tools/lua/scene.lua $S/guest/scripts/
MODE=gameplay; [ "$SC" = shootout ] && MODE=shootout
{ echo "SC_HOME,SC_AWAY,SC_AT,SC_MODE,SC_C,SC_D=$HOME_T,$AWAY_T,$AT,\"$MODE\",${SCENE_C:-0},${SCENE_D:--1}"; echo "SC_TL={}"
  [ -n "$SCENE_LUA" ] && cat "$SCENE_LUA"; } > $S/guest/scripts/scene_cfg.lua
cp -al $W/nhl-scrapetest/cache/shaders $S/shaders 2>/dev/null
echo "== $TAG start $(date +%T)"
[ -n "$DUMP" ] && { sudo python3 $B/memdump.py ${DUMP_AT:-30} ${DUMP%%:*} ${DUMP##*:} $OUT/mem.bin & }
ROUTE=quick DUR=$DUR NHL_DIR=$ND PIN=0 RETRY=0 $B/abrun.sh sc-$TAG "$(cat $B/playenv.txt) REX_NHL_FENCE_POLL=3 REX_CACHE_ROOT=/home/retro/nhl-scenes/$TAG NHL_VPAD_SCRIPT=$PADS NHL_SHOT_AFTER_SEC=$SHOTS $XENV" > $OUT/abrun.txt 2>&1
LG=$(grep -l "nhl-scenes/$TAG/" $(ls -t $D/logs/*.log | head -6) | head -1)
mkdir -p $OUT/shots; cp $D/shot_*.png $OUT/shots/ 2>/dev/null; cp "$LG" $OUT/game.log
python3 $B/scene_report.py $OUT/game.log $PRESET > $OUT/result.txt; cat $OUT/result.txt
[ -n "$(ls $OUT/shots/*.png 2>/dev/null)" ] && python3 $B/contact_sheet.py $OUT/sheet.jpg $(ls -v $OUT/shots/shot_*s.png) && [ -z "$KEEPSHOTS" ] && rm -rf $OUT/shots   # disk: keep the contact sheet only
rm -rf $S
echo "== $TAG end $(date +%T)"
