#!/usr/bin/env python3
"""Self-test for p22_common.Image.read(): compares 32-bit words read from the image
against the PPC instruction comments in the lifted C++ (<gen>/*_recomp.*.cpp).
Comments carry mnemonics but no addresses, so the first N decodable instructions
of each function (consecutive from its start) are decoded from the image and compared.
usage: p22_selftest.py [--image F] [--gen DIR] [--n 20] [--old]   (--old = PE raw-pointer mapping)"""
import argparse, glob, os, random, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p22_common as C

def s16(x): return x - 0x10000 if x & 0x8000 else x
def decode(w):
    op = w >> 26; rd = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF
    if w == 0x7D8802A6: return "mflr r12"
    if w & 0xFFFFF != 0x802A6 and (w >> 21) & 0x3FF == 0x3E0 and False: pass
    if op == 14: return f"li r{rd},{s16(imm)}" if ra == 0 else f"addi r{rd},r{ra},{s16(imm)}"
    if op == 15: return f"lis r{rd},{s16(imm)}" if ra == 0 else f"addis r{rd},r{ra},{s16(imm)}"
    if op == 37: return f"stwu r{rd},{s16(imm)}(r{ra})"
    if op == 36: return f"stw r{rd},{s16(imm)}(r{ra})"
    if op == 32: return f"lwz r{rd},{s16(imm)}(r{ra})"
    if op == 24: return f"ori r{ra},r{rd},{imm}"
    if op == 18 and w & 3 == 1: return "bl"
    if w == 0x7D6903A6: return "mtctr r11"
    return None

def comments(gen, fn):
    for p in glob.glob(os.path.join(gen, "*_recomp.*.cpp")):
        txt = open(p, errors="replace").read()
        m = re.search(r"DEFINE_REX_FUNC\(%s\) \{\n(.*?)\n\}\n" % fn, txt, re.S)
        if m:
            out = []
            for l in m.group(1).split("\n"):
                l = l.strip()
                if l.startswith("// ") and not l.startswith("// bl 0x"):
                    out.append(l[3:])
                elif l.startswith("// bl 0x"):
                    out.append("bl")
            return out
    return None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--image"); ap.add_argument("--gen")
    ap.add_argument("--n", type=int, default=20); ap.add_argument("--old", action="store_true")
    a = ap.parse_args()
    gen = a.gen or C.GEN
    img = C.Image(a.image)
    if a.old: img.read = img.read_pe
    starts = C.func_starts(gen)
    rnd = random.Random(1)
    fixed = [0x8316AAB8, 0x82670A78, 0x8331E470]
    pick = fixed + rnd.sample(starts, a.n)
    ok = bad = 0
    for va in pick:
        cm = comments(gen, "sub_%08X" % va)
        if not cm: print("no comments", hex(va)); continue
        words = struct.unpack(">%dI" % 12, img.read(va, 48).ljust(48, b"\0"))
        res = []
        for i, w in enumerate(words[:min(12, len(cm))]):
            d = decode(w)
            if d is None: continue
            res.append(d == cm[i] or (d == "bl" and cm[i] == "bl"))
        if not res: print("%08X SKIP (nothing decodable)" % va); continue
        good = all(res)
        ok += good; bad += not good
        print("%08X %s (%d/%d decodable match)" % (va, "OK " if good else "BAD", sum(res), len(res)))
    print("PASS" if bad == 0 else "FAIL", "ok=%d bad=%d" % (ok, bad))
    return bad
sys.exit(1 if main() else 0)
