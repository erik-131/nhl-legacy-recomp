#!/bin/bash
# bootab.sh <outdir> <N> <dirA> <dirB>: N interleaved boots of two installs; a boot counts as OK once the EA logo movie starts,
# as STUCK if it hasn't after 90 s (BOOT_TIMEOUT) (thread dump and log to outdir). Prints a running count per install. BOOT_ENV adds env. STOP_STUCK=1 ends at the first stall, game left running.
W=/etc/wolf/profile-data/user/WolfSteam; OUT=$1; N=$2; shift 2; mkdir -p "$OUT"; declare -A ok stuck
C=$(sudo docker ps --format '{{.Names}}' | grep '^WolfSteam_' | head -1)
for i in $(seq "$N"); do for d in "$@"; do D=$W/$d
  sudo docker exec "$C" pkill -x nhllegacy; sleep 1; n0=$(grep -c ealogo "$D/vp6_bridge.txt"); t0=$(date +%s); r=STUCK
  sudo docker exec -d -u retro -w /home/retro/$d "$C" env NHL_NO_SETUP_UI=1 SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045E/0x028E NHL_NO_GUIDE=1 NHL_DIRECT_PRESENT=1 NHL_SWAP_INTERVAL=0 NHL_VK_READBACK_MODE=fast NHL_VP6_FFMPEG=/home/retro/$d/ffmpeg $BOOT_ENV ./nhllegacy
  while [ $(( $(date +%s) - t0 )) -lt ${BOOT_TIMEOUT:-90} ]; do [ "$(grep -c ealogo "$D/vp6_bridge.txt")" -gt "$n0" ] && { r=OK; break; }; sleep 1; done
  e=$(( $(date +%s) - t0 ))
  if [ $r = OK ]; then ok[$d]=$(( ${ok[$d]:-0} + 1 )); else stuck[$d]=$(( ${stuck[$d]:-0} + 1 )); P=$(pgrep -x nhllegacy | head -1)
    sudo gdb $HOME/nhl-legacy-recomp/out/linux-rel/nhllegacy -p "$P" -batch -ex 'thread apply all bt 40' > "$OUT/stuck-$d-$i.bt" 2>&1; cp "$(ls -t "$D"/logs/*.log | head -1)" "$OUT/stuck-$d-$i.log"; fi
  echo "$i $d $r ${e}s | $(for x in "$@"; do printf '%s ok=%s stuck=%s  ' "$x" "${ok[$x]:-0}" "${stuck[$x]:-0}"; done)"
  [ $r = STUCK ] && [ -n "$STOP_STUCK" ] && exit 2
done; done
sudo docker exec "$C" pkill -x nhllegacy
