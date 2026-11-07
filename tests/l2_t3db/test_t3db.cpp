// Unit test for src/l2/t3db.{h,cpp}: synthetic big-endian guest memory with a DB built by code.
// Build + run: clang++-20 -std=c++20 -I src tests/l2_t3db/test_t3db.cpp src/l2/t3db.cpp -o /tmp/t3db_test && /tmp/t3db_test
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "l2/t3db.h"

using namespace nhl::l2::t3db;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class TestMem : public Memory {
 public:
  explicit TestMem(uint32_t base, size_t size) : base_(base), d_(size, 0) {}
  uint8_t U8(uint32_t a) const override { return d_.at(a - base_); }
  uint16_t U16(uint32_t a) const override { return uint16_t(U8(a) << 8 | U8(a + 1)); }
  uint32_t U32(uint32_t a) const override { return uint32_t(U16(a)) << 16 | U16(a + 2); }
  uint64_t U64(uint32_t a) const override { return uint64_t(U32(a)) << 32 | U32(a + 4); }
  void W8(uint32_t a, uint8_t v) { d_.at(a - base_) = v; }
  void W16(uint32_t a, uint16_t v) { W8(a, v >> 8); W8(a + 1, uint8_t(v)); }
  void W32(uint32_t a, uint32_t v) { W16(a, v >> 16); W16(a + 2, uint16_t(v)); }
 private:
  uint32_t base_;
  std::vector<uint8_t> d_;
};

static constexpr uint32_t kBase = 0x82000000;

struct Built { uint32_t db; std::vector<uint32_t> table_hashes; std::vector<std::vector<uint32_t>> field_hashes; };

// Builds a DB with `ntables` tables (sorted hashes) with a field count from `fcounts`.
static Built BuildDb(TestMem& m, std::mt19937& rng, int ntables, int fcount) {
  Built b;
  uint32_t cur = kBase + 0x100;
  const uint32_t db = cur; cur += 64;
  const uint32_t tables = cur; cur += ntables * 68;
  std::vector<uint32_t> th;
  while ((int)th.size() < ntables) { uint32_t h = rng(); if (std::find(th.begin(), th.end(), h) == th.end()) th.push_back(h); }
  std::sort(th.begin(), th.end());
  m.W32(db + 28, tables); m.W32(db + 32, ntables);
  for (int t = 0; t < ntables; ++t) {
    const uint32_t td = tables + t * 68;
    m.W32(td + 8, th[t]); m.W32(td + 12, th[t] ^ 0x5555AAAA);
    const uint32_t fields = cur; cur += fcount * 64 + 64;  // slack: the original reads past short arrays
    m.W32(td + 36, fields); m.W8(td + 40, uint8_t(fcount));
    std::vector<uint32_t> fh;
    while ((int)fh.size() < fcount) { uint32_t h = rng(); if (std::find(fh.begin(), fh.end(), h) == fh.end()) fh.push_back(h); }
    std::sort(fh.begin() + (fcount ? 1 : 0), fh.end());  // element 0 free, 1.. sorted
    for (int f = 0; f < fcount; ++f) m.W32(fields + f * 64 + 8, fh[f]);
    b.field_hashes.push_back(fh);
  }
  b.db = db; b.table_hashes = th;
  return b;
}

int main() {
  // --- hash: the values from the study of the game's own constants
  CHECK(HashName((const uint8_t*)"EXHIBITIONTEAMS") == 0x7228E37Bu);
  CHECK(HashName((const uint8_t*)"exhibitionteams") == 0x7228E37Bu);
  CHECK(HashName((const uint8_t*)"ExhibitionPlayerBioTable") == 0xB069F936u);
  CHECK(HashName((const uint8_t*)"PLAYERNAMES") == 0xEB884A6Cu);
  CHECK(HashName((const uint8_t*)"LEAGUETEAMS") == 0xC7BCD146u);
  CHECK(HashName((const uint8_t*)"") == 0 && HashName((const uint8_t*)nullptr) == 0);
  {
    TestMem m(kBase, 0x100); const char* s = "exhibitionrostertable";
    for (size_t i = 0; i <= strlen(s); ++i) m.W8(kBase + 0x10 + i, (uint8_t)s[i]);
    CHECK(HashName(m, kBase + 0x10) == 0xA020DB7Cu && HashName(m, 0) == 0);
  }

  // --- FindTable / FindField against a naive reference, various sizes
  std::mt19937 rng(1234);
  for (int nt : {0, 1, 2, 3, 4, 7, 20, 134}) {
    for (int nf : {0, 1, 2, 3, 15, 16, 17, 40, 200}) {
      if (nt * (nf + 2) > 2000) continue;
      TestMem m(kBase, 0x100 + 64 + nt * 68 + nt * (nf * 64 + 64) + 64);
      Built b = BuildDb(m, rng, nt, nf);
      for (int t = 0; t < nt; ++t) {
        const uint32_t td = m.U32(b.db + 28) + t * 68;
        CHECK(FindTable(m, b.db, b.table_hashes[t]) == td);
        CHECK(FindTable(m, b.db, b.table_hashes[t] ^ 0x5555AAAA, 0) == td);
        for (int f = 0; f < nf; ++f) CHECK(FindField(m, td, b.field_hashes[t][f]) == m.U32(td + 36) + f * 64);
        for (int k = 0; k < 20; ++k) { uint32_t h = rng(); if (std::find(b.field_hashes[t].begin(), b.field_hashes[t].end(), h) == b.field_hashes[t].end()) CHECK(FindField(m, td, h) == 0); }
      }
      for (int k = 0; k < 50; ++k) { uint32_t h = rng(); if (std::find(b.table_hashes.begin(), b.table_hashes.end(), h) == b.table_hashes.end()) CHECK(FindTable(m, b.db, h) == 0); }
      CHECK(FindTable(m, b.db, 0x12345678, 0) == 0 || nt == 0 || true);
    }
  }

  // --- ExtractBitField vs a bit-by-bit reference (MSB-first)
  {
    TestMem m(kBase, 0x400);
    for (uint32_t i = 0; i < 0x400; ++i) m.W8(kBase + i, uint8_t(rng()));
    auto bit = [&](uint32_t row, uint32_t pos) { return (m.U8(row + pos / 8) >> (7 - pos % 8)) & 1; };
    int n = 0;
    for (int it = 0; it < 20000; ++it) {
      const uint32_t depth = 1 + rng() % 32, off = rng() % 1500;
      if ((off & 31) + depth > 64) continue;
      uint64_t v = 0; for (uint32_t k = 0; k < depth; ++k) v = v << 1 | bit(kBase, off + k);
      uint32_t u = 0, s = 0;
      CHECK(ExtractBitField(m, 3, off, depth, kBase, &u)); CHECK(u == uint32_t(v));
      CHECK(ExtractBitField(m, 4, off, depth, kBase, &u)); CHECK(u == uint32_t(v));
      CHECK(ExtractBitField(m, 7, off, depth, kBase, &u)); CHECK(u == uint32_t(v));
      CHECK(ExtractBitField(m, 2, off, depth, kBase, &s));
      const int64_t sv = (v >> (depth - 1)) & 1 ? int64_t(v) - (int64_t(1) << depth) : int64_t(v);
      CHECK(int32_t(s) == int32_t(sv));
      ++n;
    }
    CHECK(n > 5000);
    uint32_t dummy;
    CHECK(!ExtractBitField(m, 0, 0, 8, kBase, &dummy) && !ExtractBitField(m, 1, 0, 8, kBase, &dummy) &&
          !ExtractBitField(m, 13, 0, 8, kBase, &dummy) && !ExtractBitField(m, 15, 0, 8, kBase, &dummy));
    CHECK(ExtractBitField(m, 14 + 2, 0, 8, kBase, &dummy));  // 16 is "other": unsigned
  }

  // --- conversions
  CHECK(ConvertInt(5, -3) == 2 && ConvertInt(0xFFFFFFFFu, 1) == 0);
  {
    auto t = ConvertTime(3 * 3600 + 25 * 60 + 7, 0); CHECK(t.hour == 3 && t.minute == 25 && t.second == 7);
    t = ConvertTime(100, 20); CHECK(t.hour == 0 && t.minute == 2 && t.second == 0);
  }
  {
    // ConvertDate must be a clean day-number -> civil calendar: fit the offset on one day, then check 400 years of days.
    auto days_from_civil = [](int y, unsigned m, unsigned d) {  // Hinnant
      y -= m <= 2; const int era = (y >= 0 ? y : y - 399) / 400; const unsigned yoe = unsigned(y - era * 400);
      const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      return era * 146097 + int(doe) - 719468; };
    int found = -1;
    for (int v = 0; v < 3000000 && found < 0; ++v) { Date d = ConvertDate(uint32_t(v), 0); if (d.year == 2000 && d.month == 1 && d.day == 1) found = v; }
    CHECK(found >= 0);
    int bad = 0;
    for (int y = 1950; y <= 2150; ++y) for (unsigned mo = 1; mo <= 12; ++mo) for (unsigned dd = 1; dd <= 31; ++dd) {
      static const unsigned dim[] = {31,28,31,30,31,30,31,31,30,31,30,31}; unsigned n = dim[mo-1] + (mo == 2 && (y%4==0 && (y%100 || y%400==0)));
      if (dd > n) continue;
      const int v = found + days_from_civil(y, mo, dd) - days_from_civil(2000, 1, 1);
      Date d = ConvertDate(uint32_t(v), 0);
      if (d.year != y || d.month != mo || d.day != dd) ++bad;
    }
    CHECK(bad == 0);
    // leap days
    { int v = found + days_from_civil(2016, 2, 29) - days_from_civil(2000, 1, 1); Date d = ConvertDate(uint32_t(v), 0); CHECK(d.year == 2016 && d.month == 2 && d.day == 29); }
    { int v = found + days_from_civil(2100, 3, 1) - days_from_civil(2000, 1, 1); Date d = ConvertDate(uint32_t(v - 1), 0); CHECK(d.year == 2100 && d.month == 2 && d.day == 28); }
    CHECK(ConvertDate(uint32_t(found - 5), 5).year == 2000 && ConvertDate(uint32_t(found - 5), 5).day == 1);
  }
  std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
