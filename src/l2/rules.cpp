// L2 module 3 implementation. Docs: docs/l2-rules.md.
#include "rules.h"

namespace nhl::l2::rules {

int32_t Env::ClockRate() {
  const uint32_t rules = U32(kRulesPtr);
  return static_cast<int32_t>(U32(rules + kPeriodLenOff));
}

uint32_t ActiveFlag(Env& e) {
  const uint32_t gate = e.U32(kTeamGate);
  if (gate != 0) return gate;
  return e.U32(e.U32(kModeObjPtr) + kModeObjActiveOff);
}

int16_t PeriodLength(Env& e, int16_t kind) {
  int32_t minutes = kRegulationPeriodMinutes;
  if (kind == kKindOvertime) {
    bool found = false;
    int32_t ruleset = 0;
    if (ActiveFlag(e) != 0) {
      const uint32_t rules = e.U32(kRulesPtr);
      if (rules != 0) {
        ruleset = static_cast<int32_t>(e.U32(rules + kRulesetOff));
        found = true;
      }
    }
    if (!found) {  // gate 0, or no rules struct: go through the mode object
      const uint32_t mode = e.U32(kModePtr);
      if (mode != 0) {
        const uint32_t alt = e.U32(mode + kModeRulesOff);
        if (alt != 0) {
          ruleset = static_cast<int32_t>(e.U32(alt + kRulesetOff));
          found = true;
        }
      }
    }
    if (found && (ruleset == 3 || ruleset == 4 || ruleset == 2 || ruleset == 7)) minutes = kShortOtMinutes;
  }
  return static_cast<int16_t>(minutes * 60);
}

void InitPeriodClock(Env& e) {
  const int16_t kind = static_cast<int16_t>(e.U32(kPeriodKind));
  e.W16(kClockSeconds, static_cast<uint16_t>(PeriodLength(e, kind)));
  e.W16(kClockSub, 0);
}

void SetClock(Env& e, uint16_t seconds, uint16_t sub) {
  e.W16(kClockSeconds, seconds);
  e.W16(kClockSub, sub);
}

void SetPeriodIndex(Env& e, uint32_t block, int32_t index) {
  e.W32(block + (kPeriodIndex - kGameData), static_cast<uint32_t>(index));
  if (index >= kKindOvertime) {
    e.OvertimePeriodEntered();
    e.W32(block + (kPeriodKind - kGameData), kKindOvertime);
  } else {
    e.W32(block + (kPeriodKind - kGameData), static_cast<uint32_t>(index));
  }
}

void NextPeriod(Env& e) { SetPeriodIndex(e, kGameData, static_cast<int32_t>(e.U32(kPeriodIndex)) + 1); }

bool ClockBelowPeriodStart(Env& e) {
  const int16_t clock = static_cast<int16_t>(e.U16(kClockSeconds));
  const int16_t len = PeriodLength(e, static_cast<int16_t>(e.U32(kPeriodKind)));
  return clock < len;
}

int32_t ElapsedGameSeconds(Env& e) {
  const int32_t kind = static_cast<int32_t>(e.U32(kPeriodKind));
  if (kind == kKindNone) return 0;
  const uint32_t index = e.U32(kPeriodIndex);
  const int16_t len = PeriodLength(e, static_cast<int16_t>(kind));
  const int64_t clock = static_cast<int16_t>(e.U16(kClockSeconds));
  const int64_t r = static_cast<int64_t>(index) * kSecondsPerRegulationPeriod - clock + len;
  return static_cast<int32_t>(r);
}

int32_t ScaleByPeriodLength(Env& e, int32_t x, bool rate_above_20) {
  int32_t n = static_cast<int32_t>(e.U32(e.U32(kRulesPtr) + kPeriodLenOff));
  if (rate_above_20) n = 15;
  const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(20 - n) * static_cast<uint32_t>(x));  // mullw: low 32 bits
  return prod / 20;
}

bool RuleTypeIs(Env& e, int32_t value) {
  if (e.U8(kRuleTypeGateByte) == 0) return false;
  return static_cast<int32_t>(e.U32(e.U32(kRulesPtr) + kRuleTypeOff)) == value;
}

bool RuleTypeIs01(Env& e) {
  if (e.U8(kRuleTypeGateByte) == 0) return false;
  const int32_t v = static_cast<int32_t>(e.U32(e.U32(kRulesPtr) + kRuleTypeOff));
  return v == 0 || v == 1;
}

bool ClockTickArith(Env& e) {
  const int16_t sub = static_cast<int16_t>(e.U16(kClockSub) - 1);
  e.W16(kClockSub, static_cast<uint16_t>(sub));
  if (sub >= 0) return false;
  const int16_t sec = static_cast<int16_t>(e.U16(kClockSeconds));
  if (sec <= 0) {
    e.W16(kClockSub, 0);
    return false;
  }
  e.W16(kClockSeconds, static_cast<uint16_t>(sec - 1));
  e.W16(kClockSub, static_cast<uint16_t>(e.ClockRate() + sub));
  return true;
}

}  // namespace nhl::l2::rules
