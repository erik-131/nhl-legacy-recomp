#!/bin/bash
# abq.sh "<tag>|<cvar>=<a>,<b>" ... : one in-process A/B match per item, one after the other (7 min each, play config).
B=$(dirname "$(readlink -f "$0")"); E=$(cat "$B/playenv.txt")
for i in "$@"; do T=${i%%|*}; A=${i#*|}
  echo "== $T $A $(date +%T) floor=$(cat /sys/class/drm/card0/gt/gt0/rps_min_freq_mhz)"
  DUR=${DUR:-420} "$B/abrun.sh" "$T" "$E NHL_AB_TOGGLE=$A@${PHASE:-20}"
done
