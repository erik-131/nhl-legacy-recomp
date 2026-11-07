#!/bin/bash
# buildab.sh <rounds> <dirA> <dirB>: interleaved runs of two installs (play config + latency route), with reaction times.
B=$(dirname "$(readlink -f "$0")"); N=$1; shift; E="$(cat "$B/playenv.txt") NHL_LAT_PROBE=1 $(cat "$B/latroute.txt")"
for r in $(seq "$N"); do for d in "$@"; do T=$d-${TAG:-b}$r
  echo "== $T $(date +%T)"; NHL_DIR=$d DUR=${DUR:-300} "$B/abrun.sh" "$T" "$E"
  python3 "$B/lat2.py" "$(ls -t /etc/wolf/profile-data/user/WolfSteam/$d/logs/*.log | head -1)" | tail -2
done; done
