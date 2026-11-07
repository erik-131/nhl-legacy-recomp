#!/usr/bin/env python3
"""abphase.py <log> [skip_s] [freq_file]: split an NHL_AB_TOGGLE run by phase.

Uses the per-second [nhl-present] lines (presented fps + present-interval histogram) and [nhl-vk-fps]
(draws/frame) in gameplay (>1000 draws), drops the first skip_s seconds (default 2) after every phase
switch, and prints per phase: seconds, presented fps median/p5, % of frames per interval bucket
(<=17/20/25/34/>34 ms) and mean draws. Also the per-segment medians in order, so drift (heat, scene)
shows up as a trend in both phases rather than as a phase difference. With abrun's freq file also the
mean GPU clock per phase.
"""
import datetime
import re
import sys


def ts(line):
    return datetime.datetime.strptime(line[1:24], '%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp()


lines = open(sys.argv[1], errors='replace').read().splitlines()
skip = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
switches = [(ts(l), re.search(r'phase=([AB])', l)[1]) for l in lines if '[nhl-ab] phase=' in l]
if not switches:
    sys.exit('no [nhl-ab] phase lines - was NHL_AB_TOGGLE set?')
draws = [(ts(l), int(re.search(r' draws/frame=(\d+)', l)[1])) for l in lines if 'nhl-vk-fps' in l]


def phase_at(t):
    seg, cur = None, None
    for i, (st, ph) in enumerate(switches):
        if st <= t:
            seg, cur = i, (st, ph)
    if cur is None or t - cur[0] < skip:
        return None, None
    return seg, cur[1]


def draws_at(t):
    near = [d for dt, d in draws if abs(dt - t) <= 1.0]
    return near[0] if near else 0


freq = [tuple(map(float, x.split())) for x in open(sys.argv[3])] if len(sys.argv) > 3 else []
freq_phase = {'A': [], 'B': []}
for t, mhz in freq:
    seg, ph = phase_at(t)
    if ph and draws_at(t) > 1000:
        freq_phase[ph].append(mhz)

stats = {'A': [], 'B': []}
segs = {}
for l in lines:
    m = re.search(r'nhl-present.* fps=([\d.]+).*hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)', l)
    if not m:
        continue
    t = ts(l)
    seg, ph = phase_at(t)
    d = draws_at(t)
    if ph is None or d <= 1000:
        continue
    rec = (float(m[1]), [int(x) for x in m.groups()[1:]], d)
    stats[ph].append(rec)
    segs.setdefault(seg, (ph, []))[1].append(rec[0])

q = lambda a, x: sorted(a)[int(x * (len(a) - 1))]
for ph in 'AB':
    r = stats[ph]
    if not r:
        print(f'{ph}: no gameplay seconds')
        continue
    fps = [x[0] for x in r]
    hs = [sum(x[1][i] for x in r) for i in range(5)]
    tot = max(sum(hs), 1)
    hp = ' '.join(f'{100 * h / tot:.1f}' for h in hs)
    print(f'{ph}: {len(r)} s | presented median {q(fps, .5):.1f} p5 {q(fps, .05):.1f} | '
          f'draws {sum(x[2] for x in r) / len(r):.0f} | frame-time % <=17/20/25/34/>34: {hp}'
          + (f' | GPU {sum(freq_phase[ph]) / len(freq_phase[ph]):.0f} MHz' if freq_phase[ph] else ''))
print('segments:', ' '.join(f'{ph}{q(v, .5):.0f}' for _, (ph, v) in sorted(segs.items()) if v))
