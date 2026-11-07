#!/usr/bin/env python3
"""cpuframe.py <install dir> [start_s] [dur_s]: CPU per frame of a run over a window after the game starts, from /proc and [nhl-vk-fps] frames_total."""
import glob, os, re, subprocess, sys, time
D = sys.argv[1]; S = int(sys.argv[2]) if len(sys.argv) > 2 else 190; T = int(sys.argv[3]) if len(sys.argv) > 3 else 90
for _ in range(120):
    p = subprocess.run(['pgrep', '-x', 'nhllegacy'], capture_output=True, text=True).stdout.split()
    if p:
        break
    time.sleep(1)
P = p[0]; t0 = time.time(); HZ = os.sysconf('SC_CLK_TCK')
def cpu():
    return sum(map(int, open(f'/proc/{P}/stat').read().rsplit(')', 1)[1].split()[11:13])) / HZ
def frames():
    L = max(glob.glob(D + '/logs/*.log'), key=os.path.getmtime)
    v = re.findall(r'frames_total=(\d+)', open(L, errors='replace').read()[-200000:])
    return int(v[-1])
time.sleep(max(0, S - (time.time() - t0)))
a, fa, ta = cpu(), frames(), time.time()
time.sleep(T)
b, fb, tb = cpu(), frames(), time.time()
print(f'cores {(b - a) / (tb - ta):.2f}, {fb - fa} frames, CPU per frame {(b - a) / max(fb - fa, 1) * 1000:.1f} ms')
