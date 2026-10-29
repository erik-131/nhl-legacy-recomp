#!/bin/bash
# Benchmark i den aktive Wolf Steam-okta (krever at en Moonlight-okt er oppe).
# Scriptet kontroller inn i kamp, maaler veggtid mellom gjestebilde 6500 og 8000.
# Bruk: nhl_bench.sh <tag> [ekstra env]
TAG=${1:-run}; EXTRA="${2:-}"
W=/etc/wolf/profile-data/user/WolfSteam; B=$W/nhl-linux; OUT=/root/nhlbench; mkdir -p $OUT
C=$(docker ps --format '{{.Names}}' | grep -i steam | head -1)
[ -z "$C" ] && { echo "Ingen Steam-container (Moonlight-okt mangler)" > $OUT/$TAG.txt; cat $OUT/$TAG.txt; exit 1; }
rm -f $B/frame_*.png $B/core.*
docker exec "$C" pkill -x nhllegacy.exe; docker exec "$C" pkill -x nhllegacy; sleep 2
START=$(date +%s)
docker exec -d -u retro -w /home/retro/nhl-linux "$C" env NHL_NO_SETUP_UI=1 REX_FENCE_WAIT_STATS=1 NHL_VK_NO_VSYNC=1 NHL_VPAD_SCRIPT_FRAMES='2422:start:150,2572:a:150,2662:a:150,2752:a:150,2842:a:150,2932:a:150,3022:a:150,3112:a:150,3202:a:150,3292:a:150,3382:a:150,3472:a:150,3562:a:150,3652:a:150,3742:a:150,3832:a:150,3922:a:150,4012:a:150,4102:a:150,4192:a:150,4282:a:150,4372:a:150,4462:a:150,4552:a:150,4642:a:150,4732:a:150,4822:a:150,4912:a:150,5002:a:150,5092:a:150,5182:a:150,5272:a:150,5362:a:150,5452:a:150,5542:a:150,5632:a:150,5722:a:150,5812:a:150,5902:a:150,5992:a:150,6082:a:150,6172:a:150,6262:a:150,6352:a:150,6600:start:150,6750:start:150,6900:start:150' NHL_SHOT_AFTER_FRAME=9000,10500 $EXTRA ./nhllegacy
sleep 5
n=0; while [ ! -f $B/frame_10500.png ] && [ $n -lt 450 ]; do sleep 2; n=$((n+1)); docker exec "$C" pgrep -x nhllegacy >/dev/null || break; done
sleep 2
ALIVE=$(docker exec "$C" pgrep -x nhllegacy >/dev/null && echo ja || echo NEI-krasjet-eller-avsluttet)
T0=$(stat -c %.3Y $B/frame_9000.png 2>/dev/null); T1=$(stat -c %.3Y $B/frame_10500.png 2>/dev/null)
docker exec "$C" pkill -x nhllegacy
L=$(ls -t $B/logs/*.log 2>/dev/null | head -1)
{
echo "tag=$TAG extra=$EXTRA start=$(date -d @$START '+%H:%M:%S') varighet=$(( $(date +%s)-START ))s prosess-levde-ved-slutt=$ALIVE log=$L"
[ -n "$L" ] && grep -m1 "Vulkan device" $L
[ -n "$L" ] && python3 - "$T0" "$T1" "$L" <<'PY'
import sys,re
t0,t1,log=sys.argv[1],sys.argv[2],sys.argv[3]
lines=open(log,errors='replace').read().splitlines()
fps=[l for l in lines if 'nhl-vk-fps' in l]
win=[]
for l in fps:
    m=re.search(r'frames_total=(\d+)',l); d=re.search(r' draws/frame=(\d+)',l); f=re.search(r'frame_ms=([\d.]+)',l)
    if m and d and 9000<=int(m.group(1))<=10500: win.append((float(f.group(1)),int(d.group(1))))
fw=[l for l in lines if 'nhl-fencewait' in l]
print("fps-linjer:",len(fps),"fencewait-linjer:",len(fw),"siste frames_total:", (re.search(r'frames_total=(\d+)',fps[-1]).group(1) if fps else None))
if t0 and t1:
    dt=float(t1)-float(t0); print(f"RESULTAT: 1500 gameplay-bilder paa {dt:.2f}s = {1500/dt:.2f} fps, {1000*dt/1500:.1f} ms/bilde")
else: print("RESULTAT: mangler frame_9000/8000.png")
if win: print(f"vindu: snitt draws/frame {sum(d for _,d in win)/len(win):.0f}, snitt frame_ms {sum(f for f,_ in win)/len(win):.1f}")
for l in fw[-10:]: print(l.split('] ')[-1])
for l in fps[-3:]: print(l.split('] ')[-1])
PY
[ -n "$L" ] && grep -iE "\] \[(error|critical)\]" $L | cut -c1-200 | sort | uniq -c | sort -rn | head -5
} > $OUT/$TAG.txt 2>&1
cat $OUT/$TAG.txt
