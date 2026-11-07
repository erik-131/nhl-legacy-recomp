#!/bin/sh
# Prepare shaders: plays the game by itself for N minutes (default 30) with random button presses and no sound,
# so that the pipeline storage fills before the user plays (a first-seen pipeline freezes play for 50-300 ms;
# measured 2026-11-06: 12 gaps over 50 ms in two minutes of play on empty storage, 0-1 on a filled one).
# Usage: nhl_fill.sh [minutes]      Needs a display like the normal launcher (Wolf session or a desktop).
# A random 30 min run reaches ~290 pipelines; scenes the presses don't reach (replays, goals, penalties, fights)
# are added by normal play. Stop with pkill nhllegacy: the storage is flushed as it grows.
# Uses a scratch copy of nothing: it runs on the user's profile and saves, so it presses buttons in menus too;
# run it on a fresh install or a scratch profile, not on a profile you care about.
MIN="${1:-30}"
cd "$(dirname "$0")" || exit 1
SEC=$((MIN * 60))
# quick start through the boot, then random presses every 0.8-2.4 s (seed from the clock); 'start' is rarer in the random part
VP="14:a:120,16:a:120,18:a:120,20:a:120,22:a:120,24:a:120,28:start:120"
RANDOM_SEED=$(date +%s)
# with NHL_FILL_REPLAY=1, every ~100 s a replay block (off by default: it didn't add pipelines, see the worklog): pause, Instant Replay (second item), roam the replay camera (right, LB, RB, up),
# leave it (B, B) and resume (Start, A); the rest is random presses. Found 2026-11-06 with screenshots: this
# visits the replay UI and free-roam camera, and play then goes on (a penalty broadcast followed in the test).
VP="$VP$(awk -v seed="$RANDOM_SEED" -v end="$((SEC - 20))" -v replay="${NHL_FILL_REPLAY:-0}" 'BEGIN{srand(seed); split("a a a a a a a a start b b up down left right up down left right lb rb",b," "); n=21; split("0.8 1.2 1.6 2.4",g," "); split("100 150 250",h," "); t=34; nextblock=(replay==1)?60:1e9; while(t<end){ if(t>=nextblock){ printf(",%.1f:start:120,%.1f:down:120,%.1f:a:120,%.1f:right:600,%.1f:lb:600,%.1f:rb:600,%.1f:up:600,%.1f:b:500,%.1f:b:500,%.1f:start:300,%.1f:a:300",t,t+2,t+4,t+10,t+13,t+16,t+19,t+22,t+26,t+30,t+33); t+=36; nextblock=t+90+int(rand()*30) } else { printf(",%.1f:%s:%d",t,b[int(rand()*n)+1],h[int(rand()*3)+1]); t+=g[int(rand()*4)+1] } }}')"
export NHL_VPAD_SCRIPT="$VP"
export REX_AUDIO_MUTE=true
export NHL_HUD=0
timeout "$SEC" ./nhl_play.sh "$@" 2>&1 | tail -n 1
pkill -x nhllegacy 2>/dev/null
echo "nhl_fill: done after $MIN min"
