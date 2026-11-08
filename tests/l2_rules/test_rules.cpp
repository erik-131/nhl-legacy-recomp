// Unit test for src/l2/rules.{h,cpp} with SYNTHETIC data (no game data): a sparse big-endian memory.
// Build + run: clang++-20 -std=c++20 -I src tests/l2_rules/test_rules.cpp src/l2/rules.cpp -o /tmp/rules_test && /tmp/rules_test
#include <cstdio>
#include <map>

#include "l2/rules.h"

using namespace nhl::l2::rules;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class TestEnv : public Env {
 public:
  std::map<uint32_t, uint8_t> m;
  int ot_calls = 0;
  int32_t rate = 20;
  bool use_rate = false;
  uint8_t U8(uint32_t a) override { auto it = m.find(a); return it == m.end() ? 0 : it->second; }
  void W8(uint32_t a, uint8_t v) override { m[a] = v; }
  void OvertimePeriodEntered() override { ++ot_calls; }
  int32_t ClockRate() override { return use_rate ? rate : Env::ClockRate(); }
};

static constexpr uint32_t kRules = 0x100000, kMode = 0x110000, kAlt = 0x120000, kModeObj = 0x130000;

// rules struct reachable through kRulesPtr, gate non-zero
static void World(TestEnv& e, int32_t ruleset, bool gate = true, bool rules_ptr = true) {
  e.W32(kRulesPtr, rules_ptr ? kRules : 0);
  e.W32(kRules + kRulesetOff, uint32_t(ruleset));
  e.W32(kTeamGate, gate ? 1 : 0);
}

static void TestPeriodLength() {
  // Non-overtime kinds are always 20*60, whatever the ruleset.
  for (int rs = -2; rs <= 9; ++rs)
    for (int kind = -3; kind <= 6; ++kind) {
      if (kind == 3) continue;
      TestEnv e; World(e, rs);
      CHECK(PeriodLength(e, int16_t(kind)) == 1200);
    }
  // Overtime: gate on + rules ptr -> short OT for {2,3,4,7}.
  for (int rs = -2; rs <= 9; ++rs) {
    TestEnv e; World(e, rs);
    const bool shortot = rs == 2 || rs == 3 || rs == 4 || rs == 7;
    CHECK(PeriodLength(e, 3) == (shortot ? 300 : 1200));
  }
  // kind is a s16: 3 + 65536 wraps to 3.
  { TestEnv e; World(e, 3); CHECK(PeriodLength(e, int16_t(3 + 65536)) == 300); }
  // Gate off: rules struct ignored, mode path used (mode -> +4 -> struct with ruleset at +36).
  for (int rs = 0; rs <= 8; ++rs) {
    TestEnv e; World(e, 4, /*gate=*/false);  // the rules struct says "short", must be ignored
    e.W32(kModePtr, kMode); e.W32(kMode + kModeRulesOff, kAlt); e.W32(kAlt + kRulesetOff, uint32_t(rs));
    const bool shortot = rs == 2 || rs == 3 || rs == 4 || rs == 7;
    CHECK(PeriodLength(e, 3) == (shortot ? 300 : 1200));
  }
  // Gate on but rules pointer null: also falls back to the mode path.
  { TestEnv e; World(e, 4, true, false); e.W32(kModePtr, kMode); e.W32(kMode + kModeRulesOff, kAlt); e.W32(kAlt + kRulesetOff, 1);
    CHECK(PeriodLength(e, 3) == 1200);
    e.W32(kAlt + kRulesetOff, 7); CHECK(PeriodLength(e, 3) == 300); }
  // Gate off via the mode object: ActiveFlag reads (*kModeObjPtr)+64; non-zero still means "use the rules ptr".
  { TestEnv e; World(e, 2, false); e.W32(kModeObjPtr, kModeObj); e.W32(kModeObj + kModeObjActiveOff, 5);
    CHECK(ActiveFlag(e) == 5);
    CHECK(PeriodLength(e, 3) == 300);
    e.W32(kModeObj + kModeObjActiveOff, 0); CHECK(ActiveFlag(e) == 0);
    CHECK(PeriodLength(e, 3) == 1200);  // gate 0, mode ptr null -> default
  }
  // No pointers at all: default.
  { TestEnv e; CHECK(PeriodLength(e, 3) == 1200); }
  { TestEnv e; e.W32(kTeamGate, 0xDEAD); CHECK(ActiveFlag(e) == 0xDEAD); }
}

static void TestClockInit() {
  TestEnv e; World(e, 1);
  e.W32(kPeriodKind, 0);
  e.W16(kClockSeconds, 17); e.W16(kClockSub, 9);
  InitPeriodClock(e);
  CHECK(e.U16(kClockSeconds) == 1200 && e.U16(kClockSub) == 0);
  World(e, 3); e.W32(kPeriodKind, 3);
  InitPeriodClock(e);
  CHECK(e.U16(kClockSeconds) == 300 && e.U16(kClockSub) == 0);
  SetClock(e, 0xFFFF, 0x8001);
  CHECK(e.U16(kClockSeconds) == 0xFFFF && e.U16(kClockSub) == 0x8001);
  // big-endian layout check
  CHECK(e.U8(kClockSeconds) == 0xFF && e.U8(kClockSub) == 0x80 && e.U8(kClockSub + 1) == 0x01);
}

static void TestPeriodIndex() {
  TestEnv e;
  for (int idx = -2; idx <= 7; ++idx) {
    e.ot_calls = 0;
    SetPeriodIndex(e, kGameData, idx);
    CHECK(int32_t(e.U32(kPeriodIndex)) == idx);
    CHECK(int32_t(e.U32(kPeriodKind)) == (idx < 3 ? idx : 3));
    CHECK(e.ot_calls == (idx >= 3 ? 1 : 0));
  }
  // block argument: another block gets written, kGameData untouched
  TestEnv f; SetPeriodIndex(f, 0x5000, 5);
  CHECK(f.U32(0x5000 + 436) == 5 && f.U32(0x5000 + 432) == 3 && f.U32(kPeriodIndex) == 0);
  // NextPeriod: 0 -> 1 -> 2 -> 3 (OT) -> 4 (OT)
  TestEnv g; SetPeriodIndex(g, kGameData, 0);
  const int want_kind[] = {1, 2, 3, 3}; const int want_ot[] = {0, 0, 1, 2};
  for (int i = 0; i < 4; ++i) {
    NextPeriod(g);
    CHECK(int32_t(g.U32(kPeriodKind)) == want_kind[i]);
    CHECK(g.ot_calls == want_ot[i]);
    CHECK(int32_t(g.U32(kPeriodIndex)) == i + 1);
  }
}

static void TestClockQueries() {
  TestEnv e; World(e, 1);
  e.W32(kPeriodKind, 0);
  for (int clock = -3; clock <= 1203; ++clock) {
    e.W16(kClockSeconds, uint16_t(int16_t(clock)));
    CHECK(ClockBelowPeriodStart(e) == (clock < 1200));
  }
  e.W32(kPeriodKind, 3); World(e, 2);  // OT length 300
  e.W16(kClockSeconds, 300); CHECK(!ClockBelowPeriodStart(e));
  e.W16(kClockSeconds, 299); CHECK(ClockBelowPeriodStart(e));
  e.W16(kClockSeconds, 1200); CHECK(!ClockBelowPeriodStart(e));
  e.W16(kClockSeconds, 0x8000); CHECK(ClockBelowPeriodStart(e));  // negative s16
}

static void TestElapsed() {
  TestEnv e; World(e, 1);
  e.W32(kPeriodKind, 0); e.W32(kPeriodIndex, 0); e.W16(kClockSeconds, 1200);
  CHECK(ElapsedGameSeconds(e) == 0);
  e.W16(kClockSeconds, 1100); CHECK(ElapsedGameSeconds(e) == 100);
  e.W32(kPeriodKind, 1); e.W32(kPeriodIndex, 1); e.W16(kClockSeconds, 1200); CHECK(ElapsedGameSeconds(e) == 1200);
  e.W32(kPeriodKind, 2); e.W32(kPeriodIndex, 2); e.W16(kClockSeconds, 0); CHECK(ElapsedGameSeconds(e) == 2400 + 1200);
  e.W32(kPeriodKind, 3); e.W32(kPeriodIndex, 3); e.W16(kClockSeconds, 1200); CHECK(ElapsedGameSeconds(e) == 3600);
  World(e, 3);  // OT 300 s
  e.W16(kClockSeconds, 300); CHECK(ElapsedGameSeconds(e) == 3600);
  e.W16(kClockSeconds, 0); CHECK(ElapsedGameSeconds(e) == 3900);
  // sentinel
  e.W32(kPeriodKind, 238); CHECK(ElapsedGameSeconds(e) == 0);
  // negative clock adds
  e.W32(kPeriodKind, 0); e.W32(kPeriodIndex, 0); e.W16(kClockSeconds, uint16_t(int16_t(-5))); CHECK(ElapsedGameSeconds(e) == 1205);
}

static void TestScale() {
  TestEnv e; World(e, 1);
  for (int n = 0; n <= 40; ++n)
    for (int x = -50; x <= 5000; x += 37) {
      e.W32(kRules + kPeriodLenOff, uint32_t(n));
      CHECK(ScaleByPeriodLength(e, x, false) == (20 - n) * x / 20);
      CHECK(ScaleByPeriodLength(e, x, true) == 5 * x / 20);
    }
  e.W32(kRules + kPeriodLenOff, 20);
  CHECK(ScaleByPeriodLength(e, 12345, false) == 0);
  CHECK(ScaleByPeriodLength(e, -7, true) == -1);  // truncation toward zero (divw)
  CHECK(!RateAbove20(20) && RateAbove20(21) && RateAbove20(29) && !RateAbove20(-1) && !RateAbove20(0));
}

static void TestRuleTypes() {
  for (int gate = 0; gate <= 1; ++gate)
    for (int v = -1; v <= 6; ++v) {
      TestEnv e; World(e, 1);
      e.m[kRuleTypeGateByte] = uint8_t(gate * 7);
      e.W32(kRules + kRuleTypeOff, uint32_t(v));
      for (int t = 0; t <= 5; ++t) CHECK(RuleTypeIs(e, t) == (gate && v == t));
      CHECK(RuleTypeIs01(e) == (gate && (v == 0 || v == 1)));
    }
}

static void TestTick() {
  TestEnv e; World(e, 1);
  e.use_rate = true; e.rate = 20;
  e.W16(kClockSeconds, 3); e.W16(kClockSub, 2);
  CHECK(!ClockTickArith(e) && e.U16(kClockSub) == 1 && e.U16(kClockSeconds) == 3);
  CHECK(!ClockTickArith(e) && e.U16(kClockSub) == 0);
  // sub goes to -1: a virtual second elapses, sub = rate - 1
  CHECK(ClockTickArith(e) && e.U16(kClockSeconds) == 2 && e.U16(kClockSub) == 19);
  // rate is asked after the decrement: let the rate depend on the new clock value
  struct RE : TestEnv { int32_t ClockRate() override { return int16_t(U16(kClockSeconds)) < 2 ? 29 : 20; } } r;
  World(r, 1); r.W16(kClockSeconds, 2); r.W16(kClockSub, 0);
  CHECK(ClockTickArith(r) && r.U16(kClockSeconds) == 1 && r.U16(kClockSub) == 28);
  // run down to zero: stays at 0 with sub 0
  r.W16(kClockSeconds, 0); r.W16(kClockSub, 0);
  CHECK(!ClockTickArith(r) && r.U16(kClockSeconds) == 0 && r.U16(kClockSub) == 0);
  r.W16(kClockSeconds, 0); r.W16(kClockSub, 5); CHECK(!ClockTickArith(r) && r.U16(kClockSub) == 4);
  // default rate = rules period-length value
  TestEnv d; World(d, 1); d.W32(kRules + kPeriodLenOff, 7);
  d.W16(kClockSeconds, 10); d.W16(kClockSub, 0);
  CHECK(ClockTickArith(d) && d.U16(kClockSub) == 6);
  // ticks for a full period at rate N
  TestEnv t; World(t, 1); t.W32(kRules + kPeriodLenOff, 5);
  t.W16(kClockSeconds, 1200); t.W16(kClockSub, 0);
  int ticks = 0; while (t.U16(kClockSeconds) != 0 && ticks < 1000000) { ClockTickArith(t); ++ticks; }
  CHECK(ticks == 1 + 1199 * 5);  // the first tick already ends a second (sub starts at 0), then N ticks per second
}

int main() {
  TestPeriodLength(); TestClockInit(); TestPeriodIndex(); TestClockQueries(); TestElapsed(); TestScale(); TestRuleTypes(); TestTick();
  std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
  return g_fail != 0;
}
