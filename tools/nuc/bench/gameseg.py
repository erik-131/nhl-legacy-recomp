#!/usr/bin/env python3
"""gameseg.py <log> [...]: presented fps per log in gameplay (>1000 draws): median, p25, p5, and median per draw-count band."""
import re, sys
for f in sys.argv[1:]:
    L = open(f, errors='replace').read().splitlines()
    dr = {l[1:20]: int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
    v = sorted(float(re.search(r' fps=([\d.]+)', l)[1]) for l in L if '[nhl-present]' in l and dr.get(l[1:20], 0) > 1000)
    if v:
        n = len(v)
        print(f"{f.split('/')[-1]}: {n} s median {v[n // 2]:.1f} p25 {v[n // 4]:.1f} p5 {v[n // 20]:.1f} min {v[0]:.1f}")
