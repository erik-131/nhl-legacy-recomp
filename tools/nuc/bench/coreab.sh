#!/bin/bash
# coreab.sh <rounds> "<name>:<cpuset>:<pin>" ... : interleaved runs that limit the Wolf container to a CPU set from boot
# (the game's thread counts follow it), e.g. "old8:1,3,6-10,13:1" "all14:0-13:1" "all14np:0-13:0". Env per abrun.sh; prints one
# abrun line + the fast-press latency per run. Uses NHL_DIR, EXTRA_ENV (default: play config, latency route).
B=$(dirname "$(readlink -f "$0")"); SP=${NHL_WORK:-$HOME/nhl-work}; N=$1; shift
ENVS="${EXTRA_ENV:-$(cat "$B/playenv.txt") NHL_LAT_PROBE=1 $(cat "$B/latroute.txt")}"
C=$(sudo docker ps --format '{{.Names}}' | grep '^WolfSteam_' | head -1); [ -z "$C" ] && { echo "no lobby"; exit 1; }
# Written to the cgroup directly: dockerd remembers the CPU list from when it started and refuses anything else.
CG=/sys/fs/cgroup/system.slice/docker-$(sudo docker inspect -f '{{.Id}}' "$C").scope/cpuset.cpus
setcpus() { echo "$1" | sudo tee "$CG" >/dev/null || exit 1; [ "$(cat "$CG")" = "$1" ] || echo "cpuset is $(cat "$CG"), wanted $1"; }
D=/etc/wolf/profile-data/user/WolfSteam/${NHL_DIR:-nhl-linux}
for r in $(seq "$N"); do for c in "$@"; do IFS=: read -r name set pin <<< "$c"
  T=$name-${TAG:-c}$r
  setcpus "$set"
  echo "== $T $(date +%T) cpuset=$set pin=$pin floor=$(cat /sys/class/drm/card0/gt/gt0/rps_min_freq_mhz) pkgC=$(($(cat /sys/class/thermal/thermal_zone5/temp)/1000))"
  PIN=$pin DUR=${DUR:-300} "$B/abrun.sh" "$T" "$ENVS"
  L=$(ls -t "$D"/logs/*.log | head -1); python3 "$B/lat2.py" "$L" | tail -4
done; done
setcpus "$(cat /sys/fs/cgroup/cpuset.cpus.effective)"
