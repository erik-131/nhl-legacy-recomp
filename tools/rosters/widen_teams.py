#!/usr/bin/env python3
"""Widen the 5-bit "team + 1" fields of an EA TDB database (nhlng.db, draft_roster.db) so team 31 fits.

Background (docs/rosters-32-teams.md): fields such as exhibitionplayerbiotable.proteam hold team+1 in 5 bits
(schema range -1..30), so teams 0..30 and "none" fit but team 31 (stored 32) does not. This tool makes every such
field 6 bits wide (range -1..62) in the database AND in the meta XML (depth and rangehigh), using tdb.Table.widen
and tdb.widen_meta, then reseals everything. Values are untouched: stored bits are zero-extended.

A field is picked automatically when all of this holds: integer, name in TEAM_FIELDS, schema low limit -1
(the "none" offset), width below the target, and rangehigh = low + 2^bits - 1 (the field uses its full range).
Fields with no offset (home, away, teamid, 0..31 in 5 bits) already hold 32 teams and are left alone.
--field TABLE.FIELD adds more fields (any integer field, any width below the target); --skip removes some.

  widen_teams.py DB OUT --meta META --meta-out META_OUT [--bits 6] [--field T.F]... [--skip T.F]... [--list]
  --list           only print what would be widened (and the record size effect); writes nothing
  META defaults to nhlng-meta.xml next to DB. draft_roster.db has no meta of its own: pass nhlng-meta.xml.
  OUT and META_OUT must differ from the inputs. Run the same tool on draft_roster.db with the same --bits.
After writing, the result is checked with Db.check(); the exit code is 1 if it shows a problem the input did not have.
Never put the output under the repo: the files hold EA data.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdb

# Names of fields that hold a team index (+1) in the stock schema; found by survey, see the doc.
TEAM_FIELDS = {'proteam', 'draftteam', 'owner', 'origteamid', 'teamnotification', 'signedteamid',
               'originalteamid', 'tradeblockteam', 'team'}


def candidates(db, bits=6, extra=(), skip=()):
    """[(table, field)] to widen: the automatic picks plus extra, minus skip. Tables are tdb.Table objects."""
    skip = set(skip)
    out = []
    for t in sorted(db.tables.values(), key=lambda t: t.name):
        for f in sorted(t.fields.values(), key=lambda f: f.bitoff):
            key = f'{t.name}.{f.name}'
            if key in skip or f.type != tdb.TYPE_INT or f.bits >= bits:
                continue
            auto = (f.name in TEAM_FIELDS and f.low == -1 and f.high is not None and f.high == f.low + (1 << f.bits) - 1)
            if auto or key in extra:
                out.append((t, f))
    missing = set(extra) - {f'{t.name}.{f.name}' for t, f in out}
    if missing:
        raise KeyError('not widenable (missing, not an integer, or already wide enough): ' + ', '.join(sorted(missing)))
    return out


def widen_teams(db, meta_text, bits=6, extra=(), skip=()):
    """Widen the chosen fields of the open Db. Returns (meta_text, report); report rows are dicts with
    table, field, old_bits, rec_bytes_before/after, rec_bits_before/after, max_rows, rows, bytes_delta (table space)."""
    if db.meta_path is None:
        raise ValueError('no meta XML: field names and ranges are unknown')
    todo = [(t.code, f.code, t.name, f.name) for t, f in candidates(db, bits, extra, skip)]
    report = []
    for tcode, fcode, tname, fname in todo:
        t = db.tables[tcode]
        f = t.fields[fcode]
        row = dict(table=tname, field=fname, old_bits=f.bits, rec_bits_before=t.rec_bits, rec_bytes_before=t.rec_bytes,
                   max_rows=t.max_rows, rows=t.rows)
        t.widen(fcode, bits)
        meta_text = tdb.widen_meta(meta_text, tcode, fcode, bits)
        row.update(rec_bits_after=t.rec_bits, rec_bytes_after=t.rec_bytes,
                   bytes_delta=(t.rec_bytes - row['rec_bytes_before']) * t.max_rows)
        report.append(row)
    return meta_text, report


def read_text(path):
    with open(path, encoding='utf-8', newline='') as fh:
        return fh.read()


def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        fh.write(text)


def print_report(report, size_before=None, size_after=None):
    for r in report:
        cross = ' BYTE-CHANGE' if r['rec_bytes_after'] != r['rec_bytes_before'] else ''
        print(f"{r['table']}.{r['field']}: {r['old_bits']} bits, record {r['rec_bits_before']}->{r['rec_bits_after']} bits, "
              f"{r['rec_bytes_before']}->{r['rec_bytes_after']} bytes, max rows {r['max_rows']}, +{r['bytes_delta']} bytes{cross}")
    if size_before is not None:
        print(f'file {size_before} -> {size_after} bytes')


def main(argv):
    args = list(argv[1:])

    def opt(name, many=False):
        vals = []
        while name in args:
            i = args.index(name)
            if i + 1 >= len(args):
                raise SystemExit(f'{name} needs a value')
            vals.append(args[i + 1])
            del args[i:i + 2]
        return vals if many else (vals[-1] if vals else None)
    meta = opt('--meta')
    meta_out = opt('--meta-out')
    bits = int(opt('--bits') or 6)
    extra = opt('--field', True)
    skip = opt('--skip', True)
    listing = '--list' in args
    if listing:
        args.remove('--list')
    if len(args) != (1 if listing else 2):
        print(__doc__)
        return 2
    src = args[0]
    db = tdb.Db(src, meta)
    if db.meta_path is None:
        print('no meta XML found; give --meta')
        return 2
    text = read_text(db.meta_path)
    size0 = len(db.data)
    base = set(db.check())  # problems the input already has (the stock file has 6 'reaches the free flag bit' ones)
    try:
        new_text, report = widen_teams(db, text, bits, extra, skip)
    except (KeyError, ValueError) as e:
        print(f'failed: {e}')
        return 1
    print_report(report, size0, len(db.data))
    if listing:
        return 0
    out = args[1]
    if not meta_out:
        print('--meta-out is required (the meta XML must change with the database)')
        return 2
    if os.path.abspath(out) == os.path.abspath(src) or os.path.abspath(meta_out) == os.path.abspath(db.meta_path):
        print('OUT and META_OUT must differ from the inputs')
        return 2
    db.save(out)
    write_text(meta_out, new_text)
    bad = [b for b in tdb.Db(out, meta_out).check() if b not in base]
    print(f'{out}: {len(report)} fields widened to {bits} bits, {len(bad)} new problems ({len(base)} already in the input)')
    for b in bad:
        print(' ', b)
    return int(bool(bad))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
