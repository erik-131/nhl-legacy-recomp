// L2 hooks for the readable period-length / clock / rule-type module (rules.h). Mode via cvar nhl_l2_rules
// (env REX_NHL_L2_RULES):
//   0 off (default, pure passthrough)
//   1 VERIFY: run the readable version first against a shadow overlay (reads real memory, buffers its writes), then the
//             original, then compare every buffered write with the real memory and the return value. Mismatches log
//             "[nhl-l2] MISMATCH ..." (first 25 per site) + a totals line once a second.
//   2 REPLACE: use the readable version only.
// Docs: docs/l2-rules.md. Independent of nhl_period_length (rules_hooks.cpp), which writes the rules struct and so is
// seen by both the original and the readable code.
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "rules.h"

REXCVAR_DEFINE_INT32(nhl_l2_rules, 0, "NHL", "L2 period length/clock/rule-type functions: 0 off, 1 verify against the original, 2 replace the original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(sub_82C366E0);  // OT-entered notifier called by SetPeriodIndex
REX_EXTERN(sub_83480C10);  // clock rate (not modelled)
REX_EXTERN(sub_834859A8);  // rate >= 21

namespace {

using namespace nhl::l2::rules;

enum Fn { kActive, kPeriodLen, kInitClock, kSetClock, kSetPeriod, kBelowStart, kElapsed, kScale, kRuleType, kFnCount };
const char* const kFnName[kFnCount] = {"ActiveFlag", "PeriodLength", "InitPeriodClock", "SetClock", "SetPeriodIndex",
                                       "ClockBelowPeriodStart", "ElapsedGameSeconds", "ScaleByPeriodLength", "RuleTypeIs*"};
struct Stats {
  std::atomic<uint64_t> calls{0}, mismatches{0};
} g_stats[kFnCount];

inline int Mode() { return REXCVAR_GET(nhl_l2_rules); }

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
  std::string line;
  for (int i = 0; i < kFnCount; ++i)
    line += fmt::format(" {}={}/{}", kFnName[i], g_stats[i].calls.load(), g_stats[i].mismatches.load());
  REXLOG_INFO("[nhl-l2] rules mode={} calls/mismatches:{}", Mode(), line);
}

// Returns true if this mismatch should be logged in detail (first 25 per function).
bool Count(Fn f, bool mismatch) {
  g_stats[f].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeReport();
  if (!mismatch) return false;
  return g_stats[f].mismatches.fetch_add(1, std::memory_order_relaxed) < 25;
}

// Replace mode: real memory; the OT notifier and the clock rate go through the original guest functions.
class GuestEnv final : public Env {
 public:
  GuestEnv(PPCContext& c, uint8_t* b) : ctx(c), base(b) {}
  uint8_t U8(uint32_t a) override { return REX_LOAD_U8(a); }
  void W8(uint32_t a, uint8_t v) override { REX_STORE_U8(a, v); }
  void OvertimePeriodEntered() override {
    PPCContext c = ctx;
    c.r3.u64 = REX_LOAD_U32(kOtNoticeObjPtr);
    c.r4.u64 = 4;
    c.r5.u64 = 1;
    sub_82C366E0(c, base);
  }
  int32_t ClockRate() override {
    PPCContext c = ctx;
    sub_83480C10(c, base);
    return static_cast<int32_t>(c.r3.u32);
  }
  PPCContext& ctx;
  uint8_t* base;
};

// Verify mode: reads fall through to real memory unless the readable code wrote there first; writes are only recorded.
class ShadowEnv final : public Env {
 public:
  explicit ShadowEnv(uint8_t* b) : base(b) {}
  uint8_t U8(uint32_t a) override {
    for (auto it = w_.rbegin(); it != w_.rend(); ++it)
      if (it->addr == a) return it->v;
    return REX_LOAD_U8(a);
  }
  void W8(uint32_t a, uint8_t v) override { w_.push_back({a, v}); }
  // Compares each buffered write with the real memory (the last write per address wins). Returns the first bad address or 0.
  uint32_t FirstDiff(uint32_t* want, uint32_t* got) const {
    for (size_t i = 0; i < w_.size(); ++i) {
      bool last = true;
      for (size_t j = i + 1; j < w_.size(); ++j) last &= w_[j].addr != w_[i].addr;
      if (!last) continue;
      const uint8_t real = REX_LOAD_U8(w_[i].addr);
      if (real != w_[i].v) { *want = w_[i].v; *got = real; return w_[i].addr; }
    }
    return 0;
  }
  uint8_t* base;

 private:
  struct W { uint32_t addr; uint8_t v; };
  std::vector<W> w_;
};

}  // namespace

// ---- ActiveFlag 0x8330C540 (no args, returns the u32 in r3) --------------------------------------------------------
REX_EXTERN(__imp__sub_8330C540);
extern "C" REX_FUNC(sub_8330C540) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_8330C540(ctx, base);
  GuestEnv env(ctx, base);
  const uint32_t mine = ActiveFlag(env);
  if (mode == 2) {
    ctx.r3.u64 = mine;
    return;
  }
  __imp__sub_8330C540(ctx, base);
  if (Count(kActive, ctx.r3.u32 != mine)) REXLOG_WARN("[nhl-l2] MISMATCH ActiveFlag original={:08X} readable={:08X}", ctx.r3.u32, mine);
}

// ---- PeriodLength 0x83475D50 (r3 = kind as s16, returns s16 sign-extended in r3) -----------------------------------
REX_EXTERN(__imp__sub_83475D50);
extern "C" REX_FUNC(sub_83475D50) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83475D50(ctx, base);
  const int16_t kind = static_cast<int16_t>(ctx.r3.u32);
  GuestEnv env(ctx, base);
  const int16_t mine = PeriodLength(env, kind);
  if (mode == 2) {
    ctx.r3.s64 = mine;
    return;
  }
  __imp__sub_83475D50(ctx, base);
  if (Count(kPeriodLen, ctx.r3.s16 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH PeriodLength kind={} original={} readable={}", kind, ctx.r3.s16, mine);
}

// ---- InitPeriodClock 0x83480A40 (arg ignored; writes the two clock words) ----------------------------------------
REX_EXTERN(__imp__sub_83480A40);
extern "C" REX_FUNC(sub_83480A40) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83480A40(ctx, base);
  if (mode == 2) {
    GuestEnv env(ctx, base);
    InitPeriodClock(env);
    return;
  }
  ShadowEnv sh(base);
  InitPeriodClock(sh);
  __imp__sub_83480A40(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kInitClock, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH InitPeriodClock addr={:08X} original={:02X} readable={:02X}", bad, got, want);
}

// ---- SetClock 0x833FB6A0 (r3 = seconds, r4 = sub-tick) ---------------------------------------------------------------
REX_EXTERN(__imp__sub_833FB6A0);
extern "C" REX_FUNC(sub_833FB6A0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_833FB6A0(ctx, base);
  const uint16_t sec = static_cast<uint16_t>(ctx.r3.u32), sub = static_cast<uint16_t>(ctx.r4.u32);
  if (mode == 2) {
    GuestEnv env(ctx, base);
    SetClock(env, sec, sub);
    return;
  }
  ShadowEnv sh(base);
  SetClock(sh, sec, sub);
  __imp__sub_833FB6A0(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kSetClock, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH SetClock addr={:08X} original={:02X} readable={:02X}", bad, got, want);
}

// ---- SetPeriodIndex 0x833FB500 (r3 = block address, r4 = index; NextPeriod 0x833FB558 and the init wrapper reach it) ----
REX_EXTERN(__imp__sub_833FB500);
extern "C" REX_FUNC(sub_833FB500) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_833FB500(ctx, base);
  const uint32_t blk = ctx.r3.u32;
  const int32_t idx = ctx.r4.s32;
  if (mode == 2) {
    GuestEnv env(ctx, base);
    SetPeriodIndex(env, blk, idx);
    return;
  }
  ShadowEnv sh(base);
  SetPeriodIndex(sh, blk, idx);
  __imp__sub_833FB500(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kSetPeriod, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH SetPeriodIndex idx={} addr={:08X} original={:02X} readable={:02X}", idx, bad, got, want);
}

// ---- ClockBelowPeriodStart 0x83480A88 (no args, bool in r3) -----------------------------------------------------------
REX_EXTERN(__imp__sub_83480A88);
extern "C" REX_FUNC(sub_83480A88) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83480A88(ctx, base);
  GuestEnv env(ctx, base);
  const bool mine = ClockBelowPeriodStart(env);
  if (mode == 2) {
    ctx.r3.u64 = mine ? 1 : 0;
    return;
  }
  __imp__sub_83480A88(ctx, base);
  if (Count(kBelowStart, ctx.r3.u32 != (mine ? 1u : 0u)))
    REXLOG_WARN("[nhl-l2] MISMATCH ClockBelowPeriodStart original={} readable={}", ctx.r3.u32, mine ? 1 : 0);
}

// ---- ElapsedGameSeconds 0x83475E08 (arg ignored, int32 in r3) ------------------------------------------------------------
REX_EXTERN(__imp__sub_83475E08);
extern "C" REX_FUNC(sub_83475E08) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83475E08(ctx, base);
  GuestEnv env(ctx, base);
  const int32_t mine = ElapsedGameSeconds(env);
  if (mode == 2) {
    ctx.r3.s64 = mine;
    return;
  }
  __imp__sub_83475E08(ctx, base);
  if (Count(kElapsed, ctx.r3.s32 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH ElapsedGameSeconds original={} readable={}", ctx.r3.s32, mine);
}

// ---- ScaleByPeriodLength 0x8337FF90 (r3 = x, int32 in r3). The rate>=21 flag comes from the original 0x834859A8. ----
REX_EXTERN(__imp__sub_8337FF90);
extern "C" REX_FUNC(sub_8337FF90) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_8337FF90(ctx, base);
  const int32_t x = ctx.r3.s32;
  PPCContext c = ctx;
  c.r3.u64 = REX_LOAD_U32(kClockObjPtr);
  sub_834859A8(c, base);
  const bool above = (c.r3.u32 & 0xFF) != 0;
  GuestEnv env(ctx, base);
  const int32_t mine = ScaleByPeriodLength(env, x, above);
  if (mode == 2) {
    ctx.r3.s64 = mine;
    return;
  }
  __imp__sub_8337FF90(ctx, base);
  if (Count(kScale, ctx.r3.s32 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH ScaleByPeriodLength x={} original={} readable={}", x, ctx.r3.s32, mine);
}

// ---- RuleTypeIs predicates (no args, bool in r3): 0x834DC5E8 =5, 620 =0, 658 =1, 690 in {0,1}, 6D8 =4, 710 =2, 748 =3 ----
#define NHL_RULE_TYPE_HOOK(ADDR, EXPR)                                         \
  REX_EXTERN(__imp__sub_##ADDR);                                               \
  extern "C" REX_FUNC(sub_##ADDR) {                                            \
    const int mode = Mode();                                                   \
    if (mode == 0) return __imp__sub_##ADDR(ctx, base);                        \
    GuestEnv env(ctx, base);                                                   \
    const bool mine = (EXPR);                                                  \
    if (mode == 2) {                                                           \
      ctx.r3.u64 = mine ? 1 : 0;                                               \
      return;                                                                  \
    }                                                                          \
    __imp__sub_##ADDR(ctx, base);                                              \
    if (Count(kRuleType, (ctx.r3.u32 & 0xFF) != (mine ? 1u : 0u)))             \
      REXLOG_WARN("[nhl-l2] MISMATCH RuleTypeIs " #ADDR " original={} readable={}", ctx.r3.u32 & 0xFF, mine ? 1 : 0); \
  }
NHL_RULE_TYPE_HOOK(834DC5E8, RuleTypeIs(env, 5))
NHL_RULE_TYPE_HOOK(834DC620, RuleTypeIs(env, 0))
NHL_RULE_TYPE_HOOK(834DC658, RuleTypeIs(env, 1))
NHL_RULE_TYPE_HOOK(834DC690, RuleTypeIs01(env))
NHL_RULE_TYPE_HOOK(834DC6D8, RuleTypeIs(env, 4))
NHL_RULE_TYPE_HOOK(834DC710, RuleTypeIs(env, 2))
NHL_RULE_TYPE_HOOK(834DC748, RuleTypeIs(env, 3))
