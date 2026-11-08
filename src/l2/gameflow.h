// L2 module 4: period-end game flow decision (game over / continue / shootout / restart) and the shootout round count, as
// readable C++. Same pattern as rules.h: pure functions over an Env (guest memory + two side-effect-free callbacks that
// the hooks route to the original guest functions). Docs: docs/l2-gameflow.md.
//
// Guest functions covered ("read" = from the lifted code):
//   0x8331DFE8 EvalPeriodEnd()   -> EvalPeriodEnd()    (kinds 2, 3 and the plain kinds; 255/238 see below)
//   0x834039E0 ShootoutRounds()  -> ShootoutRounds()
// Callee reads that are NOT modelled (Env callbacks, the guest env calls the originals):
//   0x833FADA0(0) goal difference (two per-side goal reads via 0x833FAD58 through a guest object chain)
//   0x8331DED0 ShootoutDecision (kind 255; it has side effects, so verify mode never calls it twice)
//   kind 238 diagnostic path (fires UI events when the clock is zero and attempts >= 6): the hook leaves kind 238 to the original.
#pragma once
#include <cstdint>

#include "rules.h"

namespace nhl::l2::gameflow {

using rules::Env;

// ---- guest layout (read) -----------------------------------------------------------------------------------
constexpr uint32_t kFlagBytes = 0x83AC5F60;          // u8 flag array
constexpr uint32_t kNoRulesFlag = kFlagBytes + 14;   // u8: non-zero -> a zero clock ends the game at once
constexpr uint32_t kCustomRoundsFlag = kFlagBytes + 24;  // u8: custom shootout rounds in use
constexpr uint32_t kFlowFlags = 0x83C1D244;          // u32 flow struct (0x83C1D220) + 36
constexpr uint32_t kFlowRestartBit = 0x00800000;     // F+36 bit: restart (kind 238 only)
constexpr uint32_t kCustomRoundsOff = 384;           // u8 at (*(*kModePtr + 4)) + 384
constexpr uint32_t kVariantObjOff = 24;              // *(kModePtr) + 24 -> object, +16 = s32 game-mode variant
constexpr uint32_t kVariantOff = 16;

constexpr int32_t kKindLastRegulation = 2;  // literal 2 in the original
constexpr int32_t kKindShootout = 255;

enum Outcome : int32_t { kGameOver = 0, kContinue = 1, kShootout = 2, kRestart = 3 };

// ---- env with the unmodelled callees ---------------------------------------------------------------------------
class FlowEnv : public Env {
 public:
  // 0x833FAD58(side): goals of side 0/1 (the original reads them through a guest object chain). Default 0.
  virtual int32_t Goals(int side) { (void)side; return 0; }
  // 0x833FADA0(0): Goals(0) - Goals(1) as int32.
  int32_t GoalDiff() { return int32_t(uint32_t(Goals(0)) - uint32_t(Goals(1))); }
  // 0x8331DED0: result for kind 255 (0 decided, 2 go on). Default 2.
  virtual int32_t ShootoutDecision() { return kShootout; }
};

// ---- optional custom end-of-game override (default OFF) ---------------------------------------------------------
// Applied only when passed to EvalPeriodEnd (replace mode); verify mode never passes it.
enum class EndMode : int32_t {
  kStock = 0,
  kFirstToN = 1,        // any kind 0..3: a side with >= goals_n goals ends the game (sudden-death / mercy). Else stock rules.
  kNoOvertimeDraw = 2,  // a tie after the last regulation period ends the game (like ruleset 5)
  kShootoutAfterReg = 3 // a tie after the last regulation period goes straight to a shootout (like ruleset 1)
};
struct EndOverride {
  EndMode mode = EndMode::kStock;
  int32_t goals_n = 0;  // for kFirstToN
};
// DESIGN ONLY (not implemented): regulation period count 2/3/4 would replace `kind == 2` below with `index == last` and
// needs SetPeriodIndex/PeriodLength cooperation (docs/l2-gameflow.md "Data fields to change for a custom mode").

// ---- functions -----------------------------------------------------------------------------------------------
// EvalPeriodEnd (0x8331DFE8): returns Outcome. Pure given the Env. Kind 238 returns kRestart if the flow restart bit is set,
// else kContinue, WITHOUT the original's diagnostic event (hooks delegate kind 238 to the original). Kind 255 returns
// env.ShootoutDecision(). Reads the ruleset from (*kRulesPtr)+36 without a null check, like the original.
int32_t EvalPeriodEnd(FlowEnv& e, const EndOverride* ov = nullptr);

// ShootoutRounds (0x834039E0): u8 custom value if kCustomRoundsFlag != 0, else 3 for variants {0,1,2,4,5,6,11,12,13}, else 5.
int32_t ShootoutRounds(Env& e);

}  // namespace nhl::l2::gameflow
