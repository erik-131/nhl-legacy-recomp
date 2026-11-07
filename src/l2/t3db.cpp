// L2 module 1: T3db table reader as readable C++. See t3db.h and docs/l2-t3db.md.
#include "t3db.h"

#include <array>

namespace nhl::l2::t3db {

namespace {

// The game's table at 0x82304320 is the standard reflected CRC32 table (poly 0xEDB88320): [read] by the formula
// below matching every hash the game computes (checked live by the VERIFY mode).
constexpr std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    t[i] = c;
  }
  return t;
}
constexpr auto kCrcTable = MakeCrcTable();

inline uint32_t CrcStep(uint32_t crc, uint8_t ch) {
  const uint32_t c = ch & 0xDFu;  // rlwinm r8,r11,0,27,25: clears bit 0x20
  return kCrcTable[(c ^ crc) & 0xFF] ^ (crc >> 8);
}

// The binary search shared by FindTable (lo = 0) and FindField (lo = 1). It is the game's own variant: when
// the probe is the lower/upper bound it does one last compare against the other bound instead of shrinking.
// entry_hash(i) is the unsigned sort key of element i; returns the element index or -1.
template <class F>
int32_t BinarySearch(int32_t lo, int32_t hi, uint32_t key, F&& entry_hash) {
  if (hi < lo) return -1;
  while (lo <= hi) {
    const int32_t mid = lo + ((hi - lo) >> 1);
    const uint32_t h = entry_hash(mid);
    if (key == h) return mid;
    if (key > h) {  // unsigned compare (cmplw / bge)
      if (mid == lo) return key == entry_hash(hi) ? hi : -1;
      lo = mid;
    } else {
      if (mid == hi) return key == entry_hash(lo) ? lo : -1;
      hi = mid;
    }
  }
  return -1;
}

}  // namespace

uint32_t HashName(const uint8_t* name) {
  if (!name) return 0;
  uint32_t crc = 0;
  for (; *name; ++name) crc = CrcStep(crc, *name);
  return crc;
}

uint32_t HashName(const Memory& mem, uint32_t name_addr) {
  if (!name_addr) return 0;
  uint32_t crc = 0;
  for (uint8_t ch; (ch = mem.U8(name_addr)) != 0; ++name_addr) crc = CrcStep(crc, ch);
  return crc;
}

uint32_t FindTable(const Memory& mem, uint32_t db, uint32_t hash, uint32_t sorted) {
  const uint32_t tables = mem.U32(db + offsetof(Db, tables));
  const uint32_t count = mem.U32(db + offsetof(Db, table_count));
  if (sorted != 1) {  // linear on hash2; also returns 0 when not found
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t t = tables + i * sizeof(TableDesc);
      if (mem.U32(t + offsetof(TableDesc, hash2)) == hash) return t;
    }
    return 0;
  }
  const int32_t idx = BinarySearch(0, static_cast<int32_t>(count) - 1, hash, [&](int32_t i) {
    return mem.U32(tables + static_cast<uint32_t>(i) * sizeof(TableDesc) + offsetof(TableDesc, hash));
  });
  return idx < 0 ? 0 : tables + static_cast<uint32_t>(idx) * sizeof(TableDesc);
}

uint32_t FindField(const Memory& mem, uint32_t table, uint32_t hash) {
  const uint32_t count = mem.U8(table + offsetof(TableDesc, field_count));
  const uint32_t fields = mem.U32(table + offsetof(TableDesc, fields));
  auto field_hash = [&](uint32_t i) { return mem.U32(fields + i * sizeof(FieldDesc) + offsetof(FieldDesc, hash)); };
  if (count <= 16) {
    // Linear scan. (The original runs a fixed 16 iterations and so also reads past a short array; it can only
    // differ from this when that out-of-range memory holds the searched hash.)
    for (uint32_t i = 0; i < count; ++i)
      if (field_hash(i) == hash) return fields + i * sizeof(FieldDesc);
  }
  // Element 0 on its own, then the binary search over 1..count-1.
  if (field_hash(0) == hash) return fields;
  const int32_t idx = BinarySearch(1, static_cast<int32_t>(count) - 1, hash,
                                   [&](int32_t i) { return field_hash(static_cast<uint32_t>(i)); });
  return idx < 0 ? 0 : fields + static_cast<uint32_t>(idx) * sizeof(FieldDesc);
}

bool ExtractBitField(const Memory& mem, uint32_t type, uint32_t bit_offset, uint32_t depth, uint32_t rowbase,
                     uint32_t* out) {
  if (type == 0 || type == 1 || (type >= 13 && type <= 15)) return false;  // byte copy / special: original
  // `rlwinm r9,r11,29,3,29`: byte offset = (bit_offset >> 3) with the low 2 bits cleared, i.e. 4-byte aligned;
  // the shift is the remaining 5 bits (bit_offset & 31, mask 0xFFFFFFFF0000001F).
  const uint64_t word = mem.U64(rowbase + ((bit_offset >> 3) & ~3u));
  const uint64_t shifted = word << (bit_offset & 31);
  const uint32_t sh = (64u - depth) & 0x7F;  // PPC srd/srad use 7 bits of the count; bit 6 set = whole word
  uint64_t v;
  if (type == 2) {  // signed: srad
    const int64_t s = static_cast<int64_t>(shifted);
    v = static_cast<uint64_t>(sh >= 64 ? (s < 0 ? -1 : 0) : (s >> sh));
  } else {  // 3, 4 and every other type: srd
    v = sh >= 64 ? 0 : (shifted >> sh);
  }
  *out = static_cast<uint32_t>(v);  // stw
  return true;
}

bool ExtractBitField(const Memory& mem, uint32_t column, uint32_t rowbase, uint32_t* out) {
  return ExtractBitField(mem, mem.U32(column + offsetof(Column, type)), mem.U32(column + offsetof(Column, bit_offset)),
                         mem.U32(column + offsetof(Column, depth)), rowbase, out);
}

int32_t ConvertInt(uint32_t raw, int32_t low) { return static_cast<int32_t>(static_cast<uint32_t>(low) + raw); }

namespace {
// 32-bit PPC arithmetic: multiplies wrap, divw truncates toward zero, srawi+addze is a truncating /4.
inline int32_t Mul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t Add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t Div(int32_t a, int32_t b) { return a / b; }
}  // namespace

// Day number -> calendar date, the integer algorithm of 0x827B3184 (a Fliegel/Van Flandern style Julian-day
// conversion: 146097 days per 400 years, 1461001/4000 for the century step, 2447/80 for the month). The
// constants are exactly the guest's; 2367729 = 36*65536 + 8433 is added to the stored day count first.
Date ConvertDate(uint32_t raw, int32_t low) {
  int32_t j = Add(ConvertInt(raw, low), 36 * 65536 + 8433);
  int32_t q = Div(static_cast<int32_t>(static_cast<uint32_t>(j) << 2), 146097);       // r9
  int32_t t = Add(Mul(q, 146097), 3) / 4;                                                // r11
  int32_t century = Mul(Add(q, 65536 - 49), 100);                                        // r9
  int32_t d = j - t;                                                                     // r11
  int32_t y = Div(Mul(Add(d, 1), 4000), 1461001);                                        // r10
  int32_t year_base = Add(century, y);                                                   // r8
  d = d - Mul(y, 1461) / 4;
  int32_t day_raw = Add(d, 31);                                                          // r10
  int32_t m = Div(Mul(day_raw, 80), 2447);                                               // r11
  int32_t m11 = Div(m, 11);                                                              // r9
  Date out;
  out.year = static_cast<uint16_t>(Add(year_base, m11));
  out.day = static_cast<uint8_t>(day_raw - Div(Mul(m, 2447), 80));
  out.month = static_cast<uint8_t>(Add(Add(Mul(m11, 244), m), 2));
  return out;
}

// Seconds since midnight -> hour/minute/second (signed divw, so negative values give negative fields, as in the guest).
Time ConvertTime(uint32_t raw, int32_t low) {
  const int32_t v = ConvertInt(raw, low);
  const int32_t m = v / 60;
  Time t;
  t.second = static_cast<uint8_t>(v - m * 60);
  t.minute = static_cast<uint8_t>(m - (m / 60) * 60);
  t.hour = static_cast<uint8_t>(m / 60);
  return t;
}

}  // namespace nhl::l2::t3db
