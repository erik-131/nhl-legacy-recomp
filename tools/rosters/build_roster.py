#!/usr/bin/env python3
"""Stage 1 roster build (roadmap 2d): stock nhlng.db + NHL API cache -> new resealed nhlng.db. Documented in docs/rosters-pipeline.md.

  build_roster.py STOCK_DB OUT_DB [--cache DIR] [--meta PATH] [--surplus delete|keep] [--teams ANA,BOS,...]

Per mapped team the existing donor player rows are reused (no rows inserted): each donor's bio, skater/goalie AI row and
roster row are rewritten for a player of the current club roster. The input DB is never modified; OUT_DB must differ.
Ratings are derived from season stats (see rate_* below), never copied from any game. Output and cache stay outside git.
"""
import json
import os
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb
import rosters
from nhl_fetch import DEFAULT_CACHE

# stock abbrname -> NHL API tricode. The stock slots are the 2014-15 league: ARZ (slot 22) becomes Utah;
# SEA and VGK have no slot in the stock file and are not built (stage 2e).
TEAM_MAP = {'ANA': 'ANA', 'WPG': 'WPG', 'BOS': 'BOS', 'BUF': 'BUF', 'CGY': 'CGY', 'CAR': 'CAR', 'CHI': 'CHI', 'COL': 'COL',
            'CBJ': 'CBJ', 'DAL': 'DAL', 'DET': 'DET', 'EDM': 'EDM', 'FLA': 'FLA', 'LA': 'LAK', 'MIN': 'MIN', 'MTL': 'MTL',
            'NSH': 'NSH', 'NJ': 'NJD', 'NYI': 'NYI', 'NYR': 'NYR', 'OTT': 'OTT', 'PHI': 'PHI', 'ARZ': 'UTA', 'PIT': 'PIT',
            'STL': 'STL', 'SJ': 'SJS', 'TB': 'TBL', 'TOR': 'TOR', 'VAN': 'VAN', 'WSH': 'WSH'}
POS_CODE = {'C': 0, 'L': 1, 'R': 2, 'D': 3, 'G': 4}
NAME_BYTES = 24        # name fields hold 25 bytes including the terminator
PLACE_BYTES = 39
SHRINK = 20            # games of prior in every shrunk stat
UNKNOWN_Q = 0.30       # quality of a player with no games in the cache
HEAD_GENERIC = 60000   # most common stock headid (318 players), a generic head
YEAR0, HEIGHT0, WEIGHT0 = 1900, 54, 123   # bio year = birth year - 1900; height = inches - 54; weight = lbs - 123 (stock, verified on known players)


# ---------------------------------------------------------------- cache

def load_json(cache, name):
    p = os.path.join(cache, name)
    return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else None


def nm(d):
    return (d.get('default') if isinstance(d, dict) else d) or ''


def load_cache(cache, tricodes):
    """{tricode: [player dict]} and {player id: stats dict summed over the cached seasons and clubs}."""
    rosters_, stats = {}, {}
    for tc in tricodes:
        r = load_json(cache, f'roster_{tc}.json')
        if r is None:
            continue
        lst = []
        for grp in ('forwards', 'defensemen', 'goalies'):
            for p in r.get(grp, []):
                lst.append(dict(id=p['id'], first=nm(p.get('firstName')), last=nm(p.get('lastName')), number=p.get('sweaterNumber'),
                                pos=p.get('positionCode'), shoots=p.get('shootsCatches'), height=p.get('heightInInches'),
                                weight=p.get('weightInPounds'), birth=p.get('birthDate'), city=nm(p.get('birthCity'))))
        rosters_[tc] = lst
    for fname in sorted(os.listdir(cache)):
        if not fname.startswith('stats_') or not fname.endswith('.json'):
            continue
        d = load_json(cache, fname)
        for kind in ('skaters', 'goalies'):
            for s in d.get(kind, []):
                a = stats.setdefault(s['playerId'], dict(gp=0, pts=0, pm=0, pim=0, toi=0.0, shots=0, goals=0, fo_n=0.0, fo=0.0,
                                                         gs=0, saves=0, sa=0))
                gp = s.get('gamesPlayed', 0)
                a['gp'] += gp
                if kind == 'skaters':
                    a['pts'] += s.get('points', 0)
                    a['pm'] += s.get('plusMinus', 0)
                    a['pim'] += s.get('penaltyMinutes', 0)
                    a['toi'] += s.get('avgTimeOnIcePerGame', 0) * gp
                    a['shots'] += s.get('shots', 0)
                    a['goals'] += s.get('goals', 0)
                    if s.get('faceoffWinPctg') is not None:
                        a['fo'] += s['faceoffWinPctg'] * gp
                        a['fo_n'] += gp
                else:
                    a['gs'] += s.get('gamesStarted', 0)
                    a['saves'] += s.get('saves', 0)
                    a['sa'] += s.get('shotsAgainst', 0)
    return rosters_, stats


# ---------------------------------------------------------------- names

def charset_of(db):
    """Characters the stock file already uses in names and birthplaces: these are known to be storable (UTF-8)."""
    bio = db.table('exhibitionplayerbiotable')
    cs = set()
    for r in bio.live_rows():
        for f in ('firstname', 'lastname', 'birthplace'):
            cs.update(bio.get(r, f))
    return cs


def fold(s, allowed, limit):
    """Keep characters the stock file uses, ASCII-fold the rest (NFKD without marks; a few letters by hand), cut to limit bytes."""
    special = {'ø': 'o', 'Ø': 'O', 'đ': 'd', 'Đ': 'D', 'ł': 'l', 'Ł': 'L', 'ß': 'ss', 'æ': 'ae', 'Æ': 'AE', 'œ': 'oe', 'ð': 'd',
               'þ': 'th', 'ı': 'i', '’': "'", '–': '-'}
    out = []
    for ch in s:
        if ord(ch) < 127 or ch in allowed:
            out.append(ch)
        elif ch in special:
            out.append(special[ch])
        else:
            out.append(''.join(c for c in unicodedata.normalize('NFKD', ch) if ord(c) < 127))
    s = ''.join(out).strip()
    while len(s.encode('utf-8')) > limit:
        s = s[:-1]
    return s


# ---------------------------------------------------------------- ratings (stage 1, from stats only)

def pct_rank(values):
    """{value index: rank in 0..1} (midrank percentile) for a list of numbers."""
    n = len(values)
    order = sorted(values)
    import bisect
    return [(bisect.bisect_left(order, v) + bisect.bisect_right(order, v)) / 2 / n for v in values]


class Rater:
    """Percentile ranks of shrunk per-game stats within position group (F, D) or goalies, over every player in the cache."""

    def __init__(self, stats, pos_of):
        # pos_of: {player id: position letter} for everyone we know a position of (all rostered players)
        self.q = {}
        groups = {'F': [], 'D': [], 'G': []}
        for pid, s in stats.items():
            g = pos_of.get(pid)
            if g:
                groups['G' if g == 'G' else 'D' if g == 'D' else 'F'].append(pid)
        self.stats = stats
        for g, ids in groups.items():
            if g == 'G':
                self._goalies(ids)
            else:
                self._skaters(g, ids)

    def _skaters(self, g, ids):
        S = self.stats
        ids = [i for i in ids if S[i]['gp'] > 0]
        if not ids:
            return
        mean = lambda f: sum(f(S[i]) for i in ids) / sum(S[i]['gp'] for i in ids)
        m_ppg, m_toi, m_shots = mean(lambda s: s['pts']), mean(lambda s: s['toi']), mean(lambda s: s['shots'])
        m_pim = mean(lambda s: s['pim'])
        k = SHRINK
        feats = {
            'off': [(S[i]['pts'] + k * m_ppg) / (S[i]['gp'] + k) for i in ids],
            'toi': [(S[i]['toi'] + k * m_toi) / (S[i]['gp'] + k) for i in ids],
            'pm': [S[i]['pm'] / (S[i]['gp'] + k) for i in ids],
            'shots': [(S[i]['shots'] + k * m_shots) / (S[i]['gp'] + k) for i in ids],
            'pim': [(S[i]['pim'] + k * m_pim) / (S[i]['gp'] + k) for i in ids],
        }
        pr = {n: pct_rank(v) for n, v in feats.items()}
        for j, i in enumerate(ids):
            off, toi, pm = pr['off'][j], pr['toi'][j], pr['pm'][j]
            q = (0.5 * off + 0.3 * toi + 0.2 * pm) if g == 'F' else (0.3 * off + 0.5 * toi + 0.2 * pm)
            qdef = 0.6 * toi + 0.4 * pm
            s = S[i]
            fo = (s['fo'] / s['fo_n']) if s['fo_n'] else None
            self.q[i] = dict(q=q, off=off, df=qdef, toi=toi, shots=pr['shots'][j], pim=pr['pim'][j], fo=fo, gp=s['gp'])

    def _goalies(self, ids):
        S = self.stats
        ids = [i for i in ids if S[i]['sa'] > 0]
        if not ids:
            return
        lg = sum(S[i]['saves'] for i in ids) / sum(S[i]['sa'] for i in ids)
        K = 500  # shots of prior
        for i in ids:
            s = S[i]
            sv = (s['saves'] + K * lg) / (s['sa'] + K)
            q = min(1.0, max(0.0, 0.5 + (sv - lg) / 0.040))  # +-2 save-percentage points = the whole range
            self.q[i] = dict(q=q, sv=sv, gp=s['gp'], gs=s['gs'])

    def get(self, pid):
        return self.q.get(pid)


def base(x, lo=36, hi=58):
    """x in 0..1 -> stored 6-bit rating. 36..58 is the stock range for players on NHL teams (p0..max, 611 skaters)."""
    return int(round(lo + min(1.0, max(0.0, x)) * (hi - lo)))


def skater_ratings(pos, info, weight_pct):
    """Rating dict (field -> stored value) for a skater from its quality record (None = no games)."""
    if info is None:
        info = dict(q=UNKNOWN_Q, off=UNKNOWN_Q, df=UNKNOWN_Q, toi=UNKNOWN_Q, shots=0.5, pim=0.5, fo=None, gp=0)
    else:  # shrink towards UNKNOWN_Q for tiny samples
        w = info['gp'] / (info['gp'] + SHRINK)
        info = dict(info, q=w * info['q'] + (1 - w) * UNKNOWN_Q, off=w * info['off'] + (1 - w) * 0.5,
                    df=w * info['df'] + (1 - w) * 0.5)
    q, off, df = info['q'], info['off'], info['df']
    phys = 0.25 + 0.5 * q
    r = {}
    for f in ('wristshotpower', 'slapshotpower'):
        r['c_' + f] = base(0.5 * off + 0.5 * info['shots'])
    for f in ('wristaccuracy', 'slapaccuracy', 'deking', 'puckcontrol', 'passing', 'offawareness', 'handeye'):
        r['c_' + f] = base(off)
    r['c_poise'] = base(q)
    for f in ('defawareness', 'stickchecking', 'shotblocking'):
        r['c_' + f] = base(df)
    r['c_bodychecking'] = base(0.5 * df + 0.5 * weight_pct)
    for f in ('speed', 'acceleration', 'agility', 'balance', 'endurance'):
        r['c_' + f] = base(phys)
    r['c_strength'] = base(weight_pct)
    r['c_durability'] = base(0.5)
    r['c_discipline'] = base(1 - info['pim'])
    r['c_aggression'] = base(info['pim'])
    r['c_fightingskill'] = base(info['pim'], 24, 50)
    r['c_fighting'] = 2 if info['pim'] > 0.97 else 1 if info['pim'] > 0.85 else 0
    fo = info['fo']
    r['c_faceoffs'] = base((fo - 0.40) / 0.20, 30, 58) if (pos == 'C' and fo is not None) else (36 if pos == 'C' else 20)
    keys = ['c_wristshotpower', 'c_passing', 'c_puckcontrol', 'c_offawareness', 'c_defawareness', 'c_stickchecking',
            'c_speed', 'c_acceleration', 'c_strength']
    r['c_overall'] = int(round(sum(r[k] for k in keys) / len(keys)))
    return r


GOALIE_CONST = {'c_speed': 51, 'c_agility': 50, 'c_endurance': 49, 'c_puckplayfrequency': 47, 'c_pokecheck': 48, 'c_passing': 48,
                'c_aggression': 50, 'c_durability': 49, 'c_fighting': 0}   # stock medians
GOALIE_Q = ('c_gsh', 'c_gsl', 'c_ssh', 'c_ssl', 'c_5hole', 'c_positioning', 'c_reboundcontrol', 'c_shotrecover', 'c_vision',
            'c_breakaway', 'c_poise')


def goalie_ratings(info):
    q = info['q'] if info else UNKNOWN_Q
    if info:
        w = info['gp'] / (info['gp'] + 5)
        q = w * q + (1 - w) * UNKNOWN_Q
    r = {f: base(q, 40, 56) for f in GOALIE_Q}
    r.update(GOALIE_CONST)
    r['c_overall'] = int(round(sum(r[f] for f in GOALIE_Q) / len(GOALIE_Q)))
    return r


# ---------------------------------------------------------------- build

LINE_F = ('c', 'lw', 'rw')
LINE_D = ('ld', 'rd')


def donor_slots(ro, rows, bio, bio_row, i2p):
    """Describe the donor roster rows of one team: row, game_id, role, line rank, special count."""
    flags = [f.name for f in ro.fields.values() if f.bits == 1 and f.name not in ('rosterstatus',)]
    names = set(flags)
    slots = []
    for r in rows:
        gid = i2p[ro.get(r, 'playerindex')]
        br = bio_row[gid]
        pos = bio.get(br, 'position')
        rank, role_line = 9, None
        for n in (1, 2, 3, 4):
            for s in (LINE_F + LINE_D):
                if f'l{n}{s}' in names and ro.get(r, f'l{n}{s}'):
                    rank, role_line = n, s
        special = sum(1 for f in flags if ro.get(r, f) and not (f.startswith('l') and f[1:2].isdigit()))
        if pos == 4:
            role = 'G'
        elif pos == 3:
            role = {'ld': 'LD', 'rd': 'RD'}.get(role_line) or ('RD' if bio.get(br, 'handedness') else 'LD')
        else:
            role = {0: 'C', 1: 'L', 2: 'R'}[pos]
        slots.append(dict(row=r, gid=gid, bio=br, role=role, rank=rank, special=special, key=ro.get(r, 'key')))
    return slots


def assign(slots, players, qof):
    """Match players to donor slots. players: dicts with pos letter and 'q'. Returns ({slot index: player}, dropped, surplus slots)."""
    skate = sorted([p for p in players if p['pos'] != 'G'], key=lambda p: -p['q'])
    goal = sorted([p for p in players if p['pos'] == 'G'], key=lambda p: (-p['gs'], -p['q']))
    out = {}
    gslots = sorted([i for i, s in enumerate(slots) if s['role'] == 'G'], key=lambda i: (slots[i]['key']))
    # starter first: the donor row with the g1 flag, else lowest key
    gslots.sort(key=lambda i: (not slots[i].get('g1'), slots[i]['key']))
    for i, p in zip(gslots, goal):
        out[i] = p
    order = sorted([i for i, s in enumerate(slots) if s['role'] != 'G'], key=lambda i: (slots[i]['rank'], -slots[i]['special'], slots[i]['key']))
    left = list(skate)

    def take(pred):
        c = [p for p in left if pred(p)]
        if c:
            left.remove(c[0])
            return c[0]
    for i in order:
        role = slots[i]['role']
        if role in ('C', 'L', 'R'):
            p = take(lambda p: p['pos'] == role) or take(lambda p: p['pos'] in ('C', 'L', 'R'))
        else:
            hand = 'L' if role == 'LD' else 'R'
            best = next((p for p in left if p['pos'] == 'D'), None)
            p = None
            if best:
                p = take(lambda p: p['pos'] == 'D' and p['shoots'] == hand and p['q'] >= best['q'] - 0.10) or take(lambda p: p['pos'] == 'D')
        if p:
            out[i] = p
    for i in order:  # leftover players into leftover rows (non-line scratches mostly)
        if i not in out and left:
            out[i] = left.pop(0)
    surplus = [i for i in range(len(slots)) if i not in out]
    return out, left + goal[len(gslots):], surplus


def build(stock, out_path, cache=DEFAULT_CACHE, meta=None, surplus='delete', only=None, log=print):
    if os.path.abspath(stock) == os.path.abspath(out_path):
        raise ValueError('OUT must differ from the input')
    db = rosters.open_db(stock, meta)
    T = db.table
    te, ro, pl, bio = T('exhibitionteams'), T('exhibitionrostertable'), T('exhibitionplayers'), T('exhibitionplayerbiotable')
    sk, go = T('exhibitionskaterai'), T('exhibitiongoalieai')
    abbr = {te.get(r, 'index'): te.get(r, 'abbrname') for r in te.live_rows()}
    i2p = {pl.get(r, 'index'): pl.get(r, 'playerid') for r in pl.live_rows()}
    bio_row = {bio.get(r, 'game_id'): r for r in bio.live_rows()}
    sk_row = {sk.get(r, 'game_id'): r for r in sk.live_rows()}
    go_row = {go.get(r, 'game_id'): r for r in go.live_rows()}
    allowed = charset_of(db)
    team_rows = {}
    for r in ro.live_rows():
        team_rows.setdefault(ro.get(r, 'team'), []).append(r)
    todo = {t: TEAM_MAP[a] for t, a in abbr.items() if a in TEAM_MAP and t < 30 and (not only or TEAM_MAP[a] in only)}
    rost, stats = load_cache(cache, set(todo.values()) | set(TEAM_MAP.values()))
    pos_of = {p['id']: p['pos'] for lst in rost.values() for p in lst}
    rater = Rater(stats, pos_of)
    wts = sorted(p['weight'] for lst in rost.values() for p in lst if p['weight'] and p['pos'] != 'G')
    wpct = lambda w: (min(max((w - wts[0]) / (wts[-1] - wts[0]), 0), 1) if w and wts and wts[-1] > wts[0] else 0.5)
    gid_users = {}
    for t, rows in team_rows.items():
        for r in rows:
            gid_users.setdefault(i2p[ro.get(r, 'playerindex')], set()).add(t)
    summary = {}
    for t in sorted(todo):
        tc = todo[t]
        if tc not in rost or not rost[tc]:
            log(f'{abbr[t]}: no cached roster for {tc}, left as in the stock file')
            continue
        players = []
        for p in rost[tc]:
            inf = rater.get(p['id'])
            players.append(dict(p, q=(inf['q'] if inf else UNKNOWN_Q), gs=(inf or {}).get('gs', 0), info=inf))
        # goalies are ranked by games started; the rest by quality
        slots = donor_slots(ro, team_rows[t], bio, bio_row, i2p)
        for s in slots:
            s['g1'] = bool(ro.get(s['row'], 'g1'))
        # a donor player also on another NHL team's roster cannot be reused twice: its row is surplus
        shared = {s['gid'] for s in slots if len([x for x in gid_users[s['gid']] if x < 30]) > 1}
        usable = [s for s in slots if s['gid'] not in shared]
        match, dropped, extra = assign(usable, players, None)
        stat = dict(team=abbr[t], tricode=tc, donors=len(slots), new=len(players), changed=len(match), dropped=len(dropped),
                    surplus=len(extra) + len(slots) - len(usable))
        for i, p in match.items():
            write_player(usable[i], p, ro, bio, sk, go, sk_row, go_row, allowed, wpct)
        # captain and two assistants: the three best skaters by quality (placeholder: the API roster has no captaincy)
        for s in slots:
            ro.set(s['row'], 'captain', 0)
        best = sorted([(p['q'], usable[i]['row']) for i, p in match.items() if p['pos'] != 'G'], reverse=True)[:3]
        for n, (_, row) in enumerate(best):
            ro.set(row, 'captain', 1 if n == 0 else 2)
        gone = [usable[i] for i in extra] + [s for s in slots if s['gid'] in shared]
        for s in gone:
            if surplus == 'delete':
                ro.delete_row(s['row'])
            else:
                for f in ro.fields.values():
                    if f.bits == 1 and f.name != 'rosterstatus':
                        ro.set(s['row'], f.name, 0)
                ro.set(s['row'], 'rosterstatus', 0)
                ro.set(s['row'], 'captain', 0)
        summary[abbr[t]] = stat
        log(f'{abbr[t]:>3} <- {tc}: donors {stat["donors"]}, API {stat["new"]}, written {stat["changed"]}, dropped {stat["dropped"]}, surplus rows {stat["surplus"]}')
    db.save(out_path)
    return summary


def write_player(slot, p, ro, bio, sk, go, sk_row, go_row, allowed, wpct):
    br, row = slot['bio'], slot['row']
    gid = slot['gid']
    isg = p['pos'] == 'G'
    bio.set(br, 'firstname', fold(p['first'], allowed, NAME_BYTES))
    bio.set(br, 'lastname', fold(p['last'], allowed, NAME_BYTES))
    bio.set(br, 'birthplace', fold(p['city'], allowed, PLACE_BYTES))
    bio.set(br, 'position', POS_CODE[p['pos']])
    bio.set(br, 'handedness', 1 if p['shoots'] == 'R' else 0)
    num = p['number'] if p['number'] is not None else 0
    bio.set(br, 'jersey', num)
    ro.set(row, 'jersey', num)
    if p['birth']:
        y, m, d = (int(x) for x in p['birth'].split('-'))
        bio.set(br, 'year', min(max(y - YEAR0, 0), 255))
        bio.set(br, 'month', m - 1)
        bio.set(br, 'day', d - 1)
    if p['height']:
        bio.set(br, 'height', min(max(p['height'] - HEIGHT0, 0), 31))
    if p['weight']:
        bio.set(br, 'weight', min(max(p['weight'] - WEIGHT0, 0), 255))
    # wipe what belongs to the donor: portrait, audio, specific head, draft record, career counters
    bio.set(br, 'hasportrait', 0)
    bio.set(br, 'artid', 0)
    bio.set(br, 'audioid', 0)
    bio.set(br, 'headid', HEAD_GENERIC)
    bio.set(br, 'draftyear', 255)
    bio.set(br, 'draftround', 0)
    bio.set(br, 'draftposition', 0)
    bio.set(br, 'draftteam', -1)
    bio.set(br, 'rookie', 0)
    gp = (p['info'] or {}).get('gp', 0)
    for f in ('nhlgamesplayedthreeseasonsago', 'nhlgamesplayedtwoseasonsago', 'nhlgamesplayedlastseason', 'nhlgamesplayedcareer',
              'progamesplayedcareer'):
        bio.set(br, f, 0)
    bio.set(br, 'nhlgamesplayedlastseason', min(gp, 127))
    bio.set(br, 'nhlgamesplayedcareer', min(gp, 4095))
    if isg:
        ar, at, rat = go_row[gid], go, goalie_ratings(p['info'])
    else:
        ar, at, rat = sk_row[gid], sk, skater_ratings(p['pos'], p['info'], wpct(p['weight']))
    for f in at.fields.values():
        if f.name.startswith('trait'):
            at.set(ar, f.name, 0)
    for k, v in rat.items():
        at.set(ar, k, v)


def main(argv):
    a = list(argv)

    def opt(name, default=None):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default
    cache = opt('--cache', DEFAULT_CACHE)
    meta = opt('--meta')
    surplus = opt('--surplus', 'delete')
    only = opt('--teams')
    if len(a) != 2 or surplus not in ('delete', 'keep'):
        print(__doc__)
        return 2
    build(a[0], a[1], cache, meta, surplus, set(only.split(',')) if only else None)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
