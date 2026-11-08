#!/usr/bin/env python3
"""triage.py [LOG|INSTALL_DIR|-] [--dur SEC] - did the run crash?  One screen of facts, ~1 s.

Takes a game log, or an install/snapshot dir (newest log inside; default nhl-linux-dev).  Prints: how far the boot got
(logo, quick start done, gameplay window), where the log stops and how long ago, whether the game is still running,
the last lines, [error]/[critical] lines, null-call context, core files and kernel segfault/OOM lines (if dmesg is readable).
Verdict: OK (log ends because the run ended), RUNNING, CRASH (log stops abruptly, game gone, shorter than --dur),
BOOT-STALL (never reached the main menu), NO-GAMEPLAY.  Exit code 0 = OK/RUNNING, 1 = anything else.
"""
import sys, os, re, glob, time, datetime, subprocess, argparse
WOLF = '/etc/wolf/profile-data/user/WolfSteam'
TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')
def ts(l):
    m = TS.match(l)
    return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp() if m else None
ap = argparse.ArgumentParser(); ap.add_argument('target', nargs='?', default='nhl-linux-dev'); ap.add_argument('--dur', type=float, default=0, help='expected run length in s (0 = unknown)')
ap.add_argument('--tail', type=int, default=6)
a = ap.parse_args()
t = a.target
if not os.path.exists(t) and os.path.isdir(os.path.join(WOLF, t)): t = os.path.join(WOLF, t)
inst = None
if os.path.isdir(t):
    inst = t; c = sorted(glob.glob(t + '/logs/*.log'), key=os.path.getmtime)
    if not c: print('no logs in', t); sys.exit(1)
    t = c[-1]
else:
    d = os.path.dirname(os.path.abspath(t))
    if os.path.basename(d) == 'logs': inst = os.path.dirname(d)
L = open(t, errors='replace').read().splitlines()
tl = [(ts(l), l) for l in L if ts(l)]
if not tl: print('empty log', t); sys.exit(1)
first, last = tl[0][0], tl[-1][0]
mt = os.path.getmtime(t); age = time.time() - mt
def first_t(pat):
    for x, l in tl:
        if re.search(pat, l): return x - first
    return None
ev = [('logo movie', r'ealogo|vp6|movie'), ('main menu (quick done)', r'nhl-quick\] done'), ('first present', r'nhl-present')]
print(f'log: {t}  ({len(L)} lines)')
print(f'span: {datetime.datetime.fromtimestamp(first, datetime.timezone.utc):%H:%M:%S} .. {datetime.datetime.fromtimestamp(last, datetime.timezone.utc):%H:%M:%S}  = {last-first:.0f} s, file written {age:.0f} s ago')
reached = {}
for n, p in ev:
    r = first_t(p); reached[n] = r
    print(f'  {n:24s}', f'+{r:.0f} s' if r is not None else 'NOT reached')
g = [(x, int(re.search(r' draws/frame=(\d+)', l)[1])) for x, l in tl if 'nhl-vk-fps' in l and re.search(r' draws/frame=(\d+)', l)]
gp = next((g[i][0] for i in range(len(g) - 5) if all(p[1] > 1000 for p in g[i:i + 5])), None)
print('  gameplay (>1000 draws)    ', f'+{gp-first:.0f} s' if gp else 'NOT reached')
fpsl = [l for x, l in tl if 'nhl-vk-fps' in l]
if fpsl: print('  last fps line:', re.sub(r'^\[[^\]]*\] \[[^\]]*\] \[[^\]]*\] \[[^\]]*\] ', '', fpsl[-1])[:120])
# running?
running = False
for p in subprocess.run(['pgrep', '-x', 'nhllegacy'], capture_output=True, text=True).stdout.split():
    try:
        cwd = os.readlink(f'/proc/{p}/cwd')
        if inst is None or os.path.realpath(cwd) == os.path.realpath(inst): running = True
    except OSError: pass
print('game process:', 'running' if running else 'not running')
errs = {}
for x, l in tl:
    if re.search(r'\[(error|critical)\]', l):
        k = re.sub(r'^\[[^\]]*\] ', '', re.sub(r'\[t\d+\]', '', l))[:150]; errs[k] = errs.get(k, 0) + 1
if errs:
    print(f'[error]/[critical] lines ({len(errs)} distinct):')
    for k, n in list(errs.items())[:6]: print(f'  {n:4d}x {k}')
extra = []
if inst:
    for f in glob.glob(inst + '/core.*') + glob.glob(inst + '/nullcall_ctx.txt'):
        if os.path.getmtime(f) >= first - 5: extra.append(f)
    for f in extra:
        print('fresh file:', f, '' if 'core' in f else '-> ' + open(f, errors='replace').read(300).strip().replace('\n', ' | '))
try:
    dm = subprocess.run(['dmesg', '-T'], capture_output=True, text=True, timeout=5).stdout.splitlines()
    hits = [l for l in dm if re.search(r'nhllegacy.*(segfault|trap|general protection)|Out of memory|oom-kill', l)][-3:]
    for l in hits: print('dmesg:', l[:160])
except Exception: pass
print('last lines:')
for x, l in tl[-a.tail:]: print('  ' + l[:170])
dur = last - first
if running and age < 30: v = 'RUNNING'
elif reached['main menu (quick done)'] is None and (gp is None): v = 'BOOT-STALL (log never reached the main menu)' if dur > 60 else 'CRASH (died during boot)'
elif gp is None: v = 'NO-GAMEPLAY (menu reached, no match with >1000 draws)'
elif any('shutting down' in l or 'Title terminated' in l for _, l in tl[-8:]): v = 'OK (clean shutdown: window closed / title terminated)'
elif a.dur and dur < a.dur - 15 and not running: v = f'CRASH (log stops at {dur:.0f} s, expected {a.dur:.0f} s)'
elif extra and any('core' in f for f in extra): v = 'CRASH (core file)'
else: v = 'OK (ran %.0f s into gameplay window, ended without a visible fault; pass --dur to check the length)' % ((last - gp) if gp else 0)
print('VERDICT:', v)
sys.exit(0 if v.startswith(('OK', 'RUNNING')) else 1)
