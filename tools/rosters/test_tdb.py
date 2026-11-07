#!/usr/bin/env python3
"""Tests for tdb.py on a small made-up database, so no EA data is needed. Run: python3 test_tdb.py"""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb

be16 = tdb.be16


def make_table(idx, rows=3, max_rows=3, pad=0, deleted=()):
    """3 fields as above, record 95 bits (bit 94 = free flag); slot r holds A=r*7+21+idx, B=r*100+3, C='Name' (row 0) or 'N<r>'."""
    fields = b''
    for ftype, bitoff, code, bits in ((3, 0, b'AAAA', 5), (3, 5, b'BBBB', 11), (0, 16, b'CCCC', 64)):
        fields += struct.pack('>II4sI', ftype, bitoff, code, bits)
    rec = 12
    body = b''
    for r in range(max_rows):
        v = 0
        if r < rows:
            c = int.from_bytes(('Name' if r == 0 else 'N%d' % r).encode().ljust(8, b'\0'), 'big')
            v = ((r * 7 + 21 + idx) % 32 << 91) | ((r * 100 + 3) << 80) | (c << 16) | ((r in deleted) << 1)
        body += v.to_bytes(rec, 'big')
    body += bytes(pad)
    hdr = struct.pack('>IIIIHHI', 2, rec, 95, 0, max_rows, rows, 0xFFFF) + bytes([3, 0, 0, 0]) + bytes(4)
    hdr += struct.pack('>I', tdb.crc(hdr))
    return hdr + fields + body + struct.pack('>I', tdb.crc(fields + body))


def make_db(path, tables=1, **kw):
    """Tables TABL, TAB2, ... of make_table(); keywords go to every table."""
    blobs = [make_table(i, **kw) for i in range(tables)]
    dirn = b''
    pos = 0
    for i, b in enumerate(blobs):
        dirn += (b'TABL' if i == 0 else b'TAB%d' % (i + 1)) + struct.pack('>I', pos)
        pos += len(b)
    size = 0x18 + len(dirn) + 4 + pos
    head = b'DB\0\x08' + struct.pack('>IIII', 0x01000000, size, 0, tables)
    d = head + struct.pack('>I', tdb.crc(head)) + dirn + struct.pack('>I', tdb.crc(dirn)) + b''.join(blobs)
    with open(path, 'wb') as f:
        f.write(d)


META = """<?xml version="1.0"?>
<database>
  <table name="first" shortname="TABL">
    <fields>
      <field name="a" shortname="AAAA" type="int" depth="5" rangelow="-1" rangehigh="30"/>
      <field name="b" shortname="BBBB" type="int" depth="11" rangelow="0" rangehigh="2047" />
    </fields>
  </table>
  <table name="second" shortname="TAB2">
    <fields>
      <field name="a" shortname="AAAA" type="int" depth="5" rangelow="0" rangehigh="31"/>
    </fields>
  </table>
</database>
"""


class TdbTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, 'x.db')
        make_db(self.path)
        self.db = tdb.Db(self.path)
        self.t = self.db.table('TABL')

    def tearDown(self):
        self.dir.cleanup()

    def test_read(self):
        self.assertEqual(self.db.check(), [])
        self.assertEqual(self.t.get(0, 'AAAA'), 21)
        self.assertEqual(self.t.get(0, 'BBBB'), 3)
        self.assertEqual(self.t.get(0, 'CCCC'), 'Name')
        self.assertEqual(self.t.get(1, 'CCCC'), 'N1')

    def test_rewrite_identical(self):
        before = bytes(self.db.data)
        self.db.seal_all()
        self.assertEqual(before, bytes(self.db.data))

    def test_set_keeps_neighbours_and_seals(self):
        self.t.set(0, 'BBBB', 1234)
        self.t.set(0, 'CCCC', 'Other')
        self.assertEqual(self.t.get(0, 'AAAA'), 21)
        self.assertEqual(self.t.get(0, 'BBBB'), 1234)
        self.assertEqual(self.t.get(0, 'CCCC'), 'Other')
        self.assertTrue(self.db.check())
        out = self.path + '.out'
        self.db.save(out)
        again = tdb.Db(out)
        self.assertEqual(again.check(), [])
        self.assertEqual(again.table('TABL').get(0, 'BBBB'), 1234)
        self.assertEqual(os.path.getsize(out), os.path.getsize(self.path))

    def test_low_limit_offset(self):
        # a field with schema low -1: stored 0 reads as -1, and the top value fits
        self.t.fields['AAAA'].low, self.t.fields['AAAA'].high = -1, 30
        self.assertEqual(self.t.get(0, 'AAAA', raw=True), 21)
        self.assertEqual(self.t.get(0, 'AAAA'), 20)
        self.t.set(0, 'AAAA', 30)
        self.assertEqual(self.t.get(0, 'AAAA', raw=True), 31)
        self.t.set(0, 'AAAA', -1)
        self.assertEqual(self.t.get(0, 'AAAA', raw=True), 0)
        with self.assertRaises(ValueError):
            self.t.set(0, 'AAAA', 31)

    def test_limits(self):
        with self.assertRaises(ValueError):
            self.t.set(0, 'AAAA', 32)
        with self.assertRaises(ValueError):
            self.t.set(0, 'CCCC', 'Too long!')
        with self.assertRaises(IndexError):
            self.t.get(3, 'AAAA')
        self.t.set(0, 'CCCC', 'Seven77')
        self.assertEqual(self.t.get(0, 'CCCC'), 'Seven77')

class TdbFeatureTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, 'x.db')

    def tearDown(self):
        self.dir.cleanup()

    def open(self, **kw):
        make_db(self.path, **kw)
        self.db = tdb.Db(self.path)
        self.t = self.db.table('TABL')
        return self.t

    def snapshot(self, t):
        return [(r, t.get(r, 'AAAA'), t.get(r, 'BBBB'), t.get(r, 'CCCC'), t.is_deleted(r)) for r in range(t.rows)]

    def test_deleted_rows(self):
        t = self.open(deleted=(1,))
        self.assertEqual(t.live_rows(), [0, 2])
        self.assertTrue(t.is_deleted(1))
        self.assertEqual(t.get(1, 'BBBB'), 103)  # the flag does not disturb the fields
        out = io.StringIO()
        t.export(out)
        self.assertEqual([l.split(',')[0] for l in out.getvalue().split()], ['row', '0', '2'])
        out = io.StringIO()
        t.export(out, include_deleted=True)
        lines = out.getvalue().split()
        self.assertEqual([l.split(',')[0] for l in lines], ['row', '0', '1', '2'])
        self.assertEqual([l.split(',')[-1] for l in lines], ['deleted', '0', '1', '0'])
        t.delete_row(0)
        self.assertEqual(t.live_rows(), [2])
        self.assertEqual(t.get(0, 'AAAA'), 21)
        self.db.save(self.path)
        self.assertEqual(tdb.Db(self.path).check(), [])

    def test_rebuild_identical(self):
        make_db(self.path, tables=3, rows=2, max_rows=4, pad=16, deleted=(1,))
        before = open(self.path, 'rb').read()
        db = tdb.Db(self.path)
        self.assertEqual(db.check(), [])
        db.rebuild()
        self.assertEqual(bytes(db.data), before)
        self.assertEqual(db.check(), [])

    def test_widen_keeps_values(self):
        make_db(self.path, tables=2, rows=3, max_rows=5, pad=8, deleted=(1,))
        db = tdb.Db(self.path)
        t, t2 = db.table('TABL'), db.table('TAB2')
        before, before2 = self.snapshot(t), self.snapshot(t2)
        old_size = len(db.data)
        t.widen('AAAA', 6)
        self.assertEqual(db.check(), [])
        self.assertEqual((t.rec_bits, t.rec_bytes), (96, 12))
        self.assertEqual((t.field('BBBB').bitoff, t.field('CCCC').bitoff), (6, 17))
        self.assertEqual(self.snapshot(t), before)
        self.assertEqual(self.snapshot(t2), before2)
        self.assertEqual(len(db.data), old_size)  # 12 bytes still hold 96 bits
        t.set(0, 'AAAA', 63)
        t.set(0, 'CCCC', 'Seven77')
        self.assertEqual((t.get(0, 'AAAA'), t.get(0, 'BBBB'), t.get(0, 'CCCC')), (63, 3, 'Seven77'))
        self.assertEqual(t.live_rows(), [0, 2])
        db.seal()
        self.assertEqual(db.check(), [])

    def test_widen_grows_record_and_file(self):
        make_db(self.path, tables=2, rows=3, max_rows=5, pad=8, deleted=(2,))
        db = tdb.Db(self.path)
        t, t2 = db.table('TABL'), db.table('TAB2')
        before, before2 = self.snapshot(t), self.snapshot(t2)
        old_size, old_off2 = len(db.data), t2.off
        t.widen('BBBB', 20)  # 9 more bits: 104 bits, 13 bytes
        self.assertEqual((t.rec_bits, t.rec_bytes), (104, 13))
        self.assertEqual(len(db.data), old_size + 5 * 1)
        self.assertEqual(t2.off, old_off2 + 5)
        self.assertEqual(db.check(), [])
        self.assertEqual(self.snapshot(t), before)
        self.assertEqual(self.snapshot(t2), before2)
        self.assertTrue(t.is_deleted(2))
        self.assertEqual(t.field('CCCC').bitoff, 25)
        t.set(1, 'BBBB', 1000000)
        out = self.path + '.out'
        db.save(out)
        again = tdb.Db(out)
        self.assertEqual(again.check(), [])
        self.assertEqual(again.table('TABL').get(1, 'BBBB'), 1000000)
        self.assertEqual(self.snapshot(again.table('TAB2')), before2)
        self.assertEqual(again.table('TABL').add_row(), 2)  # the deleted row, carried over

    def test_widen_chain_equals_direct(self):
        make_db(self.path, rows=3, max_rows=4)
        a, b = tdb.Db(self.path), tdb.Db(self.path)
        a.table('TABL').widen('AAAA', 6)
        a.table('TABL').widen('AAAA', 9)
        b.table('TABL').widen('AAAA', 9)
        self.assertEqual(bytes(a.data), bytes(b.data))

    def test_widen_with_low_limit_moves_high(self):
        t = self.open()
        f = t.field('AAAA')
        f.low, f.high = -1, 30
        t.widen('AAAA', 6)
        self.assertEqual((f.low, f.high), (-1, 62))
        t.set(0, 'AAAA', 62)
        self.assertEqual(t.get(0, 'AAAA', raw=True), 63)
        self.db.seal()
        self.assertEqual(self.db.check(), [])

    def test_widen_errors(self):
        t = self.open()
        before = bytes(self.db.data)
        for name, bits in (('AAAA', 5), ('AAAA', 4), ('AAAA', 33), ('CCCC', 80)):
            with self.assertRaises(ValueError):
                t.widen(name, bits)
        with self.assertRaises(KeyError):
            t.widen('ZZZZ', 8)
        t.field('CCCC').type = tdb.TYPE_INT
        t.field('CCCC').bits = 78  # reaches bit 94, the free flag
        with self.assertRaises(ValueError):
            t.widen('CCCC', 80)
        t.field('CCCC').bits = 64
        t.field('CCCC').type = tdb.TYPE_STRING
        self.assertEqual(bytes(self.db.data), before)
        t.widen('AAAA', 6)
        with self.assertRaises(ValueError):
            t.set(0, 'AAAA', 64)  # too large for the new width
        self.assertEqual(t.get(0, 'AAAA'), 21)

    def test_widen_meta(self):
        out = tdb.widen_meta(META, 'TABL', 'AAAA', 6)
        self.assertIn('shortname="AAAA" type="int" depth="6" rangelow="-1" rangehigh="62"/>', out)
        self.assertEqual(out.count('depth="5"'), 1)  # the other table's field is untouched
        self.assertEqual(out.replace('depth="6"', 'depth="5"').replace('rangehigh="62"', 'rangehigh="30"'), META)
        out = tdb.widen_meta(META, 'TAB2', 'AAAA', 6)
        self.assertIn('depth="6" rangelow="0" rangehigh="63"/>', out)
        self.assertIn('depth="11" rangelow="0" rangehigh="2047" />', out)
        for args in (('NOPE', 'AAAA'), ('TABL', 'NOPE')):
            with self.assertRaises(KeyError):
                tdb.widen_meta(META, *args, 6)

    def test_meta_and_db_agree(self):
        make_db(self.path)
        with open(os.path.join(self.dir.name, 'nhlng-meta.xml'), 'w') as f:
            f.write(META)
        db = tdb.Db(self.path)
        t = db.table('first')
        self.assertEqual(db.check(), [])
        t.widen('a', 6)
        self.assertEqual(db.check(), [])
        t.set(0, 'a', 62)
        # old meta against a widened file is fine; a new meta against an old file is caught
        db2 = tdb.Db(self.path)
        with open(os.path.join(self.dir.name, 'nhlng-meta.xml'), 'w') as f:
            f.write(tdb.widen_meta(META, 'TABL', 'AAAA', 6))
        self.assertTrue(any('does not fit' in b for b in tdb.Db(self.path).check()))
        self.assertEqual(db2.check(), [])

    def test_add_row_reuses_deleted_then_appends(self):
        t = self.open(rows=3, max_rows=5, deleted=(1, 2))
        self.assertEqual(t.add_row({'AAAA': 9, 'CCCC': 'Fresh'}), 1)
        self.assertEqual((t.get(1, 'AAAA'), t.get(1, 'BBBB'), t.get(1, 'CCCC')), (9, 0, 'Fresh'))
        self.assertFalse(t.is_deleted(1))
        self.assertEqual(t.add_row(), 2)
        self.assertEqual(t.rows, 3)
        self.assertEqual(t.add_row(), 3)
        self.assertEqual(t.add_row(), 4)
        self.assertEqual((t.rows, t.live_rows()), (5, [0, 1, 2, 3, 4]))
        self.db.save(self.path)
        again = tdb.Db(self.path)
        self.assertEqual(again.check(), [])
        self.assertEqual(again.table('TABL').rows, 5)
        self.assertEqual(again.table('TABL').get(1, 'CCCC'), 'Fresh')

    def test_add_row_full(self):
        t = self.open(rows=3, max_rows=3)
        before = bytes(self.db.data)
        with self.assertRaises(tdb.TableFull):
            t.add_row()
        self.assertEqual(bytes(self.db.data), before)
        t.delete_row(0)
        self.assertEqual(t.add_row(), 0)
        with self.assertRaises(tdb.TableFull):
            t.add_row()

    def test_add_row_bad_value_changes_nothing(self):
        t = self.open(rows=2, max_rows=4, deleted=(0,))
        before = self.snapshot(t)
        with self.assertRaises(ValueError):
            t.add_row({'AAAA': 99})
        self.assertEqual(self.snapshot(t), before)
        t.live_rows()
        with self.assertRaises(ValueError):
            t.add_row({'BBBB': 5000})  # reuses nothing: row 0 is deleted, so restored as it was
        self.assertEqual(self.snapshot(t), before)
        t = self.open(rows=2, max_rows=4)
        with self.assertRaises(ValueError):
            t.add_row({'AAAA': 99})
        self.assertEqual(t.rows, 2)
        self.assertEqual(be16(self.db.data, t.off + 0x12), 2)

    def test_max_rows(self):
        t = self.open(rows=2, max_rows=3, pad=30)  # space holds 5 rows of 12 bytes
        t.set_max_rows(5)
        self.assertEqual(t.add_row(), 2)
        self.db.save(self.path)
        self.assertEqual(tdb.Db(self.path).check(), [])
        with self.assertRaises(ValueError):
            t.set_max_rows(6)
        with self.assertRaises(ValueError):
            t.set_max_rows(2)  # below rows in use
        self.assertEqual(t.max_rows, 5)

    def test_check_finds_inconsistencies(self):
        make_db(self.path, rows=2, max_rows=3, pad=4)
        good = open(self.path, 'rb').read()

        def broken(patch):
            d = bytearray(good)
            patch(d)
            db = tdb.Db.__new__(tdb.Db)
            open(self.path, 'wb').write(d)
            db = tdb.Db(self.path)
            db.seal_all()  # good CRCs: only the structure is wrong
            return db.check()
        base = 0x18 + 8 + 4
        self.assertEqual(broken(lambda d: None), [])
        r = broken(lambda d: struct.pack_into('>H', d, base + 0x10, 4))  # max rows 4 need 48 of 40 bytes
        self.assertTrue(any('max rows 4' in x for x in r), r)
        r = broken(lambda d: struct.pack_into('>H', d, base + 0x12, 4))
        self.assertTrue(any('above max rows' in x for x in r), r)
        r = broken(lambda d: struct.pack_into('>I', d, base + 4, 11))
        self.assertTrue(any('record bytes' in x for x in r), r)
        r = broken(lambda d: struct.pack_into('>I', d, base + 0x24 + 16 * 2 + 12, 80))  # field C reaches the flag
        self.assertTrue(any('free flag' in x for x in r), r)
        r = broken(lambda d: struct.pack_into('>I', d, base + 0x24 + 16 + 4, 3))
        self.assertTrue(any('overlaps' in x for x in r), r)
        d = bytearray(good)
        struct.pack_into('>I', d, 8, len(d) + 1)
        open(self.path, 'wb').write(d)
        db = tdb.Db(self.path)
        db.seal_all()
        self.assertTrue(any('file size' in x for x in db.check()))
        db.rebuild()
        self.assertEqual(db.check(), [])
        self.assertEqual(bytes(db.data), good)  # rebuild repairs the size

    def test_check_finds_bad_crc(self):
        self.open()
        self.db.data[-8] ^= 1
        self.assertEqual(self.db.check(), ['TABL data'])

    def test_cli(self):
        make_db(self.path, tables=2, rows=3, max_rows=5, deleted=(1,))
        out = os.path.join(self.dir.name, 'o.db')
        meta = os.path.join(self.dir.name, 'nhlng-meta.xml')
        with open(meta, 'w') as f:
            f.write(META)

        def run(*a):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = tdb.main(['tdb.py'] + list(a))
            return rc, buf.getvalue()
        rc, text = run('live', self.path, 'first')
        self.assertEqual((rc, 'live 2, deleted 1' in text), (0, True))
        rc, text = run('export', self.path, 'first')
        self.assertEqual(len(text.split()), 3)
        rc, text = run('export', self.path, 'first', '--deleted')
        self.assertEqual(len(text.split()), 4)
        self.assertEqual(run('widen', self.path, out, 'first', 'a', '6', '--meta-out', meta + '.new')[0], 0)
        self.assertEqual(run('check', out)[0], 0)
        self.assertIn('depth="6"', open(meta + '.new').read())
        self.assertEqual(run('widen', self.path, out, 'first', 'a', '5', '--meta-out', os.devnull)[0], 1)
        self.assertEqual(run('widen', self.path, out, 'first', 'a', '5')[0], 1)
        os.replace(meta + '.new', meta)  # the widened meta goes with the widened db
        self.assertEqual(run('add', out, out, 'first', 'a=62', 'CCCC=Hi')[0], 0)  # row 1 is reused
        self.assertEqual(tdb.Db(out).table('TABL').get(1, 'CCCC'), 'Hi')
        self.assertEqual(run('add', out, out, 'first', 'a=63')[0], 1)  # 63 is above the new range
        self.assertEqual(run('delete', out, out, 'first', '0')[0], 0)
        self.assertEqual(run('maxrows', out, out, 'first', '9')[0], 1)
        self.assertEqual(run('rebuild', out, out + '2')[0], 0)
        self.assertEqual(open(out, 'rb').read(), open(out + '2', 'rb').read())
        self.assertEqual(run('widen-meta', meta, out + '.xml', 'TAB2', 'AAAA', '7')[0], 0)
        self.assertIn('rangehigh="127"', open(out + '.xml').read())
        self.assertEqual(run('verify', out)[0], 0)


if __name__ == '__main__':
    unittest.main()
