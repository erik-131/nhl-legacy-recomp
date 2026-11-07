// L2 hooks for the readable T3db module (t3db.h). Mode via cvar nhl_l2_t3db (env REX_NHL_L2_T3DB):
//   0 off (default, the hooks are a pure passthrough)
//   1 VERIFY: call the original guest function AND the readable implementation, compare, log
//             "[nhl-l2] MISMATCH ..." and a running calls/mismatches line once a second. The original's result
//             is what the game continues with.
//   2 REPLACE: use the readable implementation, skip the original (only for functions verified in mode 1).
// Docs: docs/l2-t3db.md.
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "t3db.h"

REXCVAR_DEFINE_INT32(nhl_l2_t3db, 0, "NHL", "L2 T3db reader: 0 off, 1 verify against the original, 2 replace the original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using namespace nhl::l2::t3db;

// Guest memory as the module's Memory interface (big-endian loads from the recomp's guest base).
class GuestMem final : public Memory {
 public:
  explicit GuestMem(uint8_t* b) : base(b) {}
  uint8_t U8(uint32_t a) const override { return REX_LOAD_U8(a); }
  uint16_t U16(uint32_t a) const override { return REX_LOAD_U16(a); }
  uint32_t U32(uint32_t a) const override { return REX_LOAD_U32(a); }
  uint64_t U64(uint32_t a) const override { return REX_LOAD_U64(a); }
  uint8_t* base;
};

enum Fn { kHash, kFindTable, kFindField, kExtract, kGetValue, kFnCount };
const char* const kFnName[kFnCount] = {"HashName", "FindTable", "FindField", "ExtractBitField", "GetFieldValue(conv)"};
struct Stats {
  std::atomic<uint64_t> calls{0}, mismatches{0}, skipped{0};
} g_stats[kFnCount];

inline int Mode() { return REXCVAR_GET(nhl_l2_t3db); }

// Once a second (whoever notices first) print the running totals.
void MaybeReport() {
  static std::atomic<int64_t> last_ms{0};
  static uint64_t last_total = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t prev = last_ms.load(std::memory_order_relaxed);
  if (now - prev < 1000 || !last_ms.compare_exchange_strong(prev, now)) return;
  uint64_t total = 0;
  for (auto& s : g_stats) total += s.calls.load();
  if (total == last_total) return;
  last_total = total;
  REXLOG_INFO("[nhl-l2] t3db mode={} calls: hash={} findtable={} findfield={} extract={} getvalue={} | mismatches {}/{}/{}/{}/{} | skipped extract={} getvalue={}",
              Mode(), g_stats[kHash].calls.load(), g_stats[kFindTable].calls.load(), g_stats[kFindField].calls.load(),
              g_stats[kExtract].calls.load(), g_stats[kGetValue].calls.load(), g_stats[kHash].mismatches.load(),
              g_stats[kFindTable].mismatches.load(), g_stats[kFindField].mismatches.load(),
              g_stats[kExtract].mismatches.load(), g_stats[kGetValue].mismatches.load(),
              g_stats[kExtract].skipped.load(), g_stats[kGetValue].skipped.load());
}

// Count a call; returns true if this was a mismatch worth logging in detail (first 25 per function).
bool Count(Fn f, bool mismatch) {
  g_stats[f].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeReport();
  if (!mismatch) return false;
  return g_stats[f].mismatches.fetch_add(1, std::memory_order_relaxed) < 25;
}

// The raw 32-bit value of the last numeric ExtractBitField on this thread, for the GetFieldValue conversion check.
thread_local uint32_t tl_last_raw = 0;
thread_local bool tl_last_raw_valid = false;

}  // namespace

// ---- db name hash 0x827B6708 (r3 = C string, returns the hash in r3) ----------------------------------------
REX_EXTERN(__imp__sub_827B6708);
extern "C" REX_FUNC(sub_827B6708) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_827B6708(ctx, base);
  GuestMem mem(base);
  const uint32_t str = ctx.r3.u32;
  const uint32_t mine = HashName(mem, str);
  if (mode == 2) {
    ctx.r3.u64 = mine;
    return;
  }
  __imp__sub_827B6708(ctx, base);
  const bool bad = ctx.r3.u64 != mine;
  if (Count(kHash, bad))
    REXLOG_WARN("[nhl-l2] MISMATCH HashName str={:08X} \"{}\" original={:016X} readable={:08X}", str,
                str ? reinterpret_cast<const char*>(base + str) : "", ctx.r3.u64, mine);
}

// ---- db table lookup 0x827B6FA8 (r3 = db, r4 = hash, r5 = 1 sorted/binary; 0x827B5D40 is the r5=1 wrapper) ------
REX_EXTERN(__imp__sub_827B6FA8);
extern "C" REX_FUNC(sub_827B6FA8) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_827B6FA8(ctx, base);
  GuestMem mem(base);
  const uint32_t db = ctx.r3.u32, hash = ctx.r4.u32, sorted = ctx.r5.u32;
  const uint32_t mine = FindTable(mem, db, hash, sorted);
  if (mode == 2) {
    ctx.r3.u64 = mine;
    return;
  }
  __imp__sub_827B6FA8(ctx, base);
  if (Count(kFindTable, ctx.r3.u64 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH FindTable db={:08X} hash={:08X} sorted={} original={:016X} readable={:08X}", db, hash,
                sorted, ctx.r3.u64, mine);
}

// ---- db field lookup 0x827B6EA0 (r3 = TableDesc, r4 = hash) ---------------------------------------------------
REX_EXTERN(__imp__sub_827B6EA0);
extern "C" REX_FUNC(sub_827B6EA0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_827B6EA0(ctx, base);
  GuestMem mem(base);
  const uint32_t table = ctx.r3.u32, hash = ctx.r4.u32;
  const uint32_t mine = FindField(mem, table, hash);
  if (mode == 2) {
    ctx.r3.u64 = mine;
    return;
  }
  __imp__sub_827B6EA0(ctx, base);
  if (Count(kFindField, ctx.r3.u64 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH FindField table={:08X} hash={:08X} original={:016X} readable={:08X}", table, hash,
                ctx.r3.u64, mine);
}

// ---- db bit-field extractor 0x827A3288 (r3 = ctx, r4 = Column, r5 = row base, r6 = out ptr) ----------------------
// Byte-copy (type 0/1) and special (13..15) columns always go to the original.
REX_EXTERN(__imp__sub_827A3288);
extern "C" REX_FUNC(sub_827A3288) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_827A3288(ctx, base);
  GuestMem mem(base);
  const uint32_t col = ctx.r4.u32, row = ctx.r5.u32, out = ctx.r6.u32;
  uint32_t mine = 0;
  const bool ours = ExtractBitField(mem, col, row, &mine);
  if (mode == 2 && ours) {
    REX_STORE_U32(out, mine);
    tl_last_raw = mine;
    tl_last_raw_valid = true;
    return;  // r3 is unspecified after the original for numeric columns (callers ignore it)
  }
  __imp__sub_827A3288(ctx, base);
  if (!ours) {
    g_stats[kExtract].skipped.fetch_add(1, std::memory_order_relaxed);
    tl_last_raw_valid = false;
    return;
  }
  const uint32_t orig = REX_LOAD_U32(out);
  tl_last_raw = orig;
  tl_last_raw_valid = true;
  if (Count(kExtract, orig != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH ExtractBitField col={:08X} type={} bitoff={} depth={} row={:08X} original={:08X} readable={:08X}",
                col, REX_LOAD_U32(col), REX_LOAD_U32(col + 4), REX_LOAD_U32(col + 12), row, orig, mine);
}

// ---- db value-conversion stage 0x827B3058: value-conversion stage only (verify only, never replaced) ---------
// (r3 = cursor, r4 = TableDesc, r5 = FieldDesc, r6 = type 1 int/2 float/3 string/4 date/5 time, r7 = out).
// The row lookup in the middle (0x8279F3D0/0x8279ECD8) is not readable C++ yet, so the original always runs; the
// raw value it fetched is captured from the ExtractBitField hook and fed to ConvertInt/Date/Time for comparison.
REX_EXTERN(__imp__sub_827B3058);
extern "C" REX_FUNC(sub_827B3058) {
  if (Mode() != 1) return __imp__sub_827B3058(ctx, base);
  GuestMem mem(base);
  const uint32_t fld = ctx.r5.u32, type = ctx.r6.u32, out = ctx.r7.u32;
  tl_last_raw_valid = false;
  __imp__sub_827B3058(ctx, base);
  if (type < 1 || type > 5 || type == 3) return;
  if (!tl_last_raw_valid) {
    g_stats[kGetValue].skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t raw = tl_last_raw;
  tl_last_raw_valid = false;
  const int32_t low = mem.S32(fld + offsetof(FieldDesc, low));
  uint8_t want[4] = {0, 0, 0, 0}, got[4] = {0, 0, 0, 0};
  size_t n = 4;
  if (type == 1 || type == 2) {
    const uint32_t v = type == 1 ? static_cast<uint32_t>(ConvertInt(raw, low)) : raw;
    want[0] = v >> 24; want[1] = v >> 16; want[2] = v >> 8; want[3] = v;
  } else if (type == 4) {
    const Date d = ConvertDate(raw, low);
    want[0] = d.year >> 8; want[1] = uint8_t(d.year); want[2] = d.month; want[3] = d.day;
  } else {
    const Time t = ConvertTime(raw, low);
    want[0] = t.hour; want[1] = t.minute; want[2] = t.second;
    n = 3;
  }
  for (size_t i = 0; i < n; ++i) got[i] = REX_LOAD_U8(out + i);
  if (Count(kGetValue, std::memcmp(want, got, n) != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH GetFieldValue type={} fld={:08X} raw={:08X} low={} original={:02X}{:02X}{:02X}{:02X} readable={:02X}{:02X}{:02X}{:02X}",
                type, fld, raw, low, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
}
