#!/usr/bin/env python3
"""halfpx.py <log>: GPU ms per frame with nhl_test_half_scissor off (A) and on (B), binned by draws per frame. What halves is pixel
cost, what stays is vertex and pipeline cost."""
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
            f = fps[t]; d = dr[t]; bin_ = 0 if d < 2000 else 1 if d < 3000 else 2
            cur = {'b': float(re.search(r' busy=([\d.]+)', l)[1]) / f, 'draw': float(re.search(r' draw=([\d.]+)', l)[1]) / f, 'd': d, 'main': 0}
            R[bin_, ph].append(cur)
    elif cur is not None and '[nhl-gpushader]' in l:
        for k, ms in re.findall(r' ([0-9A-F]{16}):([\d.]+)ms/', l):
            t_ = int(k, 16)
            if t_ & 0x3FFF == 640 and (t_ >> 14) & 3 == 1 and (t_ >> 16) & 0xFFF == 736 and (t_ >> 41) & 1:
                cur['main'] += float(ms) / fps[t]
for k in sorted(R):
    g = R[k]; m = lambda n: sum(s[n] for s in g) / len(g)
    print(['<2000', '2000-3000', '>3000'][k[0]], k[1], f"{len(g)} s draws {m('d'):.0f} busy {m('b'):.1f} draw {m('draw'):.1f} main pass {m('main'):.1f}")
