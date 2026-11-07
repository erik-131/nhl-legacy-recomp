#!/usr/bin/env python3
"""P22 task 2: how much of the recompiled code is third-party/library, by family.

Usage (E-cores):
  P22_IMAGE=/path/image.bin P22_GEN=generated/default \
    taskset -c 4-5 nice -n 19 ionice -c3 python3 -I tools/study/p22_libshare.py \
      [--window 65536] [--out /path/not-in-git/func_labels.tsv]

Method (all heuristic, see report for uncertainty):
 1. Extract ASCII strings from .rdata/.data of the decompressed PE image.
 2. Classify each string to a family with generic keyword/path rules (RULES below:
    library names such as EASTL, Speex, Lynx, Blaze/OSDK, rw::, xdk paths, CRT
    messages, and game package paths such as nhl\\gameplay, nhl\\fe).
 3. Linear scan of .text (big-endian PPC words) with a tiny register tracker that
    resolves lis+addi / lis+ori / lis+load/store pairs into absolute addresses
    that point into .rdata/.data  ("xrefs").
 4. A reference gets a label (a) directly from the string it points into, or
    (b) by adjacency: the nearest classified string before and after in the same
    section agree, and are within --window bytes (linkers lay each object file's
    .rdata/.data out contiguously, so neighbours usually come from the same object).
 5. A function takes the label with most weight (direct 3, adjacency 1).
 6. Unlabelled functions between two labelled functions (in address order) with the
    same label inherit it (object files are contiguous in .text) -- "interpolated".
Outputs % of functions and % of code bytes (size from .pdata else gap to next start)
per family and per evidence class. No string text or addresses are printed.
--out writes "start<TAB>family<TAB>evidence" per function (keep out of git).
"""
import os, re, sys, bisect, argparse, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C

# (family, regex). First match wins. Generic library / package names only.
RULES = [
    ("xdk",      r"xdksep|\\xdk\\|xgraphics|\bD3D[A-Z0-9_]\w*|D3DX|xam\.xex|xboxkrnl|\bXam[A-Z]|\bXAudio|\bXMA|\bXInput|\bXUI\b|\bXNet|\bXLive"),
    ("eastl",    r"\bEASTL\b|eastl::|\\eastl\\"),
    ("speex",    r"speex"),
    ("lynx",     r"lynx"),
    ("eabase",   r"EA::(Thread|StdC|IO|Allocator|Jobs|Memory)|\bEAThread|\bEAStdC|\\ea(thread|stdc|io|base)\\"),
    ("audiolib", r"EAAudioCore|EA::Audio|AudioFramework|AudioControl::|Audio::Event|\\wf\\audio\\"),
    ("blaze",    r"Blaze::|\bBlaze|OSDK|dirtysock|DirtySock|\\wf\\online\\|CARDHOUSE|CardHouse|EA::Store|\bDIME\b|DIME::"),
    ("rw",       r"\brw::|renderware|RenderWare|\\wf\\sys\\|rwmath|MemoryFramework|\bRW[A-Z]\w+"),
    ("ant",      r"\\wf\\animation\\|\bANT\b|ant::|antcommon"),
    ("crt",      r"Visual C\+\+ CRT|runtime error|R60\d\d|_strerror|\bstrerror|MSVCRT|_CrtDbg"),
    ("game_gameplay", r"\\nhl\\gameplay\\|nhlgameplay|\\nhlsystems\\|nhlmemory"),
    ("game_frontend", r"\\nhl\\fe\\|nhlfrontend|leaguelogic|LEAGUELOGIC|LeagueManager|GmLogic|\bLEAGUE::"),
    ("game_online",   r"\\nhl\\online\\|nhlonline|CPreGame"),
    ("game_render",   r"nhlrender|\bRENDER::|\\nhl\\render"),
    ("game_other",    r"\\p4\\nhl\\|^g[A-Z][A-Za-z0-9]{3,}$|\bSkater|\bGoalie|Hik\w+|TeamStrategy"),
]
RULES = [(f, re.compile(r, re.I if f in ("lynx", "speex", "xdk") else 0)) for f, r in RULES]
LIBS = {"xdk", "eastl", "speex", "lynx", "eabase", "audiolib", "blaze", "rw", "ant", "crt"}


def classify(s):
    for f, rx in RULES:
        if rx.search(s):
            return f
    return None


def xrefs(img, starts, lo, hi):
    """{func_start: [target,...]} for lis+addi/ori/load/store pairs resolving into [lo,hi)."""
    tbase, raw = C.text_words(img)
    tend = tbase + len(raw)
    out = {}
    calls = {}
    for i, a in enumerate(starts):
        b = starts[i + 1] if i + 1 < len(starts) else tend
        b = min(b, tend)
        if a < tbase or a >= b:
            continue
        hiv = {}
        tg = []
        cl = set()
        for off in range((a - tbase), (b - tbase) - 3, 4):
            w = int.from_bytes(raw[off:off + 4], "big")
            op = w >> 26
            if op == 15:   # addis
                rd = (w >> 21) & 31; ra = (w >> 16) & 31; im = w & 0xFFFF
                if im & 0x8000: im -= 0x10000
                if ra == 0: hiv[rd] = (im << 16) & 0xFFFFFFFF
                elif ra in hiv: hiv[rd] = (hiv[ra] + (im << 16)) & 0xFFFFFFFF
                else: hiv.pop(rd, None)
            elif op == 14:  # addi
                rd = (w >> 21) & 31; ra = (w >> 16) & 31; im = w & 0xFFFF
                if im & 0x8000: im -= 0x10000
                if ra in hiv and ra != 0:
                    v = (hiv[ra] + im) & 0xFFFFFFFF
                    if lo <= v < hi: tg.append(v)
                    hiv[rd] = v
                else: hiv.pop(rd, None)
            elif op == 24:  # ori
                rs = (w >> 21) & 31; ra = (w >> 16) & 31
                if rs in hiv:
                    v = hiv[rs] | (w & 0xFFFF)
                    if lo <= v < hi: tg.append(v)
                    hiv[ra] = v
                else: hiv.pop(ra, None)
            elif 32 <= op <= 55:  # load/store D-form
                ra = (w >> 16) & 31; rd = (w >> 21) & 31; im = w & 0xFFFF
                if im & 0x8000: im -= 0x10000
                if ra in hiv and ra != 0:
                    v = (hiv[ra] + im) & 0xFFFFFFFF
                    if lo <= v < hi: tg.append(v)
                if op in (32, 33, 34, 35, 40, 41, 42, 43): hiv.pop(rd, None)
            elif op == 18:  # b/bl: record call edge; bl clobbers
                li = w & 0x03FFFFFC
                if li & 0x02000000: li -= 0x04000000
                tgt = (tbase + off + li) & 0xFFFFFFFF
                if not (w & 2) and not (a <= tgt < b): cl.add(tgt)
                if w & 1: hiv.clear()
            elif op == 19 and (w & 1):
                hiv.clear()
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo == 444 and ((w >> 21) & 31) == ((w >> 11) & 31):  # mr
                    rs = (w >> 21) & 31; ra = (w >> 16) & 31
                    if rs in hiv: hiv[ra] = hiv[rs]
                    else: hiv.pop(ra, None)
        out[a] = tg
        calls[a] = cl
    return out, calls


def interpolate(starts, fam, ev):
    labelled = [i for i, s in enumerate(starts) if s in fam]
    n = 0
    for k in range(len(labelled) - 1):
        l, r = labelled[k], labelled[k + 1]
        if fam[starts[l]] == fam[starts[r]]:
            fl = fam[starts[l]]
            for i in range(l + 1, r):
                s = starts[i]
                if s not in fam:
                    fam[s] = fl; ev[s] = "interpolated"; n += 1
    return n


def callgraph_fill(starts, calls, fam, ev, rounds=3):
    callers = collections.defaultdict(set)
    for a, cs in calls.items():
        for t in cs: callers[t].add(a)
    n = 0
    for _ in range(rounds):
        add = {}
        for s in starts:
            if s in fam: continue
            cs = callers.get(s)
            if not cs: continue
            labs = set(fam.get(c) for c in cs)
            if len(labs) == 1 and None not in labs:
                add[s] = labs.pop()
        for s, f in add.items(): fam[s] = f; ev[s] = "callgraph"
        n += len(add)
        if not add: break
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image"); ap.add_argument("--gen")
    ap.add_argument("--window", type=int, default=65536)
    ap.add_argument("--out")
    ap.add_argument("--anchor-range", action="append", default=[],
                    help="FAMILY:LOHEX-HIHEX, a range known from elsewhere to be one library (repeatable); "
                         "applied after xref labelling, overrides it")
    a = ap.parse_args()
    img = C.Image(a.image)
    starts = C.func_starts(a.gen)
    sizes = C.func_sizes(img, starts)

    # strings -> classified anchors
    S = list(C.ascii_strings(img))
    anchors = []  # (va, end, family)
    nclass = collections.Counter()
    for va, s in S:
        f = classify(s)
        if f:
            anchors.append((va, va + len(s) + 1, f)); nclass[f] += 1
    anchors.sort()
    avas = [x[0] for x in anchors]
    print("strings: %d, classified anchors: %d" % (len(S), len(anchors)))
    print("anchors per family:", dict(nclass))

    lo = img.base; hi = img.section(".data")[1] + img.section(".data")[2]
    refs, calls = xrefs(img, starts, lo, hi)
    nref = sum(len(v) for v in refs.values())
    print("resolved data xrefs: %d in %d functions (%.1f%% of functions have >=1)" %
          (nref, sum(1 for v in refs.values() if v), 100 * sum(1 for v in refs.values() if v) / len(starts)))

    def section_of(v):
        for n, va, vs, rs, rp, fl in img.sections:
            if va <= v < va + vs: return n
        return None

    def label(t):
        """-> (family, 'direct'|'adj') or None"""
        i = bisect.bisect_right(avas, t) - 1
        if i >= 0 and anchors[i][0] <= t < anchors[i][1]:
            return anchors[i][2], "direct"
        j = i + 1
        if i >= 0 and j < len(anchors):
            p, n = anchors[i], anchors[j]
            if p[2] == n[2] and t - p[1] <= a.window and n[0] - t <= a.window and section_of(p[0]) == section_of(n[0]) == section_of(t):
                return p[2], "adj"
        return None

    fam = {}
    ev = {}
    conflicts = multi = 0
    for s in starts:
        w = collections.Counter(); dl = set()
        for t in refs.get(s, ()):
            r = label(t)
            if r:
                w[r[0]] += 3 if r[1] == "direct" else 1
                if r[1] == "direct": dl.add(r[0])
        if w:
            fam[s] = w.most_common(1)[0][0]
            ev[s] = "direct" if dl else "adjacent"
            if len(dl) >= 1 and len(w) >= 1:
                multi += 1
                if len(w) > 1: conflicts += 1
    n_direct = len(fam)
    print("functions labelled by xref evidence: %d (%.1f%%); with >1 distinct label among labelled refs: %d (%.1f%% of labelled)" %
          (n_direct, 100 * n_direct / len(starts), conflicts, 100 * conflicts / max(1, n_direct)))
    nanchor = 0
    for spec in a.anchor_range:
        f, rng = spec.split(":"); l_, h_ = [int(x, 16) for x in rng.split("-")]
        for s in starts:
            if l_ <= s <= h_:
                fam[s] = f; ev[s] = "anchor"; nanchor += 1
    print("functions set by --anchor-range: %d" % nanchor)
    base_fam = dict(fam)
    interp = interpolate(starts, fam, ev)
    print("interpolated between same-label neighbours: %d" % interp)
    cg = callgraph_fill(starts, calls, fam, ev)
    print("filled from call graph (all labelled callers agree): %d" % cg)
    # hold-out test of the interpolation: hide 20%% of the xref-labelled functions, re-interpolate, compare
    import random
    random.seed(1)
    ho = set(s for s in base_fam if random.random() < 0.2)
    f2 = {s: f for s, f in base_fam.items() if s not in ho}
    e2 = {s: "x" for s in f2}
    interpolate(starts, f2, e2)
    got = [s for s in ho if s in f2]
    ok = sum(1 for s in got if f2[s] == base_fam[s])
    okl = sum(1 for s in got if (f2[s] in LIBS) == (base_fam[s] in LIBS))
    print("hold-out (20 pct of xref-labelled hidden): interpolation covered %.1f%%, exact family right %.1f%%, lib-vs-game right %.1f%%" %
          (100 * len(got) / max(1, len(ho)), 100 * ok / max(1, len(got)), 100 * okl / max(1, len(got))))
    tot_f = len(starts); tot_b = sum(v[0] for v in sizes.values())
    rows = collections.defaultdict(lambda: [0, 0, collections.Counter()])
    for s in starts:
        f = fam.get(s, "unlabelled"); e = ev.get(s, "-")
        r = rows[f]; r[0] += 1; r[1] += sizes[s][0]; r[2][e] += 1
    print("\n%-14s %8s %7s %10s %7s   evidence direct/adjacent/interpolated/callgraph/anchor" % ("family", "funcs", "%funcs", "bytes", "%bytes"))
    for f, (n, b, e) in sorted(rows.items(), key=lambda x: -x[1][1]):
        print("%-14s %8d %6.1f%% %10d %6.1f%%   %d/%d/%d/%d" % (f, n, 100 * n / tot_f, b, 100 * b / tot_b, e["direct"], e["adjacent"], e["interpolated"], e["callgraph"]) + ("/%d" % e["anchor"]))
    lib = [sum(rows[f][i] for f in LIBS if f in rows) for i in (0, 1)]
    game = [sum(v[i] for k, v in rows.items() if k.startswith("game")) for i in (0, 1)]
    unl = rows["unlabelled"][:2]
    print("\nLIBRARY total : %d funcs %.1f%%, %.1f%% bytes" % (lib[0], 100 * lib[0] / tot_f, 100 * lib[1] / tot_b))
    print("GAME labelled : %d funcs %.1f%%, %.1f%% bytes" % (game[0], 100 * game[0] / tot_f, 100 * game[1] / tot_b))
    print("UNLABELLED    : %d funcs %.1f%%, %.1f%% bytes" % (unl[0], 100 * unl[0] / tot_f, 100 * unl[1] / tot_b))
    if a.out:
        with open(a.out, "w") as o:
            for s in starts: o.write("%08x\t%s\t%s\n" % (s, fam.get(s, "unlabelled"), ev.get(s, "-")))


if __name__ == "__main__":
    main()
