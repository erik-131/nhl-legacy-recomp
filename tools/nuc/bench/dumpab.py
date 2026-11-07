#!/usr/bin/env python3
"""dumpab.py <log>: nhl_dump_only_sample off (A) v on (B): GPU ms per frame for dump and resolve, by the 4x shadow resolves being present."""
import collections, re, sys
L = open(sys.argv[1], errors='replace').read().splitlines()
fps = {l[1:20]: float(re.search(r' fps=([\d.]+)', l)[1]) for l in L if '[nhl-present]' in l}
dr = {l[1:20]: int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
ph, since, cur, R = None, 0, None, collections.defaultdict(list)
for l in L:
    m = re.search(r'\[nhl-ab\] phase=([AB])', l)
    if m:
        ph, since = m[1], 0
    if '[nhl-gpuprof] window=' in l:
        since += 1; t = l[1:20]; cur = None
        if ph and since > 2 and t in fps and dr.get(t, 0) > 1000:
            f = fps[t]
            cur = {k: float(re.search(rf' {k}=([\d.]+)', l)[1]) / f for k in ('busy', 'dump', 'resolve', 'draw')}; cur['ph'] = ph; cur['f'] = f
    elif cur is not None and '[nhl-gpuresolve]' in l:
        cur['shadow'] = 'd1:96x20' in l or ':m2:d1' in l
        R[cur['shadow'], cur['ph']].append(cur); cur = None
for k in sorted(R):
    g = R[k]; m = lambda n: sum(s[n] for s in g) / len(g)
    print('shadow scene' if k[0] else 'other', k[1], f"{len(g)} s fps {sorted(s['f'] for s in g)[len(g) // 2]:.1f} busy {m('busy'):.2f} dump {m('dump'):.2f} resolve {m('resolve'):.2f} draw {m('draw'):.2f}")
