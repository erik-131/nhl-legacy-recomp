// L2 module 4 implementation, see gameflow.h.
#include "gameflow.h"

namespace nhl::l2::gameflow {

using namespace rules;

int32_t EvalPeriodEnd(FlowEnv& e, const EndOverride* ov) {
  const int16_t sec = int16_t(e.U16(kClockSeconds));
  const int16_t sub = int16_t(e.U16(kClockSub));
  const bool zero = sec == 0 && sub == 0;
  if (e.U8(kNoRulesFlag) != 0 && zero) return kGameOver;

  const int32_t kind = int32_t(e.U32(kPeriodKind));
  const int32_t ruleset = int32_t(e.U32(e.U32(kRulesPtr) + kRulesetOff));
  const EndMode mode = ov ? ov->mode : EndMode::kStock;

  if (mode == EndMode::kFirstToN && ov->goals_n > 0 && kind >= 0 && kind <= 3 &&
      (e.Goals(0) >= ov->goals_n || e.Goals(1) >= ov->goals_n))
    return kGameOver;

  switch (kind) {
    case kKindLastRegulation: {
      if (!zero) return kContinue;
      if (e.GoalDiff() != 0) return kGameOver;
      int32_t rs = ruleset;
      if (mode == EndMode::kNoOvertimeDraw) rs = 5;
      else if (mode == EndMode::kShootoutAfterReg) rs = 1;
      if (rs == 5) return kGameOver;   // tie allowed, no OT
      if (rs == 1) return kShootout;   // straight to shootout
      return kContinue;                // OT period
    }
    case kKindOvertime:
      if (e.GoalDiff() != 0) return kGameOver;  // sudden death, also while the clock runs
      if (!zero) return kContinue;
      if (ruleset == 3 || ruleset == 4) return kGameOver;
      if (ruleset == 2 || ruleset == 7) return kShootout;
      return kContinue;  // another OT period
    case kKindNone:
      return (e.U32(kFlowFlags) & kFlowRestartBit) ? kRestart : kContinue;
    case kKindShootout:
      return e.ShootoutDecision();
    default:
      return kContinue;
  }
}

int32_t ShootoutRounds(Env& e) {
  if (e.U8(kCustomRoundsFlag) != 0) return e.U8(e.U32(e.U32(kModePtr) + kModeRulesOff) + kCustomRoundsOff);
  const int32_t v = int32_t(e.U32(e.U32(e.U32(kModePtr) + kVariantObjOff) + kVariantOff));
  switch (v) {
    case 0: case 1: case 2: case 4: case 5: case 6: case 11: case 12: case 13: return 3;
    default: return 5;
  }
}

}  // namespace nhl::l2::gameflow
