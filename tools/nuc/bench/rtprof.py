#!/usr/bin/env python3
"""rtprof.py <log>: GPU draw time per render target (REX_GPU_PROF=1 REX_GPU_PROF_SHADERS=2), slowest quarter of gameplay seconds
against the fastest half, plus the distinct resolves ([nhl-res-new]) and per-resolve cost ([nhl-gpuresolve])."""
import collections, re, sys
L = open(sys.argv[1], errors='replace').read().splitlines()
fps = {l[1:20]: float(re.search(r' fps=([\d.]+)', l)[1]) for l in L if '[nhl-present]' in l}
dr = {l[1:20]: int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
S, cur = [], None
for l in L:
    if '[nhl-gpuprof] window=' in l:
        t = l[1:20]
        cur = None
        if t in fps and dr.get(t, 0) > 1000:
            cur = {'t': t, 'f': fps[t], 'd': dr[t], 'b': float(re.search(r' busy=([\d.]+)', l)[1]) / fps[t], 'c': {m[0]: float(m[1]) / fps[t] for m in re.findall(r' ([a-z]+)=([\d.]+)ms/\d+', l)}, 's': {}, 'r': {}}
            S.append(cur)
    elif cur is not None and '[nhl-gpushader]' in l:
        cur['s'] = {int(m[0], 16): (float(m[1]) / cur['f'], int(m[2]) / cur['f']) for m in re.findall(r' ([0-9A-F]{16}):([\d.]+)ms/(\d+)', l)}
    elif cur is not None and '[nhl-gpuresolve]' in l:
        cur['r'] = {(m[0], m[1]): (float(m[2]) / cur['f'], int(m[3]) / cur['f']) for m in re.findall(r' (dump|copy):(s\d+:m\d+:d\d+:\d+x\d+):([\d.]+)ms/(\d+)', l)}
S.sort(key=lambda s: s['b'])
slow, fast = S[-len(S) // 4:], S[:len(S) // 2]
def dec(t):
    return f"pitch {t & 0x3FFF} {1 << ((t >> 14) & 3)}x depth@{(t >> 16) & 0xFFF} color@{(t >> 28) & 0xFFF} zw{(t >> 40) & 1} col{(t >> 41) & 1}"
def tot(g, key):
    a, n = collections.Counter(), collections.Counter()
    for s in g:
        for k, (ms, c) in s[key].items():
            a[k] += ms; n[k] += c
    return {k: (a[k] / len(g), n[k] / len(g)) for k in a}
for name, g in (('slow', slow), ('fast', fast)):
    m = lambda f: sum(f(s) for s in g) / len(g)
    print(f"{name}: {len(g)} s fps {m(lambda s: s['f']):.1f} draws {m(lambda s: s['d']):.0f} busy {m(lambda s: s['b']):.1f} | " + ' '.join(f"{c} {m(lambda s: s['c'].get(c, 0)):.2f}" for c in ('draw', 'resolve', 'dump', 'rt', 'tex')))
a, b = tot(slow, 's'), tot(fast, 's')
print('draw time by target, ms per frame slow v fast (segments per frame):')
for k in sorted(a, key=lambda k: -a[k][0])[:14]:
    print(f"  {dec(k)}: {a[k][0]:.2f} v {b.get(k, (0, 0))[0]:.2f} ({a[k][1]:.0f} v {b.get(k, (0, 0))[1]:.0f})")
a, b = tot(slow, 'r'), tot(fast, 'r')
print('resolves, ms per frame slow v fast (per frame):')
for k in sorted(a, key=lambda k: -a[k][0])[:10]:
    print(f"  {k[0]} {k[1]}: {a[k][0]:.2f} v {b.get(k, (0, 0))[0]:.2f} ({a[k][1]:.1f} v {b.get(k, (0, 0))[1]:.1f})")
for l in L:
    if '[nhl-res-new]' in l:
        print(l[26:].split('[nhl-res-new] ')[1][:150])
