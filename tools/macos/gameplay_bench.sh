#!/bin/zsh
# Gameplay throughput benchmark for guest-CPU work (PGO, -mcpu, codegen).
#
#   tools/macos/gameplay_bench.sh <build dir> <tag>
#
# Times the wall clock between guest frames 6500 and 8000 using the capture
# files' mtimes. The route in vpad_route.txt is frame-indexed, so the window is
# the same moment of the same match for every build: 1500 frames of on-ice play
# at ~20 fps, below the 30 fps cap, i.e. genuinely CPU-bound.
#
# Why not simpler measurements (each of these produced a false result once):
#  - fps between two live runs: the game diverges, runs never show the same thing.
#  - wall time to a frame: ~70% of the route is menus pinned at the 30 fps cap.
#  - any run whose input script is missing: the game idles on a menu at a flat
#    30.00 fps and the "benchmark" looks perfectly clean. So this script refuses
#    to report unless the window demonstrably contained gameplay.
set -u
BIN_DIR="$1"; TAG="$2"
ROUTE_FILE="${0:A:h}/vpad_route.txt"
ROUTE="$(cat "$ROUTE_FILE" 2>/dev/null)"
if [ -z "$ROUTE" ]; then echo "$TAG: ABORT - no route in $ROUTE_FILE"; exit 1; fi
cd "$BIN_DIR" || exit 1
find . -maxdepth 1 -name 'frame_*.png' -delete
BEFORE=$(ls logs/*.log 2>/dev/null | sed 's/\..*//' | sort -u | tail -1)
NHL_NO_SETUP_UI=1 NHL_VPAD_SCRIPT_FRAMES="$ROUTE" NHL_SHOT_AFTER_FRAME="6500,8000" \
  ./nhllegacy --game_data_root=/Users/redth/code/nhl-legacy-recomp-mac/game > /tmp/gb_$TAG.txt 2>&1 &
n=0; while [ ! -f frame_8000.png ] && [ $n -lt 600 ]; do sleep 2; n=$((n+1)); done
sleep 1
T0=$(stat -f %Fm frame_6500.png 2>/dev/null); T1=$(stat -f %Fm frame_8000.png 2>/dev/null)
pkill -f "nhllegacy --game_data_root"; sleep 3
LOGSET=$(ls -t logs/*.log | head -1 | sed 's/\.log$//; s/\.[0-9]*$//')
# Mean draws/frame over the window: menus are ~64, on-ice gameplay is ~1800+.
DRAWS=$(cat ${LOGSET}*.log | grep -h "nhl-vk-fps" | awk '{for(i=1;i<=NF;i++){if($i~/frames_total=/){split($i,a,"=");f=a[2]}; if($i~/^draws\/frame=/){split($i,b,"=");d=b[2]}} if(f>=6500&&f<=8000){s+=d;c++}} END{if(c)printf "%.0f",s/c; else print 0}')
python3 - "$TAG" "${T0:-0}" "${T1:-0}" "$DRAWS" <<'PY'
import sys
tag, t0, t1, draws = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
d = t1 - t0
if d <= 0:
    print(f"{tag}: FAILED - missing capture"); sys.exit(1)
if draws < 1000:
    print(f"{tag}: INVALID - window averaged {draws} draws/frame, i.e. not gameplay "
          f"({1500/d:.2f} fps measured, meaningless)"); sys.exit(1)
print(f"{tag}: {d:.2f}s for 1500 gameplay frames = {1500/d:.2f} fps  (window mean {draws} draws/frame)")
PY
