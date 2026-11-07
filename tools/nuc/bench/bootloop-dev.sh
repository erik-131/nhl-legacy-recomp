#!/bin/bash
# N boots with the bench env (vpad script, fence stats); on a stall, dump guest thread stacks.
D=/etc/wolf/profile-data/user/WolfSteam/nhl-linux-dev; OUT=$1; N=${2:-6}; mkdir -p $OUT
VP='76:start:150,80:a:150,83:a:150,86:a:150'
for i in $(seq $N); do C=$(docker ps --format '{{.Names}}' | grep '^WolfSteam_' | head -1)
  docker exec $C pkill -x nhllegacy; sleep 1; n0=$(grep -c 'ealogo' $D/vp6_bridge.txt)
  docker exec -d -u retro -w /home/retro/nhl-linux-dev $C env NHL_NO_SETUP_UI=1 REX_FENCE_WAIT_STATS=1 SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x045E/0x028E NHL_NO_GUIDE=1 NHL_DIRECT_PRESENT=1 NHL_SWAP_INTERVAL=0 NHL_VK_READBACK_MODE=fast NHL_VP6_FFMPEG=/home/retro/nhl-linux-dev/ffmpeg NHL_VPAD_SCRIPT=$VP ./nhllegacy
  sleep 75; n1=$(grep -c 'ealogo' $D/vp6_bridge.txt)
  L=$(ls -t $D/logs/*.log | head -1); DPC=$(grep -c "nhl-dpc" $L)
  if [ $n1 -gt $n0 ]; then echo "$i OK dpc_lines=$DPC"; else echo "$i STUCK dpc_lines=$DPC"; touch $D/stalldump; sleep 4; P=$(pgrep -x nhllegacy | head -1)
    sudo top -H -b -n1 -p $P | head -30 > $OUT/stuck$i.top
    sudo gdb $HOME/nhl-legacy-recomp/out/linux-rel/nhllegacy -p $P -batch -ex 'thread apply all bt 60' > $OUT/stuck$i.bt 2>&1
    cp $(ls -t $D/logs/*.log | head -1) $OUT/stuck$i.log; fi
  docker exec $C pkill -x nhllegacy; sleep 2; done
