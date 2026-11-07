// L2 hooks for the readable penalty / skaters-on-ice module (penalty.h). Mode via cvar nhl_l2_penalty
// (env REX_NHL_L2_PENALTY):
//   0 off (default, pure passthrough)
//   1 VERIFY: run the readable version first against a shadow overlay (reads real memory, buffers its writes),
//             then the original, then compare every buffered write with the real memory and the return value.
//             Mismatches log "[nhl-l2] MISMATCH ..." (first 25 per site) + a totals line once a second.
//   2 REPLACE: use the readable version only (counts go through the guest accessors, so nhl_skaters_per_side works).
// Verify with nhl_skaters_per_side = 0 (the shadow reads the raw count array, the rule hook clamps getters).
// Docs: docs/l2-penalty.md.
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "penalty.h"

REXCVAR_DEFINE_INT32(nhl_l2_penalty, 0, "NHL", "L2 penalty/skater bookkeeping: 0 off, 1 verify against the original, 2 replace the original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(sub_83381AE8);  // GetSkaters(side)
REX_EXTERN(sub_83381B00);  // SetSkaters(side, value)

namespace {

using namespace nhl::l2::penalty;

enum Fn { kAdd, kRemove, kOtReduce, kGate, kPick, kSync, kFnCount };
const char* const kFnName[kFnCount] = {"AddSkater", "RemoveSkater", "OvertimeReduce", "RuleGate", "PickBestAvailableSlot", "SyncPenaltyPlayerStates"};
struct Stats {
  std::atomic<uint64_t> calls{0}, mismatches{0};
} g_stats[kFnCount];

inline int Mode() { return REXCVAR_GET(nhl_l2_penalty); }

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
  REXLOG_INFO("[nhl-l2] penalty mode={} calls: add={} remove={} otreduce={} gate={} pick={} sync={} | mismatches {}/{}/{}/{}/{}/{}", Mode(),
              g_stats[kAdd].calls.load(), g_stats[kRemove].calls.load(), g_stats[kOtReduce].calls.load(),
              g_stats[kGate].calls.load(), g_stats[kPick].calls.load(), g_stats[kSync].calls.load(),
              g_stats[kAdd].mismatches.load(), g_stats[kRemove].mismatches.load(), g_stats[kOtReduce].mismatches.load(),
              g_stats[kGate].mismatches.load(), g_stats[kPick].mismatches.load(), g_stats[kSync].mismatches.load());
}

// Returns true if this mismatch should be logged in detail (first 25 per function).
bool Count(Fn f, bool mismatch) {
  g_stats[f].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeReport();
  if (!mismatch) return false;
  return g_stats[f].mismatches.fetch_add(1, std::memory_order_relaxed) < 25;
}

// Replace mode: real memory, counts via the guest accessors.
class GuestEnv final : public Env {
 public:
  GuestEnv(PPCContext& c, uint8_t* b) : ctx(c), base(b) {}
  uint8_t U8(uint32_t a) override { return REX_LOAD_U8(a); }
  uint32_t U32(uint32_t a) override { return REX_LOAD_U32(a); }
  void W8(uint32_t a, uint8_t v) override { REX_STORE_U8(a, v); }
  int32_t GetSkaters(uint32_t side) override {
    PPCContext c = ctx;
    c.r3.u64 = side;
    sub_83381AE8(c, base);
    return static_cast<int32_t>(c.r3.u32);
  }
  void SetSkaters(uint32_t side, int32_t v) override {
    PPCContext c = ctx;
    c.r3.u64 = side;
    c.r4.u64 = static_cast<uint32_t>(v);
    sub_83381B00(c, base);
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
  uint32_t U32(uint32_t a) override {
    return uint32_t(U8(a)) << 24 | uint32_t(U8(a + 1)) << 16 | uint32_t(U8(a + 2)) << 8 | U8(a + 3);
  }
  void W8(uint32_t a, uint8_t v) override { w_.push_back({a, v}); }
  void SetSkaters(uint32_t side, int32_t v) override {
    const uint32_t a = kSkaterCount + 4 * side, u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) w_.push_back({a + i, uint8_t(u >> (24 - 8 * i))});
  }
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
  size_t writes() const { return w_.size(); }
  uint8_t* base;

 private:
  struct W { uint32_t addr; uint8_t v; };
  std::vector<W> w_;
};

}  // namespace

// ---- AddSkater 0x8330BEA0 (r3 = side) ------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8330BEA0);
extern "C" REX_FUNC(sub_8330BEA0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_8330BEA0(ctx, base);
  const uint32_t side = ctx.r3.u32;
  if (mode == 2) {
    GuestEnv env(ctx, base);
    AddSkater(env, side);
    return;
  }
  ShadowEnv sh(base);
  AddSkater(sh, side);
  __imp__sub_8330BEA0(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kAdd, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH AddSkater side={} addr={:08X} original={:02X} readable={:02X}", side, bad, got, want);
}

// ---- RemoveSkater 0x83311140 (r3 = side) ---------------------------------------------------------------------
REX_EXTERN(__imp__sub_83311140);
extern "C" REX_FUNC(sub_83311140) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83311140(ctx, base);
  const uint32_t side = ctx.r3.u32;
  if (mode == 2) {
    GuestEnv env(ctx, base);
    RemoveSkater(env, side);
    return;
  }
  ShadowEnv sh(base);
  const int queued = RemoveSkater(sh, side);
  __imp__sub_83311140(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kRemove, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH RemoveSkater side={} queued_idx={} addr={:08X} original={:02X} readable={:02X}", side,
                queued, bad, got, want);
}

// ---- OvertimeReduce 0x8331B508 (no args) ---------------------------------------------------------------------
REX_EXTERN(__imp__sub_8331B508);
extern "C" REX_FUNC(sub_8331B508) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_8331B508(ctx, base);
  if (mode == 2) {
    GuestEnv env(ctx, base);
    OvertimeReduce(env);
    return;
  }
  ShadowEnv sh(base);
  const bool reduced = OvertimeReduce(sh);
  __imp__sub_8331B508(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kOtReduce, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH OvertimeReduce reduced={} addr={:08X} original={:02X} readable={:02X}", reduced, bad,
                got, want);
}

// ---- RuleGate 0x833159B0 (no args, returns bool in r3) -------------------------------------------------------
REX_EXTERN(__imp__sub_833159B0);
extern "C" REX_FUNC(sub_833159B0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_833159B0(ctx, base);
  GuestEnv env(ctx, base);
  const bool mine = RuleGate(env);
  if (mode == 2) {
    ctx.r3.u64 = mine ? 1 : 0;
    return;
  }
  __imp__sub_833159B0(ctx, base);
  if (Count(kGate, ctx.r3.u64 != (mine ? 1u : 0u)))
    REXLOG_WARN("[nhl-l2] MISMATCH RuleGate original={} readable={}", ctx.r3.u64, mine ? 1 : 0);
}

// ---- PickBestAvailableSlot 0x83310718 (r3 = side, r4 = exclude, returns slot or -1 in r3; writes nothing) ----------
REX_EXTERN(__imp__sub_83310718);
extern "C" REX_FUNC(sub_83310718) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_83310718(ctx, base);
  const int32_t side = ctx.r3.s32, exclude = ctx.r4.s32;
  GuestEnv env(ctx, base);
  const int32_t mine = PickBestAvailableSlot(env, side, exclude);
  if (mode == 2) {
    ctx.r3.s64 = mine;
    return;
  }
  __imp__sub_83310718(ctx, base);
  if (Count(kPick, ctx.r3.s32 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH PickBestAvailableSlot side={} exclude={} original={} readable={}", side, exclude,
                ctx.r3.s32, mine);
}

// ---- SyncPenaltyPlayerStates 0x833104D0 (no args; writes player-object state words) ---------------------------
REX_EXTERN(__imp__sub_833104D0);
extern "C" REX_FUNC(sub_833104D0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_833104D0(ctx, base);
  if (mode == 2) {
    GuestEnv env(ctx, base);
    SyncPenaltyPlayerStates(env);
    return;
  }
  ShadowEnv sh(base);
  SyncPenaltyPlayerStates(sh);
  __imp__sub_833104D0(ctx, base);
  uint32_t want = 0, got = 0;
  const uint32_t bad = sh.FirstDiff(&want, &got);
  if (Count(kSync, bad != 0))
    REXLOG_WARN("[nhl-l2] MISMATCH SyncPenaltyPlayerStates addr={:08X} original={:02X} readable={:02X}", bad, got, want);
}
