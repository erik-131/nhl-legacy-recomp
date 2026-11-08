#!/usr/bin/env python3
"""abstat.py - one-command A/B statistics from the existing game logs (no new instrumentation).

  abstat.py A=logA1.log,logA2.log B=logB1.log,logB2.log [C=...]   compare groups (first group is the baseline)
  abstat.py run1.log run2.log                                      a single group: per-run + pooled numbers
  abstat.py --csv results.csv ...                                  also append one row per run to a results database
  abstat.py --list [results.csv]                                   show the database (latest rows)

Group members may be log files, directories (newest log inside) or a bq/abrun output dir with *.log.
Per run it takes the 1 s [nhl-present] samples in the same window abrun.sh uses (gameplay = first 5 consecutive
[nhl-vk-fps] lines with >1000 draws/frame; window = +15 s .. +75 s, change with --skip/--win) and reports
min / p1 / p5 / median / fraction of seconds >= TARGET (100) / mean draws.  Between groups it prints the difference
of the pooled metrics with a 95 % bootstrap CI (two levels: runs resampled, then 5 s blocks inside each run, because
run-to-run variance is 10-30 % and seconds inside a run are correlated).  With < 3 runs per group the CI is only a
warning: it cannot be narrower than what two or three runs allow.
"""
import sys, re, os, glob, random, datetime, statistics, csv, argparse

TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')

def ts(l):
    m = TS.match(l)
    return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp() if m else None

def pct(a, q):
    a = sorted(a)
    return a[int(q * (len(a) - 1))] if a else float('nan')

def load_run(path, skip, win):
    L = open(path, errors='replace').read().splitlines()
    g = []
    for l in L:
        if 'nhl-vk-fps' in l:
            t = ts(l); m = re.search(r' draws/frame=(\d+)', l)
            if t and m: g.append((t, int(m.group(1))))
    start = None
    for i in range(len(g) - 5):
        if all(p[1] > 1000 for p in g[i:i + 5]): start = g[i][0]; break
    run = {'path': path, 'valid': start is not None, 'fps': [], 'draws': [], 'crash': None}
    if start is None: return run
    t0 = start + skip; t1 = t0 + win
    for l in L:
        if 'nhl-present' in l:
            t = ts(l)
            if t and t0 <= t <= t1:
                m = re.search(r' fps=([\d.]+)', l)
                if m: run['fps'].append(float(m.group(1)))
    run['draws'] = [d for t, d in g if t0 <= t <= t1]
    return run

def metrics(fps, target):
    if not fps: return dict(n=0, min=float('nan'), p1=float('nan'), p5=float('nan'), median=float('nan'), frac=float('nan'))
    return dict(n=len(fps), min=min(fps), p1=pct(fps, .01), p5=pct(fps, .05), median=pct(fps, .5),
                frac=sum(1 for f in fps if f >= target) / len(fps))

def boot_group(runs, target, rng, block=5):
    """one bootstrap replicate of a group: resample runs, then 5 s blocks inside each"""
    pool = []
    for _ in runs:
        r = rng.choice(runs); f = r['fps']
        if not f: continue
        nb = max(1, len(f) // block)
        for _ in range(nb):
            s = rng.randrange(0, max(1, len(f) - block + 1)); pool += f[s:s + block]
    return metrics(pool, target)

def resolve(spec):
    out = []
    for p in spec.split(','):
        if not p: continue
        if os.path.isdir(p):
            c = sorted(glob.glob(os.path.join(p, '*.log')), key=os.path.getmtime)
            c = c or sorted(glob.glob(os.path.join(p, 'logs', '*.log')), key=os.path.getmtime)
            if c: out.append(c[-1])
        else:
            m = sorted(glob.glob(p)) or [p]
            out += m
    return out

def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('groups', nargs='*')
    ap.add_argument('--target', type=float, default=100.0)
    ap.add_argument('--skip', type=float, default=15.0)
    ap.add_argument('--win', type=float, default=60.0)
    ap.add_argument('--boot', type=int, default=2000)
    ap.add_argument('--csv', help='append one row per run here (results database)')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        p = (a.groups[0] if a.groups else os.path.expanduser('~/nhl-work/results/ab-runs.csv'))
        rows = list(csv.reader(open(p)))
        print(','.join(rows[0]))
        for r in rows[-30:][::-1] if len(rows) > 1 else []: print(','.join(r))
        return
    groups = {}
    for i, g in enumerate(a.groups):
        if '=' in g and not os.path.exists(g): name, spec = g.split('=', 1)
        else: name, spec = ('A' if not groups else list(groups)[-1]), g
        groups.setdefault(name, []).extend(resolve(spec))
    if not groups: ap.error('no groups')
    data = {}
    rng = random.Random(1)
    print(f"{'group/run':34s} {'secs':>4s} {'min':>6s} {'p1':>6s} {'p5':>6s} {'median':>7s} {'>=%d%%'%a.target:>7s} {'draws':>6s}")
    for name, files in groups.items():
        runs = [load_run(f, a.skip, a.win) for f in files]
        for r in runs:
            if not r['valid'] or not r['fps']:
                print(f"  {os.path.basename(r['path']):32s} INVALID (no gameplay window found) - did it crash? try triage.py")
                continue
            m = metrics(r['fps'], a.target)
            print(f"  {os.path.basename(r['path'])[:32]:32s} {m['n']:4d} {m['min']:6.1f} {m['p1']:6.1f} {m['p5']:6.1f} {m['median']:7.1f} {100*m['frac']:6.1f}% {statistics.mean(r['draws']) if r['draws'] else 0:6.0f}")
            if a.csv:
                new = not os.path.exists(a.csv)
                os.makedirs(os.path.dirname(os.path.abspath(a.csv)), exist_ok=True)
                with open(a.csv, 'a', newline='') as fh:
                    w = csv.writer(fh)
                    if new: w.writerow(['when', 'group', 'log', 'secs', 'min', 'p1', 'p5', 'median', 'frac_ge_target', 'draws', 'target'])
                    w.writerow([datetime.datetime.now().strftime('%F %T'), name, r['path'], m['n'], f"{m['min']:.1f}", f"{m['p1']:.1f}", f"{m['p5']:.1f}",
                                f"{m['median']:.1f}", f"{m['frac']:.4f}", f"{statistics.mean(r['draws']) if r['draws'] else 0:.0f}", a.target])
        good = [r for r in runs if r['valid'] and r['fps']]
        data[name] = good
        pooled = metrics([f for r in good for f in r['fps']], a.target)
        meds = [metrics(r['fps'], a.target)['median'] for r in good]
        spread = f" run medians {min(meds):.0f}..{max(meds):.0f}" if meds else ''
        print(f"{name+' (pooled, %d runs)'%len(good):34s} {pooled['n']:4d} {pooled['min']:6.1f} {pooled['p1']:6.1f} {pooled['p5']:6.1f} {pooled['median']:7.1f} {100*pooled['frac']:6.1f}%{spread}")
    names = [n for n in data if data[n]]
    if len(names) < 2: return
    base = names[0]
    for other in names[1:]:
        print(f"\n{other} minus {base}   (95% bootstrap CI over runs and 5 s blocks)")
        keys = ['min', 'p1', 'p5', 'median', 'frac']
        pa = metrics([f for r in data[base] for f in r['fps']], a.target); pb = metrics([f for r in data[other] for f in r['fps']], a.target)
        diffs = {k: [] for k in keys}
        for _ in range(a.boot):
            ma = boot_group(data[base], a.target, rng); mb = boot_group(data[other], a.target, rng)
            for k in keys: diffs[k].append(mb[k] - ma[k])
        for k in keys:
            d = pb[k] - pa[k]; lo = pct(diffs[k], .025); hi = pct(diffs[k], .975)
            sig = 'significant' if (lo > 0 or hi < 0) else 'within noise'
            if k == 'frac': print(f"  fraction >= {a.target:.0f}: {100*d:+6.1f} pts  [{100*lo:+.1f}, {100*hi:+.1f}]  {sig}")
            else: print(f"  {k:8s} {d:+7.1f} fps ({100*d/pa[k] if pa[k] else 0:+.1f}%)  [{lo:+.1f}, {hi:+.1f}]  {sig}")
        if min(len(data[base]), len(data[other])) < 3:
            print(f"  note: only {len(data[base])} vs {len(data[other])} runs; run-to-run variance is 10-30 %, take >= 3 interleaved runs (A,B,A,B,A,B) before believing a verdict")

if __name__ == '__main__':
    main()
