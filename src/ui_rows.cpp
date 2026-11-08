// Extra rows in the game's own Game Settings screens (opt-in, default off). Roles, mechanism and findings: docs/ui-rows.md.
//   nhl_ui_rows  comma list: "skaters" = row "Skaters: 5 on 5 / 4 on 4 / 3 on 3" on the pages that show the rules,
//                "lines" = row "Line Changes: Auto / Manual" (cvar nhl_manual_lines, rules_hooks.cpp),
//                "log" = log the calls the settings screens make into the native settings object (research aid),
//                "all" = every row
// Env: REX_NHL_UI_ROWS=skaters
//
// How a row gets in (no file is changed, everything is guest memory written after boot):
//  1. The settings screens are a generic renderer. Rows are records of a static table (label key, control type, max,
//     default, count, pointer to an array of choice-label keys) and the pages are arrays of record indices, reached
//     through a runtime table (pointer, count) per page.
//  2. A spare record of the table is overwritten with the new row (its index is the row's setting number), the key
//     strings and the choice array are written to a spare page of guest memory, and every page array that holds the
//     rules rows gets a copy with the new index appended (the runtime table entry is repointed).
//  3. The native getter and setter of a setting's value are hand-written switches; calls for the new record are
//     answered here and never reach them.
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(nhl_ui_rows, "", "NHL", "Extra rows in the game's Game Settings screens: comma list of skaters, log, all");
REXCVAR_DECLARE(int32_t, nhl_skaters_per_side);
REXCVAR_DECLARE(bool, nhl_manual_lines);

namespace {

bool Has(const char* word) {
  const std::string s = REXCVAR_GET(nhl_ui_rows);
  if (s.empty()) return false;
  if (s.find("all") != std::string::npos && std::strcmp(word, "log") != 0) return true;
  return s.find(word) != std::string::npos;
}

using namespace rex::memory;
constexpr uint32_t kTable = 0x82050FB0;       // setting table: 32-byte records
// Runtime tables of {pointer to index array, count}, 8 bytes per entry: the settings of a page (indexed by page) and the
// settings of a sub-page (indexed by page * 47 + sub). The first one is the table for the in-game rules page.
struct PageTable { uint32_t at; int n; };
constexpr PageTable kTables[] = {{0x83BD65A0, 24}, {0x83BD3E50, 47 * 12}};
constexpr uint32_t kSpareSize = 4096;         // one block of the guest system heap (the image padding is read-only)

// Spare memory: bump allocator over one system-heap block.
uint32_t g_bump = 0, g_end = 0;
uint32_t Alloc(uint32_t n) {
  if (!g_bump) {
    auto* ks = rex::runtime::current_kernel_state();
    if (!ks || !ks->memory()) return 0;
    g_bump = ks->memory()->SystemHeapAlloc(kSpareSize);
    g_end = g_bump + kSpareSize;
    REXLOG_INFO("[nhl-uirows] spare block at {:08X}", g_bump);
  }
  if (!g_bump) return 0;
  n = (n + 3) & ~3u;
  if (g_bump + n > g_end) return 0;
  const uint32_t at = g_bump;
  g_bump += n;
  return at;
}
uint32_t PutString(uint8_t* base, const char* s) {
  const uint32_t n = uint32_t(std::strlen(s)) + 1;
  const uint32_t at = Alloc(n);
  if (!at) return 0;
  for (uint32_t i = 0; i < n; ++i) REX_STORE_U8(at + i, uint8_t(s[i]));
  return at;
}

// A row: token in nhl_ui_rows, a hidden host record (name 0, shown by no page), label key, choice-label keys, and the
// value access. Choice index 0 is the default.
struct Row {
  const char* token;
  uint32_t record;
  const char* label;
  const char* label_long;  // used with the token "<token>long" (needs a loc mod), may be null
  std::vector<const char*> choices;
  int (*get)();
  void (*set)(int);
  bool done = false;
};

int SkatersGet() {
  const int n = REXCVAR_GET(nhl_skaters_per_side);
  return n == 4 ? 1 : n == 3 ? 2 : 0;
}
void SkatersSet(int c) {
  REXCVAR_SET(nhl_skaters_per_side, c == 1 ? 4 : c == 2 ? 3 : 0);
  REXLOG_INFO("[nhl-uirows] skaters per side -> {} (choice {})", c == 1 ? 4 : c == 2 ? 3 : 5, c);
}
int LinesGet() { return REXCVAR_GET(nhl_manual_lines) ? 1 : 0; }
void LinesSet(int c) {
  REXCVAR_SET(nhl_manual_lines, c == 1);
  REXLOG_INFO("[nhl-uirows] manual line changes -> {}", c == 1);
}

std::vector<Row>& Rows() {
  static std::vector<Row> rows = {
      {"skaters", 126, "Skaters", "TXT_SKATERS_PER_SIDE", {"5on5", "4on4", "3on3"}, SkatersGet, SkatersSet},
      {"lines", 127, "LineChanges", nullptr, {"Automatic_abbr", "Manual"}, LinesGet, LinesSet},
  };
  return rows;
}
Row* RowByRecord(uint32_t idx) {
  if (idx < 126 || idx > 127) return nullptr;
  for (auto& r : Rows())
    if (r.record == idx && r.done && Has(r.token)) return &r;
  return nullptr;
}

std::mutex g_mu;
std::map<uint64_t, uint32_t> g_patched;  // (original page array, rows added) -> extended copy

// The table sits in the executable's data image, which may be mapped read-only: make its pages writable.
void MakeWritable(uint32_t addr, uint32_t len) {
  auto* ks = rex::runtime::current_kernel_state();
  if (!ks || !ks->memory()) return;
  auto* heap = ks->memory()->LookupHeap(addr);
  if (!heap) {
    REXLOG_WARN("[nhl-uirows] no heap for {:08X}", addr);
    return;
  }
  uint32_t old = 0;
  const bool ok = heap->Protect(addr & ~0xFFFu, (len + 0x1FFFu) & ~0xFFFu, kMemoryProtectRead | kMemoryProtectWrite, &old);
  REXLOG_INFO("[nhl-uirows] make {:08X}+{:X} writable: {} (was {:X})", addr, len, ok, old);
}

// Writes a row's record and strings (once).
bool EnsureRow(uint8_t* base, Row& r) {
  if (r.done) return true;
  MakeWritable(kTable + 32 * r.record, 32);
  const bool lng = r.label_long && Has((std::string(r.token) + "long").c_str());
  const uint32_t name = PutString(base, lng ? r.label_long : r.label);
  const uint32_t list = Alloc(4 * uint32_t(r.choices.size()));
  if (!name || !list) return false;
  for (size_t i = 0; i < r.choices.size(); ++i) {
    const uint32_t k = PutString(base, r.choices[i]);
    if (!k) return false;
    REX_STORE_U32(list + 4 * uint32_t(i), k);
  }
  const uint32_t rec = kTable + 32 * r.record, n = uint32_t(r.choices.size());
  // {id (kept), name key, type 3 = choice list, min, max index, default index, count, choice keys}
  REX_STORE_U32(rec + 4, name);
  REX_STORE_U32(rec + 8, 3);
  REX_STORE_U32(rec + 12, 0);
  REX_STORE_U32(rec + 16, n - 1);
  REX_STORE_U32(rec + 20, 0);
  REX_STORE_U32(rec + 24, n);
  REX_STORE_U32(rec + 28, list);
  r.done = true;
  REXLOG_INFO("[nhl-uirows] row {} record {} written (name {:08X}, list {:08X}), initial choice {}", r.token, r.record, name,
              list, r.get());
  return true;
}

void DumpPages(uint8_t* base) {
  static bool done = false;
  if (done) return;
  done = true;
  for (const auto& t : kTables)
    for (int c = 0; c < t.n; ++c) {
      const uint32_t p = REX_LOAD_U32(t.at + 8 * c), n = REX_LOAD_U32(t.at + 8 * c + 4);
      if (!p && !n) continue;
      std::string a;
      if (p >= 0x82000000 && p < 0x84000000 && n < 40)
        for (uint32_t i = 0; i < n; ++i) a += std::to_string(int32_t(REX_LOAD_U32(p + 4 * i))) + " ";
      REXLOG_INFO("[nhl-uirows] table {:08X} page {} ptr {:08X} count {}: {}", t.at, c, p, n, a);
    }
}

// Which pages get a row: the page's index array must hold all of "need" (a page can match by any of its alternatives).
// skaters: the rules pages (Offsides and Penalties) and the pre-game Quick Settings (period length, rules, goalie camera);
// lines: the pre-game Advanced Settings (home/away line changes and shot aim, tuner version).
bool PageWants(const Row& r, const uint32_t* v, uint32_t n) {
  auto has = [&](uint32_t x) {
    for (uint32_t i = 0; i < n; ++i)
      if (v[i] == x) return true;
    return false;
  };
  if (has(r.record)) return false;
  if (!std::strcmp(r.token, "skaters")) return (has(11) && has(17)) || (has(3) && has(2) && has(146) && has(197));
  if (!std::strcmp(r.token, "lines")) return has(7) && has(8) && has(9) && has(10) && has(152);
  return false;
}

// Appends the enabled new rows to the pages that want them (runtime page tables: entry repointed to a copy).
void PatchPages(uint8_t* base) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (Has("log")) DumpPages(base);
  std::vector<Row*> on;
  for (auto& r : Rows())
    if (Has(r.token) && EnsureRow(base, r)) on.push_back(&r);
  if (on.empty()) return;
  for (const auto& t : kTables)
    for (int c = 0; c < t.n; ++c) {
      const uint32_t at = t.at + 8 * c;
      const uint32_t p = REX_LOAD_U32(at), n = REX_LOAD_U32(at + 4);
      if (p < 0x82000000 || p >= 0x84000000 || n < 1 || n > 24) continue;
      uint32_t v[24];
      for (uint32_t i = 0; i < n; ++i) v[i] = REX_LOAD_U32(p + 4 * i);
      std::vector<uint32_t> add;
      for (Row* r : on)
        if (PageWants(*r, v, n)) add.push_back(r->record);
      if (add.empty()) continue;
      const uint32_t m = n + uint32_t(add.size());
      uint32_t np;
      const uint64_t key = (uint64_t(p) << 32) | add[0] << 8 | uint32_t(add.size());
      if (auto it = g_patched.find(key); it != g_patched.end()) {
        np = it->second;
      } else {
        np = Alloc(4 * m);
        if (!np) return;
        for (uint32_t i = 0; i < n; ++i) REX_STORE_U32(np + 4 * i, v[i]);
        for (size_t i = 0; i < add.size(); ++i) REX_STORE_U32(np + 4 * (n + uint32_t(i)), add[i]);
        g_patched[key] = np;
      }
      REX_STORE_U32(at, np);
      REX_STORE_U32(at + 4, m);
      REXLOG_INFO("[nhl-uirows] table {:08X} page {} ({} rows at {:08X}) -> {} rows at {:08X}", t.at, c, n, p, m, np);
    }
}

void LogCall(uint32_t fn, PPCContext& ctx, uint8_t* base) {
  static std::mutex mu;
  static std::map<uint32_t, int> seen;
  {
    std::lock_guard<std::mutex> lk(mu);
    if (++seen[fn] > 40) return;
  }
  const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  const int32_t id5 = r5 < 230 ? int32_t(REX_LOAD_U32(kTable + 32 * r5)) : -1;
  REXLOG_INFO("[nhl-uirows] call {:08X} r3={:08X} r4={:X} r5={:X}(id {}) r6={:08X} [+0 {:X} +4 {:X} +128 {:X}] r7={:X}", fn,
              ctx.r3.u32, r4, r5, id5, r6, r6 > 0x10000 ? REX_LOAD_U32(r6) : 0, r6 > 0x10000 ? REX_LOAD_U32(r6 + 4) : 0,
              r6 > 0x10000 ? REX_LOAD_U32(r6 + 128) : 0, ctx.r7.u32);
}

}  // namespace

#define UIROWS_HOOK(ADDR, PRE)                                         \
  REX_EXTERN(__imp__sub_##ADDR);                                       \
  extern "C" REX_FUNC(sub_##ADDR) {                                    \
    PRE                                                                \
    if (Has("log")) {                                                  \
      LogCall(0x##ADDR, ctx, base);                                    \
      const uint32_t r6 = ctx.r6.u32;                                  \
      __imp__sub_##ADDR(ctx, base);                                    \
      static std::atomic<int> k{0};                                    \
      if (k.fetch_add(1) < 400)                                        \
        REXLOG_INFO("[nhl-uirows] ret  {:08X} r3={:08X} out0={:X} out1={:X}", 0x##ADDR##u, ctx.r3.u32, \
                    r6 > 0x10000 ? REX_LOAD_U32(r6) : 0, r6 > 0x10000 ? REX_LOAD_U32(r6 + 4) : 0); \
      return;                                                          \
    }                                                                  \
    __imp__sub_##ADDR(ctx, base);                                      \
  }

#define NOPRE
// Page queries (category ids / settings of a page): add the row to the pages first.
#define PAGES_PRE if (Has("skaters")) PatchPages(base);

// GetCurrentSettingValue(this, ?, index, out{0, value}) -> value. A new record answers itself.
REX_EXTERN(__imp__sub_82A01C18);
extern "C" REX_FUNC(sub_82A01C18) {
  if (Row* r = RowByRecord(ctx.r5.u32)) {
    const uint32_t v = uint32_t(r->get());
    REX_STORE_U32(ctx.r6.u32, 0);
    REX_STORE_U32(ctx.r6.u32 + 4, v);
    ctx.r3.u64 = v;
    return;
  }
  if (Has("log")) LogCall(0x82A01C18, ctx, base);
  __imp__sub_82A01C18(ctx, base);
}

// SetCurrentSettingValue(this, ?, index, value object with the new choice at +128).
REX_EXTERN(__imp__sub_829FE960);
extern "C" REX_FUNC(sub_829FE960) {
  if (Row* r = RowByRecord(ctx.r5.u32)) {
    const uint32_t v = ctx.r6.u32 > 0x10000 ? REX_LOAD_U32(ctx.r6.u32 + 128) : 0;
    REXLOG_INFO("[nhl-uirows] set row {}: value {}", r->token, v);
    if (v < r->choices.size()) r->set(int(v));
    return;
  }
  if (Has("log")) LogCall(0x829FE960, ctx, base);
  __imp__sub_829FE960(ctx, base);
}

UIROWS_HOOK(82A05390, PAGES_PRE)  // GetCategoryIDs(page, out)
UIROWS_HOOK(82A054A8, PAGES_PRE)  // page queries
UIROWS_HOOK(82A05580, PAGES_PRE)
UIROWS_HOOK(82A01438, NOPRE)
UIROWS_HOOK(82A04B70, NOPRE)
UIROWS_HOOK(82A06160, NOPRE)
UIROWS_HOOK(82A02F80, NOPRE)
UIROWS_HOOK(82A001D8, NOPRE)
UIROWS_HOOK(82A00248, NOPRE)
UIROWS_HOOK(82A055F0, NOPRE)
UIROWS_HOOK(829F4008, NOPRE)
UIROWS_HOOK(82A05750, NOPRE)
UIROWS_HOOK(829F4080, NOPRE)
UIROWS_HOOK(82A03200, NOPRE)
UIROWS_HOOK(82A027A0, NOPRE)
UIROWS_HOOK(829FEA58, NOPRE)
UIROWS_HOOK(829FDEE8, NOPRE)
UIROWS_HOOK(829F4150, NOPRE)
