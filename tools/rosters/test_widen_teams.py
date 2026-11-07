#!/usr/bin/env python3
"""Tests for widen_teams.py on made-up databases (tdb's test helper), no EA data. Run: python3 test_widen_teams.py"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb
import test_tdb
import widen_teams

META = ("﻿<?xml version=\"1.0\"?>\r\n<database>\r\n"
        "  <table name=\"exhibitionplayerbiotable\" shortname=\"TABL\">\r\n    <fields>\r\n"
        "      <field name=\"proteam\" shortname=\"AAAA\" type=\"int\" depth=\"5\" rangelow=\"-1\" rangehigh=\"30\"/>\r\n"
        "      <field name=\"jersey\" shortname=\"BBBB\" type=\"int\" depth=\"11\" rangelow=\"-1\" rangehigh=\"2046\"/>\r\n"
        "    </fields>\r\n  </table>\r\n"
        "  <table name=\"leaguedraftpick\" shortname=\"TAB2\">\r\n    <fields>\r\n"
        "      <field name=\"owner\" shortname=\"AAAA\" type=\"int\" depth=\"5\" rangelow=\"0\" rangehigh=\"31\"/>\r\n"
        "      <field name=\"slot\" shortname=\"BBBB\" type=\"int\" depth=\"11\" rangelow=\"0\" rangehigh=\"2047\"/>\r\n"
        "    </fields>\r\n  </table>\r\n</database>\r\n")


class WidenTeamsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = os.path.join(self.dir, 'nhlng.db')
        self.meta = os.path.join(self.dir, 'nhlng-meta.xml')
        test_tdb.make_db(self.db, tables=2)
        with open(self.meta, 'w', encoding='utf-8', newline='') as fh:
            fh.write(META)

    def tearDown(self):
        for f in os.listdir(self.dir):
            os.remove(os.path.join(self.dir, f))
        os.rmdir(self.dir)

    def p(self, name):
        return os.path.join(self.dir, name)

    def test_candidates_only_offset_team_fields(self):
        db = tdb.Db(self.db, self.meta)
        got = [f'{t.name}.{f.name}' for t, f in widen_teams.candidates(db)]
        self.assertEqual(got, ['exhibitionplayerbiotable.proteam'])  # owner has no -1 offset, jersey is not a team
        got = [f'{t.name}.{f.name}' for t, f in widen_teams.candidates(db, extra=['leaguedraftpick.owner'])]
        self.assertEqual(got, ['exhibitionplayerbiotable.proteam', 'leaguedraftpick.owner'])
        self.assertEqual(widen_teams.candidates(db, skip=['exhibitionplayerbiotable.proteam']), [])
        with self.assertRaises(KeyError):
            widen_teams.candidates(db, extra=['leaguedraftpick.nothing'])
        with self.assertRaises(KeyError):  # already 11 bits
            widen_teams.candidates(db, extra=['leaguedraftpick.slot'])

    def test_widen_keeps_every_value_and_takes_31(self):
        a = tdb.Db(self.db, self.meta)
        before = {(c, r, f.name): t.get(r, f.code, raw=True) for c, t in a.tables.items() for r in range(t.rows) for f in t.fields.values()}
        db = tdb.Db(self.db, self.meta)
        text, report = widen_teams.widen_teams(db, widen_teams.read_text(self.meta))
        self.assertEqual(len(report), 1)
        self.assertEqual(report[0]['old_bits'], 5)
        db.save(self.p('out.db'))
        widen_teams.write_text(self.p('out.xml'), text)
        b = tdb.Db(self.p('out.db'), self.p('out.xml'))
        self.assertEqual(b.check(), [])
        for (c, r, n), v in before.items():
            t = b.tables[c]
            self.assertEqual(t.get(r, t.field(n).code, raw=True), v, (c, r, n))
        t = b.table('exhibitionplayerbiotable')
        f = t.field('proteam')
        self.assertEqual((f.bits, f.low, f.high), (6, -1, 62))
        t.set(0, 'proteam', 31)
        t.set(1, 'proteam', 30)
        b.save(self.p('out2.db'))
        t = tdb.Db(self.p('out2.db'), self.p('out.xml')).table('exhibitionplayerbiotable')
        self.assertEqual([t.get(0, 'proteam'), t.get(1, 'proteam')], [31, 30])
        self.assertEqual(t.get(0, 'jersey'), before[('TABL', 0, 'jersey')] - 1)  # shown = stored + low (-1)
        with self.assertRaises(ValueError):
            tdb.Db(self.db, self.meta).table('exhibitionplayerbiotable').set(0, 'proteam', 31)  # stock refuses

    def test_meta_only_the_field_changes_and_line_endings_kept(self):
        db = tdb.Db(self.db, self.meta)
        text, _ = widen_teams.widen_teams(db, widen_teams.read_text(self.meta))
        self.assertTrue(text.startswith('﻿') and '\r\n' in text and '\n' not in text.replace('\r\n', ''))
        old, new = META.splitlines(), text.splitlines()
        changed = [i for i, (x, y) in enumerate(zip(old, new)) if x != y]
        self.assertEqual(len(old), len(new))
        self.assertEqual(len(changed), 1)
        self.assertIn('depth="6"', new[changed[0]])
        self.assertIn('rangehigh="62"', new[changed[0]])

    def test_extra_field_and_record_growth(self):
        db = tdb.Db(self.db, self.meta)
        t = db.table('leaguedraftpick')
        rb = t.rec_bytes
        _, report = widen_teams.widen_teams(db, widen_teams.read_text(self.meta), extra=['leaguedraftpick.owner'])
        self.assertEqual({r['table'] for r in report}, {'leaguedraftpick', 'exhibitionplayerbiotable'})
        # 95-bit records are 12 bytes (96 bits): one more bit fits (still 12), the second table's bit makes 96, still 12
        self.assertEqual((rb, db.table('leaguedraftpick').rec_bits, db.table('leaguedraftpick').rec_bytes), (12, 96, 12))
        # widening twice in one table crosses the byte boundary (97 bits -> 13 bytes) and rows are rewritten
        t = db.table('leaguedraftpick')
        t.widen('BBBB', 12)
        self.assertEqual((t.rec_bits, t.rec_bytes), (97, 13))
        self.assertEqual(db.check(), [])

    def test_cli(self):
        def run(*a):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = widen_teams.main(['widen_teams.py', *a])
            return rc, buf.getvalue()
        rc, out = run(self.db, '--list')
        self.assertEqual((rc, os.listdir(self.dir).count('out.db')), (0, 0))
        self.assertIn('exhibitionplayerbiotable.proteam', out)
        rc, out = run(self.db, self.p('out.db'))  # no --meta-out
        self.assertEqual(rc, 2)
        rc, out = run(self.db, self.db, '--meta-out', self.p('o.xml'))
        self.assertEqual(rc, 2)
        rc, out = run(self.db, self.p('out.db'), '--meta-out', self.p('o.xml'))
        self.assertEqual(rc, 0, out)
        self.assertEqual(tdb.Db(self.p('out.db'), self.p('o.xml')).check(), [])
        rc, out = run(self.db, self.p('x.db'), '--meta-out', self.p('x.xml'), '--field', 'nope.nothing')
        self.assertEqual(rc, 1)


if __name__ == '__main__':
    unittest.main()
