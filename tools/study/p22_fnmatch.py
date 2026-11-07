#!/usr/bin/env python3
"""P22 task 3: match functions between two Xbox 360 images by instruction pattern.

Usage (E-cores):
  # stats for one image (duplicate rate = how well the hash discriminates):
  P22_GEN=generated/default taskset -c 4-5 nice -n 19 ionice -c3 \
      python3 -I tools/study/p22_fnmatch.py --image A.bin --gen genA
  # compare two images (needs a function list for each, from the codegen output
  # or any file with one hex start address per line):
  python3 -I tools/study/p22_fnmatch.py --image A.bin --gen genA --image-b B.bin --gen-b genB \
      [--min-insns 8] [--dump matches.tsv]

Per function two hashes over its instruction words:
  exact : opcode + register fields + extended opcode kept; immediates, branch
          displacements, SPR numbers masked (so code at other addresses, with other
          constants/struct offsets, still hashes the same).
  fuzzy : primary opcode (+ extended opcode for op 4/19/31/59/63) only; registers
          and immediates ignored (survives register allocation changes).
Function length = .pdata length, else gap to the next start. Functions shorter than
--min-insns are skipped (too generic). A match is only counted when the hash is
UNIQUE in both images (1:1), and also reported as "any" (hash present in B).
Writes no names. --dump writes addresses and keep it out of git.
"""
import os, sys, argparse, hashlib, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C

# primary opcodes whose low 16 bits are an immediate/displacement
IMM16 = {7, 8, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29} | set(range(32, 56)) | {46, 47, 58, 62}
EXT = {4, 19, 31, 59, 63}


def norm(w):
    op = w >> 26
    if op in (16,):        # bc: keep BO,BI, mask displacement and AA/LK abs bit
        return w & 0xFFFF0000 | (w & 1)
    if op == 18:           # b/bl: keep opcode and LK only
        return (op << 26) | (w & 1)
    if op in IMM16:
        return w & 0xFFFF0000
    if op == 31 and ((w >> 1) & 0x3FF) in (339, 467):   # mfspr/mtspr: SPR field masked
        return w & 0xFFE007FF
    return w


def fuzz(w):
    op = w >> 26
    if op in EXT:
        return (op << 16) | ((w >> 1) & 0x3FF)
    return op << 16


def hashes(img, starts, min_insns):
    tbase, raw = C.text_words(img)
    sizes = C.func_sizes(img, starts)
    ex, fz = {}, {}
    for s in starts:
        n = sizes[s][0] // 4
        if n < min_insns: continue
        o = s - tbase
        if o < 0 or o + n * 4 > len(raw): continue
        words = [int.from_bytes(raw[o + 4 * i:o + 4 * i + 4], "big") for i in range(n)]
        ex[s] = hashlib.blake2b(b"".join(norm(w).to_bytes(4, "big") for w in words), digest_size=12).digest()
        fz[s] = hashlib.blake2b(b"".join(fuzz(w).to_bytes(4, "little") for w in words), digest_size=12).digest()
    return ex, fz, sizes


def stats(name, h, sizes, nfunc):
    c = collections.Counter(h.values())
    uniq = sum(1 for v in h.values() if c[v] == 1)
    print("%s: %d funcs hashed (of %d), %d distinct hashes, %d unique (%.1f%%), %d share a hash with another function (%.1f%%)" %
          (name, len(h), nfunc, len(c), uniq, 100 * uniq / max(1, len(h)), len(h) - uniq, 100 * (len(h) - uniq) / max(1, len(h))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image"); ap.add_argument("--gen"); ap.add_argument("--image-b"); ap.add_argument("--gen-b")
    ap.add_argument("--min-insns", type=int, default=8); ap.add_argument("--dump")
    a = ap.parse_args()
    ia = C.Image(a.image); sa = C.func_starts(a.gen)
    exa, fza, sza = hashes(ia, sa, a.min_insns)
    print("A: %d functions, %d with >= %d insns" % (len(sa), len(exa), a.min_insns))
    stats("A exact", exa, sza, len(sa)); stats("A fuzzy", fza, sza, len(sa))
    if not a.image_b:
        return
    ib = C.Image(a.image_b); sb = C.func_starts(a.gen_b or a.gen)
    exb, fzb, szb = hashes(ib, sb, a.min_insns)
    print("B: %d functions, %d hashed" % (len(sb), len(exb)))
    for name, ha, hb in (("exact", exa, exb), ("fuzzy", fza, fzb)):
        ca = collections.Counter(ha.values()); cb = collections.Counter(hb.values())
        anym = [s for s, v in ha.items() if v in cb]
        one = [s for s, v in ha.items() if cb.get(v) == 1 and ca[v] == 1]
        by = sum(sza[s][0] for s in anym); bo = sum(sza[s][0] for s in one)
        tot = sum(v[0] for v in sza.values())
        print("%s: A funcs with hash present in B: %d (%.1f%% of hashed A, %.1f%% of A bytes); 1:1 unique: %d (%.1f%%, %.1f%% bytes)" %
              (name, len(anym), 100 * len(anym) / len(ha), 100 * by / tot, len(one), 100 * len(one) / len(ha), 100 * bo / tot))
    if a.dump:
        inv = collections.defaultdict(list)
        for s, v in exb.items(): inv[v].append(s)
        with open(a.dump, "w") as o:
            for s, v in exa.items():
                if len(inv.get(v, ())) == 1: o.write("%08x\t%08x\n" % (s, inv[v][0]))


if __name__ == "__main__":
    main()
