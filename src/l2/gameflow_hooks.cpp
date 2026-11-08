// L2 hooks for the readable period-end game-flow module (gameflow.h). Mode via cvar nhl_l2_gameflow
// (env REX_NHL_L2_GAMEFLOW):
//   0 off (default, pure passthrough)
//   1 VERIFY: both functions are pure, so the readable version runs on real memory, then the original runs, and the return
//             values (r3) are compared. Mismatches log "[nhl-l2] MISMATCH ..." (first 25 per site) + a totals line once a
//             second. EvalPeriodEnd kinds 238 (diagnostic UI events) and 255 (ShootoutDecision has side effects) are only
//             counted as "skipped", never compared.
//   2 REPLACE: use the readable version (kind 238 still goes to the original; kind 255 calls the original ShootoutDecision).
// Custom game-end layer (default OFF, only honoured in mode 2): cvar nhl_game_end_mode (env REX_NHL_GAME_END_MODE)
//   0 stock, 1 first to N goals (nhl_game_end_goals, env REX_NHL_GAME_END_GOALS) ends the game, 2 no overtime / a tie after
//   regulation is a draw, 3 a tie after regulation goes straight to a shootout.
// Docs: docs/l2-gameflow.md.
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "gameflow.h"

REXCVAR_DEFINE_INT32(nhl_l2_gameflow, 0, "NHL", "L2 period-end game flow / shootout rounds: 0 off, 1 verify against the original, 2 replace the original")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(nhl_game_end_mode, 0, "NHL", "Custom game end (needs nhl_l2_gameflow=2): 0 stock, 1 first to N goals, 2 no overtime (draw), 3 shootout after regulation")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(nhl_game_end_goals, 5, "NHL", "N for nhl_game_end_mode=1 (first side to N goals wins)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(sub_833FAD58);  // goals of a side (r3 = 0/1)
REX_EXTERN(sub_8331DED0);  // ShootoutDecision

namespace {

using namespace nhl::l2::gameflow;

enum Fn { kEval, kRounds, kFnCount };
const char* const kFnName[kFnCount] = {"EvalPeriodEnd", "ShootoutRounds"};
struct Stats {
  std::atomic<uint64_t> calls{0}, mismatches{0}, skipped{0};
} g_stats[kFnCount];

inline int Mode() { return REXCVAR_GET(nhl_l2_gameflow); }

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
    line += fmt::format(" {}={}/{}/{}", kFnName[i], g_stats[i].calls.load(), g_stats[i].mismatches.load(), g_stats[i].skipped.load());
  REXLOG_INFO("[nhl-l2] gameflow mode={} calls/mismatches/skipped:{}", Mode(), line);
}

// Returns true if this mismatch should be logged in detail (first 25 per function).
bool Count(Fn f, bool mismatch) {
  g_stats[f].calls.fetch_add(1, std::memory_order_relaxed);
  MaybeReport();
  if (!mismatch) return false;
  return g_stats[f].mismatches.fetch_add(1, std::memory_order_relaxed) < 25;
}

// Real memory; goals and the shootout decision go through the original guest functions (on a copy of the context).
class GuestEnv final : public FlowEnv {
 public:
  GuestEnv(PPCContext& c, uint8_t* b) : ctx(c), base(b) {}
  uint8_t U8(uint32_t a) override { return REX_LOAD_U8(a); }
  void W8(uint32_t a, uint8_t v) override { REX_STORE_U8(a, v); }
  int32_t Goals(int side) override {
    PPCContext c = ctx;
    c.r3.u64 = uint32_t(side);
    sub_833FAD58(c, base);
    return c.r3.s32;
  }
  int32_t ShootoutDecision() override {
    PPCContext c = ctx;
    sub_8331DED0(c, base);
    return c.r3.s32;
  }
  PPCContext& ctx;
  uint8_t* base;
};

}  // namespace

// ---- EvalPeriodEnd 0x8331DFE8 (no args, outcome 0..3 in r3) ------------------------------------------------------
REX_EXTERN(__imp__sub_8331DFE8);
extern "C" REX_FUNC(sub_8331DFE8) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_8331DFE8(ctx, base);
  const int32_t kind = int32_t(REX_LOAD_U32(nhl::l2::rules::kPeriodKind));
  const bool skip = kind == nhl::l2::rules::kKindNone || kind == kKindShootout;
  if (mode == 2) {
    if (kind == nhl::l2::rules::kKindNone) return __imp__sub_8331DFE8(ctx, base);
    GuestEnv env(ctx, base);
    EndOverride ov;
    ov.mode = static_cast<EndMode>(REXCVAR_GET(nhl_game_end_mode));
    ov.goals_n = REXCVAR_GET(nhl_game_end_goals);
    ctx.r3.s64 = EvalPeriodEnd(env, ov.mode == EndMode::kStock ? nullptr : &ov);
    return;
  }
  if (skip) {  // verify: not comparable without double side effects
    g_stats[kEval].skipped.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8331DFE8(ctx, base);
    Count(kEval, false);
    return;
  }
  GuestEnv env(ctx, base);
  const int32_t mine = EvalPeriodEnd(env);
  __imp__sub_8331DFE8(ctx, base);
  if (Count(kEval, ctx.r3.s32 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH EvalPeriodEnd kind={} original={} readable={}", kind, ctx.r3.s32, mine);
}

// ---- ShootoutRounds 0x834039E0 (no args, count in r3) --------------------------------------------------------------
REX_EXTERN(__imp__sub_834039E0);
extern "C" REX_FUNC(sub_834039E0) {
  const int mode = Mode();
  if (mode == 0) return __imp__sub_834039E0(ctx, base);
  GuestEnv env(ctx, base);
  const int32_t mine = ShootoutRounds(env);
  if (mode == 2) {
    ctx.r3.s64 = mine;
    return;
  }
  __imp__sub_834039E0(ctx, base);
  if (Count(kRounds, ctx.r3.s32 != mine))
    REXLOG_WARN("[nhl-l2] MISMATCH ShootoutRounds original={} readable={}", ctx.r3.s32, mine);
}
