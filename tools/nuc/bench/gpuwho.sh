#!/bin/bash
# gpuwho.sh [seconds]: GPU idle share (rc6) and render-engine time per process visible here, to see what else uses the GPU.
# Processes outside this container don't show, so idle below 100 % minus the sum of these is someone else (host, other CTs).
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
S=${1:-5}; R6=/sys/class/drm/card0/gt/gt0/rc6_residency_ms
snap() { for f in /proc/[0-9]*/fdinfo/*; do awk -v f="$f" '/^drm-engine-[a-z]+:/{split(f,a,"/"); e[a[3]]+=$2} END{for(p in e) print p, e[p]}' "$f" 2>/dev/null; done | sort -u -k1,1 -k2,2nr | sort -u -k1,1; }
A=$(snap); t0=$(date +%s%N); a=$(cat $R6); sleep "$S"; b=$(cat $R6); t1=$(date +%s%N); B=$(snap); S=$(( (t1 - t0) / 1000000000 ))
echo "GPU idle $(( (b - a) * 100000000 / (t1 - t0) ))%"
join <(echo "$A") <(echo "$B") | awk -v s="$S" '{d=($3-$2)/1e7/s; if (d>0.05) printf "%s %.1f%%\n",$1,d}' | while read -r p u; do echo "$u $p $(cat /proc/$p/comm 2>/dev/null)"; done | sort -rn
