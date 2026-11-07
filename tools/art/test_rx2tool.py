#!/usr/bin/env python3
"""Self-test for rx2tool.py using a SYNTHETIC .rx2 built here (no game data).
Run: python3 tools/art/test_rx2tool.py
"""
import os, struct, sys, tempfile, unittest, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rx2tool as R

W = H = 128
NMIP = 8  # 128 .. 1


def build_synth_rx2(fmt=18, seed=1):
    """One buffer record + one raster record, big-endian RW4 xb2, DXT tiled, 8in16, packed tail."""
    d0 = 0x80000002 | ((W // 32) << 22)
    d1 = fmt | (1 << 6)                       # format, endian 8in16, buffer = record 0
    d2 = (W - 1) | ((H - 1) << 13)
    d4 = ((NMIP - 1) << 6)
    nb = 8 if fmt == 18 else 16
    base_pages = (32 * 32 * nb + 4095) // 4096
    d5 = (base_pages << 12) | (1 << 11) | (1 << 9)
    fetch = struct.pack('>6I', d0, d1, d2, 0x688, d4, d5)
    # size the buffer from the repo's layout maths
    probe = bytearray(0x1000 + 0x400)
    f = R.P.decode_fetch([d0, d1, d2, 0x688, d4, d5], True)
    lay = R.P.texture_layout(f)
    mip_end = base_pages * 4096 + max(o + lay['mips'][l]['level_data_extent_bytes'] for l, o in lay['mip_offsets'].items())
    size = (mip_end + 4095) // 4096 * 4096
    hdr = bytearray(0x1000)
    hdr[0:8] = b'\x89RW4xb2\x00'
    struct.pack_into('>I', hdr, 0x20, 2)
    struct.pack_into('>I', hdr, 0x30, 0x100)
    struct.pack_into('>I', hdr, 0x44, 0x1000)
    for i, (off, sz, typ) in enumerate([(0, size, 0x10031), (0x400, 0x38, 0x20003)]):
        struct.pack_into('>IQIII', hdr, 0x100 + 24 * i, off, sz, 16, 0, typ)
    hdr[0x400 + 0x1C:0x400 + 0x1C + 24] = fetch
    # pseudo-random but deterministic texel data
    body = bytearray(size)
    x = seed
    for i in range(size):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        body[i] = x >> 16 & 0xFF
    return bytes(hdr) + bytes(body)


def solid_mips(color, fmt):
    from PIL import Image
    out = []
    for l in range(NMIP):
        n = max(W >> l, 1)
        im = Image.new('RGBA', (n, n), color)
        out.append(R.encode_dxt(im, fmt))
    return out


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        open(p, 'wb').write(data)
        return p

    def test_parse(self):
        d = build_synth_rx2()
        recs, slots, hdr = R.parse(d)
        self.assertEqual(len(slots), 1)
        self.assertEqual((slots[0].fetch.width, slots[0].fetch.height), (W, H))
        self.assertEqual(slots[0].fetch.mip_max_level_effective + 1, NMIP)

    def test_bad_magic(self):
        with self.assertRaises(ValueError):
            R.parse(b'not an rx2 file at all' * 10)

    def test_untile_retile_identity(self):
        # a buffer is only guaranteed to round-trip where texels are referenced; build one from mips
        d = build_synth_rx2()
        _, slots, _ = R.parse(d)
        s = slots[0]
        mips = solid_mips((200, 30, 40, 255), 18)
        buf = R.retile(s, mips)
        d2 = d[:s.res_off] + buf + d[s.res_off + s.res_size:]
        _, s2, _ = R.parse(d2)
        self.assertEqual(R.untile(d2, s2[0]), mips)          # untile(retile(x)) == x
        self.assertEqual(R.retile(s2[0], R.untile(d2, s2[0])), buf)  # retile(untile(buf)) == buf

    def test_dds_roundtrip(self):
        d = build_synth_rx2()
        _, slots, _ = R.parse(d)
        mips = R.untile(d, slots[0])
        w, h, fmt, m2 = R.read_dds(R.dds_bytes(slots[0], mips))
        self.assertEqual((w, h, fmt), (W, H, 18))
        self.assertEqual(m2, mips)

    def test_replace_dds(self):
        d = build_synth_rx2()
        src = self.write('a.rx2', d)
        _, slots, _ = R.parse(d)
        mips = solid_mips((10, 200, 30, 255), 18)
        dds = self.write('c.dds', R.dds_bytes(slots[0], mips))
        out = os.path.join(self.tmp, 'sub', 'b.rx2')
        r = subprocess.run([sys.executable, R.__file__, 'replace', src, '0', dds, '--out', out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        d2 = open(out, 'rb').read()
        self.assertEqual(len(d2), len(d))
        self.assertEqual(d2[:0x1000], d[:0x1000])             # header untouched
        _, s2, _ = R.parse(d2)
        self.assertEqual(R.untile(d2, s2[0]), mips)
        self.assertEqual(open(src, 'rb').read(), d)           # input untouched

    def test_replace_png_and_dxt5(self):
        from PIL import Image
        d = build_synth_rx2(fmt=20, seed=7)
        src = self.write('a5.rx2', d)
        png = os.path.join(self.tmp, 'i.png')
        Image.new('RGBA', (64, 64), (250, 250, 250, 128)).save(png)  # wrong size: gets resized
        out = os.path.join(self.tmp, 'b5.rx2')
        r = subprocess.run([sys.executable, R.__file__, 'replace', src, '0', png, '--out', out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        d2 = open(out, 'rb').read()
        _, s2, _ = R.parse(d2)
        w, h, fmt, m = R.read_dds(R.dds_bytes(s2[0], R.untile(d2, s2[0])))
        self.assertEqual(fmt, 20)
        from io import BytesIO
        im = Image.open(BytesIO(R.dds_bytes(s2[0], R.untile(d2, s2[0])))).convert('RGBA')
        px = im.getpixel((50, 50))
        self.assertTrue(all(abs(a - b) <= 8 for a, b in zip(px, (250, 250, 250, 128))), px)

    def test_replace_rejects(self):
        d = build_synth_rx2()
        src = self.write('a.rx2', d)
        r = subprocess.run([sys.executable, R.__file__, 'replace', src, '3', src, '--out', src + '.o'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        r = subprocess.run([sys.executable, R.__file__, 'replace', src, '0', 'x.png', '--out', src], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

    def test_roundtrip_cmd(self):
        d = build_synth_rx2()
        _, slots, _ = R.parse(d)
        mips = solid_mips((1, 2, 3, 255), 18)
        buf = R.retile(slots[0], mips)
        p = self.write('rt.rx2', d[:slots[0].res_off] + buf)
        r = subprocess.run([sys.executable, R.__file__, 'roundtrip', p], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
