#!/usr/bin/env python3
"""P22 task 4: entry points for custom game modes, from strings and xrefs only.

Usage (E-cores):
  P22_IMAGE=/path/image.bin P22_GEN=generated/default \
    taskset -c 4-5 nice -n 19 ionice -c3 python3 -I tools/study/p22_modes.py \
      [--tunables docs/tunables_schema.tsv] [--show 12] [--out /path/not-in-git/modes.tsv]

For each category (CATS below) counts matching strings in .rdata/.data, the number of
functions that reference one of them (lis/addi xref scan, as in p22_libshare), the
address span of those functions, and the number of distinct source-file paths
involved. --tunables takes the repo's tunables schema (name<TAB>category<TAB>path...)
and counts matching tunable names too. --show prints that many example names per
category (short lists only; keep full lists out of git, use --out).
No runtime tracing, no code is emitted.
"""
import os, re, sys, bisect, argparse, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C
from p22_libshare import xrefs

CATS = collections.OrderedDict([
    ("mode_select",  r"GameMode|ModeSelect|GmMode|ModeData|PlayMode|GameType|\bMode(Type|Id)\b|game_mode"),
    ("league_logic", r"LeagueManager|LeagueLogic|LEAGUELOGIC|GmLogic|Franchise|Playoff|Tournament|Season(Mode|Type)|\bOrgRoster|Waiver|FreeAgent"),
    ("rules_settings", r"Offside|Icing|Penalt|Overtime|Shootout|NumPeriods|PeriodLen|PeriodTime|GameSetting|\bGSET|Fight|Fatigue|Rules\b"),
    ("onice_roster", r"SkatersOnIce|NumSkaters|NumPlayersOnIce|OnIce|3on3|4on4|5on5|LineChange|NumLines|MaxPlayers|RosterSize"),
])
CATS = {k: re.compile(v, re.I) for k, v in CATS.items()}
PATH = re.compile(r"^[A-Za-z]:\\.*\.(cpp|c|h)$", re.I)
TUN = {k: CATS[k] for k in ("rules_settings", "onice_roster", "mode_select")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image"); ap.add_argument("--gen"); ap.add_argument("--tunables")
    ap.add_argument("--show", type=int, default=0); ap.add_argument("--out")
    a = ap.parse_args()
    img = C.Image(a.image); starts = C.func_starts(a.gen)
    S = list(C.ascii_strings(img))
    hit = {k: [] for k in CATS}
    for va, s in S:
        if len(s) > 200: continue
        for k, rx in CATS.items():
            if rx.search(s): hit[k].append((va, s))
    lo = img.base; hi = img.section(".data")[1] + img.section(".data")[2]
    refs, _ = xrefs(img, starts, lo, hi)
    # per category: functions referencing a hit string (target inside [va, va+len])
    spans = {k: sorted((va, va + len(s) + 1) for va, s in v) for k, v in hit.items()}
    out = open(a.out, "w") if a.out else None
    print("%-15s %8s %9s %22s" % ("category", "strings", "ref.funcs", "func address span"))
    for k, sp in spans.items():
        starts_i = [x[0] for x in sp]
        fs = set()
        for f, tg in refs.items():
            for t in tg:
                i = bisect.bisect_right(starts_i, t) - 1
                if i >= 0 and t < sp[i][1]:
                    fs.add(f); break
        span = ("0x%08X-0x%08X" % (min(fs), max(fs))) if fs else "-"
        print("%-15s %8d %9d %22s" % (k, len(sp), len(fs), span))
        for va, s in hit[k][:a.show]:
            print("     e.g. %s" % s[:70])
        if out:
            for va, s in hit[k]: out.write("%s\t%08x\t%s\n" % (k, va, s))
    paths = [s for va, s in S if PATH.match(s.strip())]
    pk = collections.Counter()
    for s in paths:
        p = re.split(r"[\\/]", s.lower())
        pk["nhlfrontend" if "nhlfrontend" in p else "nhlgameplay" if "nhlgameplay" in p else
           "nhlonline" if "nhlonline" in p else "nhlrender" if any("render" in x for x in p) and "nhl" in p else "other"] += 1
    print("source-path strings: %d  by package: %s" % (len(paths), dict(pk)))
    if a.tunables:
        names = [l.split("\t")[0] for l in open(a.tunables, errors="replace") if l.strip()]
        print("tunables in schema: %d" % len(names))
        for k, rx in TUN.items():
            m = [n for n in names if rx.search(n)]
            print("  tunable names matching %-14s: %d" % (k, len(m)))
            for n in m[:a.show]: print("     e.g. %s" % n)
            if out:
                for n in m: out.write("tunable:%s\t-\t%s\n" % (k, n))
        for kw in ("Offside", "Icing", "Period", "Overtime", "Shootout", "Penalty", "NumPlayers", "OnIce"):
            print("  tunable names containing %-9s: %d" % (kw, sum(1 for n in names if kw.lower() in n.lower())))


if __name__ == "__main__":
    main()
