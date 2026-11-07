#!/usr/bin/env python3
"""Tests for nhl_fetch.py and build_roster.py on a synthetic database and a synthetic fake API cache (no real data).
Run: python3 test_build_roster.py"""
import hashlib
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_roster
import nhl_fetch
import rosters
import tdb
import test_rosters as tr

I, S = tdb.TYPE_INT, tdb.TYPE_STRING
SK_RATINGS = ['wristshotpower', 'slapshotpower', 'wristaccuracy', 'slapaccuracy', 'deking', 'puckcontrol', 'passing', 'offawareness',
              'handeye', 'poise', 'defawareness', 'stickchecking', 'shotblocking', 'bodychecking', 'speed', 'acceleration', 'agility',
              'balance', 'endurance', 'strength', 'durability', 'discipline', 'aggression', 'fightingskill', 'faceoffs', 'overall']
GO_RATINGS = ['gsh', 'gsl', 'ssh', 'ssl', '5hole', 'positioning', 'reboundcontrol', 'shotrecover', 'vision', 'breakaway', 'poise',
              'speed', 'agility', 'endurance', 'puckplayfrequency', 'pokecheck', 'passing', 'aggression', 'durability', 'overall']


def fl(prefix, items):
    return [(n, f'{prefix}{i:03d}', I, b, low) for i, (n, b, low) in enumerate(items)]


BIO = [('game_id', 14, 0), ('position', 3, 0), ('handedness', 1, 0), ('jersey', 7, 0), ('year', 8, 0), ('month', 4, 0), ('day', 5, 0),
       ('height', 5, 0), ('weight', 8, 0), ('hasportrait', 1, 0), ('artid', 16, 0), ('audioid', 16, 0), ('headid', 16, 0),
       ('draftyear', 8, 0), ('draftround', 4, 0), ('draftposition', 9, 0), ('draftteam', 5, -1), ('rookie', 1, 0),
       ('nhlgamesplayedthreeseasonsago', 7, 0), ('nhlgamesplayedtwoseasonsago', 7, 0), ('nhlgamesplayedlastseason', 7, 0),
       ('nhlgamesplayedcareer', 12, 0), ('progamesplayedcareer', 12, 0), ('team', 8, -1)]
LINES = ['l1c', 'l1lw', 'l1rw', 'l1ld', 'l1rd', 'l2c', 'g1', 'g2', 'rosterstatus', 'pp1c']
SCHEMA = dict(tr.SCHEMA)
SCHEMA['exhibitionteams'] = ('TEAM', fl('t', [('index', 8, 0)]) + [('abbrname', 'tAbb', S, 32, None)], ['index'])
SCHEMA['exhibitionplayerbiotable'] = ('BIOT', fl('b', BIO) + [('firstname', 'bFir', S, 200, None), ('lastname', 'bLas', S, 200, None),
                                                              ('birthplace', 'bBir', S, 320, None)], ['game_id'])
SCHEMA['exhibitionskaterai'] = ('SKAI', fl('s', [('game_id', 14, 0), ('trait0', 6, 0), ('c_fighting', 2, 0)] +
                                           [('c_' + n, 6, 0) for n in SK_RATINGS]), ['game_id'])
SCHEMA['exhibitiongoalieai'] = ('GOAI', fl('g', [('game_id', 14, 0), ('trait0', 3, 0), ('c_fighting', 2, 0)] +
                                           [('c_' + n, 6, 0) for n in GO_RATINGS]), ['game_id'])
SCHEMA['exhibitionrostertable'] = ('ROST', fl('r', [('key', 14, 0), ('team', 8, 0), ('playerindex', 14, 0), ('jersey', 7, 0),
                                                    ('captain', 2, 0)] + [(n, 1, 0) for n in LINES]), ['key'])
bio_names = [f[0] for f in SCHEMA['exhibitionplayerbiotable'][1]]
ro_names = [f[0] for f in SCHEMA['exhibitionrostertable'][1]]

# donors of team 0 (ANA) and team 1 (LA); team 2 (ZZZ) is not mapped. (gid, position, hand, jersey, lines set)
DON0 = [(10, 0, 0, 1, ['l1c']), (11, 0, 0, 2, ['l2c']), (12, 1, 0, 3, ['l1lw']), (13, 2, 1, 4, ['l1rw']), (14, 3, 0, 5, ['l1ld']),
        (15, 3, 1, 6, ['l1rd']), (16, 4, 0, 30, ['g1']), (17, 4, 0, 31, ['g2'])]
DON1 = [(20, 0, 0, 11, ['l1c']), (21, 1, 0, 12, ['l1lw']), (22, 2, 1, 13, ['l1rw']), (23, 3, 0, 14, ['l1ld']), (24, 3, 1, 15, ['l1rd']),
        (25, 4, 0, 40, ['g1']), (26, 4, 0, 41, ['g2']), (27, 0, 0, 42, [])]
DON2 = [(30, 0, 0, 51, ['l1c']), (31, 4, 0, 52, ['g1'])]


def synth_tables():
    t = {}
    t['exhibitionteams'] = ([[0, 'ANA'], [1, 'LA'], [2, 'ZZZ']], set())
    donors = [(0, d) for d in DON0] + [(1, d) for d in DON1] + [(2, d) for d in DON2]
    bio, sk, go, ro, pl, ske, goe = [], [], [], [], [], [], []
    for n, (team, (gid, pos, hand, jer, lines)) in enumerate(donors):
        b = {k: 0 for k in bio_names}
        b.update(game_id=gid, position=pos, handedness=hand, jersey=jer, team=team, hasportrait=1, artid=gid, audioid=gid, headid=gid,
                 draftyear=100, draftteam=3, rookie=1)
        names = ['Old', f'Donor{gid}', 'Nowhere']
        bio.append([b[f[0]] if f[2] == I else names[['firstname', 'lastname', 'birthplace'].index(f[0])] for f in SCHEMA['exhibitionplayerbiotable'][1]])
        if pos == 4:
            go.append([{'game_id': gid, 'trait0': 3, 'c_fighting': 1}.get(f[0], 30) for f in SCHEMA['exhibitiongoalieai'][1]])
            goe.append([gid, 6])
        else:
            sk.append([{'game_id': gid, 'trait0': 9, 'c_fighting': 1}.get(f[0], 30) for f in SCHEMA['exhibitionskaterai'][1]])
            ske.append([gid, 5])
        r = {k: 0 for k in ro_names}
        r.update(key=team * 40 + n, team=team, playerindex=100 + n, jersey=jer, rosterstatus=1, captain=1 if n == 0 else 0)
        for l in lines:
            r[l] = 1
        ro.append([r[k] for k in ro_names])
        pl.append([100 + n, gid, 0])
    t['exhibitionplayerbiotable'] = (bio, set())
    t['exhibitionskaterai'] = (sk, set())
    t['exhibitiongoalieai'] = (go, set())
    t['exhibitionrostertable'] = (ro, set())
    t['exhibitionplayers'] = (pl, set())
    t['exhibitionskaterequipment'] = (ske, set())
    t['exhibitiongoalieequipment'] = (goe, set())
    # fix the roster key to team*40 + slot (slot = index within the team)
    cnt = {}
    for row in ro:
        cnt[row[1]] = cnt.get(row[1], -1) + 1
        row[0] = row[1] * 40 + cnt[row[1]]
    return {k: v for k, v in t.items()}


def fake_player(pid, first, last, pos, num, shoots='L', city='Town', h=72, w=190):
    return dict(id=pid, firstName={'default': first}, lastName={'default': last}, sweaterNumber=num, positionCode=pos, shootsCatches=shoots,
                heightInInches=h, weightInPounds=w, birthDate='1999-02-03', birthCity={'default': city}, birthCountry='CAN')


def fake_stats(pid, gp, pts, toi=900, goalie=False, saves=0, sa=0):
    if goalie:
        return dict(playerId=pid, gamesPlayed=gp, gamesStarted=gp, saves=saves, shotsAgainst=sa, savePercentage=saves / max(sa, 1))
    return dict(playerId=pid, gamesPlayed=gp, points=pts, plusMinus=0, penaltyMinutes=4, shots=gp * 2, goals=0,
                avgTimeOnIcePerGame=toi, faceoffWinPctg=0.5)


# fake API: ANA has more players than donors, LAK fewer; a goalie ranks by games started
ANA = [fake_player(1, 'Jürgen', 'Slafkovský', 'C', 91, 'R'), fake_player(2, 'Bo', 'Two', 'C', 92), fake_player(3, 'Cy', 'Three', 'C', 93),
       fake_player(4, 'Di', 'Four', 'L', 94), fake_player(5, 'Ed', 'Five', 'L', 95), fake_player(6, 'Fo', 'Six', 'R', 96, 'R'),
       fake_player(7, 'Gus', 'Seven', 'D', 97, 'L'), fake_player(8, 'Hal', 'Eight', 'D', 98, 'R'), fake_player(9, 'Ian', 'Nine', 'D', 99, 'L'),
       fake_player(10, 'Starter', 'Goal', 'G', 31), fake_player(11, 'Backup', 'Goal', 'G', 35), fake_player(12, 'Third', 'Goal', 'G', 70)]
LAK = [fake_player(21, 'Al', 'One', 'C', 11), fake_player(22, 'Bea', 'Two', 'L', 12), fake_player(23, 'Cat', 'Three', 'R', 13, 'R'),
       fake_player(24, 'Dan', 'Four', 'D', 14), fake_player(25, 'Eve', 'Five', 'G', 30), fake_player(26, 'Fin', 'Six', 'G', 31)]
STAT_ANA = ([fake_stats(1, 20, 20, 1200), fake_stats(2, 20, 10), fake_stats(3, 20, 5, 700), fake_stats(4, 20, 12), fake_stats(5, 20, 2),
             fake_stats(6, 20, 8), fake_stats(7, 20, 9, 1300), fake_stats(8, 20, 3), fake_stats(9, 20, 1, 800)],
            [fake_stats(10, 15, 0, goalie=True, saves=400, sa=430), fake_stats(11, 5, 0, goalie=True, saves=120, sa=140)])
STAT_LAK = ([fake_stats(21, 20, 15), fake_stats(22, 20, 10), fake_stats(23, 20, 8), fake_stats(24, 20, 6)],
            [fake_stats(25, 10, 0, goalie=True, saves=250, sa=280), fake_stats(26, 10, 0, goalie=True, saves=230, sa=260)])


def write_cache(cache):
    os.makedirs(cache, exist_ok=True)
    for tc, plist, (sk, go) in (('ANA', ANA, STAT_ANA), ('LAK', LAK, STAT_LAK)):
        d = dict(forwards=[p for p in plist if p['positionCode'] in 'CLR'], defensemen=[p for p in plist if p['positionCode'] == 'D'],
                 goalies=[p for p in plist if p['positionCode'] == 'G'])
        json.dump(d, open(f'{cache}/roster_{tc}.json', 'w'))
        json.dump(dict(season=20262027, gameType=2, skaters=sk, goalies=go), open(f'{cache}/stats_cur_{tc}.json', 'w'))
        json.dump(dict(season=20252026, gameType=2, skaters=[], goalies=[]), open(f'{cache}/stats_prev_{tc}.json', 'w'))


def sha(p):
    return hashlib.sha1(open(p, 'rb').read()).hexdigest()


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        self.src = os.path.join(self.d, 'src', 'nhlng.db')
        os.makedirs(os.path.dirname(self.src))
        old = tr.SCHEMA
        tr.SCHEMA = SCHEMA
        try:
            tr.write_db(self.src, synth_tables())
        finally:
            tr.SCHEMA = old
        self.cache = os.path.join(self.d, 'cache')
        write_cache(self.cache)
        self.out = os.path.join(self.d, 'out.db')
        self.meta = os.path.join(self.d, 'src', 'nhlng-meta.xml')
        self.h = sha(self.src)

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, out=None, **kw):
        return build_roster.build(self.src, out or self.out, self.cache, self.meta, log=lambda *a: None, **kw)

    def rows(self, path, team):
        db = rosters.open_db(path, self.meta)
        ro, pl, bio = db.table('exhibitionrostertable'), db.table('exhibitionplayers'), db.table('exhibitionplayerbiotable')
        i2p = {pl.get(r, 'index'): pl.get(r, 'playerid') for r in pl.live_rows()}
        br = {bio.get(r, 'game_id'): r for r in bio.live_rows()}
        out = []
        for r in ro.live_rows():
            if ro.get(r, 'team') == team:
                b = br[i2p[ro.get(r, 'playerindex')]]
                out.append((ro, r, bio, b))
        return db, out

    def test_input_unchanged_and_check_clean(self):
        s = self.build()
        self.assertEqual(sha(self.src), self.h)
        self.assertEqual(s['ANA']['changed'], 8)
        db = rosters.open_db(self.out, self.meta)
        self.assertEqual(db.check(), [])
        rep = rosters.cmd_check(self.out, meta=self.meta)
        self.assertEqual(rep, 0)

    def test_same_path_refused(self):
        with self.assertRaises(ValueError):
            self.build(out=self.src)

    def test_idempotent(self):
        self.build()
        a = sha(self.out)
        self.build(out=os.path.join(self.d, 'out2.db'))
        self.assertEqual(a, sha(os.path.join(self.d, 'out2.db')))

    def test_names_folding_fields_and_wipes(self):
        self.build()
        db, rows = self.rows(self.out, 0)
        names = {(bio.get(b, 'firstname'), bio.get(b, 'lastname')): (ro, r, bio, b) for ro, r, bio, b in rows}
        ro, r, bio, b = names[('Jurgen', 'Slafkovsky')]       # ASCII-folded: the synthetic stock has no accents
        self.assertEqual(bio.get(b, 'jersey'), 91)
        self.assertEqual(ro.get(r, 'jersey'), 91)
        self.assertEqual((bio.get(b, 'year'), bio.get(b, 'month'), bio.get(b, 'day')), (99, 1, 2))
        self.assertEqual((bio.get(b, 'height'), bio.get(b, 'weight')), (72 - 54, 190 - 123))
        self.assertEqual(bio.get(b, 'birthplace'), 'Town')
        self.assertEqual((bio.get(b, 'hasportrait'), bio.get(b, 'artid'), bio.get(b, 'audioid')), (0, 0, 0))
        self.assertEqual(bio.get(b, 'headid'), build_roster.HEAD_GENERIC)
        self.assertEqual(bio.get(b, 'handedness'), 1)
        sk = db.table('exhibitionskaterai')
        gid = bio.get(b, 'game_id')
        srow = [x for x in sk.live_rows() if sk.get(x, 'game_id') == gid][0]
        self.assertEqual(sk.get(srow, 'trait0'), 0)
        self.assertTrue(30 < sk.get(srow, 'c_overall') < 63)

    def test_lines_and_goalies(self):
        self.build()
        db, rows = self.rows(self.out, 0)
        by_flag = {}
        for ro, r, bio, b in rows:
            for f in ('l1c', 'l2c', 'l1ld', 'l1rd', 'g1', 'g2'):
                if ro.get(r, f):
                    by_flag[f] = bio.get(b, 'lastname')
        self.assertEqual(by_flag['g1'], 'Goal')
        # starter (most games started) in g1, the backup in g2, the third goalie dropped
        gl = {bio.get(b, 'firstname') for ro, r, bio, b in rows if bio.get(b, 'position') == 4}
        self.assertEqual(gl, {'Starter', 'Backup'})
        g1 = [bio.get(b, 'firstname') for ro, r, bio, b in rows if ro.get(r, 'g1')]
        self.assertEqual(g1, ['Starter'])
        self.assertEqual(by_flag['l1c'], 'Slafkovsky')       # best centre on line 1
        # D slots: right-shooting D on the right-defence slot
        ld = [bio.get(b, 'handedness') for ro, r, bio, b in rows if ro.get(r, 'l1rd')]
        self.assertEqual(ld, [1])
        # one captain, two assistants, all skaters
        caps = sorted(ro.get(r, 'captain') for ro, r, bio, b in rows)
        self.assertEqual([c for c in caps if c], [1, 2, 2])

    def test_surplus_rows_deleted_or_kept(self):
        s = self.build()
        self.assertEqual(s['LA']['surplus'], 2)   # 8 donors, 6 players
        db, rows = self.rows(self.out, 1)
        self.assertEqual(len(rows), 6)
        keep = os.path.join(self.d, 'keep.db')
        self.build(out=keep, surplus='keep')
        db, rows = self.rows(keep, 1)
        self.assertEqual(len(rows), 8)
        self.assertEqual(rosters.cmd_check(keep, meta=self.meta), 0)
        left = [bio.get(b, 'firstname') for ro, r, bio, b in rows if ro.get(r, 'rosterstatus') == 0]
        self.assertEqual(left, ['Old'] * 2)

    def test_unmapped_team_untouched(self):
        self.build()
        db, rows = self.rows(self.out, 2)
        self.assertEqual({bio.get(b, 'firstname') for ro, r, bio, b in rows}, {'Old'})

    def test_fold_and_limits(self):
        allowed = set('abcé')
        self.assertEqual(build_roster.fold('Éric', allowed, 24), 'Eric')
        self.assertEqual(build_roster.fold('café', allowed, 24), 'café')
        self.assertEqual(build_roster.fold('Łukasz Øster', allowed, 24), 'Lukasz Oster')
        self.assertLessEqual(len(build_roster.fold('x' * 40, allowed, 24)), 24)
        self.assertLessEqual(len(build_roster.fold('é' * 40, allowed, 24).encode()), 24)


class FetchTests(unittest.TestCase):
    def test_fetch_idempotent_manifest(self):
        calls = []

        def opener(url):
            calls.append(url)
            if '/roster/' in url:
                return url.replace('current', '20262027'), json.dumps(dict(forwards=[], defensemen=[], goalies=[])).encode()
            return url, json.dumps(dict(season=20262027, skaters=[], goalies=[])).encode()
        with tempfile.TemporaryDirectory() as d:
            nhl_fetch.fetch_all(d, ['ANA'], delay=0, opener=opener, log=lambda *a: None)
            self.assertEqual(len(calls), 3)
            self.assertIn('club-stats/ANA/20252026/2', calls[2])
            m = json.load(open(os.path.join(d, 'manifest.json')))
            self.assertEqual(len(m), 3)
            self.assertIn('fetched', m['roster_ANA.json'])
            nhl_fetch.fetch_all(d, ['ANA'], delay=0, opener=opener, log=lambda *a: None)
            self.assertEqual(len(calls), 3)          # nothing fetched again
            nhl_fetch.fetch_all(d, ['ANA'], delay=0, refresh=True, opener=opener, log=lambda *a: None)
            self.assertEqual(len(calls), 6)

    def test_prev_season(self):
        self.assertEqual(nhl_fetch.prev_season(20262027), '20252026')


if __name__ == '__main__':
    unittest.main()
