#!/usr/bin/env python3
"""Tests for rosters.py on small made-up databases (no EA data). Run: python3 test_rosters.py"""
import contextlib
import csv
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rosters
import tdb

# table name -> (4-letter code, fields [(name, code, type, bits, low)], key field names)
I, S = tdb.TYPE_INT, tdb.TYPE_STRING
SCHEMA = {
    'exhibitionteams': ('TEAM', [('index', 'tIdx', I, 8, 0), ('abbrname', 'tAbb', S, 32, None)], ['index']),
    'exhibitionplayerbiotable': ('BIOT', [('game_id', 'bGid', I, 14, 0), ('firstname', 'bFir', S, 64, None),
                                          ('lastname', 'bLas', S, 64, None), ('position', 'bPos', I, 3, 0),
                                          ('team', 'bTea', I, 8, -1), ('jersey', 'bJer', I, 7, 0),
                                          ('handedness', 'bHan', I, 1, 0)], ['game_id']),
    'exhibitionskaterai': ('SKAI', [('game_id', 'sGid', I, 14, 0), ('c_overall', 'sOvr', I, 6, 0),
                                    ('c_speed', 'sSpd', I, 6, 0)], ['game_id']),
    'exhibitiongoalieai': ('GOAI', [('game_id', 'gGid', I, 14, 0), ('c_overall', 'gOvr', I, 6, 0),
                                    ('c_gsh', 'gGsh', I, 6, 0)], ['game_id']),
    'exhibitionrostertable': ('ROST', [('key', 'rKey', I, 14, 0), ('team', 'rTea', I, 8, 0),
                                       ('playerindex', 'rPix', I, 14, 0), ('jersey', 'rJer', I, 7, 0)], ['key']),
    'exhibitionplayers': ('PLAY', [('index', 'pIdx', I, 14, 0), ('playerid', 'pPid', I, 14, 0),
                                   ('table', 'pTab', I, 2, 0)], ['index']),
    'exhibitionskaterequipment': ('SKEQ', [('game_id', 'eGid', I, 14, 0), ('stick', 'eStk', I, 7, 0)], ['game_id']),
    'exhibitiongoalieequipment': ('GOEQ', [('game_id', 'qGid', I, 14, 0), ('pads', 'qPad', I, 7, 0)], ['game_id']),
    'exhibitionfreeagents': ('FREE', [('playerindex', 'fPix', I, 14, 0)], ['playerindex']),
    'exhibitiondraftpick': ('DRFT', [('key', 'dKey', I, 11, 0), ('owner', 'dOwn', I, 5, -1),
                                     ('playerindex', 'dPix', I, 11, -1)], ['key']),
}

# players: id -> (first, last, position, team, jersey)
PLAYERS = {10: ('Ann', 'Alpha', 0, 0, 7), 11: ('Bob', 'Beta', 3, 0, 8), 12: ('Cy', 'Gamma', 4, 0, 30),
           20: ('Dee', 'Delta', 1, 1, 9), 21: ('Eli', 'Eps', 4, 1, 31), 22: ('Fay', 'Free', 2, -1, 5)}
ROSTER = [(0, 0, 100, 7), (1, 0, 101, 8), (2, 0, 102, 30), (40, 1, 103, 9), (41, 1, 104, 31)]  # key, team, playerindex, jersey
INDEX = {100: 10, 101: 11, 102: 12, 103: 20, 104: 21, 105: 22}


def build_table(fields, rows, extra_dead=()):
    """rows are lists of shown values in field order; the last bit of each record is the free flag."""
    rec_bits = sum(f[3] for f in fields) + 1
    rec = (rec_bits + 7) // 8
    fb = b''
    off = 0
    for name, code, ftype, bits, low in fields:
        fb += struct.pack('>II4sI', ftype, off, code.encode(), bits)
        off += bits
    body = b''
    for r, vals in enumerate(rows):
        v = 0
        for (name, code, ftype, bits, low), x in zip(fields, vals):
            if ftype == S:
                x = int.from_bytes(x.encode().ljust(bits // 8, b'\0'), 'big')
            else:
                x -= low
            v = (v << bits) | x
        v = (v << 1) | (r in extra_dead)
        body += (v << (rec * 8 - rec_bits)).to_bytes(rec, 'big')
    n = len(rows)
    hdr = struct.pack('>IIIIHHI', 2, rec, rec_bits, 0, n, n, 0xFFFF) + bytes([len(fields), 0, 0, 0]) + bytes(4)
    hdr += struct.pack('>I', tdb.crc(hdr))
    return hdr + fb + body + struct.pack('>I', tdb.crc(fb + body))


def write_db(path, tables, meta=True):
    """tables: {name: (rows, dead set)}; writes path and (optionally) nhlng-meta.xml beside it."""
    blobs, names = [], []
    for name, (rows, dead) in tables.items():
        code, fields, _ = SCHEMA[name]
        blobs.append(build_table(fields, rows, dead))
        names.append(name)
    dirn, pos = b'', 0
    for n, b in zip(names, blobs):
        dirn += SCHEMA[n][0].encode() + struct.pack('>I', pos)
        pos += len(b)
    size = 0x18 + len(dirn) + 4 + pos
    head = b'DB\0\x08' + struct.pack('>IIII', 0x01000000, size, 0, len(names))
    with open(path, 'wb') as f:
        f.write(head + struct.pack('>I', tdb.crc(head)) + dirn + struct.pack('>I', tdb.crc(dirn)) + b''.join(blobs))
    if meta:
        x = '<database>\n'
        for n in names:
            code, fields, keys = SCHEMA[n]
            x += f'<table name="{n}" shortname="{code}"><fields>\n'
            for name, fc, ftype, bits, low in fields:
                if ftype == I:
                    x += (f'<field name="{name}" shortname="{fc}" type="FIELDTYPE_INTEGER" depth="{bits}" '
                          f'rangelow="{low}" rangehigh="{low + (1 << bits) - 1}"{" key=\"True\"" if name in keys else ""}/>\n')
                else:
                    x += f'<field name="{name}" shortname="{fc}" type="FIELDTYPE_STRING" depth="{bits}"/>\n'
            x += '</fields></table>\n'
        x += '</database>\n'
        with open(os.path.join(os.path.dirname(path), 'nhlng-meta.xml'), 'w') as f:
            f.write(x)


def good_tables():
    t = {}
    t['exhibitionteams'] = ([[0, 'AAA'], [1, 'BBB']], set())
    t['exhibitionplayerbiotable'] = ([[g, p[0], p[1], p[2], p[3], p[4], 0] for g, p in sorted(PLAYERS.items())], set())
    t['exhibitionskaterai'] = ([[g, 40 + g % 7, 30] for g, p in sorted(PLAYERS.items()) if p[2] != 4], set())
    t['exhibitiongoalieai'] = ([[g, 50, 20] for g, p in sorted(PLAYERS.items()) if p[2] == 4], set())
    t['exhibitionrostertable'] = ([list(r) for r in ROSTER], set())
    t['exhibitionplayers'] = ([[i, g, 0] for i, g in sorted(INDEX.items())] + [[16383, 16383, 3]], set())
    t['exhibitionskaterequipment'] = ([[g, 5] for g, p in sorted(PLAYERS.items()) if p[2] != 4], set())
    t['exhibitiongoalieequipment'] = ([[g, 6] for g, p in sorted(PLAYERS.items()) if p[2] == 4], set())
    t['exhibitionfreeagents'] = ([[105]], set())
    t['exhibitiondraftpick'] = ([[0, 1, -1], [1, -1, 105]], set())
    return t


def run(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = rosters.main(list(args))
    return code, out.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.p = lambda *a: os.path.join(self.dir.name, *a)
        self.db = self.p('nhlng.db')
        write_db(self.db, good_tables())

    def tearDown(self):
        self.dir.cleanup()

    def rewrite(self, **mods):
        t = good_tables()
        for k, fn in mods.items():
            fn(t[k][0])
        write_db(self.db, t)


class ExportTest(Base):
    def test_export_files_and_players(self):
        code, out = run('export', self.db, self.p('o'))
        self.assertEqual(code, 0)
        for n in SCHEMA:
            self.assertTrue(os.path.exists(self.p('o', n + '.csv')), n)
        rows = list(csv.DictReader(open(self.p('o', 'exhibitionplayerbiotable.csv'))))
        self.assertEqual(rows[0]['lastname'], 'Alpha')
        self.assertEqual(rows[5]['team'], '-1')  # shown value with the schema's low limit
        pl = {r['lastname']: r for r in csv.DictReader(open(self.p('o', 'players.csv')))}
        self.assertEqual(pl['Alpha']['team'], 'AAA')
        self.assertEqual(pl['Alpha']['position'], 'C')
        self.assertEqual(pl['Alpha']['slot'], '0')
        self.assertEqual(pl['Delta']['slot'], '0')
        self.assertEqual(pl['Delta']['roster_key'], '40')
        self.assertEqual(pl['Gamma']['position'], 'G')
        self.assertEqual(pl['Gamma']['c_gsh'], '20')
        self.assertEqual(pl['Alpha']['c_gsh'], '')
        self.assertEqual(pl['Free']['status'], 'free agent')
        self.assertEqual(pl['Free']['team'], '')

    def test_deleted_rows_skipped(self):
        t = good_tables()
        t['exhibitionfreeagents'] = ([[105], [104]], {1})
        write_db(self.db, t)
        run('export', self.db, self.p('o'))
        self.assertEqual(len(list(csv.reader(open(self.p('o', 'exhibitionfreeagents.csv'))))), 2)  # header + 1


class ImportTest(Base):
    def edit(self, table, row, **vals):
        run('export', self.db, self.p('o'))
        path = self.p('o', table + '.csv')
        rows = list(csv.DictReader(open(path)))
        rows[row].update({k: str(v) for k, v in vals.items()})
        with open(path, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)

    def test_round_trip_identical(self):
        run('export', self.db, self.p('o'))
        code, out = run('import', self.db, self.p('o'), self.p('out.db'))
        self.assertEqual(code, 0, out)
        self.assertEqual(open(self.db, 'rb').read(), open(self.p('out.db'), 'rb').read())

    def test_changes_applied_and_input_untouched(self):
        before = open(self.db, 'rb').read()
        self.edit('exhibitionplayerbiotable', 0, lastname='Zed', jersey=99, team=-1)
        code, out = run('import', self.db, self.p('o'), self.p('out.db'))
        self.assertEqual(code, 0, out)
        self.assertEqual(open(self.db, 'rb').read(), before)
        db = tdb.Db(self.p('out.db'), self.p('nhlng-meta.xml'))
        self.assertEqual(db.check(), [])
        t = db.table('exhibitionplayerbiotable')
        self.assertEqual((t.get(0, 'lastname'), t.get(0, 'jersey'), t.get(0, 'team')), ('Zed', 99, -1))
        self.assertEqual(t.get(0, 'firstname'), 'Ann')
        self.assertIn('1 changed', run('diff', self.db, self.p('out.db'))[1])

    def test_rejects_and_writes_nothing(self):
        for col, val in (('jersey', 128), ('team', 255), ('lastname', 'TooLongName'), ('jersey', 'x')):
            with self.subTest(col=col, val=val):
                self.edit('exhibitionplayerbiotable', 0, **{col: val})
                code, out = run('import', self.db, self.p('o'), self.p('out.db'))
                self.assertEqual(code, 1, out)
                self.assertFalse(os.path.exists(self.p('out.db')))

    def test_new_or_unknown_rows_and_columns_rejected(self):
        self.edit('exhibitionfreeagents', 0, playerindex=500)  # key change = a row that does not exist
        self.assertEqual(run('import', self.db, self.p('o'), self.p('out.db'))[0], 1)
        run('export', self.db, self.p('o'))
        with open(self.p('o', 'exhibitionfreeagents.csv'), 'w') as f:
            f.write('row,nonsense\n0,1\n')
        self.assertEqual(run('import', self.db, self.p('o'), self.p('out.db'))[0], 1)

    def test_out_must_differ_from_input(self):
        run('export', self.db, self.p('o'))
        self.assertEqual(run('import', self.db, self.p('o'), self.db)[0], 2)

    def test_removed_csv_rows_stay(self):
        run('export', self.db, self.p('o'))
        path = self.p('o', 'exhibitionfreeagents.csv')
        open(path, 'w').write('row,playerindex\n')
        self.assertEqual(run('import', self.db, self.p('o'), self.p('out.db'))[0], 0)
        self.assertEqual(open(self.db, 'rb').read(), open(self.p('out.db'), 'rb').read())


class CheckTest(Base):
    def test_good_db_passes(self):
        code, out = run('check', self.db)
        self.assertEqual(code, 0, out)
        self.assertIn('0 errors', out)
        self.assertIn('goalies per team min 1 max 1', out)

    def errors(self, **mods):
        self.rewrite(**mods)
        code, out = run('check', self.db)
        return code, out

    def test_dangling_roster_player(self):
        code, out = self.errors(exhibitionrostertable=lambda r: r[0].__setitem__(2, 999))
        self.assertEqual(code, 1)
        self.assertIn('points to no player', out)

    def test_playerid_without_bio(self):
        code, out = self.errors(exhibitionplayers=lambda r: r[0].__setitem__(1, 77))
        self.assertEqual(code, 1)
        self.assertIn('players.playerid has no bio row', out)

    def test_missing_ai_and_equipment(self):
        code, out = self.errors(exhibitionskaterai=lambda r: r.pop(0), exhibitiongoalieequipment=lambda r: r.pop())
        self.assertEqual(code, 1)
        self.assertIn('no skater AI row', out)
        self.assertIn('no equipment row', out)

    def test_team_over_40_and_key_rules(self):
        def many(r):
            for k in range(3, 42):
                r.append([k, 0, 100 + (k % 6), 1])
        code, out = self.errors(exhibitionrostertable=many)
        self.assertEqual(code, 1)
        self.assertIn('more than 40 players', out)
        code, out = self.errors(exhibitionrostertable=lambda r: r.append([5, 1, 103, 9]))
        self.assertIn('not team*40+slot', out)

    def test_duplicate_keys_and_same_team_twice(self):
        code, out = self.errors(exhibitionplayerbiotable=lambda r: r.append(list(r[0])))
        self.assertEqual(code, 1)
        self.assertIn('duplicate keys', out)
        code, out = self.errors(exhibitionrostertable=lambda r: r.append([3, 0, 100, 7]))
        self.assertIn('twice on the same team', out)

    def test_player_on_two_teams_is_only_info(self):
        code, out = self.errors(exhibitionrostertable=lambda r: r.append([42, 1, 100, 7]))
        self.assertEqual(code, 0, out)
        self.assertIn('more than one team', out)

    def test_dangling_free_agent_and_draft_pick(self):
        code, out = self.errors(exhibitionfreeagents=lambda r: r.append([900]), exhibitiondraftpick=lambda r: r.append([2, 9, 901]))
        self.assertEqual(code, 1)
        self.assertIn('free agent points to no player', out)
        self.assertIn('draft pick points to no player', out)
        self.assertIn('draft pick has unknown team', out)

    def test_crc_error(self):
        d = bytearray(open(self.db, 'rb').read())
        d[-6] ^= 1
        open(self.db, 'wb').write(d)
        code, out = run('check', self.db)
        self.assertEqual(code, 1)
        self.assertIn('data', out)

    def test_draft_roster_in_step(self):
        draft = self.p('draft_roster.db')
        t = good_tables()
        write_db(self.db, t)
        ten = {k: v for k, v in t.items() if k != 'exhibitionteams'}
        extra = dict(ten)  # only 9 tables here: must be reported
        write_db(draft, extra, meta=False)
        code, out = run('check', self.db, '--draft', draft)
        self.assertEqual(code, 1)
        self.assertIn('exactly 10', out)
        # a draft file with differing rows reports them as warnings
        ten['exhibitionplayerbiotable'] = ([list(r) for r in ten['exhibitionplayerbiotable'][0]], set())
        ten['exhibitionplayerbiotable'][0][0][2] = 'Changed'
        write_db(draft, ten, meta=False)
        code, out = run('check', self.db, '--draft', draft)
        self.assertIn('exhibitionplayerbiotable: 0 only in draft, 0 only in nhlng, 1 changed', out)


class DiffTest(Base):
    def test_diff(self):
        other = self.p('b', 'nhlng.db')
        os.makedirs(self.p('b'))
        t = good_tables()
        t['exhibitionplayerbiotable'][0][0][2] = 'Alphb'
        t['exhibitionfreeagents'] = ([[105], [104]], set())
        t['exhibitionrostertable'][0].pop()
        write_db(other, t)
        code, out = run('diff', self.db, other, '--verbose')
        self.assertEqual(code, 0)
        self.assertIn('exhibitionplayerbiotable: 0 added, 0 removed, 1 changed', out)
        self.assertIn("lastname 'Alpha'->'Alphb'", out)
        self.assertIn('exhibitionfreeagents: 1 added', out)
        self.assertIn('exhibitionrostertable: 0 added, 1 removed', out)
        self.assertNotIn('exhibitionteams', out)

    def test_diff_same(self):
        self.assertIn('no differences', run('diff', self.db, self.db)[1])


if __name__ == '__main__':
    unittest.main()
