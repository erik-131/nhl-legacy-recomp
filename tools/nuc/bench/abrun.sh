#!/bin/bash
# abrun.sh <tag> "<env>" : one bench run with GPU-clock sampling; prints presented fps + avg GPU MHz in the window
SP=${NHL_WORK:-$HOME/nhl-work}; mkdir -p $SP; D=/etc/wolf/profile-data/user/WolfSteam/${NHL_DIR:-nhl-linux}
T=$1; E=$2; F=$SP/freq-$T.txt; : > $F
# therm-<tag>.txt: time, CPU package temp, GPU MHz, synced every second so the last lines survive a host freeze.
TF=$SP/therm-$T.txt; : > $TF
( while :; do m=$(cat /sys/class/drm/card0/gt_act_freq_mhz); echo "$(date +%s.%N) $m" >> $F
  echo "$(date +%T) $(cat /sys/class/thermal/thermal_zone5/temp) $m" >> $TF; sync $TF; sleep 1; done ) & FP=$!
n0=$(grep -c ealogo $D/vp6_bridge.txt)
( sleep 110; if [ $(grep -c ealogo $D/vp6_bridge.txt) -le $n0 ]; then P=$(pgrep -x nhllegacy | head -1); echo "$T: BOOT STALL pid=$P - dumping"
  cat /proc/$P/maps > $SP/stall-$T.maps 2>/dev/null; sudo gdb $HOME/nhl-legacy-recomp/out/linux-rel/nhllegacy -p $P -batch -ex "info threads" -ex "thread apply all bt 40" > $SP/stall-$T.bt 2>&1
  sudo top -H -b -n1 -p $P | head -45 > $SP/stall-$T.top; touch $SP/stall-$T.flag
  C=$(sudo docker ps --format '{{.Names}}' | grep '^WolfSteam_' | head -1); sudo docker exec $C pkill -x nhllegacy; fi ) & WP=$!
rm -f $SP/stall-$T.flag
tc() { cat /sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count; }; TC0=$(tc)
# Anything else using the GPU? Sleep state share over 3 s with the game not running yet (host processes aren't visible from the CT).
R6=/sys/class/drm/card0/gt/gt0/rc6_residency_ms
if [ -z "$(pgrep -x nhllegacy)" ]; then a=$(cat $R6); sleep 3; b=$(cat $R6); idle=$(( (b - a) / 30 )); echo "$T: GPU idle ${idle}% before start"; [ "$idle" -lt 85 ] && echo "$T: WARNING something else is using the GPU (Wolf itself takes ~12 %, see gpuwho.sh)"; fi
# Pin the main guest thread + GPU command processor to the P-cores like nhl_play.sh does (PIN=0: scheduler decides).
if [ "${PIN:-1}" = 1 ]; then ( for i in $(seq 90); do P=$(pgrep -x nhllegacy | head -1); [ -n "$P" ] && break; sleep 1; done
  [ -n "$P" ] && exec $(dirname "$0")/../ct105/nhl_pin.sh $P ) & fi
sudo VSYNC=1 ROUTE=${ROUTE:-} NHL_DIR=${NHL_DIR:-nhl-linux} DUR=${DUR:-285} /root/nhlbench/nhl_bench.sh $T "$E" >/dev/null 2>&1; kill $FP; wait $WP 2>/dev/null
# Temperature itself doesn't matter, throttling does: a run with a growing count is not comparable.
TC1=$(tc); [ "$TC1" != "$TC0" ] && echo "$T: THROTTLED, package_throttle_count $TC0 -> $TC1"
L=$(ls -t $D/logs/*.log | head -1); cp $(ls -t $D/shot_*.png 2>/dev/null | head -1) $SP/$T.png 2>/dev/null
# A boot stall kills the run at 110 s instead of wasting the full duration; retry once (RETRY=0 off).
if [ -f $SP/stall-$T.flag ]; then echo "$T: BOOT STALL - restarting"; [ "${RETRY:-1}" = 1 ] && RETRY=0 exec "$0" "$T-r" "$E"; exit 1; fi
# In-process A/B run (NHL_AB_TOGGLE): report per phase instead of one window.
case "$E" in *NHL_AB_TOGGLE=*) echo "$T:"; python3 $(dirname "$0")/abphase.py "$L" 2 "$F"; exit 0;; esac
python3 - "$L" "$F" "$T" <<'PY'
import sys,re,datetime
def ts(l): return datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp()
L=open(sys.argv[1],errors='replace').read().splitlines()
g=[(ts(l),int(re.search(r' draws/frame=(\d+)',l)[1])) for l in L if 'nhl-vk-fps' in l]
try: s=next(i for i in range(len(g)-5) if all(p[1]>1000 for p in g[i:i+5]))
except StopIteration: print(sys.argv[3],"INVALID"); sys.exit()
t0=g[s][0]+15; t1=t0+60
pf=sorted(float(re.search(r' fps=([\d.]+)',l)[1]) for l in L if 'nhl-present' in l and t0<=ts(l)<=t1)
dr=[p[1] for p in g if t0<=p[0]<=t1]
fr=[float(x.split()[1]) for x in open(sys.argv[2]) if t0<=float(x.split()[0])<=t1]
q=lambda a,x: a[int(x*(len(a)-1))]
hs=[0]*5
for l in L:
    m=re.search(r'nhl-present.*hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)',l)
    if m and t0<=ts(l)<=t1: hs=[a+int(b) for a,b in zip(hs,m.groups())]
tot=max(sum(hs),1); hp=" ".join(f"{100*h/tot:.1f}" for h in hs)
print(f"{sys.argv[3]}: presented median {q(pf,.5):.1f} p5 {q(pf,.05):.1f} | draws {sum(dr)/len(dr):.0f} | GPU {sum(fr)/len(fr):.0f} MHz | frame-time % <=17/20/25/34/>34: {hp}")
PY
