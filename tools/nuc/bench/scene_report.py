#!/usr/bin/env python3
"""scene_report.py <game.log> : seconds from process start (first log line) to Lua scene marks, scene switches and the ice."""
import sys, re, datetime
L = open(sys.argv[1], errors='replace').read().splitlines()
def ts(l): return datetime.datetime.strptime(l[1:24], '%Y-%m-%d %H:%M:%S.%f').timestamp()
t0 = ts(L[0])
print(f"process start {L[0][1:24]}")
for l in L:
    m = re.search(r"path='LUAMARK_(SC_[^']*)'", l)
    if m and 'FAILED' in l: print(f"+{ts(l)-t0:6.1f} s  lua   {m[1]}")
    m = re.search(r'\[nhl-scene\] (.*)', l)
    if m: print(f"+{ts(l)-t0:6.1f} s  scene {m[1]}")
    m = re.search(r'\[nhl-(vpad|shot)\][^\n]*', l)
    if m and 'shot' in m[0]: print(f"+{ts(l)-t0:6.1f} s  {m[0][:90]}")
g = [(ts(l), int(re.search(r' draws/frame=(\d+)', l)[1])) for l in L if 'nhl-vk-fps' in l]
try:
    s = next(i for i in range(len(g) - 3) if all(p[1] > 1000 for p in g[i:i + 3]))
    print(f"+{g[s][0]-t0:6.1f} s  ICE (draws/frame > 1000 for 3 s)")
except StopIteration:
    print("ICE not reached")

# seconds-to-scene for a preset (argv[2]): live/shootout/flyover = first >1000 draws/frame (arena on screen), closeup = the Lua camera
# mark, replay/goal/wipe = the native "[nhl-scene] reached <name>" line written by src/scene_force.cpp when the scene is on screen.
if len(sys.argv) > 2:
    pre = sys.argv[2]; res = None
    if pre == 'play':
        for l in L:
            if '[nhl-scene] reached play' in l: res = ts(l) - t0; break
    elif pre in ('replay', 'goal', 'wipe'):
        for l in L:
            if f'[nhl-scene] reached {pre}' in l: res = ts(l) - t0; break
    elif pre == 'closeup':
        for l in L:
            if "LUAMARK_SC_CAM_" in l and 'FAILED' in l: res = ts(l) - t0; break
    else:
        try: res = g[s][0] - t0
        except Exception: pass
    print(f"PRESET {pre}: " + (f"{res:.1f} s from process start" if res is not None else "scene NOT reached"))
