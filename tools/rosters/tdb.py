#!/usr/bin/env python3
"""Read and write EA TDB v8 databases (db/nhlng.db and friends).

Layout, big endian:
  0x00  'DB', version, ... ; 0x08 file size; 0x10 table count; 0x14 CRC of [0:0x14]
  0x18  directory: count * (4-byte name, 4-byte offset); then its CRC
  table: 0x24-byte header (0x04 record bytes, 0x08 record bits, 0x10 max rows, 0x12 rows,
         0x18 field count, 0x20 CRC of [0:0x20]), 16 bytes per field (type, bit offset, name, bits),
         max rows * record bytes of data, CRC of fields + data.
Rows are bit-packed, MSB first. CRC: poly 0x04C11DB7, MSB first, init FFFFFFFF, no final xor.
The CRC closes the table's space, which can be larger than max rows. Integer fields are stored as value minus
the schema's low limit.

Deleted rows (a claim from another project's tool, implemented here and tested on made-up data only, not yet
confirmed on the real file): the last bit of a record (record bits - 1) is the engine's "free" flag in tables where no
field reaches it. That is not general: on the stock nhlng.db six tables (the foreign schedules and
stockplayerhelmetplacement) have a field reaching the last bit, so they cannot have a flag there (docs/rosters-tool.md);
check() reports those as warnings. Rows with the flag set are skipped by live_rows() and export. add_row() reuses the lowest deleted row
before appending, and never goes past max rows. The header word at 0x14 (a possible free-list head) is not
interpreted or changed; the flags alone decide which rows are free.
Widening a field (Table.widen) inserts zero bits at the field's high end: later fields move, record bits and bytes
are recomputed, every row (all max rows) is rewritten with the flag still last, and Db.rebuild() lays the file out
again (offsets, file size, CRCs). widen_meta() makes the same change in the meta XML text (depth and rangehigh).
A wrong max rows gives "damaged" in the game even with good CRCs, so Db.check() also looks at sizes and layout.

Table and field names in the file are 4-letter codes; <db>-meta.xml maps them to names.
Never commit a database, a CSV export or anything else holding EA data: this repo is public.

  tdb.py verify DB...                  check every CRC and that a rewrite is byte-identical
  tdb.py check DB [META]               CRCs plus file size, record size, max rows, field layout
  tdb.py tables DB [META]              list tables
  tdb.py export DB TABLE [META] [--deleted]   table as CSV on stdout; deleted rows only with --deleted
  tdb.py live DB TABLE [META]          rows, live and deleted counts
  tdb.py set DB OUT TABLE ROW FIELD VALUE [META]   change one value and write a resealed copy
  tdb.py delete DB OUT TABLE ROW       set the free flag of a row
  tdb.py add DB OUT TABLE [FIELD=VALUE...]   new row (reuses a deleted one first), prints its index
  tdb.py widen DB OUT TABLE FIELD BITS [--meta-out PATH]   widen an integer field, rewrite the file
                                       and (with --meta-out) the meta XML read from META
  tdb.py widen-meta META OUT TABLE FIELD BITS   the meta XML change alone (TABLE and FIELD are 4-letter codes)
  tdb.py maxrows DB OUT TABLE N        change max rows if the table's space allows
  tdb.py rebuild DB OUT [META]         rewrite the file from the parsed tables and reseal everything
  META can also be given as --meta PATH; by default nhlng-meta.xml next to the DB.
"""
import csv
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

TYPE_STRING = 0
TYPE_INT = 3

_crc_table = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _crc_table.append(_c)


def crc(data):
    c = 0xFFFFFFFF
    for x in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _crc_table[(c >> 24) ^ x]
    return c


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def be16(d, o):
    return struct.unpack_from('>H', d, o)[0]


class TableFull(Exception):
    """add_row() found no deleted row and the table is at max rows."""


class Field:
    def __init__(self, code, ftype, bitoff, bits, name=None, low=None, high=None):
        self.code, self.type, self.bitoff, self.bits = code, ftype, bitoff, bits
        self.name = name or code
        self.low, self.high = low, high


class Table:
    def __init__(self, db, code, off):
        self.db, self.code, self.off = db, code, off
        d = db.data
        self.rec_bytes = be32(d, off + 4)
        self.rec_bits = be32(d, off + 8)
        self._blob = None  # replacement bytes for rebuild(), set by widen()
        self.max_rows = be16(d, off + 0x10)
        self.rows = be16(d, off + 0x12)
        n = d[off + 0x18]
        self.fields_off = off + 0x24
        self.data_off = self.fields_off + n * 16
        self.name = code
        self.fields = {}
        for i in range(n):
            p = self.fields_off + i * 16
            f = Field(bytes(d[p + 8:p + 12]).decode('latin1'), be32(d, p), be32(d, p + 4), be32(d, p + 12))
            self.fields[f.code] = f

    def field(self, name):
        for f in self.fields.values():
            if name in (f.name, f.code):
                return f
        raise KeyError(f'{self.name}: no field {name}')

    def _row_off(self, row):
        if not 0 <= row < self.rows:
            raise IndexError(f'{self.name}: row {row} of {self.rows}')
        return self.data_off + row * self.rec_bytes

    def _bits(self, off, bitoff, n):
        if n == 0:
            return 0
        d = self.db.data
        first, last = off + bitoff // 8, off + (bitoff + n - 1) // 8
        chunk = int.from_bytes(d[first:last + 1], 'big')
        return (chunk >> ((last - first + 1) * 8 - bitoff % 8 - n)) & ((1 << n) - 1)

    def _put(self, off, bitoff, n, v):
        if n == 0:
            return
        d = self.db.data
        first, last = off + bitoff // 8, off + (bitoff + n - 1) // 8
        chunk = int.from_bytes(d[first:last + 1], 'big')
        shift = (last - first + 1) * 8 - bitoff % 8 - n
        chunk = (chunk & ~(((1 << n) - 1) << shift)) | (v << shift)
        d[first:last + 1] = chunk.to_bytes(last - first + 1, 'big')

    def get(self, row, name, raw=False):
        """Integers are returned as the game shows them: stored value + the schema's low limit."""
        f = self.field(name)
        off = self._row_off(row)
        v = self._bits(off, f.bitoff, f.bits)
        if f.type == TYPE_STRING:
            return v.to_bytes(f.bits // 8, 'big').split(b'\0')[0].decode('utf-8', 'surrogateescape')
        return v if raw else v + (f.low or 0)

    def set(self, row, name, value, raw=False):
        f = self.field(name)
        off = self._row_off(row)
        if f.type == TYPE_STRING:
            raw = value.encode('utf-8', 'surrogateescape')
            size = f.bits // 8
            if len(raw) >= size:
                raise ValueError(f'{f.name}: {len(raw)} bytes, field holds {size - 1}')
            self._put(off, f.bitoff, f.bits, int.from_bytes(raw.ljust(size, b'\0'), 'big'))
        else:
            value = int(value)
            if (f.low is not None and value < f.low) or (f.high is not None and value > f.high):
                raise ValueError(f'{f.name}: {value} outside {f.low}..{f.high}')
            if not raw:
                value -= f.low or 0
            if not 0 <= value < (1 << f.bits):
                raise ValueError(f'{f.name}: {value} does not fit in {f.bits} bits')
            self._put(off, f.bitoff, f.bits, value)
        self.db.dirty.add(self.code)

    def is_deleted(self, row):
        """The free flag: the last bit of the record (only meaningful where no field reaches that bit)."""
        if self.rec_bits < 1:
            return False
        return bool(self._bits(self._row_off(row), self.rec_bits - 1, 1))

    def _set_flag(self, row, deleted):
        self._put(self._row_off(row), self.rec_bits - 1, 1, int(deleted))
        self.db.dirty.add(self.code)

    def delete_row(self, row):
        self._set_flag(row, True)

    def live_rows(self):
        """Indexes of the rows that are not deleted."""
        return [r for r in range(self.rows) if not self.is_deleted(r)]

    def add_row(self, values=None):
        """Fill the lowest deleted row, else append (never past max rows). Returns the row index.
        New rows are all zero (integers read as their low limit) and live; values is {field: value}."""
        free = [r for r in range(self.rows) if self.is_deleted(r)]
        if free:
            row = free[0]
        elif self.rows < self.max_rows and self.data_off + (self.rows + 1) * self.rec_bytes <= self.crc_off:
            row = self.rows
        else:
            raise TableFull(f'{self.name}: no deleted row and {self.rows} of {self.max_rows} rows used')
        off = self.data_off + row * self.rec_bytes
        old = bytes(self.db.data[off:off + self.rec_bytes])
        old_rows = self.rows
        self.db.data[off:off + self.rec_bytes] = bytes(self.rec_bytes)
        if row == self.rows:
            self.rows += 1
            struct.pack_into('>H', self.db.data, self.off + 0x12, self.rows)
        self.db.dirty.add(self.code)
        try:
            for k, v in (values or {}).items():
                self.set(row, k, v)
        except Exception:
            self.db.data[off:off + self.rec_bytes] = old
            self.rows = old_rows
            struct.pack_into('>H', self.db.data, self.off + 0x12, old_rows)
            raise
        return row

    def set_max_rows(self, n):
        """Only if the table's space (up to its CRC) holds n rows and none is cut off."""
        if not self.rows <= n <= 0xFFFF:
            raise ValueError(f'{self.name}: max rows {n} below the {self.rows} rows in use')
        if self.data_off + n * self.rec_bytes > self.crc_off:
            raise ValueError(f'{self.name}: {n} rows need {n * self.rec_bytes} bytes, space is {self.crc_off - self.data_off}')
        self.max_rows = n
        struct.pack_into('>H', self.db.data, self.off + 0x10, n)
        self.db.dirty.add(self.code)

    def widen(self, name, new_bits):
        """Make an integer field new_bits wide. The value stays as stored; later fields move up, record bits
        and bytes are recomputed, every row (up to max rows) is rewritten, the free flag stays the last bit,
        and the file is laid out again and resealed. The field's high limit, if known, becomes the new maximum."""
        f = self.field(name)
        if f.type != TYPE_INT:
            raise ValueError(f'{f.name}: only integer fields can be widened')
        if not f.bits < new_bits <= 32:
            raise ValueError(f'{f.name}: {f.bits} bits cannot become {new_bits} (wider, at most 32)')
        if f.bitoff + f.bits > self.rec_bits - 1:
            raise ValueError(f'{f.name}: field reaches the free flag bit')
        d = self.db.data
        delta = new_bits - f.bits
        old_end = f.bitoff + f.bits
        space = self.crc_off - self.data_off
        if self.max_rows * self.rec_bytes > space:
            raise ValueError(f'{self.name}: max rows {self.max_rows} do not fit the table space; fix that first')
        rec_bits = self.rec_bits + delta
        rec_bytes = (rec_bits + 7) // 8
        old_pad = (self.rec_bytes * 8) - self.rec_bits
        tail = self.rec_bits - f.bitoff  # bits from the field's start to the end of the record
        rows = bytearray()
        for r in range(self.max_rows):
            p = self.data_off + r * self.rec_bytes
            v = int.from_bytes(d[p:p + self.rec_bytes], 'big') >> old_pad
            v = ((v >> tail) << (tail + delta)) | (v & ((1 << tail) - 1))
            rows += (v << (rec_bytes * 8 - rec_bits)).to_bytes(rec_bytes, 'big')
        extra = bytes(d[self.data_off + self.max_rows * self.rec_bytes:self.crc_off])
        head = bytearray(d[self.off:self.data_off])
        struct.pack_into('>II', head, 4, rec_bytes, rec_bits)
        for i in range(len(self.fields)):
            p = 0x24 + i * 16
            code = bytes(head[p + 8:p + 12]).decode('latin1')
            bitoff = be32(head, p + 4)
            if code == f.code:
                struct.pack_into('>I', head, p + 12, new_bits)
            elif bitoff >= old_end:
                struct.pack_into('>I', head, p + 4, bitoff + delta)
        self._blob = bytes(head) + bytes(rows) + extra + bytes(4)
        for g in self.fields.values():
            if g.bitoff >= old_end:
                g.bitoff += delta
        if f.high is not None:
            f.high = (f.low or 0) + (1 << new_bits) - 1
        f.bits = new_bits
        self.rec_bits, self.rec_bytes = rec_bits, rec_bytes
        self.db.rebuild()

    def export(self, out, include_deleted=False):
        """CSV of the live rows; include_deleted adds the deleted ones and a trailing 'deleted' column."""
        names = sorted(self.fields.values(), key=lambda f: f.bitoff)
        w = csv.writer(out)
        w.writerow(['row'] + [f.name for f in names] + (['deleted'] if include_deleted else []))
        for r in range(self.rows):
            dead = self.is_deleted(r)
            if dead and not include_deleted:
                continue
            w.writerow([r] + [self.get(r, f.code) for f in names] + ([int(dead)] if include_deleted else []))


class Db:
    def __init__(self, path, meta_path=None):
        self.path = path
        with open(path, 'rb') as f:
            self.data = bytearray(f.read())
        d = self.data
        if d[:2] != b'DB':
            raise ValueError(f'{path}: not a TDB file')
        self.dirty = set()
        n = be32(d, 0x10)
        self.dir_crc_off = 0x18 + n * 8
        base = self.dir_crc_off + 4
        self.tables = {}
        for i in range(n):
            code = bytes(d[0x18 + i * 8:0x1c + i * 8]).decode('latin1')
            self.tables[code] = Table(self, code, be32(d, 0x1c + i * 8) + base)
        # The CRC closes the table's space, which can be larger than max rows (draft_roster.db).
        order = sorted(self.tables.values(), key=lambda t: t.off)
        for a, b in zip(order, order[1:] + [None]):
            a.crc_off = (b.off if b else len(d)) - 4
        if meta_path is None:
            meta_path = os.path.join(os.path.dirname(path), 'nhlng-meta.xml')
        self.meta_path = meta_path if os.path.exists(meta_path) else None
        if self.meta_path:
            self._apply_meta(meta_path)

    def _apply_meta(self, meta_path):
        for mt in ET.parse(meta_path).getroot().findall('table'):
            t = self.tables.get(mt.get('shortname'))
            if t is None:
                continue
            t.name = mt.get('name')
            for mf in mt.find('fields'):
                f = t.fields.get(mf.get('shortname'))
                if f is None:
                    continue
                f.name = mf.get('name')
                if f.type == TYPE_INT and mf.get('rangelow') is not None:
                    f.low, f.high = int(mf.get('rangelow')), int(mf.get('rangehigh'))

    def table(self, name):
        for t in self.tables.values():
            if name in (t.name, t.code):
                return t
        raise KeyError(f'no table {name}')

    def check(self):
        """Problems found, as text: wrong CRCs ('NAME header', 'NAME data', 'file header', 'directory'), then
        file size, record size, rows against max rows, max rows against the table's space, fields that overlap
        or touch the free flag bit, and ranges that do not fit the field's bits. Empty when consistent."""
        d = self.data
        bad = []
        if crc(d[:0x14]) != be32(d, 0x14):
            bad.append('file header')
        if crc(d[0x18:self.dir_crc_off]) != be32(d, self.dir_crc_off):
            bad.append('directory')
        for t in self.tables.values():
            if crc(d[t.off:t.off + 0x20]) != be32(d, t.off + 0x20):
                bad.append(f'{t.name} header')
            if crc(d[t.fields_off:t.crc_off]) != be32(d, t.crc_off):
                bad.append(f'{t.name} data')
        if be32(d, 8) != len(d):
            bad.append(f'file size field {be32(d, 8)} but file is {len(d)} bytes')
        for t in self.tables.values():
            if t.rec_bytes != (t.rec_bits + 7) // 8:
                bad.append(f'{t.name} record bytes {t.rec_bytes} do not match record bits {t.rec_bits}')
            if t.rows > t.max_rows:
                bad.append(f'{t.name} rows {t.rows} above max rows {t.max_rows}')
            space = t.crc_off - t.data_off
            if t.max_rows * t.rec_bytes > space:
                bad.append(f'{t.name} max rows {t.max_rows} need {t.max_rows * t.rec_bytes} bytes, space is {space}')
            end = 0
            for f in sorted(t.fields.values(), key=lambda f: f.bitoff):
                if f.bitoff < end:
                    bad.append(f'{t.name} field {f.name} overlaps the one before it')
                end = max(end, f.bitoff + f.bits)
                if f.bitoff + f.bits > t.rec_bits - 1:
                    bad.append(f'{t.name} field {f.name} reaches the free flag bit (record bits {t.rec_bits})')
                if f.type == TYPE_INT and f.low is not None and f.high is not None and f.high - f.low >= 1 << f.bits:
                    bad.append(f'{t.name} field {f.name} range {f.low}..{f.high} does not fit {f.bits} bits')
        return bad

    def rebuild(self):
        """Write the file again from the parsed tables: tables follow each other in directory-offset order, offsets
        and the file size are recomputed, and every CRC is recomputed. A consistent file comes out byte-identical."""
        d = self.data
        base = self.dir_crc_off + 4
        order = sorted(self.tables.values(), key=lambda t: t.off)
        blobs = [t._blob if t._blob is not None else bytes(d[t.off:t.crc_off + 4]) for t in order]
        head = bytearray(d[:self.dir_crc_off + 4])
        rel = {}
        pos = 0
        for t, b in zip(order, blobs):
            rel[t.code] = pos
            pos += len(b)
        for i in range(be32(d, 0x10)):
            code = bytes(d[0x18 + i * 8:0x1c + i * 8]).decode('latin1')
            struct.pack_into('>I', head, 0x1c + i * 8, rel[code])
        struct.pack_into('>I', head, 8, base + pos)
        self.data = head + b''.join(blobs)
        for t, b in zip(order, blobs):
            t._blob = None
            t.off = base + rel[t.code]
            t.fields_off = t.off + 0x24
            t.data_off = t.fields_off + len(t.fields) * 16
            t.crc_off = t.off + len(b) - 4
        self.seal_all()
        return self

    def seal(self):
        """Recompute the CRC of every table changed through set()."""
        for code in self.dirty:
            t = self.tables[code]
            struct.pack_into('>I', self.data, t.off + 0x20, crc(self.data[t.off:t.off + 0x20]))
            struct.pack_into('>I', self.data, t.crc_off, crc(self.data[t.fields_off:t.crc_off]))
        self.dirty.clear()

    def seal_all(self):
        d = self.data
        struct.pack_into('>I', d, 0x14, crc(d[:0x14]))
        struct.pack_into('>I', d, self.dir_crc_off, crc(d[0x18:self.dir_crc_off]))
        for t in self.tables.values():
            struct.pack_into('>I', d, t.off + 0x20, crc(d[t.off:t.off + 0x20]))
            struct.pack_into('>I', d, t.crc_off, crc(d[t.fields_off:t.crc_off]))
        self.dirty.clear()

    def save(self, path):
        self.seal()
        with open(path, 'wb') as f:
            f.write(self.data)


def _set_attr(tag, attr, value):
    m = re.search(r'(\s%s\s*=\s*)(["\'])(.*?)\2' % attr, tag)
    if m:
        return tag[:m.start(3)] + str(value) + tag[m.end(3):]
    end = tag.rindex('/>') if tag.endswith('/>') else len(tag) - 1
    return tag[:end].rstrip() + f' {attr}="{value}"' + (' ' if tag.endswith('/>') else '') + tag[end:]


def widen_meta(text, table, field, new_bits):
    """The meta XML text with one field widened: depth = new_bits and rangehigh = rangelow + 2^new_bits - 1.
    table and field are the 4-letter shortnames. Everything else in the text is left as it was."""
    def find(pattern, lo, hi, what):
        hits = [m for m in re.finditer(pattern, text[lo:hi]) if re.search(r'\sshortname\s*=\s*["\']%s["\']' % what[1], m.group(0))]
        if len(hits) != 1:
            raise KeyError(f'meta: {len(hits)} matches for {what[0]} {what[1]}')
        return lo + hits[0].start(), lo + hits[0].end()
    ts, te = find(r'<table\b[^>]*>', 0, len(text), ('table', table))
    tend = text.find('</table>', te)
    fs, fe = find(r'<field\b[^>]*>', te, tend if tend >= 0 else len(text), ('field', field))
    tag = text[fs:fe]
    tag = _set_attr(tag, 'depth', new_bits)
    m = re.search(r'\srangelow\s*=\s*["\'](-?\d+)["\']', tag)
    if m or re.search(r'\srangehigh\s*=', tag):
        tag = _set_attr(tag, 'rangehigh', (int(m.group(1)) if m else 0) + (1 << new_bits) - 1)
    return text[:fs] + tag + text[fe:]


def _pop_opt(args, name, takes=True):
    if name not in args:
        return None
    i = args.index(name)
    if takes:
        if i + 1 >= len(args):
            raise SystemExit(f'{name} needs a value')
        val = args[i + 1]
        del args[i:i + 2]
        return val
    del args[i]
    return True


def main(argv):
    args = list(argv[1:])
    include_deleted = _pop_opt(args, '--deleted', False)
    meta = _pop_opt(args, '--meta')
    meta_out = _pop_opt(args, '--meta-out')
    argv = [argv[0]] + args
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]

    def pos(i):  # optional trailing META argument
        return meta or (argv[i] if len(argv) > i else None)
    if cmd == 'verify':
        rc = 0
        for path in argv[2:]:
            db = Db(path)
            bad = db.check()
            before = bytes(db.data)
            db.seal_all()
            same = before == bytes(db.data)
            print(f'{path}: {len(db.tables)} tables, CRC errors {len(bad)}, rewrite identical {same}')
            for b in bad:
                print(' ', b)
            rc |= bool(bad) or not same
        return rc
    if cmd == 'check':
        db = Db(argv[2], pos(3))
        bad = db.check()
        print(f'{argv[2]}: {len(db.tables)} tables, {len(bad)} problems')
        for b in bad:
            print(' ', b)
        return int(bool(bad))
    if cmd == 'tables':
        db = Db(argv[2], pos(3))
        for t in sorted(db.tables.values(), key=lambda t: t.off):
            print(f'{t.code} {t.name:36s} rows {t.rows:5d}/{t.max_rows:5d} record {t.rec_bytes:4d} fields {len(t.fields):3d}')
        return 0
    if cmd == 'export':
        db = Db(argv[2], pos(4))
        db.table(argv[3]).export(sys.stdout, bool(include_deleted))
        return 0
    if cmd == 'live':
        db = Db(argv[2], pos(4))
        t = db.table(argv[3])
        live = len(t.live_rows())
        print(f'{t.name}: rows {t.rows}/{t.max_rows}, live {live}, deleted {t.rows - live}')
        return 0
    if cmd == 'set':
        db = Db(argv[2], pos(8))
        t = db.table(argv[4])
        old = t.get(int(argv[5]), argv[6])
        t.set(int(argv[5]), argv[6], argv[7])
        db.save(argv[3])
        print(f'{t.name} row {argv[5]} {argv[6]}: {old} -> {argv[7]}')
        return 0
    if cmd == 'delete':
        db = Db(argv[2], meta)
        t = db.table(argv[4])
        t.delete_row(int(argv[5]))
        db.save(argv[3])
        print(f'{t.name} row {argv[5]} deleted')
        return 0
    if cmd == 'add':
        db = Db(argv[2], meta)
        t = db.table(argv[4])
        values = dict(a.split('=', 1) for a in argv[5:])
        try:
            row = t.add_row(values)
        except (TableFull, ValueError, KeyError) as e:
            print(f'add failed: {e}')
            return 1
        db.save(argv[3])
        print(f'{t.name} new row {row}')
        return 0
    if cmd == 'widen':
        db = Db(argv[2], meta)
        t = db.table(argv[4])
        f = t.field(argv[5])
        old_bits = f.bits
        try:
            t.widen(argv[5], int(argv[6]))
        except ValueError as e:
            print(f'widen failed: {e}')
            return 1
        db.save(argv[3])
        bad = db.check()
        if meta_out:
            if not db.meta_path:
                print('widen: no meta XML found to update')
                return 1
            with open(db.meta_path, encoding='utf-8') as fh:
                text = fh.read()
            with open(meta_out, 'w', encoding='utf-8') as fh:
                fh.write(widen_meta(text, t.code, f.code, int(argv[6])))
        print(f'{t.name} {f.name}: {old_bits} -> {f.bits} bits, record {t.rec_bits} bits / {t.rec_bytes} bytes, '
              f'{len(bad)} problems')
        return int(bool(bad))
    if cmd == 'widen-meta':
        with open(argv[2], encoding='utf-8') as fh:
            text = fh.read()
        with open(argv[3], 'w', encoding='utf-8') as fh:
            fh.write(widen_meta(text, argv[4], argv[5], int(argv[6])))
        return 0
    if cmd == 'maxrows':
        db = Db(argv[2], meta)
        t = db.table(argv[4])
        try:
            t.set_max_rows(int(argv[5]))
        except ValueError as e:
            print(f'maxrows failed: {e}')
            return 1
        db.save(argv[3])
        print(f'{t.name} max rows {argv[5]}')
        return 0
    if cmd == 'rebuild':
        db = Db(argv[2], pos(4))
        db.rebuild()
        db.save(argv[3])
        bad = db.check()
        print(f'{argv[3]}: rebuilt, {len(bad)} problems')
        return int(bool(bad))
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
