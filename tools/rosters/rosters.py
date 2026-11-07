#!/usr/bin/env python3
"""Roster tool on top of tdb.py: CSV export/import, cross-table check, diff. Documented in docs/rosters-tool.md.

  rosters.py export DB OUTDIR [--meta PATH]     one CSV per key table (live rows, shown values, field names) + players.csv
  rosters.py import DB CSVDIR OUT [--meta PATH] apply edited <table>.csv files to existing rows (by key), write a resealed
                                                copy to OUT; DB is never changed; nothing is written if any value is rejected
  rosters.py check DB [--draft PATH] [--meta PATH]   consistency report; exit code 1 when there are errors
  rosters.py diff DB_A DB_B [--meta PATH] [--verbose]  changed rows per table (live rows matched by key)

CSV files: <table name>.csv, first column 'row' (informational, ignored on import), then every field by its meta name;
the key field(s) (key="True" in the meta XML) find the row, so rows may be reordered or removed from the CSV (a removed
row stays as it is). Integers are the values the game shows (stored value + the schema's low limit); strings are UTF-8.
players.csv is a read-only joined view and is not imported. Never put exports in the repo: they hold EA data.
"""
import csv
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb

KEY_TABLES = ['exhibitionteams', 'exhibitionplayerbiotable', 'exhibitionskaterai', 'exhibitiongoalieai',
              'exhibitionrostertable', 'exhibitionplayers', 'exhibitionskaterequipment', 'exhibitiongoalieequipment',
              'playernames', 'exhibitionfreeagents', 'exhibitiondraftpick']
POSITIONS = {0: 'C', 1: 'LW', 2: 'RW', 3: 'D', 4: 'G'}  # guess from which roster lines each value fills (stock file)
HAND = {0: 'L', 1: 'R'}  # guess: 0 is the majority value
TEAM_SLOTS = 40  # roster key = team * 40 + slot
CSV_KW = dict(encoding='utf-8', errors='surrogateescape', newline='')


def open_db(path, meta=None):
    return tdb.Db(path, meta)


def table_keys(db):
    """{table name: [key field names]} from key="True" in the meta XML; tables without keys are matched by row number."""
    keys = {}
    if db.meta_path:
        for mt in ET.parse(db.meta_path).getroot().findall('table'):
            ks = [f.get('name') for f in mt.find('fields') if f.get('key') == 'True']
            if ks:
                keys[mt.get('name')] = ks
    return keys


def sorted_fields(t):
    return sorted(t.fields.values(), key=lambda f: f.bitoff)


def live_by_key(db, t, keys):
    """{key tuple: row} for live rows; also the list of duplicate keys. No key fields: the row number is the key."""
    ks = keys.get(t.name)
    out, dup = {}, []
    for r in t.live_rows():
        k = tuple(t.get(r, n) for n in ks) if ks else (r,)
        if k in out:
            dup.append(k)
        else:
            out[k] = r
    return out, dup


# ---------------------------------------------------------------- export

def export_table(t, path):
    fs = sorted_fields(t)
    with open(path, 'w', **CSV_KW) as f:
        w = csv.writer(f)
        w.writerow(['row'] + [x.name for x in fs])
        for r in t.live_rows():
            w.writerow([r] + [t.get(r, x.code) for x in fs])


def player_links(db):
    """Resolved links between the exhibition tables, as dicts; used by export (players.csv) and check."""
    T = db.table
    L = {}
    pl = T('exhibitionplayers')
    L['index2pid'] = {pl.get(r, 'index'): pl.get(r, 'playerid') for r in pl.live_rows()}
    bio = T('exhibitionplayerbiotable')
    L['bio'] = {bio.get(r, 'game_id'): r for r in bio.live_rows()}
    for key, name in (('skater', 'exhibitionskaterai'), ('goalie', 'exhibitiongoalieai'),
                      ('skeq', 'exhibitionskaterequipment'), ('goeq', 'exhibitiongoalieequipment')):
        t = T(name)
        L[key] = {t.get(r, 'game_id'): r for r in t.live_rows()}
    return L


def export_players(db, path):
    T = db.table
    L = player_links(db)
    bio, ro, te = T('exhibitionplayerbiotable'), T('exhibitionrostertable'), T('exhibitionteams')
    sk, go, fa = T('exhibitionskaterai'), T('exhibitiongoalieai'), T('exhibitionfreeagents')
    abbr = {te.get(r, 'index'): te.get(r, 'abbrname') for r in te.live_rows()}
    pid2roster = {}
    for r in ro.live_rows():
        pid = L['index2pid'].get(ro.get(r, 'playerindex'))
        if pid is not None:
            pid2roster.setdefault(pid, []).append(r)
    fa_pids = {L['index2pid'].get(fa.get(r, 'playerindex')) for r in fa.live_rows()}
    srat = [f.name for f in sorted_fields(sk) if f.name.startswith('c_')]
    grat = [f.name for f in sorted_fields(go) if f.name.startswith('c_') and f.name not in srat]
    bio_cols = ['birthplace', 'year', 'month', 'day', 'height', 'weight', 'jersey', 'proteam', 'draftyear', 'draftteam',
                'contractdollars', 'contractlength', 'headid']
    bio_cols = [c for c in bio_cols if any(c in (f.name, f.code) for f in bio.fields.values())]
    head = (['game_id', 'team_index', 'team', 'slot', 'roster_key', 'status', 'firstname', 'lastname', 'position',
             'position_code', 'handedness', 'roster_jersey'] + ['bio_' + c for c in bio_cols] + srat + grat)
    with open(path, 'w', **CSV_KW) as f:
        w = csv.writer(f)
        w.writerow(head)
        for gid in sorted(L['bio']):
            br = L['bio'][gid]
            pos = bio.get(br, 'position')
            ai, at = (L['goalie'].get(gid), go) if pos == 4 else (L['skater'].get(gid), sk)
            rat = {x.name: at.get(ai, x.code) for x in at.fields.values() if x.name.startswith('c_')} if ai is not None else {}
            entries = pid2roster.get(gid) or [None]
            for rr in entries:
                if rr is not None:
                    team, key = ro.get(rr, 'team'), ro.get(rr, 'key')
                    ident = [team, abbr.get(team, ''), key - team * TEAM_SLOTS, key, 'roster']
                    rj = ro.get(rr, 'jersey')
                else:
                    ident = ['', '', '', '', 'free agent' if gid in fa_pids else 'unlisted']
                    rj = ''
                w.writerow([gid] + ident + [bio.get(br, 'firstname'), bio.get(br, 'lastname'), POSITIONS.get(pos, pos), pos,
                                            HAND.get(bio.get(br, 'handedness'), ''), rj]
                           + [bio.get(br, c) for c in bio_cols] + [rat.get(c, '') for c in srat + grat])


def cmd_export(db_path, outdir, meta=None):
    db = open_db(db_path, meta)
    os.makedirs(outdir, exist_ok=True)
    for n in KEY_TABLES:
        try:
            t = db.table(n)
        except KeyError:
            print(f'skip {n}: not in this file')
            continue
        export_table(t, os.path.join(outdir, n + '.csv'))
        print(f'{n}.csv: {len(t.live_rows())} rows ({t.rows - len(t.live_rows())} deleted skipped)')
    try:
        export_players(db, os.path.join(outdir, 'players.csv'))
        print('players.csv written')
    except KeyError as e:
        print(f'players.csv skipped: {e}')
    return 0


# ---------------------------------------------------------------- import

def cmd_import(db_path, csv_dir, out_path, meta=None):
    if os.path.abspath(out_path) == os.path.abspath(db_path):
        print('error: OUT is the input file; the input is never changed')
        return 2
    db = open_db(db_path, meta)
    keys = table_keys(db)
    errors, changed = [], {}
    files = sorted(f for f in os.listdir(csv_dir) if f.endswith('.csv') and f != 'players.csv')
    for fn in files:
        name = fn[:-4]
        try:
            t = db.table(name)
        except KeyError:
            errors.append(f'{fn}: no such table')
            continue
        by_key, _ = live_by_key(db, t, keys)
        ks = keys.get(t.name)
        with open(os.path.join(csv_dir, fn), **CSV_KW) as f:
            rd = csv.DictReader(f)
            cols = [c for c in (rd.fieldnames or []) if c != 'row']
            names = {x.name for x in t.fields.values()}
            bad = [c for c in cols if c not in names]
            if bad or (ks and any(k not in cols for k in ks)) or (not ks and 'row' not in (rd.fieldnames or [])):
                errors.append(f'{fn}: unknown columns {bad} or key columns {ks} missing')
                continue
            seen = set()
            for ln, rec in enumerate(rd, 2):
                k = tuple(int(rec[n]) if t.field(n).type == tdb.TYPE_INT else rec[n] for n in ks) if ks else None
                try:
                    if ks is None:
                        k = (int(rec['row']),)
                except ValueError:
                    errors.append(f'{fn}:{ln}: bad key')
                    continue
                if k in seen:
                    errors.append(f'{fn}:{ln}: duplicate key {k}')
                    continue
                seen.add(k)
                row = by_key.get(k)
                if row is None:
                    errors.append(f'{fn}:{ln}: key {k} is not a live row (adding or reviving rows is not supported)')
                    continue
                for c in cols:
                    fld = t.field(c)
                    cur = t.get(row, fld.code)
                    try:
                        new = int(rec[c]) if fld.type == tdb.TYPE_INT else rec[c]
                    except (ValueError, TypeError):
                        errors.append(f'{fn}:{ln}: {c}={rec[c]!r} is not an integer')
                        continue
                    if new == cur:
                        continue
                    if ks and c in ks:
                        continue
                    try:
                        t.set(row, fld.code, new)
                        changed[name] = changed.get(name, 0) + 1
                    except ValueError as e:
                        errors.append(f'{fn}:{ln}: {e}')
    if errors:
        for e in errors[:50]:
            print('error:', e)
        print(f'{len(errors)} problems; nothing written')
        return 1
    db.save(out_path)
    for n, c in sorted(changed.items()):
        print(f'{n}: {c} values changed')
    print(f'wrote {out_path} ({sum(changed.values())} values changed)')
    return 0


# ---------------------------------------------------------------- check

class Report:
    def __init__(self, limit=8):
        self.limit, self.items, self.errors, self.warnings = limit, {}, 0, 0

    def add(self, level, cat, msg):
        lst = self.items.setdefault((level, cat), [])
        lst.append(msg)
        if level == 'ERROR':
            self.errors += 1
        elif level == 'WARN':
            self.warnings += 1

    def show(self, prefix=''):
        for (level, cat), lst in sorted(self.items.items()):
            if level == 'INFO':
                print(f'{prefix}INFO {cat}: {lst[0] if len(lst) == 1 and lst[0].replace(" ", "")[:1].isdigit() else len(lst)}')
                continue
            print(f'{prefix}{level} {cat}: {len(lst)}')
            for m in lst[:self.limit]:
                print(f'{prefix}    {m}')
            if len(lst) > self.limit:
                print(f'{prefix}    ... {len(lst) - self.limit} more')


def check_layout(db, rep, label):
    for p in db.check():
        # Schedule-type tables fill their record without a free flag bit: not an error, but it limits the "last bit" claim.
        level = 'WARN' if 'free flag bit' in p else 'ERROR'
        rep.add(level, f'{label} file layout', p)


def check_entities(db, rep, label, keys):
    """Cross-table checks on the exhibition tables present in db."""
    def has(n):
        try:
            db.table(n)
            return True
        except KeyError:
            return False
    need = ['exhibitionplayers', 'exhibitionplayerbiotable', 'exhibitionrostertable']
    if not all(has(n) for n in need):
        rep.add('WARN', f'{label} links', 'exhibition tables not found (no meta XML next to the file? use --meta); cross-table checks skipped')
        return
    T = db.table
    for n in KEY_TABLES:
        if has(n):
            t = T(n)
            dead = t.rows - len(t.live_rows())
            if dead:
                rep.add('INFO', f'{label} deleted rows', f'{n}: {dead} deleted of {t.rows}')
            _, dup = live_by_key(db, t, keys)
            if dup:
                rep.add('ERROR', f'{label} duplicate keys', f'{n}: {len(dup)} duplicate keys, e.g. {dup[:3]}')
    pl, bio, ro = T('exhibitionplayers'), T('exhibitionplayerbiotable'), T('exhibitionrostertable')
    L = player_links(db)
    i2p, bmap = L['index2pid'], L['bio']
    for r in pl.live_rows():
        if pl.get(r, 'table') == 3:  # special entries (index/playerid 16300..16383), no bio row
            continue
        if pl.get(r, 'playerid') not in bmap:
            rep.add('ERROR', f'{label} players.playerid has no bio row', f'players row {r}: playerid {pl.get(r, "playerid")}')
    seen_pid = {}
    for i, p in i2p.items():
        seen_pid.setdefault(p, []).append(i)
    multi = sum(1 for v in seen_pid.values() if len(v) > 1)
    if multi:
        rep.add('INFO', f'{label} player ids reached by more than one players.index', f'{multi} of {len(seen_pid)}')
    n3 = sum(1 for r in pl.live_rows() if pl.get(r, 'table') == 3)
    if n3:
        rep.add('INFO', f'{label} players rows with table=3 (special, not checked)', str(n3))
    teams = None
    if has('exhibitionteams'):
        te = T('exhibitionteams')
        teams = {te.get(r, 'index'): te.get(r, 'abbrname') for r in te.live_rows()}
    per_team, goalies, onroster, keyseen, multi_team = {}, {}, {}, {}, set()
    for r in ro.live_rows():
        team, key, pi = ro.get(r, 'team'), ro.get(r, 'key'), ro.get(r, 'playerindex')
        if key in keyseen:
            rep.add('ERROR', f'{label} roster key repeated', f'key {key} in rows {keyseen[key]} and {r}')
        keyseen[key] = r
        if key // TEAM_SLOTS != team:
            rep.add('ERROR', f'{label} roster key is not team*40+slot', f'row {r}: team {team} key {key}')
        if teams is not None and team not in teams:
            rep.add('ERROR', f'{label} roster row has unknown team', f'row {r}: team {team}')
        pid = i2p.get(pi)
        if pid is None:
            rep.add('ERROR', f'{label} roster row points to no player (players.index)', f'row {r}: team {team} key {key} playerindex {pi}')
            continue
        if pid not in bmap:
            rep.add('ERROR', f'{label} roster row points to no bio row', f'row {r}: playerid {pid}')
            continue
        if pid in onroster:
            if any(k // TEAM_SLOTS == team for k in onroster[pid]):
                rep.add('ERROR', f'{label} player twice on the same team', f'playerid {pid}: keys {onroster[pid]} and {key}')
            else:
                multi_team.add(pid)
        onroster.setdefault(pid, []).append(key)
        per_team[team] = per_team.get(team, 0) + 1
        br = bmap[pid]
        pos = bio.get(br, 'position')
        if pos == 4:
            goalies[team] = goalies.get(team, 0) + 1
        ai = L['goalie'] if pos == 4 else L['skater']
        eq = L['goeq'] if pos == 4 else L['skeq']
        if pid not in ai:
            rep.add('ERROR', f'{label} bio has no {"goalie" if pos == 4 else "skater"} AI row', f'playerid {pid}')
        if pid not in eq:
            rep.add('ERROR', f'{label} bio has no equipment row', f'playerid {pid}')
        if (pos == 4 and pid in L["skater"]) or (pos != 4 and pid in L["goalie"]):
            rep.add('ERROR', f'{label} position and AI table disagree', f'playerid {pid} position {pos}')
        if bio.get(br, 'team') != team and team < 32:
            rep.add('WARN', f'{label} bio.team differs from roster team', f'playerid {pid}: bio {bio.get(br, "team")} roster {team}')
        if bio.get(br, 'jersey') != ro.get(r, 'jersey') and team < 32:
            rep.add('WARN', f'{label} bio.jersey differs from roster jersey', f'playerid {pid}: bio {bio.get(br, "jersey")} roster {ro.get(r, "jersey")}')
    if multi_team:
        rep.add('INFO', f'{label} players on rosters of more than one team (all-star, national, ...)', str(len(multi_team)))
    for team, n in sorted(per_team.items()):
        if n > TEAM_SLOTS:
            rep.add('ERROR', f'{label} team has more than 40 players', f'team {team} ({(teams or {}).get(team, "?")}): {n}')
        if goalies.get(team, 0) < 2:
            rep.add('WARN', f'{label} team has fewer than 2 goalies', f'team {team} ({(teams or {}).get(team, "?")}): {goalies.get(team, 0)}')
    for g in L['skater']:
        if g not in bmap:
            rep.add('ERROR', f'{label} skater AI without bio row', f'game_id {g}')
    for g in L['goalie']:
        if g not in bmap:
            rep.add('ERROR', f'{label} goalie AI without bio row', f'game_id {g}')
    for g in list(L['skeq']) + list(L['goeq']):
        if g not in bmap:
            rep.add('ERROR', f'{label} equipment without bio row', f'game_id {g}')
    if has('exhibitionfreeagents'):
        fa = T('exhibitionfreeagents')
        for r in fa.live_rows():
            pi = fa.get(r, 'playerindex')
            if pi not in i2p:
                rep.add('ERROR', f'{label} free agent points to no player', f'row {r}: playerindex {pi}')
            elif i2p[pi] in onroster:
                rep.add('WARN', f'{label} free agent is also on a roster', f'playerindex {pi} (keys {onroster[i2p[pi]]})')
    if has('exhibitiondraftpick'):
        dp = T('exhibitiondraftpick')
        for r in dp.live_rows():
            pi = dp.get(r, 'playerindex')
            if pi != -1 and pi not in i2p:
                rep.add('ERROR', f'{label} draft pick points to no player', f'row {r}: playerindex {pi}')
            if teams is not None:
                for n in ('owner', 'origteamid'):
                    if all(n not in (f.name, f.code) for f in dp.fields.values()):
                        continue
                    v = dp.get(r, n)
                    if v != -1 and v not in teams:
                        rep.add('ERROR', f'{label} draft pick has unknown team', f'row {r}: {n} {v}')
    reached = set(onroster)
    if has('exhibitionfreeagents'):
        reached |= {i2p[fa.get(r, 'playerindex')] for r in fa.live_rows() if fa.get(r, 'playerindex') in i2p}
    orphan = [g for g in bmap if g not in reached]
    if orphan:
        rep.add('INFO', f'{label} bio players on no roster and not free agents', f'{len(orphan)} (draft prospects, retired, ...)')
    rep.stats = getattr(rep, 'stats', {})
    rep.stats[label] = (per_team, goalies)


def compare_tables(a, b, keys, rep, limit_examples=3):
    """Differences between two Db objects over the tables both hold; returns {table: (added, removed, changed)}."""
    out = {}
    for ta in sorted(a.tables.values(), key=lambda t: t.name):
        try:
            tb = b.table(ta.code)
        except KeyError:
            continue
        la = [(f.code, f.bits, f.bitoff) for f in sorted_fields(ta)]
        lb = [(f.code, f.bits, f.bitoff) for f in sorted_fields(tb)]
        if la != lb or ta.rec_bits != tb.rec_bits:
            if rep is not None:
                rep.add('ERROR', 'table layout differs', f'{ta.name}: fields or record size differ')
            out[ta.name] = ('layout',)
            continue
        ka, _ = live_by_key(a, ta, keys)
        kb, _ = live_by_key(b, tb, keys)
        added = [k for k in kb if k not in ka]
        removed = [k for k in ka if k not in kb]
        changed = []
        for k, ra in ka.items():
            rb = kb.get(k)
            if rb is None:
                continue
            oa, ob = ta._row_off(ra), tb._row_off(rb)
            if a.data[oa:oa + ta.rec_bytes] == b.data[ob:ob + tb.rec_bytes]:
                continue
            diffs = [(f.name, ta.get(ra, f.code), tb.get(rb, f.code)) for f in sorted_fields(ta)
                     if ta.get(ra, f.code) != tb.get(rb, f.code)]
            if diffs:
                changed.append((k, diffs))
        out[ta.name] = (added, removed, changed)
    return out


def cmd_check(db_path, draft=None, meta=None):
    rep = Report()
    db = open_db(db_path, meta)
    keys = table_keys(db)
    check_layout(db, rep, 'nhlng')
    check_entities(db, rep, 'nhlng', keys)
    if draft is None:
        cand = os.path.join(os.path.dirname(db_path), 'draft_roster.db')
        draft = cand if os.path.exists(cand) else None
    if draft:
        dr = open_db(draft, meta or db.meta_path)
        check_layout(dr, rep, 'draft')
        if len(dr.tables) != 10:
            rep.add('ERROR', 'draft_roster.db table count', f'{len(dr.tables)} tables, the game needs exactly 10')
        missing = [t.name for t in dr.tables.values() if t.code not in db.tables]
        if missing:
            rep.add('ERROR', 'draft table not in nhlng.db', ', '.join(missing))
        check_entities(dr, rep, 'draft', keys)
        res = compare_tables(db, dr, keys, rep)
        for n, r in sorted(res.items()):
            if r == ('layout',) or n not in {t.name for t in dr.tables.values()}:
                continue
            added, removed, changed = r
            if added or removed or changed:
                rep.add('WARN', 'draft_roster.db differs from nhlng.db', f'{n}: {len(added)} only in draft, {len(removed)} only in nhlng, {len(changed)} changed')
    rep.show()
    stats = getattr(rep, 'stats', {}).get('nhlng')
    if stats:
        pt, gl = stats
        sizes = sorted(pt.values())
        print(f'INFO nhlng roster: {len(pt)} teams with players, size min {sizes[0]} max {sizes[-1]}, '
              f'goalies per team min {min(gl.get(t, 0) for t in pt)} max {max(gl.get(t, 0) for t in pt)}')
    print(f'{rep.errors} errors, {rep.warnings} warnings')
    return 1 if rep.errors else 0


def cmd_diff(pa, pb, meta=None, verbose=False):
    a = open_db(pa, meta)
    b = open_db(pb, meta or a.meta_path)
    keys = table_keys(a)
    res = compare_tables(a, b, keys, None)
    only_a = sorted(t.name for t in a.tables.values() if t.code not in b.tables)
    only_b = sorted(t.name for t in b.tables.values() if t.code not in a.tables)
    if only_a:
        print(f'only in A: {len(only_a)} tables' + (', ' + ', '.join(only_a) if len(only_a) <= 12 else ''))
    if only_b:
        print(f'only in B: {len(only_b)} tables' + (', ' + ', '.join(only_b) if len(only_b) <= 12 else ''))
    total = 0
    for n, r in sorted(res.items()):
        if r == ('layout',):
            print(f'{n}: layout differs (not compared)')
            total += 1
            continue
        added, removed, changed = r
        if not (added or removed or changed):
            continue
        total += 1
        print(f'{n}: {len(added)} added, {len(removed)} removed, {len(changed)} changed')
        if verbose:
            for k, diffs in changed[:20]:
                print(f'    key {k}: ' + ', '.join(f'{f} {x!r}->{y!r}' for f, x, y in diffs[:8]))
    if not total:
        print('no differences in live rows')
    return 0


def main(argv):
    a = list(argv)

    def opt(name, takes=True):
        if name in a:
            i = a.index(name)
            v = a[i + 1] if takes else True
            del a[i:i + (2 if takes else 1)]
            return v
        return None
    meta, draft, verbose = opt('--meta'), opt('--draft'), opt('--verbose', False)
    if not a or a[0] not in ('export', 'import', 'check', 'diff'):
        print(__doc__)
        return 2
    cmd, args = a[0], a[1:]
    need = {'export': 2, 'import': 3, 'check': 1, 'diff': 2}[cmd]
    if len(args) != need:
        print(__doc__)
        return 2
    if cmd == 'export':
        return cmd_export(args[0], args[1], meta)
    if cmd == 'import':
        return cmd_import(args[0], args[1], args[2], meta)
    if cmd == 'check':
        return cmd_check(args[0], draft, meta)
    return cmd_diff(args[0], args[1], meta, bool(verbose))


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
