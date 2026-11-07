#!/usr/bin/env python3
"""perfrep.py <perf.data> [topN] [threads]: per-thread CPU use and top symbols from a perfprof.sh recording.

Merges the hybrid cpu_core/cpu_atom events by sample count (frequency mode: ~F samples per running second, so counts are
approximate CPU time). threads = how many of the busiest threads get a symbol list (default 3)."""
import collections, os, re, subprocess, sys
D = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 25; TH = int(sys.argv[3]) if len(sys.argv) > 3 else 3
o = subprocess.run(['perf', 'report', '-i', D, '--no-children', '--stdio', '-q', '--symfs', os.path.expanduser('~/nhl-work/symfs'), '--kallsyms', '/proc/kallsyms', '--sort', 'comm,dso,sym',
                    '-F', 'sample,comm,dso,sym', '-t', '\t'], capture_output=True, text=True, errors='replace').stdout
rows = collections.Counter(); comm = collections.Counter(); dsos = collections.defaultdict(collections.Counter)
for l in o.splitlines():
    p = [x.strip() for x in l.split('\t')]
    if len(p) < 4 or not p[0].isdigit(): continue
    n, c, d, s = int(p[0]), p[1], p[2], re.sub(r'^\[.\] ', '', p[3])
    if re.fullmatch(r'0x[0-9a-f]+', s): s = '(unresolved)'
    rows[c, d, s] += n; comm[c] += n; dsos[c][d] += n
tot = sum(comm.values())
print(f'samples {tot}')
for c, n in comm.most_common(14):
    top = ' '.join(f'{d.split("/")[-1]} {100*v/n:.0f}%' for d, v in dsos[c].most_common(3))
    print(f'{n:8d} {c:18s} {top}')
for c, n in comm.most_common(TH):
    print(f'\n== {c} ({n})')
    s = sorted(((v, k[1], k[2]) for k, v in rows.items() if k[0] == c), reverse=True)[:N]
    for v, d, y in s: print(f'{100*v/n:6.2f}%  {d.split("/")[-1][:20]:20s} {y[:70]}')
