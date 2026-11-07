// L2 module 1: the game's T3db table reader (EA TDB runtime), as readable C++.
//
// Everything here is a pure function over a `Memory` interface (big-endian guest memory), so it runs against
// the real guest memory in the hooks (t3db_hooks.cpp) and against a synthetic in-memory DB in the unit test
// (tests/l2_t3db/test_t3db.cpp). Docs: docs/l2-t3db.md.
//
// Guest functions covered (addresses in the original image; "read" = from the disassembly, not guessed):
//   0x827B6708 db name hash          -> HashName()
//   0x827B6FA8 db table lookup(db,hash,sorted) (0x827B5D40 = sorted=1 wrapper) -> FindTable()
//   0x827B6EA0 db field lookup(table,hash)     -> FindField()
//   0x827A3288 db bit-field extractor(ctx,col,rowbase,out) -> ExtractBitField()
//   0x827B3058 db value-conversion stage: the value-conversion stage only -> ConvertInt/Float/Time/Date()
#pragma once
#include <cstddef>
#include <cstdint>

namespace nhl::l2::t3db {

// ---- guest memory --------------------------------------------------------------------------------------------
// Addresses are 32-bit guest virtual addresses; all multi-byte loads are big-endian (PowerPC).
class Memory {
 public:
  virtual ~Memory() = default;
  virtual uint8_t U8(uint32_t addr) const = 0;
  virtual uint16_t U16(uint32_t addr) const = 0;
  virtual uint32_t U32(uint32_t addr) const = 0;
  virtual uint64_t U64(uint32_t addr) const = 0;
  int32_t S32(uint32_t addr) const { return static_cast<int32_t>(U32(addr)); }
};

// ---- in-memory structures (read from the disassembly) ------------------------------------------------------
// Guest data is big-endian; these structs only document the layout (field names + offsets, checked by
// static_assert). They are never overlaid on guest memory directly: the functions read through Memory with
// offsetof(). Be32/Be16 mark big-endian fields.
struct Be32 { uint32_t raw; };
struct Be16 { uint16_t raw; };

// Database object ("Db", the thing FindTable gets in r3). Only the two fields the table lookup uses are known.
// Reached from a cursor as cursor->+4 (session) ->+4 (db) [read, GetFieldValue/GetByIndex 0x827B3008].
struct Db {
  uint8_t unknown0[28];
  Be32 tables;       // +28  guest pointer to TableDesc[table_count], sorted ascending by TableDesc::hash
  Be32 table_count;  // +32
};
static_assert(offsetof(Db, tables) == 28 && offsetof(Db, table_count) == 32);

// One table of the database, 68 bytes. The array is sorted by `hash` (unsigned) for the binary search used by
// every game call site (sorted=1); `hash2` is the key of the linear lookup (sorted=0, no call site seen).
struct TableDesc {
  uint8_t unknown0[8];
  Be32 hash;         // +8   db name hash(UPPER-CASED table name); the key the game addresses tables by
  Be32 hash2;        // +12  alternative key, linear search only [read]
  uint8_t unknown16[20];
  Be32 fields;       // +36  guest pointer to FieldDesc[field_count]
  uint8_t field_count;  // +40 (byte)
  uint8_t unknown41[27];
};
static_assert(sizeof(TableDesc) == 68);
static_assert(offsetof(TableDesc, hash) == 8 && offsetof(TableDesc, hash2) == 12 &&
              offsetof(TableDesc, fields) == 36 && offsetof(TableDesc, field_count) == 40);

// One column of a table, 64 bytes. Element 0 is looked up by itself; elements 1..count-1 are sorted by `hash`.
// A stored value is bit-packed MSB-first in the row; the shown value is stored + `low` (range low).
struct FieldDesc {
  uint8_t unknown0[8];
  Be32 hash;         // +8   db name hash(UPPER-CASED field name)
  uint8_t unknown12[8];
  Be16 bit_offset;   // +20  bit offset of the field inside the row (guest reads it as lhz)
  uint8_t depth;     // +22  width in bits (lbz)
  uint8_t unknown23;
  Be32 low;          // +24  rangelow, added to the stored value for ints, dates and times
  uint8_t unknown28[36];
};
static_assert(sizeof(FieldDesc) == 64);
static_assert(offsetof(FieldDesc, hash) == 8 && offsetof(FieldDesc, bit_offset) == 20 &&
              offsetof(FieldDesc, depth) == 22 && offsetof(FieldDesc, low) == 24);

// Column descriptor given to ExtractBitField (r4). In the game these are 16-byte entries at +84 of the
// runtime table object (index*16; 8279F3D0 passes `obj + idx*16 + 84`) [read].
struct Column {
  Be32 type;         // +0   2 signed int, 3/4 unsigned (identical code), 0/1 raw bytes, 13..15 special, else unsigned
  Be32 bit_offset;   // +4   bit offset into the row
  uint8_t unknown8[4];
  Be32 depth;        // +12  width in bits (types 0/1: bits of the byte string)
};
static_assert(sizeof(Column) == 16);
static_assert(offsetof(Column, type) == 0 && offsetof(Column, bit_offset) == 4 && offsetof(Column, depth) == 12);

// Cursor (the "cur" of GetFieldValue/GetByIndex), only the links the table lookup uses [read]:
//   cur+4 -> session, session+4 -> Db. GetByIndex(cur, tblIdx, fldIdx) = TableDesc at db.tables+tblIdx*68,
//   FieldDesc at table.fields+fldIdx*64, then GetFieldValue(cur, table, field, ...). Row selection lives in the
//   runtime record reader 0x8279F3D0 / 0x8279ECD8 and is NOT part of this module yet.
constexpr uint32_t kCursorSessionOff = 4;
constexpr uint32_t kSessionDbOff = 4;

// ---- functions -------------------------------------------------------------------------------------------------
// db name hash 0x827B6708. CRC32, reflected, poly 0xEDB88320, init 0, no final xor. Each byte is masked with
// 0xDF (clears bit 0x20: `rlwinm r8,r11,0,27,25`), which upper-cases letters but also maps e.g. '1'->0x11
// and 0x7B '{'->0x5B. Stops at the first NUL; a null pointer hashes to 0. [read]
uint32_t HashName(const uint8_t* name);  // host string (NUL terminated), nullptr -> 0
uint32_t HashName(const Memory& mem, uint32_t name_addr);

// db table lookup 0x827B6FA8. Returns the guest address of the TableDesc or 0. sorted != 1 (r5) does the linear
// search on hash2, anything else the binary search on hash.
uint32_t FindTable(const Memory& mem, uint32_t db, uint32_t hash, uint32_t sorted = 1);

// db field lookup 0x827B6EA0. Returns the guest address of the FieldDesc or 0. Fields: linear scan if
// count <= 16, then (and for count > 16 directly) element 0 compare + binary search over elements 1..count-1.
uint32_t FindField(const Memory& mem, uint32_t table, uint32_t hash);

// db bit-field extractor 0x827A3288, the numeric cases. Reads the 64-bit big-endian word at
// rowbase + ((bit_offset >> 3) & ~3), shifts left by bit_offset & 31 and right by 64 - depth (arithmetic for
// type 2, logical otherwise). Returns false for the types handled elsewhere in the original (0/1 byte copy,
// 13..15 special): the caller must call the original for those. `*out` is the 32-bit value the game stores.
bool ExtractBitField(const Memory& mem, uint32_t type, uint32_t bit_offset, uint32_t depth, uint32_t rowbase,
                     uint32_t* out);
bool ExtractBitField(const Memory& mem, uint32_t column, uint32_t rowbase, uint32_t* out);

// Value conversion stage of db value-conversion stage 0x827B3058 (r6 = requested type):
//   1 int:   out = raw + low       2 float: out = raw reinterpreted as float (low is NOT added)
//   3 string: not here (allocates)  4 date: Julian day number (raw+low) -> year/month/day
//   5 time:  seconds since midnight (raw+low) -> hh/mm/ss bytes
int32_t ConvertInt(uint32_t raw, int32_t low);
struct Date { uint16_t year; uint8_t month; uint8_t day; };  // guest layout: u16 BE year at +0, month +2, day +3
Date ConvertDate(uint32_t raw, int32_t low);
struct Time { uint8_t hour, minute, second; };  // guest layout: hour +0, minute +1, second +2
Time ConvertTime(uint32_t raw, int32_t low);

}  // namespace nhl::l2::t3db
