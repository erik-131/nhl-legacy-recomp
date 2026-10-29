#!/bin/bash
# v9: minnevern (MIN_AVAIL_MB, mem.log). v4: tidsstyrt rute (sekunder), malevindu funnet i loggen (>1000 draws/frame), skjermbilder ved 230 og 270 s.
# Bruk: nhl_bench.sh <tag> [ekstra env]   (krever aktiv Wolf Steam-okt)
TAG=${1:-run}; EXTRA="${2:-}"; DUR=${DUR:-285}
W=/etc/wolf/profile-data/user/WolfSteam; B=$W/nhl-linux; OUT=/root/nhlbench; mkdir -p $OUT
C=$(docker ps --format '{{.Names}}' | grep -i steam | head -1)
[ -z "$C" ] && { echo "Ingen Steam-container (Moonlight-okt mangler)" | tee $OUT/$TAG.txt; exit 1; }
AV=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
[ "$AV" -lt "${MIN_AVAIL_MB:-4000}" ] && { echo "Avbrutt: bare ${AV} MB tilgjengelig minne (< ${MIN_AVAIL_MB:-4000})" | tee $OUT/$TAG.txt; exit 1; }
echo "$(date +%T) $TAG start avail=${AV}MB" >> $OUT/mem.log
rm -f $B/shot_*.png $B/frame_*.png $B/core.*
docker exec "$C" pkill -x nhllegacy.exe; docker exec "$C" pkill -x nhllegacy; sleep 2
BEFORE=$(ls $B/logs/*.log 2>/dev/null | wc -l)
START=$(date +%s)
docker exec -d -u retro -w /home/retro/nhl-linux "$C" env NHL_NO_SETUP_UI=1 REX_FENCE_WAIT_STATS=1 NHL_VK_NO_VSYNC=1 NHL_VPAD_SCRIPT='76:start:150,80:a:150,83:a:150,86:a:150,89:a:150,92:a:150,95:a:150,98:a:150,101:a:150,104:a:150,107:a:150,110:a:150,113:a:150,116:a:150,119:a:150,122:a:150,125:a:150,128:a:150,131:a:150,134:a:150,137:a:150,140:a:150,143:a:150,146:a:150,149:a:150,152:a:150,155:a:150,158:a:150,161:a:150,164:a:150,169:start:150,173:start:150,177:start:150' NHL_SHOT_AFTER_SEC=230,270 $EXTRA ./nhllegacy
while [ $(( $(date +%s)-START )) -lt $DUR ]; do sleep 5; docker exec "$C" pgrep -x nhllegacy >/dev/null || break
  E=$(( $(date +%s)-START )); if [ $E -ge 240 ] && [ ! -f $OUT/$TAG.threads ]; then P=$(pgrep -x nhllegacy | head -1); top -H -b -d 2 -n 2 -p $P | awk '/^top/{n++} n==2' | head -22 > $OUT/$TAG.threads; fi; done
ALIVE=$(docker exec "$C" pgrep -x nhllegacy >/dev/null && echo ja || echo NEI)
docker exec "$C" pkill -x nhllegacy; sleep 3
docker exec "$C" pkill -9 -x nhllegacy 2>/dev/null; sleep 1
echo "$(date +%T) $TAG end avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)MB shm=$(du -sm /dev/shm | cut -f1)MB" >> $OUT/mem.log
L=$(ls -t $B/logs/*.log 2>/dev/null | head -1)
{
echo "tag=$TAG extra=$EXTRA start=$(date -d @$START '+%H:%M:%S') varighet=$(( $(date +%s)-START ))s levde=$ALIVE log=$L"
grep -m1 "Vulkan device" $L | cut -c1-140
python3 - "$L" <<'PY'
import sys,re,datetime
lines=open(sys.argv[1],errors='replace').read().splitlines()
pts=[]
for l in lines:
    if 'nhl-vk-fps' not in l: continue
    ts=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()
    pts.append((ts,int(re.search(r'frames_total=(\d+)',l)[1]),int(re.search(r' draws/frame=(\d+)',l)[1])))
fw=[l for l in lines if 'nhl-fencewait' in l]
g=None
for i in range(len(pts)-5):
    if all(p[2]>1000 for p in pts[i:i+5]): g=i; break
if g is None: print("RESULTAT: UGYLDIG - fant ikke gameplay (>1000 draws i 5 sammenhengende sekunder)"); print("siste:",pts[-3:]); sys.exit()
t0=pts[g][0]+15; t1=t0+60
win=[p for p in pts if t0<=p[0]<=t1]
if len(win)<2: print("RESULTAT: UGYLDIG - for kort gameplay-vindu"); sys.exit()
dt=win[-1][0]-win[0][0]; df=win[-1][1]-win[0][1]
print(f"gameplay start ~{pts[g][0]-pts[0][0]:.0f}s etter forste fps-linje; vindu {dt:.0f}s")
print(f"RESULTAT: {df/dt:.2f} fps, {1000*dt/df:.1f} ms/bilde, snitt draws {sum(p[2] for p in win)/len(win):.0f}")
bl=[float(re.search(r'blocked=(\d+)%',l)[1]) for l in fw if t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
if bl: print(f"CP blokkert i vinduet: snitt {sum(bl)/len(bl):.0f}%")
for l in fw:
    ts=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()
    if t0<=ts<=t0+6: print(l.split('] ')[-1])
fs=[l for l in lines if 'nhl-fencespin' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
if fs:
    pc=[float(re.search(r'\((\d+)%\)',l)[1]) for l in fs]
    print(f"guest i fence-wait: snitt {sum(pc)/len(pc):.0f}% av veggtid ({len(fs)} vinduer)")
    for l in fs[:3]: print(l.split('] ')[-1])
ci=[l for l in lines if 'nhl-cpidle' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
if ci:
    pc=[float(re.search(r'\((\d+)%\)',l)[1]) for l in ci]
    print(f"CP tom for kommandoer: snitt {sum(pc)/len(pc):.0f}% av veggtid")
    for l in ci[:2]: print(l.split('] ')[-1])
cp=[l for l in lines if 'nhl-cpprof' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
if cp:
    n=len(cp); agg={}; W=C=E=T=0.0
    for l in cp:
        m=re.search(r'window=(\d+)ms cpu=([\d.]+)ms.*exec=([\d.]+)ms type3=([\d.]+)ms',l)
        W+=float(m[1]); C+=float(m[2]); E+=float(m[3]); T+=float(m[4])
        for op,ms,c in re.findall(r' ([0-9A-F]{2}):([\d.]+)ms/(\d+)',l):
            a=agg.setdefault(op,[0.0,0]); a[0]+=float(ms); a[1]+=int(c)
    print(f"CP-profil ({n} vinduer): cpu {100*C/W:.0f}%  exec {100*E/W:.0f}%  type3 {100*T/W:.0f}%  annet {100*(E-T)/W:.0f}% av veggtid")
    for op,(ms,c) in sorted(agg.items(),key=lambda x:-x[1][0])[:8]:
        print(f"  op {op}: {100*ms/W:.1f}% veggtid, {c/(W/1000):.0f}/s, {1000*ms/max(c,1):.1f} us/stk")
wr=[l for l in lines if 'nhl-wrm' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
for l in wr[:3]: print(l.split('] ',4)[-1][:900])
ph=[l for l in lines if 'nhl-pkthist' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
for l in ph[:4]: print(l.split('] ',4)[-1][:1400])
gp=[l for l in lines if 'nhl-gpuprof] window' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
if gp:
    W=B=I=0.0; S=0; cats={}
    for l in gp:
        m=re.search(r'window=(\d+)ms busy=([\d.]+)ms idle_gaps=([\d.]+)ms subs=(\d+)',l)
        W+=float(m[1]); B+=float(m[2]); I+=float(m[3]); S+=int(m[4])
        for n,ms,c in re.findall(r' ([a-z]+)=([\d.]+)ms/(\d+)',l):
            if n in ('busy','idle_gaps'): continue
            a=cats.setdefault(n,[0.0,0]); a[0]+=float(ms); a[1]+=int(c)
    fr=df/dt*W/1000 if dt else 1
    print(f"GPU-profil ({len(gp)} vinduer): GPU opptatt {100*B/W:.0f}% av veggtid, {B/fr:.1f} ms/bilde; hull mellom submits {I/fr:.1f} ms/bilde; {S/fr:.1f} submits/bilde")
    for n,(ms,c) in sorted(cats.items(),key=lambda x:-x[1][0]):
        print(f"  {n}: {ms/fr:.2f} ms/bilde ({100*ms/max(B,1e-9):.0f}% av GPU-tid), {c/fr:.0f} segmenter/bilde")
rl=[l for l in lines if 'nhl-reslog' in l]
for l in rl[:500]: print(l.split('[nhl-reslog] ',1)[-1][:300])
rb=[l for l in lines if 'nhl-rbtrace' in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
for l in rb[:2]: print(l.split('] ')[-1][:600])
for tag in ('nhl-drawtime','nhl-copytime','nhl-swaptime'):
    sel=[l for l in lines if tag in l and t0<=datetime.datetime.strptime(l[1:24],'%Y-%m-%d %H:%M:%S.%f').timestamp()<=t1]
    for l in sel[:3]: print(l.split('] ')[-1])
PY
echo "--- tråder (top -H, 2s snitt):"; cat $OUT/$TAG.threads 2>/dev/null; ls -la $B/shot_*.png 2>/dev/null | awk '{print $5, $NF}'
} > $OUT/$TAG.txt 2>&1
cat $OUT/$TAG.txt
