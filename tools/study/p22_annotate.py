#!/usr/bin/env python3
"""Annotate 'sub_XXXXXXXX' tokens in text (gdb backtraces, crash logs, guest_stacks) with names from names.tsv.

Usage:
  gdb -batch -ex bt ... | python3 -I tools/study/p22_annotate.py /path/not-in-git/p-names/names.tsv [--min-conf 1] [--replace]
  python3 -I tools/study/p22_annotate.py names.tsv < bt.txt
Default output appends: sub_82670A78{k_cv_wait_82670A78,c3}.  --replace swaps the token for the name.
Also works after tools/annotate_guest_stacks.py (which emits 'AAAAAAAA(sub_BBBBBBBB+off)').
names.tsv columns: address, name, confidence, evidence, detail (made by p22_names.py; keep it out of git).
"""
import re, sys, argparse

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names"); ap.add_argument("--min-conf", type=int, default=0)
    ap.add_argument("--replace", action="store_true")
    a = ap.parse_args()
    nm = {}
    for line in open(a.names):
        if line.startswith("#"):
            continue
        t = line.rstrip("\n").split("\t")
        if len(t) >= 4 and t[3] != "-" and int(t[2]) >= a.min_conf:
            nm[int(t[0], 16)] = (t[1], t[2])
    rx = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")
    def f(m):
        r = nm.get(int(m.group(1), 16))
        if not r:
            return m.group(0)
        return r[0] if a.replace else "%s{%s,c%s}" % (m.group(0), r[0], r[1])
    for line in sys.stdin:
        sys.stdout.write(rx.sub(f, line))

if __name__ == "__main__":
    main()
