#!/usr/bin/env python3
"""P22 task 1: how big is the recompiled game? Counts only, no names.

Usage (run on E-cores):
  P22_IMAGE=/path/image.bin P22_GEN=generated/default \
    taskset -c 4-5 nice -n 19 ionice -c3 python3 -I tools/study/p22_census.py [--lines] [--tsv out.tsv]

Reports: entries in PPCFuncMappings, generated .cpp/.h file counts and (with --lines)
total lines/bytes, PE section sizes (code vs data), how many functions have a
.pdata length, sum of function bytes vs .text size, size histogram.
--tsv writes "start size has_pdata" per function (keep it out of git).
"""
import os, sys, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C

ap = argparse.ArgumentParser()
ap.add_argument("--image"); ap.add_argument("--gen"); ap.add_argument("--lines", action="store_true")
ap.add_argument("--tsv")
a = ap.parse_args()
gen = a.gen or C.GEN
img = C.Image(a.image)
starts = C.func_starts(gen)
sz = C.func_sizes(img, starts)

print("image base 0x%08x" % img.base)
tot = 0
print("%-9s %10s %10s %10s  flags" % ("section", "vsize", "rawsize", "va"))
code = data = 0
for n, va, vs, rs, rp, fl in img.sections:
    kind = "code" if fl & 0x20 else "data"
    print("%-9s %10d %10d 0x%08x %s" % (n, vs, rs, va, kind))
    if fl & 0x20: code += vs
    else: data += vs
print("code (flag CNT_CODE) %d B = %.1f MiB; non-code sections %d B = %.1f MiB (includes bss/reloc)" %
      (code, code / 2**20, data, data / 2**20))
print("functions in PPCFuncMappings: %d" % len(starts))
fb = sum(s for s, _ in sz.values())
pd = sum(1 for _, p in sz.values() if p)
t = img.section(".text")
print("functions with .pdata length: %d (%.1f%%)" % (pd, 100 * pd / len(starts)))
print("sum of function bytes %d = %.1f MiB = %.1f%% of .text (%d B)" % (fb, fb / 2**20, 100 * fb / t[2], t[2]))
for lo, hi in [(0, 32), (32, 64), (64, 128), (128, 256), (256, 1024), (1024, 4096), (4096, 1 << 30)]:
    c = [s for s, _ in sz.values() if lo <= s < hi]
    print("  size %5d-%-10d: %6d funcs (%.1f%%), %.1f%% of bytes" % (lo, hi, len(c), 100 * len(c) / len(starts), 100 * sum(c) / fb))
files = os.listdir(gen)
cpp = [f for f in files if f.endswith(".cpp")]; h = [f for f in files if f.endswith(".h")]
rec = [f for f in cpp if "_recomp." in f]
print("generated files: %d total, %d recomp .cpp, %d .h" % (len(files), len(rec), len([f for f in h if "_funcs." in f])))
if a.lines:
    L = B = 0
    for f in cpp + h:
        b = open(os.path.join(gen, f), "rb").read(); B += len(b); L += b.count(b"\n")
    print("generated lines (all .cpp/.h): %d, bytes %d (%.0f MiB)" % (L, B, B / 2**20))
if a.tsv:
    with open(a.tsv, "w") as o:
        for s in starts: o.write("%08x\t%d\t%d\n" % (s, sz[s][0], sz[s][1]))
