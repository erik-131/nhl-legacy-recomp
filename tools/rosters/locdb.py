#!/usr/bin/env python3
"""Read and rewrite the EA localisation DB (fe/loc/nhl_<lang>.db, one table LanguageStrings).

  locdb.py list DB [FILTER]              KEY<TAB>TEXT for every key (FILTER: substring of key or text)
  locdb.py get DB KEY                    text of one key (matched by hashid, case-insensitive like the game)
  locdb.py search DB TEXT                keys whose text contains TEXT (case-insensitive)
  locdb.py export DB OUT.csv             key,hashid,text for all keys (game text: do not commit)
  locdb.py check DB                      CRCs, hashid order/duplicates, and that a rewrite is byte-identical
  locdb.py set DB KEY TEXT --out OUT     change one key; more pairs: --set KEY=TEXT (repeatable)
  locdb.py add DB KEY TEXT --out OUT     new key (also --set KEY=TEXT pairs): record inserted in hashid order
  locdb.py rewrite DB --out OUT          parse and write back unchanged (must equal the input)

Format (big endian; T3db container, one table of type 0x42): header 'DB' 00 08, endian flag 01, file size @8, table
count @0x10, CRC @0x14; directory @0x18; table block = CRC of the directory, 0x24-byte header, CRC of the header,
3 field descriptors (16 bytes each), then max-rows 16-byte records, then the string pool, then a CRC of the pool.
Record: key offset, text offset, hashid, 0 (offsets into the pool; text offset FFFFFFFF = no text).
Pool: a Huffman tree (BFS, 4 bytes per node: [idx,val,idx,val]; idx 0 = leaf holding byte val; a node's right child
is queued before its left; left = bit 0), then strings: key = 1-byte length + bits, text = 2-byte length + bits;
lengths count output bytes, bits are MSB first, the last byte is padded with zeros. Text is UTF-8.
Table header +0x10 = pool bytes used (the pool area may be longer, 4-aligned). hashid = CRC-32 (reflected, poly
EDB88320, start 0, no final xor) of the key bytes with bit 0x20 cleared in every byte, so case does not matter.
Records are sorted by hashid; the game looks a key up by hashid and sees the first match only.
Edit rules: the tree is fixed, so a text can only use bytes the tree has. A new text goes in place when the
old string is private to the key and the new one is not longer, else it is appended to the pool and the
key is repointed (file grows; a different file size was accepted by the game).
All CRCs are recomputed. Never commit a loc DB or an export: this repo is public.
"""
import csv
import struct
import sys
from collections import deque

MISSING = 0xFFFFFFFF


def _crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


_T = _crc_table()


def crc(data):
    c = 0xFFFFFFFF
    for x in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _T[(c >> 24) ^ x]
    return c


_HT = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _HT.append(_c)


def key_hash(key):
    h = 0
    for ch in key.encode('utf-8'):
        h = (h >> 8) ^ _HT[(h ^ (ch & ~0x20)) & 0xFF]
    return h


class LocDb:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        if d[:2] != b'DB' or d[4] != 1:
            raise ValueError('not a big-endian T3db file')
        if self.u32(0x10) != 1:
            raise ValueError('expected exactly one table')
        self.blk = 0x18 + 8 + self.u32(0x1C)
        b = self.blk
        if self.u32(b + 8) != 16:
            raise ValueError('record size is not 16: not a string table')
        nf = d[b + 0x1C]
        self.maxrows, self.rows = struct.unpack('>HH', d[b + 0x14:b + 0x18])
        self.rec0 = b + 0x28 + 16 * nf
        self.pool = self.rec0 + 16 * self.maxrows
        self.pool_end = len(d) - 4
        self._tree()

    def u32(self, o):
        return struct.unpack_from('>I', self.d, o)[0]

    def _tree(self):
        kids, q, pos, nid = {}, deque([0]), 0, 1
        while q:
            p = q.popleft()
            ent = []
            for k in (0, 2):
                idx, val = self.d[self.pool + pos + k], self.d[self.pool + pos + k + 1]
                if idx == 0:
                    ent.append(('leaf', val))
                else:
                    ent.append(('node', nid))
                    nid += 1
            pos += 4
            kids[p] = ent
            for kind, n in (ent[1], ent[0]):
                if kind == 'node':
                    q.append(n)
        self.kids, self.tree_size = kids, pos
        self.codes = {}

        def walk(n, pre):
            for bit, (kind, v) in enumerate(kids[n]):
                if kind == 'leaf':
                    self.codes[v] = pre + str(bit)
                else:
                    walk(v, pre + str(bit))
        walk(0, '')

    def decode(self, off, plen):
        o = self.pool + off
        n = self.d[o] if plen == 1 else (self.d[o] << 8) | self.d[o + 1]
        bp, out = (o + plen) * 8, bytearray()
        while len(out) < n:
            node = 0
            while True:
                bit = (self.d[bp >> 3] >> (7 - (bp & 7))) & 1
                bp += 1
                kind, v = self.kids[node][bit]
                if kind == 'leaf':
                    out.append(v)
                    break
                node = v
        return bytes(out), (bp + 7) // 8 - o

    def encode(self, raw, plen):
        bad = sorted({chr(b) for b in raw if b not in self.codes})
        if bad:
            raise ValueError(f'bytes not in the Huffman tree: {bad}')
        bits = ''.join(self.codes[b] for b in raw)
        nb = (len(bits) + 7) // 8
        body = int(bits.ljust(nb * 8, '0'), 2).to_bytes(nb, 'big') if nb else b''
        return len(raw).to_bytes(plen, 'big') + body

    def rec(self, r):
        return self.rec0 + 16 * r

    def entry(self, r):
        ko, to, h, _ = struct.unpack_from('>IIII', self.d, self.rec(r))
        return ko, to, h

    def key_at(self, r):
        return self.decode(self.entry(r)[0], 1)[0].decode('utf-8', 'replace')

    def text_at(self, r):
        to = self.entry(r)[1]
        return '' if to == MISSING else self.decode(to, 2)[0].decode('utf-8', 'replace')

    def items(self):
        for r in range(self.rows):
            yield self.key_at(r), self.text_at(r), self.entry(r)[2]

    def find(self, key):
        """Row the game would find: first record with the key's hashid, or None."""
        h = key_hash(key)
        for r in range(self.rows):
            if self.entry(r)[2] == h:
                return r
        return None

    def get(self, key):
        r = self.find(key)
        if r is None:
            raise KeyError(key)
        return self.text_at(r)

    def set(self, key, text):
        r = self.find(key)
        if r is None:
            raise KeyError(key)
        _, to, _ = self.entry(r)
        enc = self.encode(text.encode('utf-8'), 2)
        if to != MISSING:
            shared = sum(1 for q in range(self.rows) if self.entry(q)[1] == to) > 1
            if not shared and len(enc) <= self.decode(to, 2)[1]:
                self.d[self.pool + to:self.pool + to + len(enc)] = enc
                return 'in place'
        struct.pack_into('>I', self.d, self.rec(r) + 4, self._append(enc))
        return 'appended'

    def _append(self, enc):
        """Add bytes at the end of the used pool (the area and the file grow, 4-aligned); returns the offset."""
        used = self.u32(self.blk + 0x10)
        area = self.pool_end - self.pool
        new_used = used + len(enc)
        grow = (new_used + 3) // 4 * 4 - area
        if grow > 0:
            self.d[self.pool_end:self.pool_end] = bytes(grow)
            self.pool_end += grow
        self.d[self.pool + used:self.pool + used + len(enc)] = enc
        self.d[self.blk + 0x10:self.blk + 0x14] = struct.pack('>I', new_used)
        return used

    def add(self, key, text):
        """New key (error if its hashid exists): name and text go to the pool end, a 16-byte record is
        inserted in hashid order (max rows and rows +1). Returns the row."""
        h = key_hash(key)
        if self.find(key) is not None:
            raise KeyError(f'{key}: hashid {h:08X} already exists, use set')
        enc_t = self.encode(text.encode('utf-8'), 2)
        enc_k = self.encode(key.encode('utf-8'), 1)
        ko = self._append(enc_k)
        to = self._append(enc_t)
        pos = next((r for r in range(self.rows) if self.entry(r)[2] > h), self.rows)
        self.d[self.rec(pos):self.rec(pos)] = struct.pack('>IIII', ko, to, h, 0)
        self.maxrows += 1
        self.rows += 1
        struct.pack_into('>HH', self.d, self.blk + 0x14, self.maxrows, self.rows)
        self.pool += 16
        self.pool_end += 16
        return pos

    def seal(self):
        d, b = self.d, self.blk
        struct.pack_into('>I', d, 0x08, len(d))
        struct.pack_into('>I', d, 0x14, crc(d[0:0x14]))
        struct.pack_into('>I', d, b, crc(d[0x18:0x20]))
        struct.pack_into('>I', d, b + 0x24, crc(d[b + 4:b + 0x24]))
        struct.pack_into('>I', d, len(d) - 4, crc(d[b + 0x28:len(d) - 4]))

    def problems(self):
        out, d, b = [], self.d, self.blk
        exp = [(0x14, crc(d[0:0x14])), (b, crc(d[0x18:0x20])), (b + 0x24, crc(d[b + 4:b + 0x24])),
               (len(d) - 4, crc(d[b + 0x28:len(d) - 4]))]
        out += [f'CRC at {o:#x} is {self.u32(o):08X}, expected {c:08X}' for o, c in exp if self.u32(o) != c]
        if self.u32(0x08) != len(d):
            out.append('file size field differs from the real size')
        prev, seen = None, {}
        for r in range(self.rows):
            k, h = self.key_at(r), self.entry(r)[2]
            if h != key_hash(k):
                out.append(f'#{r} {k}: hashid {h:08X} is not the key hash')
            if prev is not None and h < prev:
                out.append(f'#{r} {k}: hashid out of order')
            if h in seen:
                out.append(f'#{r} {k}: same hashid as #{seen[h]} (game sees the first only)')
            seen.setdefault(h, r)
            prev = h
        return out

    def roundtrip_ok(self):
        """Every string re-encodes to its stored bytes (the coding is deterministic)."""
        for r in range(self.rows):
            ko, to, _ = self.entry(r)
            for off, plen in ((ko, 1), (to, 2)):
                if off == MISSING:
                    continue
                raw, size = self.decode(off, plen)
                if self.encode(raw, plen) != bytes(self.d[self.pool + off:self.pool + off + size]):
                    return False
        return True


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path, rest = argv[1], argv[2], argv[3:]
    sys.stdout.reconfigure(encoding='utf-8')
    src = open(path, 'rb').read()
    db = LocDb(src)
    out = None
    if '--out' in rest:
        i = rest.index('--out')
        out = rest[i + 1]
        rest = rest[:i] + rest[i + 2:]
    if cmd == 'list':
        f = (rest[0] if rest else '').lower()
        for k, t, _ in db.items():
            if f in k.lower() or f in t.lower():
                print(f'{k}\t{t}')
    elif cmd == 'get':
        print(db.get(rest[0]))
    elif cmd == 'search':
        for k, t, _ in db.items():
            if rest[0].lower() in t.lower():
                print(f'{k}\t{t}')
    elif cmd == 'export':
        with open(rest[0], 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['key', 'hashid', 'text'])
            for k, t, h in db.items():
                w.writerow([k, f'{h:08X}', t])
    elif cmd == 'check':
        p = db.problems()
        print('\n'.join(p) if p else f'OK: {db.rows} keys, CRCs good')
        print('re-encoding every string reproduces the stored bytes:', db.roundtrip_ok())
        db2 = LocDb(src)
        db2.seal()
        print('rewrite byte-identical:', bytes(db2.d) == src)
        return 1 if p else 0
    elif cmd in ('set', 'add', 'rewrite'):
        pairs = []
        if cmd in ('set', 'add'):
            if '--set' not in rest:
                pairs.append((rest[0], rest[1]))
            for i, a in enumerate(rest):
                if a == '--set':
                    k, t = rest[i + 1].split('=', 1)
                    pairs.append((k, t))
        if not out:
            sys.exit('--out is required')
        for k, t in pairs:
            if cmd == 'add':
                print(f'{k}: new key -> {t!r} (row {db.add(k, t)})')
            else:
                print(f'{k}: {db.get(k)!r} -> {t!r} ({db.set(k, t)})')
        db.seal()
        bad = db.problems()
        if bad:
            sys.exit('\n'.join(bad))
        open(out, 'wb').write(db.d)
        print('wrote', out, len(db.d), 'bytes')
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
