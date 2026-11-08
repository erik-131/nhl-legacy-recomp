// L2 module 3: period length, game clock, period index and the rule-type predicates, as readable C++.
//
// Pure functions over an `Env` (guest memory + one side-effect callback), same pattern as penalty.h. They run against real
// guest memory in the hooks (rules_hooks.cpp) and against a synthetic memory in tests/l2_rules/test_rules.cpp.
// Docs: docs/l2-rules.md.
//
// Guest functions covered ("read" = from the disassembly, not guessed):
//   0x8330C540 ActiveFlag()               -> ActiveFlag()
//   0x83475D50 PeriodLength(kind)         -> PeriodLength()           (clock seconds of one period of that kind)
//   0x83480A40 InitPeriodClock()          -> InitPeriodClock()        (clock = PeriodLength(current kind), sub-tick = 0)
//   0x833FB6A0 SetClock(sec, sub)         -> SetClock()
//   0x833FB500 SetPeriodIndex(blk, idx)   -> SetPeriodIndex()         (also 0x833FB558 NextPeriod -> NextPeriod())
//   0x83480A88 ClockBelowPeriodStart()    -> ClockBelowPeriodStart()
//   0x83475E08 ElapsedGameSeconds()       -> ElapsedGameSeconds()
//   0x8337FF90 ScaleByPeriodLength(x)     -> ScaleByPeriodLength()
//   0x834859A8                            -> RateAbove20()            (only the final compare; the rate itself, 0x83480C10, is NOT modelled)
//   0x834DC5E8/620/658/690/6D8/710/748    -> RuleTypeIs()/RuleTypeIs01()
//   (clock tick arithmetic of 0x83485740) -> ClockTickArith()         (not hooked: the surrounding function is big)
#pragma once
#include <cstdint>

namespace nhl::l2::rules {

// ---- guest layout (read) -----------------------------------------------------------------------------------
// Game-data block at guest VA kGameData (the same block penalty.h uses). All data is big-endian.
constexpr uint32_t kGameData = 0x83C87508;
constexpr uint32_t kClockSeconds = kGameData + 426;  // s16: virtual seconds left in the period (1200 = 20:00)
constexpr uint32_t kClockSub = kGameData + 428;      // s16: sub-second tick counter, counts down from the clock rate
constexpr uint32_t kPeriodKind = kGameData + 432;    // s32: min(period index, 3); 3 = any overtime period; 238 = "none" sentinel
constexpr uint32_t kPeriodIndex = kGameData + 436;   // s32: 0-based period index (0..2 regulation, 3.. overtime)

// Rules struct, pointer at kRulesPtr (what penalty.h calls the rules block).
constexpr uint32_t kRulesPtr = 0x83C1D218;
constexpr uint32_t kRulesetOff = 36;      // s32 ruleset selector: 1, 2, 3, 4, 7 seen (OT handling, OT length); same field penalty.h's RuleGate reads
constexpr uint32_t kRuleTypeOff = 56;     // s32 0..5 enum read by the seven RuleTypeIs predicates
constexpr uint32_t kPeriodLenOff = 72;    // s32 "period length" rule value N: ticks per virtual clock second (docs/rule-hooks.md)

constexpr uint32_t kTeamGate = 0x83C1E89C;     // u32 (same word as penalty.h kTeamGate)
constexpr uint32_t kModeObjPtr = 0x83BFE998;   // -> object, +64 is read by ActiveFlag when the gate is 0
constexpr uint32_t kModeObjActiveOff = 64;
constexpr uint32_t kModePtr = 0x83BFE99C;      // -> object; +4 -> a rules-like struct whose +36 is the ruleset (fallback path)
constexpr uint32_t kModeRulesOff = 4;
constexpr uint32_t kRuleTypeGateByte = 0x83AC5F63;  // u8: 0 = all RuleTypeIs predicates return false
constexpr uint32_t kClockObjPtr = 0x83C952CC;       // -> clock helper object (argument of 0x834859A8, otherwise unused here)
constexpr uint32_t kOtNoticeObjPtr = 0x83C1D120;    // first argument of the OT-entered callback 0x82C366E0 (r4 = 4, r5 = 1)

constexpr int32_t kRegulationPeriodMinutes = 20;  // PeriodLength returns minutes * 60
constexpr int32_t kShortOtMinutes = 5;            // OT period length for rulesets {2,3,4,7}
constexpr int32_t kKindNone = 238;                // sentinel in kPeriodKind
constexpr int32_t kKindOvertime = 3;
constexpr int32_t kSecondsPerRegulationPeriod = 1200;  // hard-coded in ElapsedGameSeconds

// ---- environment ---------------------------------------------------------------------------------------------
class Env {
 public:
  virtual ~Env() = default;
  virtual uint8_t U8(uint32_t addr) = 0;
  virtual void W8(uint32_t addr, uint8_t v) = 0;
  uint32_t U32(uint32_t a) { return uint32_t(U8(a)) << 24 | uint32_t(U8(a + 1)) << 16 | uint32_t(U8(a + 2)) << 8 | U8(a + 3); }
  uint16_t U16(uint32_t a) { return uint16_t(U8(a) << 8 | U8(a + 1)); }
  void W32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) W8(a + i, uint8_t(v >> (24 - 8 * i))); }
  void W16(uint32_t a, uint16_t v) { W8(a, uint8_t(v >> 8)); W8(a + 1, uint8_t(v)); }
  // SetPeriodIndex calls a guest notifier (0x82C366E0, args kOtNoticeObj value, 4, 1) when the index is >= 3. Its effect is
  // not modelled; the guest env calls the original, the test counts the calls.
  virtual void OvertimePeriodEntered() {}
  // The clock rate (ticks per virtual second) returned by 0x83480C10. NOT modelled (it has about ten special cases: last
  // 10 s / last minute of period 3 and OT -> 29, a difficulty blend, a game-mode override). Default = the common branch,
  // the rules period-length value; the guest env calls the original function.
  virtual int32_t ClockRate();
};

// ---- functions -----------------------------------------------------------------------------------------------
// ActiveFlag (0x8330C540): the gate word if non-zero, else the u32 at (*kModeObjPtr)+64. Used as "is the game system up".
uint32_t ActiveFlag(Env& e);

// PeriodLength (0x83475D50, r3 = kind as s16, returns s16 in r3): clock seconds of a period of `kind`.
// Default 20*60. For kind == 3 (overtime) only: if ActiveFlag() != 0 the ruleset is rules->+36 (rules = *kRulesPtr, if non-null),
// else (or if rules is null) *(*kModePtr+4)+36 (if both non-null); ruleset in {2,3,4,7} -> 5*60 instead.
int16_t PeriodLength(Env& e, int16_t kind);

// InitPeriodClock (0x83480A40; its argument is ignored): clock seconds = PeriodLength(current kind), sub-tick = 0.
void InitPeriodClock(Env& e);

// SetClock (0x833FB6A0): stores both 16-bit clock words.
void SetClock(Env& e, uint16_t seconds, uint16_t sub);

// SetPeriodIndex (0x833FB500, r3 = block address, r4 = index): block+436 = index; block+432 = index < 3 ? index : 3, and for
// index >= 3 the OT notifier runs first (before the kind store).
void SetPeriodIndex(Env& e, uint32_t block, int32_t index);
// NextPeriod (0x833FB558): SetPeriodIndex(kGameData, current index + 1).
void NextPeriod(Env& e);

// ClockBelowPeriodStart (0x83480A88): s16 clock seconds < PeriodLength(current kind), i.e. the clock has started running.
bool ClockBelowPeriodStart(Env& e);

// ElapsedGameSeconds (0x83475E08; r3 ignored): kind == 238 -> 0, else index * 1200 - clock + PeriodLength(kind) (32-bit wrap).
// (The 1200 is fixed, so after a 5-minute OT period it is an approximation of the real elapsed time; stock behaviour.)
int32_t ElapsedGameSeconds(Env& e);

// ScaleByPeriodLength (0x8337FF90, r3 = x): n = rules->+72 (15 if `rate_above_20`); returns (20 - n) * x / 20 (int32 wrap
// for the product, truncating division). `rate_above_20` is the result of 0x834859A8 (calls the unmodelled clock-rate function).
int32_t ScaleByPeriodLength(Env& e, int32_t x, bool rate_above_20);

// RateAbove20 (final compare of 0x834859A8): the clock rate (ticks per virtual second) is >= 21.
inline bool RateAbove20(int32_t rate) { return rate >= 21; }

// RuleTypeIs (0x834DC5E8 = 5, 0x834DC620 = 0, 0x834DC658 = 1, 0x834DC710 = 2, 0x834DC748 = 3, 0x834DC6D8 = 4): false if the
// gate byte is 0, else rules->+56 == value. RuleTypeIs01 (0x834DC690): value is 0 or 1.
bool RuleTypeIs(Env& e, int32_t value);
bool RuleTypeIs01(Env& e);

// ClockTickArith (the clock part of the per-frame tick 0x83485740, after its run gates): sub = s16(sub - 1); if sub < 0:
// seconds > 0 -> seconds -= 1 and sub = ClockRate() + sub (i.e. rate - 1; ClockRate is asked AFTER the seconds were
// decremented, it looks at the new clock value); else sub = 0. Returns true if a virtual second elapsed.
bool ClockTickArith(Env& e);

}  // namespace nhl::l2::rules
