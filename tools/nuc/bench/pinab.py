#!/usr/bin/env python3
"""pinab.py <variant> <install dir>: in-process A/B of thread placement. Follows the newest game log and at every
[nhl-ab] phase line applies, to all threads of the running game: A = the masks seen when the run started, B = the variant.
Run together with abrun.sh and a no-op NHL_AB_TOGGLE (e.g. nhl_fence_poll=0,0@20).
Variants: clean (main and CP on P-cores, the rest anywhere), ded (main alone on cpu0, CP alone on cpu1, the rest on 2-),
noP (main and CP on P-cores, the rest off them), allE (everything on the E-cores), e4 (everything on 4 E-cores), e4split (everything on 2+2 E-cores in two clusters), jobsP (as clean, plus the next four busiest XThreads on P-cores, the rest off them)."""
import glob, os, re, subprocess, sys, time

V, D = sys.argv[1], sys.argv[2]
a, b = map(int, open('/sys/devices/cpu_core/cpus').read().split('-'))
P = set(range(a, b + 1))
ALL = os.sched_getaffinity(0)


def game():
    o = subprocess.run(['pgrep', '-x', 'nhllegacy'], capture_output=True, text=True).stdout.split()
    return int(o[0]) if o else None


def threads(pid):
    r = []
    for t in os.listdir(f'/proc/{pid}/task'):
        try:
            comm = open(f'/proc/{pid}/task/{t}/comm').read().strip()
            s = open(f'/proc/{pid}/task/{t}/stat').read().rsplit(') ', 1)[1].split()
            r.append((int(t), comm, int(s[11]) + int(s[12])))
        except OSError:
            pass
    return r


def setm(t, m):
    try:
        os.sched_setaffinity(t, m)
    except OSError:
        pass


def apply_variant(pid):
    ts = threads(pid)
    xs = sorted((x for x in ts if x[1].startswith('XThread')), key=lambda x: -x[2])
    main = xs[0][0] if xs else None
    cp = next((x[0] for x in ts if x[1].startswith('GPU Commands')), None)
    jobs = {x[0] for x in xs[1:5]}
    for t, _, _ in ts:
        if V == 'e4split':
            setm(t, {4, 5, 8, 9})
        elif V in ('allE', 'e4'):
            a, b = map(int, open('/sys/devices/cpu_atom/cpus').read().split('-'))
            setm(t, set(range(a, a + 4 if V == 'e4' else b + 1)))
        elif V == 'ded':
            setm(t, {0} if t == main else {1} if t == cp else ALL - {0, 1})
        elif t in (main, cp):
            setm(t, P)
        elif V == 'clean':
            setm(t, ALL)
        elif V == 'jobsP' and t in jobs:
            setm(t, P)
        else:
            setm(t, ALL - P)


t0 = time.time()
while time.time() - t0 < 120 and not (pid := game()):
    time.sleep(1)
time.sleep(5)
log = max(glob.glob(f'{D}/logs/*.log'), key=os.path.getmtime)
saved, pos = {}, 0
while game() == pid:
    with open(log, errors='replace') as f:
        f.seek(pos)
        new = f.read()
        pos = f.tell()
    for m in re.finditer(r'\[nhl-ab\] phase=([AB])', new):
        if m[1] == 'B':
            saved = {}
            for t, _, _ in threads(pid):
                try:
                    saved[t] = os.sched_getaffinity(t)
                except OSError:
                    pass
            apply_variant(pid)
        else:
            for t, mask in saved.items():
                setm(t, mask)
    time.sleep(0.2)
