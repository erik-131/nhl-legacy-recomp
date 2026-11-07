#!/usr/bin/env python3
"""P22 names: assign names to recompiled functions from evidence in the image (source level L1,
"annotated recomp: names in a side database").  Contains no game data: every name is derived at run time
from the image and from repo docs/sources the caller points at.  Keep the OUTPUT out of git.

Usage (E-cores, low priority):
  P22_IMAGE=/path/image.bin P22_GEN=/opt/nhl/nhl-legacy-recomp/generated/default \
    nice -n 19 taskset -c 12-13 python3 -I tools/study/p22_names.py --out /path/not-in-git/p-names \
      [--repo .] [--known-overrides /path/known_overrides.tsv] [--window 65536]

Outputs in --out:
  names.tsv        address<TAB>name<TAB>confidence(0-3)<TAB>evidence kind<TAB>detail
                   (every recompiled function appears; unnamed ones have name sub_XXXXXXXX, conf 0, kind '-')
  coverage.txt     % of functions and of code bytes named, per evidence kind, confidence, module
  top_unnamed.tsv  most-called unnamed functions (distinct callers), candidates to name next
  sources.tsv      distinct source paths found as strings (path, module, #functions anchored)

Evidence kinds (best one decides the name; all kinds seen are listed in the detail column):
  known      conf 3  function mentioned in src/ or docs/ with a nearby comment, or in --known-overrides
  mentioned  conf 2  bare mention of sub_XXXXXXXX (no description) in src/docs
  srcfile    conf 2  function loads the address of a '<drive>:\\...\\X.cpp' path string (assert/__FILE__)
                     -> name <X>_<addr>.  Header paths (.h/.inl) give conf 1 (inlined code).
  d3d9       conf 2  inside the D3D9 translation unit range (--d3d-range, default 0x827DC1A0-0x827F9DC8)
  tunable    conf 2 if >=3 refs to tunables of one category, else 1: name tun_<Category>_<addr>
  funcstr    conf 1  function references a 'Class::Method'-looking string used by <=3 functions
  family     conf 1  xref to a library-family string (p22_libshare.RULES) -> <family>_<addr>
  srcfile_prop / family_prop  conf 0  neighbour in address order with the same file/family on both sides
                     (object files are contiguous), or all callers share one file/family (call graph)
RTTI: the image contains no '.?AV' type descriptors (stripped), so there is no RTTI evidence.
"""
import os, re, sys, bisect, argparse, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C
import p22_libshare as L

PATH_RX = re.compile(r"[A-Za-z]:\\[^\x00]*\\([^\\]+)\.(cpp|c|cc|h|hpp|inl)$", re.I)
PATH_RX2 = re.compile(r"(?:^|[^\\])((?:[\w.\-]+\\)+)([\w.\-]+)\.(cpp|c|cc|h|hpp|inl)$", re.I)
FUNC_RX = re.compile(r"^[A-Za-z_]\w*(?:::~?\w+)+$")
SUB_RX = re.compile(r"sub_(8[0-9A-Fa-f]{7})")
SLUG_RX = re.compile(r"sub_8[0-9A-Fa-f]{7}`?\s*(?:=|\(|—|--)\s*\**([A-Za-z][A-Za-z0-9 _\-/]{3,40})")
STOP = ("it ", "the ", "a ", "an ", "is ", "was ", "wrapped", "which", "that", "and ", "or ", "of ", "in ", "after", "from")
PKG_TOKENS = ("nhlgameplay", "nhlfrontend", "nhlonline", "nhlrender", "BlazeSDK", "OSDK", "EAStore", "Lynx",
              "MemoryFramework", "EAAudioCore", "ant", "serialization_rwmath")


def san(s):
    return re.sub(r"[^A-Za-z0-9_]+", "_", s).strip("_")


def module_of_path(p):
    low = p.lower()
    for t in PKG_TOKENS:
        if "\\" + t.lower() + "\\" in low or low.startswith(t.lower() + "\\"):
            return t.lower()
    parts = p.split("\\")
    return san(parts[-2]).lower() if len(parts) > 1 else "other"


def find_known(repo, overrides):
    """{addr: (slug, where)} from sub_8xxxxxxx mentions in src/ and docs/ (not docs/nhl12*) plus overrides tsv."""
    out = {}
    for top in ("src", "docs"):
        for dp, dn, fn in os.walk(os.path.join(repo, top)):
            for f in fn:
                if not f.endswith((".cpp", ".h", ".md", ".txt")) or f.startswith("tunable"):
                    continue
                p = os.path.join(dp, f)
                if "nhl12-decomp" in p:
                    continue
                for ln, line in enumerate(open(p, errors="replace"), 1):
                    for m in SUB_RX.finditer(line):
                        a = int(m.group(1), 16)
                        s = SLUG_RX.search(line[m.start():])
                        txt = s.group(1).strip().lower() if s else ""
                        if txt.startswith(STOP) or len(txt.split()) > 5:
                            txt = ""
                        slug = san(txt)[:32]
                        where = "%s:%d" % (os.path.relpath(p, repo), ln)
                        if a not in out or (slug and not out[a][0]):
                            out[a] = (slug, where)
    if overrides and os.path.exists(overrides):
        for line in open(overrides):
            t = line.rstrip("\n").split("\t")
            if len(t) >= 2 and not line.startswith("#"):
                out[int(t[0], 16)] = (san(t[1]), "override")
    return out


def load_tunables(repo):
    cat = {}
    for line in open(os.path.join(repo, "docs/tunables_schema.tsv"), errors="replace"):
        t = line.split("\t")
        if len(t) > 1:
            cat[t[0]] = t[1] or "Misc"
    addr = {}
    rx = re.compile(r"^\s*(\w+)\s+\[located @([0-9A-Fa-f]{8})")
    for line in open(os.path.join(repo, "docs/tunable_values_runtime.txt"), errors="replace"):
        m = rx.match(line)
        if m:
            addr[int(m.group(2), 16)] = (m.group(1), cat.get(m.group(1), "Misc"))
    return addr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image"); ap.add_argument("--gen"); ap.add_argument("--repo", default=".")
    ap.add_argument("--out", required=True)
    ap.add_argument("--known-overrides")
    ap.add_argument("--window", type=int, default=65536)
    ap.add_argument("--d3d-range", default="827DC1A0-827F9DC8")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    img = C.Image(a.image)
    starts = C.func_starts(a.gen)
    sizes = C.func_sizes(img, starts)
    sset = set(starts)
    N = len(starts)
    lo = img.base; hi = img.section(".data")[1] + img.section(".data")[2]
    refs, calls = L.xrefs(img, starts, lo, hi)
    callers = collections.defaultdict(set)
    for f, cs in calls.items():
        for t in cs:
            if t in sset and t != f:
                callers[t].add(f)

    # ---- strings
    S = list(C.ascii_strings(img, minlen=4))
    paths = []   # (va, end, path, ext)
    fstr = {}    # va -> (va_end, text)
    for va, s in S:
        m = PATH_RX.search(s)
        if m:
            p = s[m.start():]
            paths.append((va + m.start(), va + len(s) + 1, p, m.group(2).lower()))
            continue
        if FUNC_RX.match(s) and len(s) >= 6:
            fstr[va] = (va + len(s) + 1, s)
    paths.sort()
    pva = [p[0] for p in paths]
    fva = sorted(fstr)
    fam_anchor = []
    for va, s in S:
        f = L.classify(s)
        if f:
            fam_anchor.append((va, va + len(s) + 1, f))
    fam_anchor.sort()
    fav = [x[0] for x in fam_anchor]

    def at(sorted_va, items, t):
        i = bisect.bisect_right(sorted_va, t) - 1
        if i >= 0 and t < (items[i][1] if isinstance(items, list) else items[sorted_va[i]][0]):
            return i
        return -1

    # ---- per function evidence
    name = {}; conf = {}; kind = {}; detail = {}; modl = {}; allk = collections.defaultdict(set)

    def setn(s, n, c, k, d, m):
        allk[s].add(k)
        if s not in conf or c > conf[s]:
            name[s] = n; conf[s] = c; kind[s] = k; detail[s] = d; modl[s] = m

    srcfile = {}      # s -> (stem, module, ext)
    path_hits = collections.Counter()
    tun = load_tunables(a.repo)
    tkeys = sorted(tun)
    funcstr_users = collections.defaultdict(set)
    fam_direct = {}
    for s in starts:
        tun_cat = collections.Counter(); files = collections.Counter(); fs = set()
        for t in refs.get(s, ()):
            i = at(pva, paths, t)
            if i >= 0:
                p = paths[i]
                stem = san(PATH_RX.search(p[2]).group(1))
                files[(stem, module_of_path(p[2]), p[3], p[0])] += 1
            j = bisect.bisect_right(fva, t) - 1
            if j >= 0 and t < fstr[fva[j]][0]:
                fs.add(fva[j]); funcstr_users[fva[j]].add(s)
            if t in tun:
                tun_cat[tun[t][1]] += 1
        if files:
            # prefer .cpp over header; majority
            best = sorted(files.items(), key=lambda kv: (kv[0][2] not in ("cpp", "c", "cc"), -kv[1]))[0][0]
            ext = best[2]
            srcfile[s] = (best[0], best[1], ext)
            path_hits[best[3]] += 1
            setn(s, "%s_%08X" % (best[0], s), 2 if ext in ("cpp", "c", "cc") else 1, "srcfile",
                 "%s.%s refs=%d" % (best[0], ext, sum(files.values())), best[1])
        if tun_cat:
            c, n = tun_cat.most_common(1)[0]
            setn(s, "tun_%s_%08X" % (san(c), s), 2 if n >= 3 else 1, "tunable", "%s x%d (%d tunables)" % (c, n, sum(tun_cat.values())), "tunables")
        if fs:
            allk[s].add("funcstr")
        w = collections.Counter(); dl = set()
        for t in refs.get(s, ()):
            i = bisect.bisect_right(fav, t) - 1
            if i >= 0 and fam_anchor[i][0] <= t < fam_anchor[i][1]:
                w[fam_anchor[i][2]] += 3; dl.add(fam_anchor[i][2])
            elif 0 <= i < len(fam_anchor) - 1:
                p, n = fam_anchor[i], fam_anchor[i + 1]
                if p[2] == n[2] and t - p[1] <= a.window and n[0] - t <= a.window:
                    w[p[2]] += 1
        if w:
            fam_direct[s] = w.most_common(1)[0][0]
    for s, f in fam_direct.items():
        setn(s, "%s_%08X" % (f, s), 1, "family", f, f)
    # funcstr: naming pass (needs the user counts)
    for s in starts:
        best = None
        for t in refs.get(s, ()):
            j = bisect.bisect_right(fva, t) - 1
            if j >= 0 and t < fstr[fva[j]][0] and len(funcstr_users[fva[j]]) <= 3:
                best = fstr[fva[j]][1]; break
        if best:
            allk[s].add("funcstr")
            if conf.get(s, 0) <= 1:
                m0 = modl.get(s, "funcstr")
                conf.pop(s, None)
                setn(s, "%s_%08X" % (san(best), s), 1, "funcstr", best, m0)

    # ---- d3d9 block
    dl_, dh_ = [int(x, 16) for x in a.d3d_range.split("-")]
    for s in starts:
        if dl_ <= s <= dh_:
            setn(s, "d3d9_%08X" % s, 2, "d3d9", "range %s" % a.d3d_range, "d3d9")

    # ---- propagation (srcfile + family), never overrides a direct name
    def prop(label_of, kindname, fmt, modof):
        lab = {s: label_of[s] for s in starts if s in label_of}
        idx = [i for i, s in enumerate(starts) if s in lab]
        n = 0
        for k in range(len(idx) - 1):
            l, r = idx[k], idx[k + 1]
            if lab[starts[l]] == lab[starts[r]]:
                for i in range(l + 1, r):
                    s = starts[i]
                    if s not in conf:
                        setn(s, fmt(lab[starts[l]], s), 0, kindname, "between-neighbours", modof(lab[starts[l]])); n += 1
        for _ in range(3):
            add = {}
            for s in starts:
                if s in conf:
                    continue
                cs = callers.get(s)
                if not cs:
                    continue
                ls = set(lab.get(c) for c in cs)
                if len(ls) == 1 and None not in ls:
                    add[s] = ls.pop()
            for s, l in add.items():
                setn(s, fmt(l, s), 0, kindname, "callers-agree", modof(l)); lab[s] = l; n += 1
            if not add:
                break
        return n
    # srcfile labels: only .cpp anchors drive propagation
    sl = {s: (v[0], v[1]) for s, v in srcfile.items() if v[2] in ("cpp", "c", "cc")}
    n1 = prop(sl, "srcfile_prop", lambda l, s: "%s_%08X" % (l[0], s), lambda l: l[1])
    n2 = prop(fam_direct, "family_prop", lambda l, s: "%s_%08X" % (l, s), lambda l: l)

    # ---- known (highest)
    known = find_known(a.repo, a.known_overrides)
    nk = 0
    for ad, (slug, where) in known.items():
        if ad in sset:
            nm = "k_%s_%08X" % (slug, ad) if slug else "k_%08X" % ad
            # a bare mention (table row, list) is weaker than a mention with a description
            setn(ad, nm, 3 if slug else 2, "known" if slug else "mentioned", where, modl.get(ad, "known")); nk += 1

    for s in starts:
        if s not in conf:
            name[s] = "sub_%08X" % s; conf[s] = 0; kind[s] = "-"; detail[s] = ""; modl[s] = "unnamed"
    with open(os.path.join(a.out, "names.tsv"), "w") as o:
        o.write("# address\tname\tconfidence\tevidence\tdetail(kinds seen; ...)\n")
        for s in starts:
            d = detail[s]
            if len(allk[s]) > 1:
                d += " | kinds=" + ",".join(sorted(allk[s]))
            o.write("%08x\t%s\t%d\t%s\t%s\n" % (s, name[s], conf[s], kind[s], d))
    with open(os.path.join(a.out, "sources.tsv"), "w") as o:
        for va, end, p, ext in paths:
            o.write("%s\t%s\t%d\n" % (p, module_of_path(p), path_hits[va]))

    # ---- coverage
    totb = sum(v[0] for v in sizes.values())
    R = []
    def table(title, keyf):
        rows = collections.defaultdict(lambda: [0, 0])
        for s in starts:
            r = rows[keyf(s)]; r[0] += 1; r[1] += sizes[s][0]
        R.append("\n%s\n%-26s %8s %7s %10s %7s" % (title, "key", "funcs", "%funcs", "bytes", "%bytes"))
        for k, (n, b) in sorted(rows.items(), key=lambda x: -x[1][1]):
            R.append("%-26s %8d %6.1f%% %10d %6.1f%%" % (k, n, 100 * n / N, b, 100 * b / totb))
    named = [s for s in starts if kind[s] != "-"]
    R.append("functions %d, code bytes %d" % (N, totb))
    R.append("named (any evidence): %d funcs %.1f%%, %.1f%% bytes" % (len(named), 100 * len(named) / N,
             100 * sum(sizes[s][0] for s in named) / totb))
    for mn in (1, 2, 3):
        x = [s for s in starts if conf[s] >= mn and kind[s] != "-"]
        R.append("conf >= %d: %d funcs %.1f%%, %.1f%% bytes" % (mn, len(x), 100 * len(x) / N, 100 * sum(sizes[s][0] for s in x) / totb))
    R.append("distinct source paths: %d (cpp/c: %d); known entries applied: %d; prop srcfile %d family %d" %
             (len(paths), sum(1 for p in paths if p[3] in ("cpp", "c", "cc")), nk, n1, n2))
    table("by evidence kind", lambda s: kind[s])
    table("by confidence", lambda s: "conf%d%s" % (conf[s], "" if kind[s] != "-" else " (unnamed)"))
    table("by module", lambda s: modl[s])
    open(os.path.join(a.out, "coverage.txt"), "w").write("\n".join(R) + "\n")

    unn = [(len(callers[s]), s) for s in starts if kind[s] == "-"]
    unn.sort(reverse=True)
    with open(os.path.join(a.out, "top_unnamed.tsv"), "w") as o:
        o.write("# address\tdistinct_callers\tbytes\n")
        for c, s in unn[:500]:
            o.write("%08x\t%d\t%d\n" % (s, c, sizes[s][0]))
    print("\n".join(R[:9]))


if __name__ == "__main__":
    main()
