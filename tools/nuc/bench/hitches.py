#!/usr/bin/env python3
"""hitches.py <log> [...]: present gaps during gameplay and what happened just before them.

For each log: gameplay seconds (>1000 draws), gaps > 50/100/300 ms, total time in those gaps, frames not presented while pipelines
compiled ([nhl-skip]), pipelines compiled in gameplay ([nhl-pso]), and how many gaps had a pipeline created within the second before."""
import datetime, re, sys
P = lambda l: datetime.datetime.strptime(l[1:24], '%Y-%m-%d %H:%M:%S.%f').timestamp()
for f in sys.argv[1:]:
    L = open(f, errors='replace').read().splitlines()
    dr = {l[1:20]: int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
    secs, gaps = 0, []
    for l in L:
        m = re.search(r'nhl-present\].* max_gap=([\d.]+)ms', l)
        if m and dr.get(l[1:20], 0) > 1000:
            secs += 1
            if float(m[1]) > 50:
                gaps.append((P(l), float(m[1])))
    cr = [P(l) for l in L if 'Creating graphics pipeline state with VS' in l and ' PS ' in l]
    pso = [float(re.search(r'real ([\d.]+) ms', l)[1]) for l in L if '[nhl-pso] real' in l]
    start = next((P(l) for l in L if 'nhl-vk-fps' in l and dr.get(l[1:20], 0) > 1000), None)
    near = sum(any(g[0] - 1.2 <= c <= g[0] for c in cr) for g in gaps)
    skips = [(int(m[1]), float(m[2])) for m in (re.search(r'nhl-skip\] (\d+) frames not presented over (\d+) ms', l) for l in L) if m]
    inc = [int(m[1]) for m in (re.search(r'incomplete frame presented, total (\d+)', l) for l in L) if m]
    print(f'{f.split("/")[-1]}: {secs} s | gaps >50/100/300 ms: {len(gaps)}/{sum(g[1] > 100 for g in gaps)}/{sum(g[1] > 300 for g in gaps)} '
          f'sum {sum(g[1] for g in gaps):.0f} ms worst {max((g[1] for g in gaps), default=0):.0f} | near pipeline {near} | '
          f'skip runs {len(skips)} frames {sum(s[0] for s in skips)} worst {max((s[1] for s in skips), default=0):.0f} ms | '
          f'pipelines compiled {len(pso)}, median {sorted(pso)[len(pso) // 2] if pso else 0:.0f} ms, max {max(pso, default=0):.0f} | incomplete frames {inc[-1] if inc else 0}')
