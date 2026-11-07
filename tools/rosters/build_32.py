#!/usr/bin/env python3
"""Experiment (roadmap 2e): Seattle Kraken and Vegas Golden Knights in the all-star slots 30/31 of nhlng.db.
Documented in docs/rosters-32-teams.md. The all-star teams (Red/Blue) are LOST in the output.

  build_32.py IN_DB OUT_DB --meta META --meta-out META_OUT [--cache DIR] [--art donor|allstar] [--no-roster] [--ahl] [--fallback]

IN_DB is a stock or build_roster.py database, widened or not (an unwidened one is widened first with widen_teams, so
META_OUT is always written). Per slot: VGK = 30, SEA = 31 (the all-star gear colours are red in 30 and navy in 31, which
come out nearest to gold and to the Kraken navy after the remap below).

 * exhibitionteams row: cloned from a donor NHL team (art, masks, fonts, arena, audio, jersey style), then names, short
   names, abbreviations, colours, division/conference, rival (30 <-> 31), AHL affiliate (-1) set. --art allstar keeps the
   all-star art, masks and arena instead of cloning them.
 * exhibitionteamstrategies row: cloned from the donor team. stockteamjerseys: six rows copied from the donor's rows
   (default jerseys first), team id 30/31. stockteamcolorstable: 3 rows set. Equipment (54 rows) and goalie equipment:
   all-star colours remapped to team colours (table sizes are fixed, rows are never added).
 * rosters: the 22 players of the EASHL 1/2 teams (220/221: 2 goalies, 4 lines, 3 D pairs, 2 spare; nobody shared with an
   NHL team, not used in Play Now) are rewritten with the current club's players using build_roster.py's assign/write_player,
   copied into the physical roster rows of teams 30/31 (keys 1200.. / 1240..), the remaining all-star roster rows are
   deleted. The EASHL roster rows themselves stay (their players now also stand on 30/31). Players get proteam/team 30/31.
Output holds EA data: keep it out of the repo.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb
import rosters
import build_roster as br
import widen_teams
from nhl_fetch import DEFAULT_CACHE

# slot -> (tricode, donor NHL team row for cloning, donor roster team, identity)
SLOTS = {
    30: dict(tri='VGK', donor=23, roster_donor=220, short='Vegas', full='Vegas Golden Knights', abbr='VGK',
             c1=(180, 151, 90), c2=(51, 63, 72), black=(35, 31, 32)),
    31: dict(tri='SEA', donor=28, roster_donor=221, short='Seattle', full='Seattle Kraken', abbr='SEA',
             c1=(0, 22, 40), c2=(153, 217, 217), black=(35, 31, 32)),
}
# all-star gear colours -> team colours (equipment and goalie equipment); keys are what the stock rows of 30/31 use
REMAP = {
    30: {(158, 16, 28): (180, 151, 90), (133, 13, 15): (140, 115, 70)},
    31: {(21, 42, 100): (0, 22, 40), (15, 53, 97): (0, 40, 60), (21, 32, 98): (0, 30, 55)},
}
AHL = {}   # slot -> AHL team row as affiliate; empty = none (-1), --ahl fills it
KEEP_FROM_ALLSTAR = {'index', 'shortname', 'fullname', 'abbrname', 'artabbr', 'primarycolor_r', 'primarycolor_g', 'primarycolor_b',
                     'secondarycolor_r', 'secondarycolor_g', 'secondarycolor_b', 'rivalteam', 'rivalteam2', 'ahlaffiliate',
                     'divisiongroup', 'conferencegroup', 'active', 'state', 'table', 'league', 'leaguegroup'}


def clone_row(t, src, dst, skip=()):
    for f in t.fields.values():
        if f.name not in skip:
            t.set(dst, f.name, t.get(src, f.name))


def set_team_row(db, slot, art):
    s = SLOTS[slot]
    te = db.table('exhibitionteams')
    if art == 'donor':
        clone_row(te, s['donor'], slot, KEEP_FROM_ALLSTAR)
        ts = db.table('exhibitionteamstrategies')
        clone_row(ts, s['donor'], slot, {'index'})
    te.set(slot, 'shortname', s['short'])
    te.set(slot, 'fullname', s['full'])
    te.set(slot, 'abbrname', s['abbr'])
    te.set(slot, 'artabbr', s['abbr'])
    for n, c in (('primarycolor', s['c1']), ('secondarycolor', s['c2'])):
        for k, v in zip('rgb', c):
            te.set(slot, f'{n}_{k}', v)
    te.set(slot, 'rivalteam', [k for k in SLOTS if k != slot][0])   # the two teams are rivals
    te.set(slot, 'rivalteam2', -1)
    te.set(slot, 'ahlaffiliate', AHL.get(slot, -1))
    te.set(slot, 'divisiongroup', 3)
    te.set(slot, 'conferencegroup', 1)
    te.set(slot, 'active', 1)
    te.set(slot, 'table', 0)
    te.set(slot, 'league', 0)


def set_jerseys(db, slot):
    tj = db.table('stockteamjerseys')
    mine = sorted([r for r in tj.live_rows() if tj.get(r, 'teamid') == slot], key=lambda r: tj.get(r, 'jerseyindex'))
    d = SLOTS[slot]['donor']
    don = sorted([r for r in tj.live_rows() if tj.get(r, 'teamid') == d], key=lambda r: tj.get(r, 'jerseyindex'))
    pick = [r for r in don if tj.get(r, 'isdefault')] + [r for r in don if not tj.get(r, 'isdefault')]
    pick = pick[:len(mine)]
    pick.sort(key=lambda r: tj.get(r, 'jerseyindex'))
    for k, (dst, src) in enumerate(zip(mine, pick)):
        clone_row(tj, src, dst, {'teamid'})
        tj.set(dst, 'teamid', slot)
        tj.set(dst, 'jerseyindex', k)   # contiguous 0..5 like the all-star rows (defaults come last, as there)
    return len(mine), len(pick)


def set_colors(db, slot):
    s = SLOTS[slot]
    tc = db.table('stockteamcolorstable')
    for r in sorted([r for r in tc.live_rows() if tc.get(r, 'team') == slot], key=lambda r: tc.get(r, 'colorid')):
        c = (s['c1'], s['c2'], s['black'])[min(tc.get(r, 'colorid'), 2)]
        for k, v in zip('rgb', c):
            tc.set(r, f'color_{k}', v)


def remap_gear(db, slot):
    """Replace every colour triple of the all-star gear rows that is in REMAP; returns the number of triples changed."""
    n = 0
    for tn in ('stockteamequipmenttable', 'stockgoalieequipment'):
        t = db.table(tn)
        groups = {}
        for f in t.fields.values():
            if f.name.endswith(('_r', '_g', '_b')) and f.name[-2] == '_' or f.name.endswith(('color_r', 'color_g', 'color_b')):
                groups.setdefault(f.name[:-1], {})[f.name[-1]] = f.name
        groups = {k: v for k, v in groups.items() if set(v) == {'r', 'g', 'b'}}
        for r in t.live_rows():
            if t.get(r, 'team') != slot:
                continue
            for g in groups.values():
                cur = tuple(t.get(r, g[c]) for c in 'rgb')
                if cur in REMAP.get(slot, {}):
                    for c, v in zip('rgb', REMAP[slot][cur]):
                        t.set(r, g[c], v)
                    n += 1
    return n


def build_rosters(db, cache, log=print):
    T = db.table
    te, ro, pl, bio = T('exhibitionteams'), T('exhibitionrostertable'), T('exhibitionplayers'), T('exhibitionplayerbiotable')
    sk, go = T('exhibitionskaterai'), T('exhibitiongoalieai')
    i2p = {pl.get(r, 'index'): pl.get(r, 'playerid') for r in pl.live_rows()}
    bio_row = {bio.get(r, 'game_id'): r for r in bio.live_rows()}
    sk_row = {sk.get(r, 'game_id'): r for r in sk.live_rows()}
    go_row = {go.get(r, 'game_id'): r for r in go.live_rows()}
    allowed = br.charset_of(db)
    tris = set(br.TEAM_MAP.values()) | {s['tri'] for s in SLOTS.values()}
    rost, stats = br.load_cache(cache, tris)
    pos_of = {p['id']: p['pos'] for lst in rost.values() for p in lst}
    rater = br.Rater(stats, pos_of)
    wts = sorted(p['weight'] for lst in rost.values() for p in lst if p['weight'] and p['pos'] != 'G')
    wpct = lambda w: (min(max((w - wts[0]) / (wts[-1] - wts[0]), 0), 1) if w and wts and wts[-1] > wts[0] else 0.5)
    by = {}
    for r in ro.live_rows():
        by.setdefault(ro.get(r, 'team'), []).append(r)
    for slot, s in SLOTS.items():
        players = []
        for p in rost[s['tri']]:
            inf = rater.get(p['id'])
            players.append(dict(p, q=(inf['q'] if inf else br.UNKNOWN_Q), gs=(inf or {}).get('gs', 0), info=inf))
        drows = by[s['roster_donor']]
        slots = br.donor_slots(ro, drows, bio, bio_row, i2p)
        for sl in slots:
            sl['g1'] = bool(ro.get(sl['row'], 'g1'))
        match, dropped, extra = br.assign(slots, players, None)
        for i, p in match.items():
            br.write_player(slots[i], p, ro, bio, sk, go, sk_row, go_row, allowed, wpct)
            b = slots[i]['bio']
            bio.set(b, 'team', slot)
            bio.set(b, 'proteam', slot)
        # physical rows of the all-star team, in key order
        target = sorted(by[slot], key=lambda r: ro.get(r, 'key'))
        order = sorted(match, key=lambda i: slots[i]['key'])
        if len(order) > len(target):   # only with --fallback (a stock team with fewer rows): drop spare rows first
            order = sorted(sorted(order, key=lambda i: (slots[i]['rank'] == 9 and slots[i]['role'] != 'G', slots[i]['key']))[:len(target)],
                           key=lambda i: slots[i]['key'])
        for k, (dst, i) in enumerate(zip(target, order)):
            src = slots[i]['row']
            clone_row(ro, src, dst, {'team', 'key'})
            ro.set(dst, 'team', slot)
            ro.set(dst, 'key', slot * rosters.TEAM_SLOTS + k)
        for dst in target[len(order):]:
            ro.delete_row(dst)
        best = sorted([(p['q'], target[k]) for k, (i, p) in enumerate((i, match[i]) for i in order) if p['pos'] != 'G'], reverse=True)[:3]
        for dst in target[:len(order)]:
            ro.set(dst, 'captain', 0)
        for n, (_, row) in enumerate(best):
            ro.set(row, 'captain', 1 if n == 0 else 2)
        ng = sum(1 for i in order if match[i]['pos'] == 'G')
        log(f'{s["abbr"]} (slot {slot}): API {len(players)}, written {len(match)} (goalies {ng}), dropped {len(dropped)}, '
            f'all-star rows deleted {len(target) - len(order)}')


def build(src, out, meta, meta_out, cache=DEFAULT_CACHE, art='donor', roster=True, log=print):
    for a, b in ((src, out), (meta, meta_out)):
        if os.path.abspath(a) == os.path.abspath(b):
            raise ValueError('OUT must differ from the input')
    db = rosters.open_db(src, meta)
    text = widen_teams.read_text(db.meta_path)
    if db.table('exhibitionplayerbiotable').field('proteam').bits < 6:
        text, rep = widen_teams.widen_teams(db, text, 6)
        log(f'widened {len(rep)} fields')
    for slot in SLOTS:
        set_team_row(db, slot, art)
        if art == 'donor':
            nj = set_jerseys(db, slot)
        else:
            nj = 'kept'
        set_colors(db, slot)
        log(f'slot {slot} {SLOTS[slot]["full"]}: jerseys {nj}, gear colour triples remapped {remap_gear(db, slot)}')
    if roster:
        build_rosters(db, cache, log)
    db.save(out)
    widen_teams.write_text(meta_out, text)
    return out


def main(argv):
    a = list(argv)

    def opt(name, default=None):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default
    meta, meta_out = opt('--meta'), opt('--meta-out')
    cache, art = opt('--cache', DEFAULT_CACHE), opt('--art', 'donor')
    if '--ahl' in a:
        a.remove('--ahl')
        AHL.update({30: 34, 31: 33})   # donor AHL rows (Binghamton, Bakersfield): only a valid link, not a real affiliate
    if '--fallback' in a:   # content test in two LISTED slots (0 = SEA data, 25 = VGK data) instead of 30/31
        a.remove('--fallback')
        SLOTS[0], SLOTS[25] = SLOTS.pop(31), SLOTS.pop(30)
    roster = '--no-roster' not in a
    if not roster:
        a.remove('--no-roster')
    if len(a) != 2 or not meta or not meta_out or art not in ('donor', 'allstar'):
        print(__doc__)
        return 2
    build(a[0], a[1], meta, meta_out, cache, art, roster)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
