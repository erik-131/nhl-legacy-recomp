#!/usr/bin/env python3
"""rx2tool: inspect, round-trip check and replace textures in NHL Legacy .rx2 files
(Xbox 360 RenderWare4 'xb2' container). Python 3 + Pillow, no numpy. No game data inside.

Usage:
  rx2tool.py info      FILE.rx2
  rx2tool.py dump      FILE.rx2 --out DIR
        per slot: DIR/<stem>_NN.dds (linear, all mips) and DIR/<stem>_NN_mip0.png
  rx2tool.py roundtrip FILE.rx2 [FILE.rx2 ...]
        untile every mip, retile, byte-compare the resource buffer (exit 1 on any diff)
  rx2tool.py replace   FILE.rx2 SLOT IMG.(dds|png) --out NEW.rx2
        write NEW.rx2 = FILE.rx2 with slot SLOT's pixels replaced. IMG must be a DDS of the
        slot's exact size and format with at least as many mips as the slot (DXT1/3/5), or a
        PNG (resized to the slot size, mips built with a box filter, encoded with the built-in
        DXT encoder, which is a simple range-fit encoder: LOSSY, see docs/art-mods.md; prefer
        a real DXT tool and pass its DDS for final art).

The input file is never modified; output is written only where --out points (and must differ
from the input). The tiling / packed-mip maths is imported from
docs/nhl12-decomp-reference/tools/nhl12_texture_proof.py (override the directory with the
environment variable RX2_PROOF_DIR).
"""
import struct, sys, os, argparse, math

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get('RX2_PROOF_DIR') or
                os.path.normpath(os.path.join(_HERE, '..', '..', 'docs', 'nhl12-decomp-reference', 'tools')))
try:
    import nhl12_texture_proof as P
except ImportError as e:
    sys.exit('rx2tool: cannot import nhl12_texture_proof (%s); set RX2_PROOF_DIR' % e)

RES_TYPES = (0x10031, 0x10032, 0x10033)
RASTER, NAMES = 0x20003, 0xEC0010
be32 = lambda d, o: struct.unpack_from('>I', d, o)[0]


class Slot:
    pass


def parse(d):
    if d[:8] != b'\x89RW4xb2\x00':
        raise ValueError('not an RW4 xb2 (.rx2) file (bad magic)')
    n, tab, hdr = be32(d, 0x20), be32(d, 0x30), be32(d, 0x44)
    recs = []
    for i in range(n):
        e = tab + 24 * i
        recs.append(dict(i=i, off=be32(d, e), size=struct.unpack_from('>Q', d, e + 4)[0], typ=be32(d, e + 20)))
    slots = []
    first_res = next(r for r in recs if r['typ'] in RES_TYPES)['i']
    for r in recs:
        if r['typ'] != RASTER:
            continue
        s = Slot()
        s.rec = r
        s.dw = [be32(d, r['off'] + 0x1C + 4 * k) for k in range(6)]
        s.fetch = P.decode_fetch(s.dw, True)
        f = s.fetch
        s.res_idx = f.base_address_pages if recs[f.base_address_pages]['typ'] in RES_TYPES else first_res
        res = recs[s.res_idx]
        s.res_off, s.res_size = hdr + res['off'], res['size']
        s.fmt = f.format_base
        s.layout = P.texture_layout(f)
        slots.append(s)
    return recs, slots, hdr


def level_geometry(s):
    """yield (level, src_off_in_resource, level_layout, packed_xy, w_blocks, h_blocks)"""
    f, lay, fmt = s.fetch, s.layout, s.fmt
    bw, bh = P.FORMAT_INFO[fmt][0], P.FORMAT_INFO[fmt][1]
    for lv in range(f.mip_max_level_effective + 1):
        if lv == 0:
            ll, off = lay['base'], 0
            pk = P.get_packed_mip_offset(f.width, f.height, 1, fmt, 0)[:2] if lay['packed_level'] == 0 else (0, 0)
        else:
            pl = lay['packed_level']
            st = min(lv, pl) if pl is not None else lv
            ll = lay['mips'][st]
            off = f.mip_address_pages * 4096 + lay['mip_offsets'][st]
            pk = P.get_packed_mip_offset(f.width, f.height, 1, fmt, lv)[:2] if (pl is not None and lv >= pl) else (0, 0)
        wb = P.align(max(f.width >> lv, 1), bw) // bw
        hb = P.align(max(f.height >> lv, 1), bh) // bh
        yield lv, off, ll, pk, wb, hb


def bpb(fmt):
    bw, bh, bpp, _ = P.FORMAT_INFO[fmt]
    return bw * bh * bpp // 8


def swap(b, endian):
    return P.copy_swap_block(b, endian)


def untile(d, s):
    """-> list of linear mip byte strings (DDS block order, little-endian DXT)"""
    f, nb = s.fetch, bpb(s.fmt)
    res = d[s.res_off:s.res_off + s.res_size]
    lg = int(math.log2(nb))
    out = []
    for lv, off, ll, (ox, oy), wb, hb in level_geometry(s):
        buf = bytearray(wb * hb * nb)
        for y in range(hb):
            for x in range(wb):
                if f.tiled:
                    r = P.tiled_offset_2d(x + ox, y + oy, ll['row_pitch_blocks'], lg)
                else:
                    r = (y + oy) * ll['row_pitch_bytes'] + (x + ox) * nb
                src = off + r
                buf[(y * wb + x) * nb:(y * wb + x + 1) * nb] = swap(res[src:src + nb], f.endianness)
        out.append(bytes(buf))
    return out


def retile(s, mips):
    """linear mips -> resource buffer (zero padded) laid out like the original"""
    f, nb = s.fetch, bpb(s.fmt)
    res = bytearray(s.res_size)
    lg = int(math.log2(nb))
    for (lv, off, ll, (ox, oy), wb, hb) in level_geometry(s):
        m = mips[lv]
        for y in range(hb):
            for x in range(wb):
                if f.tiled:
                    r = P.tiled_offset_2d(x + ox, y + oy, ll['row_pitch_blocks'], lg)
                else:
                    r = (y + oy) * ll['row_pitch_bytes'] + (x + ox) * nb
                res[off + r:off + r + nb] = swap(m[(y * wb + x) * nb:(y * wb + x + 1) * nb], f.endianness)
    return bytes(res)


FOURCC = {18: b'DXT1', 19: b'DXT3', 20: b'DXT5'}


def dds_bytes(s, mips):
    f = s.fetch
    nb = bpb(s.fmt)
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<7I', hdr, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, f.height, f.width,
                     len(mips[0]), 0, len(mips))
    struct.pack_into('<2I4s', hdr, 76, 32, 4, FOURCC[s.fmt])
    struct.pack_into('<I', hdr, 108, 0x1000 | 0x400000 | 0x8)
    return bytes(hdr) + b''.join(mips)


def read_dds(b):
    if b[:4] != b'DDS ':
        raise ValueError('not DDS')
    h, w = struct.unpack_from('<2I', b, 12)
    nmip = struct.unpack_from('<I', b, 28)[0]
    fcc = b[84:88]
    fmt = {v: k for k, v in FOURCC.items()}.get(fcc)
    if fmt is None:
        raise ValueError('unsupported DDS fourcc %r (need DXT1/3/5; no DX10)' % fcc)
    nmip = max(nmip, 1)
    nb = bpb(fmt)
    mips, o = [], 128
    for lv in range(nmip):
        wl, hl = max(w >> lv, 1), max(h >> lv, 1)
        n = ((wl + 3) // 4) * ((hl + 3) // 4) * nb
        if o + n > len(b):
            break
        mips.append(b[o:o + n]); o += n
    return w, h, fmt, mips


# ---- tiny pure-python DXT encoder (range fit, quality 'ok'; use real tools for finals) ----
def _c565(r, g, b): return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
def _x565(c): return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def _enc_color(px):  # px: 16 (r,g,b,a)
    lo = [min(p[i] for p in px) for i in range(3)]
    hi = [max(p[i] for p in px) for i in range(3)]
    c0, c1 = _c565(*hi), _c565(*lo)
    if c0 < c1: c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    a, b = _x565(c0), _x565(c1)
    pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)), tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
    idx = 0
    for k, p in enumerate(px):
        best = min(range(4), key=lambda j: sum((p[i] - pal[j][i]) ** 2 for i in range(3)))
        idx |= best << (2 * k)
    return struct.pack('<HHI', c0, c1, idx)


def _enc_a5(px):
    lo, hi = min(p[3] for p in px), max(p[3] for p in px)
    if lo == hi:
        return bytes([hi, lo, 0, 0, 0, 0, 0, 0])
    a0, a1 = hi, lo
    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    bits = 0
    for k, p in enumerate(px):
        j = min(range(8), key=lambda j: abs(pal[j] - p[3]))
        bits |= j << (3 * k)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little')


def _enc_a4(px):
    v = 0
    for k, p in enumerate(px): v |= (p[3] >> 4) << (4 * k)
    return v.to_bytes(8, 'little')


def encode_dxt(img, fmt):
    w, h = img.size
    px = img.convert('RGBA').load()
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            blk = [px[min(bx * 4 + i, w - 1), min(by * 4 + j, h - 1)] for j in range(4) for i in range(4)]
            if fmt == 20: out += _enc_a5(blk)
            elif fmt == 19: out += _enc_a4(blk)
            out += _enc_color(blk)
    return bytes(out)


def png_to_mips(path, w, h, fmt, nmip):
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    if im.size != (w, h):
        im = im.resize((w, h), Image.LANCZOS)
    return [encode_dxt(im.resize((max(w >> l, 1), max(h >> l, 1)), Image.BOX) if l else im, fmt) for l in range(nmip)]


# ---- commands ----
def cmd_info(a):
    d = open(a.file, 'rb').read()
    recs, slots, hdr = parse(d)
    print('size', len(d), 'hdr', hex(hdr), 'records', len(recs))
    for r in recs: print('  rec', r['i'], 'off %#x size %#x type %#x' % (r['off'], r['size'], r['typ']))
    for i, s in enumerate(slots):
        f = s.fetch
        print('slot', i, f.format_raw_name, '%dx%d' % (f.width, f.height), 'mips', f.mip_max_level_effective + 1,
              'tiled', f.tiled, 'endian', f.endianness_name, 'packed_from', s.layout['packed_level'],
              'res_rec', s.res_idx, 'res_size %#x' % s.res_size, 'fetch', ' '.join('%08x' % x for x in s.dw))
    nm = [r for r in recs if r['typ'] == NAMES]
    for r in nm:
        blob = d[r['off']:r['off'] + 0x300]
        print('names:', [x.decode('latin1') for x in blob.split(b'\0') if b'Raster' in x])


def cmd_dump(a):
    from PIL import Image
    import io
    d = open(a.file, 'rb').read()
    recs, slots, _ = parse(d)
    os.makedirs(a.out, exist_ok=True)
    stem = os.path.splitext(os.path.basename(a.file))[0]
    for i, s in enumerate(slots):
        mips = untile(d, s)
        dds = dds_bytes(s, mips)
        p = os.path.join(a.out, '%s_%02d.dds' % (stem, i))
        open(p, 'wb').write(dds)
        Image.open(io.BytesIO(dds)).convert('RGBA').save(p[:-4] + '_mip0.png')
        print('wrote', p, 'and png')


def cmd_roundtrip(a):
    ok = bad = 0
    for fn in a.files:
        d = open(fn, 'rb').read()
        try:
            recs, slots, _ = parse(d)
        except Exception as e:
            print('SKIP', fn, e); continue
        for i, s in enumerate(slots):
            try:
                mips = untile(d, s)
            except Exception as e:
                print('SKIP', fn, 'slot', i, repr(e)); continue
            orig = d[s.res_off:s.res_off + s.res_size]
            new = retile(s, mips)
            # DDS-level check as well: dds -> read -> same mips
            if s.fmt in FOURCC:
                w, h, fmt, m2 = read_dds(dds_bytes(s, mips))
                dds_ok = (m2 == mips and (w, h) == (s.fetch.width, s.fetch.height))
            else:
                dds_ok = True  # 8888: no DDS path in this tool (dump/replace need DXT)
            diff = [k for k in range(len(orig)) if orig[k] != new[k]]
            cov_missing_nonzero = sum(1 for k in diff if new[k] == 0 and orig[k] != 0)
            tag = 'OK ' if not diff and dds_ok else 'DIFF'
            if tag == 'OK ': ok += 1
            else: bad += 1
            print(tag, os.path.basename(fn), 'slot', i, s.fetch.format_raw_name, '%dx%d' % (s.fetch.width, s.fetch.height),
                  'mips', len(mips), 'res bytes', len(orig), 'diff bytes', len(diff), 'orig-nonzero-not-covered', cov_missing_nonzero, 'dds_ok', dds_ok)
    print('summary ok=%d diff=%d' % (ok, bad))
    return 0 if bad == 0 else 1


def cmd_replace(a):
    d = bytearray(open(a.file, 'rb').read())
    recs, slots, _ = parse(bytes(d))
    if os.path.abspath(a.out) == os.path.abspath(a.file):
        sys.exit('rx2tool: --out must differ from the input file')
    if not 0 <= int(a.slot) < len(slots):
        sys.exit('rx2tool: slot %s out of range (file has %d slot(s); see `info`)' % (a.slot, len(slots)))
    s = slots[int(a.slot)]
    if s.fmt not in FOURCC:
        sys.exit('rx2tool: slot %s is %s; replace supports DXT1/3/5 only' % (a.slot, s.fetch.format_raw_name))
    f = s.fetch
    nmip = f.mip_max_level_effective + 1
    if a.img.lower().endswith('.dds'):
        w, h, fmt, mips = read_dds(open(a.img, 'rb').read())
        if (w, h, fmt) != (f.width, f.height, s.fmt) or len(mips) < nmip:
            sys.exit('rx2tool: DDS must be %dx%d fmt %d with >=%d mips (got %dx%d fmt %s, %d mips)' % (f.width, f.height, s.fmt, nmip, w, h, fmt, len(mips)))
    else:
        mips = png_to_mips(a.img, f.width, f.height, s.fmt, nmip)
    d[s.res_off:s.res_off + s.res_size] = retile(s, mips[:nmip])
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, 'wb').write(bytes(d))
    print('wrote', a.out, len(d))


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='c', required=True)
    p = sp.add_parser('info'); p.add_argument('file'); p.set_defaults(fn=cmd_info)
    p = sp.add_parser('dump'); p.add_argument('file'); p.add_argument('--out', required=True); p.set_defaults(fn=cmd_dump)
    p = sp.add_parser('roundtrip'); p.add_argument('files', nargs='+'); p.set_defaults(fn=cmd_roundtrip)
    p = sp.add_parser('replace'); p.add_argument('file'); p.add_argument('slot'); p.add_argument('img'); p.add_argument('--out', required=True); p.set_defaults(fn=cmd_replace)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == '__main__':
    main()
