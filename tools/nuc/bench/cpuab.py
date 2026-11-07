#!/usr/bin/env python3
"""cpuab.py <install dir> <out file> [start_s]: CPU per frame per [nhl-ab] phase, process total and main thread (busiest XThread).

Run alongside abrun.sh with an NHL_AB_TOGGLE. Uses utime+stime from /proc at every phase switch, frames_total from [nhl-vk-fps],
and skips phases before start_s (default 185: gameplay on the normal route)."""
import glob, os, re, subprocess, sys, time

D, OUT = sys.argv[1], sys.argv[2]
START = float(sys.argv[3]) if len(sys.argv) > 3 else 185
TCK = os.sysconf('SC_CLK_TCK')


def game():
    o = subprocess.run(['pgrep', '-x', 'nhllegacy'], capture_output=True, text=True).stdout.split()
    return int(o[0]) if o else None


def ticks(path):
    s = open(path).read().rsplit(') ', 1)[1].split()
    return int(s[11]) + int(s[12])


t0 = time.time()
while time.time() - t0 < 120 and not (pid := game()):
    time.sleep(1)
t0 = time.time()
time.sleep(5)
log = max(glob.glob(f'{D}/logs/*.log'), key=os.path.getmtime)


def sample():
    best = 0
    for t in os.listdir(f'/proc/{pid}/task'):
        try:
            if open(f'/proc/{pid}/task/{t}/comm').read().startswith('XThread'):
                best = max(best, ticks(f'/proc/{pid}/task/{t}/stat'))
        except OSError:
            pass
    return ticks(f'/proc/{pid}/stat'), best, time.time()


pos, frames, cur, last = 0, 0, None, None
tot = {'A': [0.0, 0.0, 0.0, 0], 'B': [0.0, 0.0, 0.0, 0]}
try:
    while game() == pid:
        with open(log, errors='replace') as f:
            f.seek(pos)
            new = f.read()
            pos = f.tell()
        for line in new.splitlines():
            m = re.search(r'frames_total=(\d+)', line)
            if m:
                frames = int(m[1])
            m = re.search(r'\[nhl-ab\] phase=([AB])', line)
            if m:
                s = sample()
                if cur and last and time.time() - t0 >= START:
                    x = tot[cur]
                    x[0] += (s[0] - last[0]) / TCK
                    x[1] += (s[1] - last[1]) / TCK
                    x[2] += s[2] - last[2]
                    x[3] += frames - last[3]
                cur, last = m[1], (*s, frames)
        time.sleep(0.2)
except OSError:
    pass
with open(OUT, 'w') as f:
    for ph, (cpu, main, wall, fr) in tot.items():
        if wall and fr:
            f.write(f'{ph}: {wall:.0f} s, {fr} frames | cores used {cpu / wall:.2f} (main {main / wall:.2f}) | '
                    f'CPU per frame {1000 * cpu / fr:.1f} ms (main {1000 * main / fr:.1f} ms)\n')
