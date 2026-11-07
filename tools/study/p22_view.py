#!/usr/bin/env python3
"""Print the lifted C++ of one recompiled function with names from a side database (nothing is written to git or to generated/).

Usage: python3 -I tools/study/p22_view.py ADDR [ADDR...] [--names /path/not-in-git/names.tsv] [--gen generated/default] [--min-conf 0]
  env P22_NAMES / P22_GEN give the defaults. Adds a '// name (cN)' header and annotates each 'bl 0x...' / 'sub_...' call with the callee's name.
names.tsv: address<TAB>name<TAB>conf<TAB>evidence<TAB>detail (p22_names.py output; keep it out of git).
No names are stored here: this tool only reads the database you point it at.
"""
import os, re, sys, glob, argparse, subprocess

def load(path, minc):
    nm = {}
    for line in open(path):
        if line.startswith("#"): continue
        t = line.rstrip("\n").split("\t")
        if len(t) >= 4 and t[3] != "-" and int(t[2]) >= minc:
            nm[int(t[0], 16)] = "%s (c%s)" % (t[1], t[2])
    return nm

def find(gen, addr):
    pat = "DEFINE_REX_FUNC(sub_%08X)" % addr
    r = subprocess.run(["grep", "-l", "-F", pat] + sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))),
                       capture_output=True, text=True)
    return (r.stdout.split() or [None])[0], pat

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", nargs="+")
    ap.add_argument("--names", default=os.environ.get("P22_NAMES", ""))
    ap.add_argument("--gen", default=os.environ.get("P22_GEN", "generated/default"))
    ap.add_argument("--min-conf", type=int, default=0)
    a = ap.parse_args()
    nm = load(a.names, a.min_conf) if a.names else {}
    rx = re.compile(r"(?:0x|sub_)([0-9A-Fa-f]{8})")
    for s in a.addr:
        addr = int(s.replace("sub_", "").replace("0x", ""), 16)
        f, pat = find(a.gen, addr)
        if not f:
            print("// %08X: not found" % addr); continue
        out, on = [], False
        for line in open(f):
            if not on:
                on = pat in line
                if on: out.append("// %08X %s\n" % (addr, nm.get(addr, "unnamed")))
            if on:
                if line.startswith("}") : out.append(line); break
                if line.lstrip().startswith("//") and (" bl " in line or " b " in line):
                    m = rx.search(line)
                    if m and int(m.group(1), 16) in nm: line = line.rstrip("\n") + "   <" + nm[int(m.group(1), 16)] + ">\n"
                out.append(line)
        sys.stdout.write("".join(out))

if __name__ == "__main__":
    main()
