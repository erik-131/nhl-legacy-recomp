#!/usr/bin/env python3
"""lowfps.py <log> [threshold]: per second in gameplay, split into low/high presented fps: mean draws and the other per-second stats."""
import re, sys
T = float(sys.argv[2]) if len(sys.argv) > 2 else 70
L = open(sys.argv[1], errors='replace').read().splitlines()
sec = lambda l: l[1:20]
draws = {sec(l): int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
rows = []
for l in L:
    m = re.search(r'nhl-present.* fps=([\d.]+)', l)
    if m and draws.get(sec(l), 0) > 1000:
        rows.append((sec(l), float(m[1]), draws[sec(l)]))
for name, r in (('low', [x for x in rows if x[1] < T]), ('high', [x for x in rows if x[1] >= T])):
    if r: print(f'{name}: {len(r)} s, mean fps {sum(x[1] for x in r)/len(r):.1f}, mean draws {sum(x[2] for x in r)/len(r):.0f}, draws range {min(x[2] for x in r)}-{max(x[2] for x in r)}')
print('timeline:', ''.join('_' if x[1] < T else '#' for x in rows))
