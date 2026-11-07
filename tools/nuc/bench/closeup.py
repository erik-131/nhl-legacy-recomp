#!/usr/bin/env python3
"""closeup.py <log>: what makes the slow scenes slow. From REX_GPU_PROF (+ REX_GPU_PROF_SHADERS) logs: gameplay seconds (>1000 draws)
split into the slowest quarter and the fastest half by GPU busy per presented frame, then per group GPU ms per frame by category and
the shader pairs that gain the most ms per frame in the slow group."""
import collections, re, sys
L = open(sys.argv[1], errors='replace').read().splitlines()
fps = {l[1:20]: float(re.search(r' fps=([\d.]+)', l)[1]) for l in L if '[nhl-present]' in l}
dr = {l[1:20]: int(re.search(r' draws/frame=(\d+)', l)[1]) for l in L if 'nhl-vk-fps' in l}
secs, last = [], None
for l in L:
    if '[nhl-gpuprof] window=' in l:
        t, f = l[1:20], fps.get(l[1:20])
        if f and dr.get(t, 0) > 1000:
            cats = {m[0]: (float(m[1]), int(m[2])) for m in re.findall(r' ([a-z]+)=([\d.]+)ms/(\d+)', l)}
            last = {'t': t, 'f': f, 'd': dr[t], 'busy': float(re.search(r' busy=([\d.]+)', l)[1]) / f, 'c': cats, 's': {}}
            secs.append(last)
        else:
            last = None
    elif '[nhl-gpushader]' in l and last:
        last['s'] = {m[0]: (float(m[1]), int(m[2])) for m in re.findall(r' ([0-9A-F]{16}):([\d.]+)ms/(\d+)', l)}
        last = None
secs.sort(key=lambda s: s['busy'])
slow, fast = secs[-len(secs) // 4:], secs[:len(secs) // 2]
print(f'{len(secs)} s: slow quarter {len(slow)} s, fast half {len(fast)} s')
mean = lambda g, f: sum(f(s) for s in g) / max(len(g), 1)
for n, g in (('slow', slow), ('fast', fast)):
    print(f"{n}: fps {mean(g, lambda s: s['f']):.1f} draws {mean(g, lambda s: s['d']):.0f} busy/frame {mean(g, lambda s: s['busy']):.1f} ms | " +
          ' '.join(f"{c}={mean(g, lambda s: s['c'].get(c, (0, 0))[0] / s['f']):.1f}ms/{mean(g, lambda s: s['c'].get(c, (0, 0))[1] / s['f']):.0f}" for c in ('other', 'prep', 'tex', 'rt', 'upload', 'draw', 'resolve', 'swap')))
def per_frame(g):
    d = collections.Counter(); n = collections.Counter()
    for s in g:
        for k, (ms, c) in s['s'].items():
            d[k] += ms / s['f']; n[k] += c / s['f']
    return {k: (v / max(len(g), 1), n[k] / max(len(g), 1)) for k, v in d.items()}
a, b = per_frame(slow), per_frame(fast)
print('largest gain in slow scenes (top 12 per second only), ms per frame slow v fast, marks per frame:')
for k in sorted(a, key=lambda k: b.get(k, (0, 0))[0] - a[k][0])[:12]:
    print(f'  {k} {a[k][0]:.2f} v {b.get(k, (0, 0))[0]:.2f} | {a[k][1]:.1f} v {b.get(k, (0, 0))[1]:.1f}')
print('slowest seconds:', ' '.join(s['t'][11:] for s in secs[-8:]))
