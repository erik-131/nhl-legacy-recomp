"""Tests for locdb.py on a synthetic loc DB built here (no game data)."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import locdb
from locdb import LocDb, crc, key_hash


def build(entries, alphabet=None, spare=0):
    """entries: [(key, text or None)] -> bytes of a one-table loc DB with a balanced Huffman tree."""
    alphabet = sorted(alphabet or {b for k, t in entries for b in (k + (t or '')).encode()})
    # complete tree over the alphabet (pad to a power of two with repeats of the last byte)
    n = 1
    while n < max(2, len(alphabet)):
        n *= 2
    spare_bytes = [b for b in range(1, 256) if b not in alphabet]
    alphabet = alphabet + spare_bytes[:n - len(alphabet)]
    leaves = [('leaf', alphabet[i]) for i in range(n)]
    while len(leaves) > 1:
        leaves = [('node', leaves[i], leaves[i + 1]) for i in range(0, len(leaves), 2)]
    root, codes = leaves[0], {}

    def walk(t, pre):
        if t[0] == 'leaf':
            codes.setdefault(t[1], pre)
        else:
            walk(t[1], pre + '0')
            walk(t[2], pre + '1')
    walk(root, '')
    tree, q, nid = bytearray(), [root], 1
    while q:
        t = q.pop(0)
        for c in (t[1], t[2]):
            if c[0] == 'leaf':
                tree += bytes([0, c[1]])
            else:
                tree += bytes([nid, 0])
                nid += 1
        q += [t[2], t[1]]
        q = [x for x in q if x[0] == 'node']

    def enc(raw, plen):
        bits = ''.join(codes[b] for b in raw)
        nb = (len(bits) + 7) // 8
        return len(raw).to_bytes(plen, 'big') + (int(bits.ljust(nb * 8, '0'), 2).to_bytes(nb, 'big') if nb else b'')
    pool = bytearray(tree)
    recs = []
    for key, text in entries:
        ko = len(pool)
        pool += enc(key.encode(), 1)
        to = 0xFFFFFFFF
        if text is not None:
            to = len(pool)
            pool += enc(text.encode(), 2)
        recs.append((key_hash(key), ko, to))
    recs.sort()
    used = len(pool)
    pool += bytes(-len(pool) % 4)
    rows = len(recs)
    maxr = rows + spare
    body = bytearray()
    for h, ko, to in recs:
        body += struct.pack('>IIII', ko, to, h, 0)
    body += bytes(16 * spare)
    hdr = bytearray(0x24)
    struct.pack_into('>I', hdr, 4, 0x42)
    struct.pack_into('>I', hdr, 8, 16)
    struct.pack_into('>I', hdr, 0x10, used)
    struct.pack_into('>HH', hdr, 0x14, maxr, rows)
    hdr[0x1C] = 3
    fields = bytes(48)
    blk = 0x20
    d = bytearray(blk)
    d[0:2] = b'DB'
    d[3] = 8
    d[4] = 1
    struct.pack_into('>I', d, 0x10, 1)
    d[0x18:0x1C] = b'TEST'
    d += hdr[4:] and b'\0\0\0\0' + bytes(hdr[4:0x24]) + b'\0\0\0\0' + fields + body + pool + b'\0\0\0\0'
    # hdr[:4] slot is the directory CRC at blk; fixed by seal
    db = LocDb(bytes(d))
    db.seal()
    return bytes(db.d)


ENTRIES = [('NHLTeamName_0', 'Anaheim Ducks'), ('NHLCityName_0', 'Anaheim'), ('NICKLINE2_0', 'Ducks'),
           ('NHLTeamName_1', 'Boston Bruins'), ('SHARED_A', 'Same Text'), ('SHARED_B', 'Same Text'),
           ('NOTEXT', None), ('TXT_UTF', 'Café ®')]


class T(unittest.TestCase):
    def setUp(self):
        self.raw = build(ENTRIES)

    def test_builder_is_valid(self):
        db = LocDb(self.raw)
        self.assertEqual(db.problems(), [])
        self.assertEqual(db.rows, len(ENTRIES))

    def test_get_and_case_insensitive(self):
        db = LocDb(self.raw)
        self.assertEqual(db.get('NHLCityName_0'), 'Anaheim')
        self.assertEqual(db.get('nhlcityname_0'), 'Anaheim')
        self.assertEqual(db.get('TXT_UTF'), 'Café ®')
        self.assertEqual(db.get('NOTEXT'), '')
        with self.assertRaises(KeyError):
            db.get('NOPE')

    def test_identical_rewrite(self):
        db = LocDb(self.raw)
        db.seal()
        self.assertEqual(bytes(db.d), self.raw)
        self.assertTrue(db.roundtrip_ok())

    def test_set_in_place_keeps_size(self):
        db = LocDb(self.raw)
        self.assertEqual(db.set('NHLTeamName_0', 'Anaheim Duck'), 'in place')
        db.seal()
        out = LocDb(bytes(db.d))
        self.assertEqual(len(db.d), len(self.raw))
        self.assertEqual(out.get('NHLTeamName_0'), 'Anaheim Duck')
        self.assertEqual(out.problems(), [])
        self.assertEqual(out.get('NHLTeamName_1'), 'Boston Bruins')

    def test_set_longer_appends_and_grows(self):
        db = LocDb(self.raw)
        self.assertEqual(db.set('NICKLINE2_0', 'Ducks Ducks Ducks Ducks'), 'appended')
        db.seal()
        out = LocDb(bytes(db.d))
        self.assertEqual(out.get('NICKLINE2_0'), 'Ducks Ducks Ducks Ducks')
        self.assertEqual(out.get('NHLCityName_0'), 'Anaheim')
        self.assertEqual(out.problems(), [])
        self.assertEqual(struct.unpack('>I', out.d[8:12])[0], len(out.d))

    def test_shared_string_is_not_changed_for_other_key(self):
        db = LocDb(self.raw)
        db.set('SHARED_A', 'Same')
        self.assertEqual(db.get('SHARED_B'), 'Same Text')
        self.assertEqual(db.get('SHARED_A'), 'Same')

    def test_set_text_for_key_without_text(self):
        db = LocDb(self.raw)
        db.set('NOTEXT', 'Boston')
        db.seal()
        self.assertEqual(LocDb(bytes(db.d)).get('NOTEXT'), 'Boston')

    def test_byte_not_in_tree_is_an_error(self):
        db = LocDb(self.raw)
        with self.assertRaises(ValueError):
            db.set('NICKLINE2_0', 'Ж')

    def test_add_new_key(self):
        db = LocDb(build(ENTRIES, alphabet=set(range(32, 127)) | {0xC2, 0xC3, 0xA9, 0xAE}, spare=0))
        n = db.rows
        db.add('NHLTeamName_SEA', 'Seattle Kraken')
        db.add('NHLCityName_SEA', 'Seattle')
        db.seal()
        out = LocDb(bytes(db.d))
        self.assertEqual((out.rows, out.maxrows), (n + 2, n + 2))
        self.assertEqual(out.problems(), [])
        self.assertEqual(out.get('nhlteamname_sea'), 'Seattle Kraken')
        self.assertEqual(out.get('NHLTeamName_0'), 'Anaheim Ducks')
        with self.assertRaises(KeyError):
            out.add('NHLCityName_SEA', 'x')
        self.assertTrue(out.roundtrip_ok())

    def test_crc_damage_found(self):
        d = bytearray(self.raw)
        d[-20] ^= 1
        self.assertTrue(LocDb(bytes(d)).problems())

    def test_key_hash_ignores_case(self):
        self.assertEqual(key_hash('TEAMLINE1_EAS'), key_hash('Teamline1_EAS'))
        self.assertNotEqual(key_hash('A'), key_hash('B'))

    def test_cli_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            src, out, csvp = (os.path.join(td, n) for n in ('a.db', 'b.db', 'c.csv'))
            open(src, 'wb').write(self.raw)
            self.assertEqual(locdb.main(['x', 'set', src, 'NHLCityName_0', 'Anaheim', '--out', out]), 0)
            self.assertEqual(open(out, 'rb').read(), self.raw)
            self.assertEqual(locdb.main(['x', 'set', src, '--set', 'NHLCityName_0=Utah', '--out', out]), 0)
            self.assertEqual(LocDb(open(out, 'rb').read()).get('NHLCityName_0'), 'Utah')
            self.assertEqual(locdb.main(['x', 'export', src, csvp]), 0)
            self.assertIn('Anaheim Ducks', open(csvp, encoding='utf-8').read())
            self.assertEqual(locdb.main(['x', 'check', out]), 0)


if __name__ == '__main__':
    unittest.main()
