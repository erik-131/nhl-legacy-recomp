#!/bin/bash
# perfprof.sh <tag> "<t_s:dur_s:label> ..." [perf record options]: while a bench runs, sample the game with perf at the
# given seconds after it starts (e.g. "110:30:lineups 200:30:gameplay"). Writes $NHL_WORK/perf-<tag>-<label>.data.
# Needs kernel.perf_event_paranoid <= 1 on the host. Read them with perfrep.sh. Start it right before abrun.sh.
SP=${NHL_WORK:-$HOME/nhl-work}; T=$1; W=$2; shift 2; O=${*:--F 1999}
for i in $(seq 120); do P=$(pgrep -x nhllegacy | head -1); [ -n "$P" ] && break; sleep 1; done
[ -z "$P" ] && { echo "perfprof: no game"; exit 1; }
S=$(date +%s)
for w in $W; do IFS=: read -r at dur lb <<< "$w"
  while [ $(( $(date +%s) - S )) -lt "$at" ]; do sleep 1; done
  [ -d /proc/$P ] || break
  perf record -q -p "$P" $O -o "$SP/perf-$T-$lb.data" -- sleep "$dur" 2>"$SP/perf-$T-$lb.err"
done
