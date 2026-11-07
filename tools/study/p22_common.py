"""Shared helpers for the P22 study tools (tools/study/p22_*.py).

Reads a decompressed Xbox 360 PE image (big-endian PPC, e.g. the xex payload
after decompression) and the codegen output of the recompiler. Contains no game
data: every value is computed from files the caller points at.

Defaults come from environment variables so nothing machine-specific is baked in:
  P22_IMAGE   path to the decompressed image (PE, 'MZ' header)
  P22_GEN     path to the codegen output dir (e.g. generated/default)
"""
import os, re, struct, sys

IMAGE = os.environ.get("P22_IMAGE", "")
GEN = os.environ.get("P22_GEN", "generated/default")


class Image:
    def __init__(self, path=None):
        path = path or IMAGE
        if not path:
            sys.exit("set P22_IMAGE or pass --image")
        self.data = open(path, "rb").read()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[e:e + 4] == b"PE\0\0", "not a PE image"
        _, ns, = struct.unpack_from("<HH", d, e + 4)
        osz = struct.unpack_from("<H", d, e + 20)[0]
        self.base = struct.unpack_from("<I", d, e + 24 + 28)[0]
        o = e + 24 + osz
        self.sections = []  # name, va(abs), vsize, rawsize, rawptr, flags
        for i in range(ns):
            n, vs, va, rs, rp = struct.unpack_from("<8sIIII", d, o + 40 * i)
            fl = struct.unpack_from("<I", d, o + 40 * i + 36)[0]
            self.sections.append((n.rstrip(b"\0").decode(), self.base + va, vs, rs, rp, fl))

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def read(self, va, n):
        """Bytes at guest address va. The decompressed image is a MEMORY-LAYOUT image:
        file offset = va - self.base (NOT the PE section raw pointers, which differ for
        .text/.data). Bounds-checked against the file size; returns b"" outside it."""
        off = va - self.base
        if off < 0 or off + n > len(self.data):
            return self.data[off:off + n] if 0 <= off < len(self.data) else b""
        return self.data[off:off + n]

    def read_pe(self, va, n):
        """OLD (wrong for this image) mapping through PE raw pointers; kept only for the self-test."""
        for name, sva, vs, rs, rp, fl in self.sections:
            if sva <= va < sva + min(vs, rs):
                off = rp + (va - sva)
                return self.data[off:off + n]
        return b""

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack(">I", b)[0] if len(b) == 4 else None

    def pdata_sizes(self):
        """{start_va: size_bytes} from .pdata (PPC: begin, flags; len = (flags>>8)&0x3fffff insns)."""
        s = self.section(".pdata")
        out = {}
        if not s:
            return out
        raw = self.read(s[1], min(s[2], s[3]))
        for i in range(0, len(raw) - 7, 8):
            a, b = struct.unpack_from(">II", raw, i)
            if a == 0:
                continue
            out[a] = ((b >> 8) & 0x3FFFFF) * 4
        return out


_MAP_RE = re.compile(r"^\s*\{ 0x([0-9A-Fa-f]+), (\w+) \}")


def func_starts(gen=None):
    """Sorted list of function start addresses from PPCFuncMappings in <gen>/*_init.cpp."""
    gen = gen or GEN
    init = [f for f in os.listdir(gen) if f.endswith("_init.cpp")]
    if not init:
        sys.exit("no *_init.cpp in " + gen)
    out = []
    for line in open(os.path.join(gen, init[0]), errors="replace"):
        m = _MAP_RE.match(line)
        if m:
            out.append(int(m.group(1), 16))
    return sorted(set(out))


def func_sizes(img, starts):
    """Function size in bytes: .pdata length if present, else gap to next start (capped to the .text end)."""
    pd = img.pdata_sizes()
    t = img.section(".text")
    tend = t[1] + t[2]
    res = {}
    for i, a in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else tend
        gap = max(0, min(nxt, tend) - a)
        p = pd.get(a)
        res[a] = (min(p, gap) if p else gap, bool(p))
    return res


def ascii_strings(img, section_names=(".rdata", ".data"), minlen=6):
    """Yield (va, str) of printable ASCII runs in the named sections."""
    rx = re.compile(rb"[\x20-\x7e]{%d,}" % minlen)
    for name, va, vs, rs, rp, fl in img.sections:
        if name not in section_names:
            continue
        raw = img.read(va, min(vs, rs))
        for m in rx.finditer(raw):
            yield va + m.start(), m.group().decode("ascii")


def text_words(img):
    t = img.section(".text")
    raw = img.read(t[1], min(t[2], t[3]))
    return t[1], raw
