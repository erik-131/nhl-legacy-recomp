// Unit test for src/l2/gameflow.{h,cpp} with SYNTHETIC data (no game data).
// Build + run: clang++-20 -std=c++20 -I src tests/l2_gameflow/test_gameflow.cpp src/l2/gameflow.cpp src/l2/rules.cpp -o /tmp/gameflow_test && /tmp/gameflow_test
#include <cstdio>
#include <map>

#include "l2/gameflow.h"

using namespace nhl::l2::gameflow;
using namespace nhl::l2::rules;

static int g_fail = 0;
static long g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { if (g_fail < 40) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class TestEnv : public FlowEnv {
 public:
  std::map<uint32_t, uint8_t> m;
  int goals[2] = {0, 0};
  int32_t shoot = kShootout;
  int goal_calls = 0;
  uint8_t U8(uint32_t a) override { auto it = m.find(a); return it == m.end() ? 0 : it->second; }
  void W8(uint32_t a, uint8_t v) override { m[a] = v; }
  int32_t Goals(int s) override { ++goal_calls; return goals[s]; }
  int32_t ShootoutDecision() override { return shoot; }
};

static constexpr uint32_t kRules = 0x100000, kMode = 0x110000, kRulesLike = 0x120000, kVarObj = 0x130000;

static void Setup(TestEnv& e, int32_t kind, int16_t sec, int16_t sub, int32_t rs, bool noRules, bool restart) {
  e.W16(kClockSeconds, uint16_t(sec)); e.W16(kClockSub, uint16_t(sub));
  e.W32(kPeriodKind, uint32_t(kind));
  e.W32(kRulesPtr, kRules); e.W32(kRules + kRulesetOff, uint32_t(rs));
  e.W8(kNoRulesFlag, noRules ? 1 : 0);
  e.W32(kFlowFlags, restart ? kFlowRestartBit : 0);
}

// Independent reference (table form of the doc's rules).
static int Ref(int kind, bool zero, int diff, int rs, bool noRules, bool restart, int shoot) {
  if (noRules && zero) return 0;
  if (kind == 2) { if (!zero) return 1; if (diff) return 0; if (rs == 5) return 0; if (rs == 1) return 2; return 1; }
  if (kind == 3) { if (diff) return 0; if (!zero) return 1; if (rs == 3 || rs == 4) return 0; if (rs == 2 || rs == 7) return 2; return 1; }
  if (kind == 238) return restart ? 3 : 1;
  if (kind == 255) return shoot;
  return 1;
}

static void TestExhaustive() {
  const int kinds[] = {-1, 0, 1, 2, 3, 4, 5, 100, 237, 238, 239, 255, 256};
  const int16_t secs[] = {0, 1, 1200, -1};
  const int16_t subs[] = {0, 1, 19, -1};
  const int diffs[] = {-2, -1, 0, 1, 3};
  for (int kind : kinds) for (int16_t sec : secs) for (int16_t sub : subs) for (int d : diffs)
  for (int rs = -1; rs <= 9; ++rs) for (int nr = 0; nr < 2; ++nr) for (int rf = 0; rf < 2; ++rf)
  for (int shoot : {0, 2}) {
    TestEnv e; Setup(e, kind, sec, sub, rs, nr, rf);
    e.goals[0] = d >= 0 ? d : 0; e.goals[1] = d >= 0 ? 0 : -d;
    e.shoot = shoot;
    CHECK(EvalPeriodEnd(e) == Ref(kind, sec == 0 && sub == 0, d, rs, nr, rf, shoot));
  }
}

static void TestOverrides() {
  // Stock override struct == no override.
  for (int kind : {0, 1, 2, 3}) for (int rs = 0; rs <= 8; ++rs) for (int z = 0; z < 2; ++z) for (int d = -1; d <= 1; ++d) {
    TestEnv e; Setup(e, kind, z ? 0 : 5, 0, rs, false, false); e.goals[0] = d > 0; e.goals[1] = d < 0;
    EndOverride ov;
    CHECK(EvalPeriodEnd(e, &ov) == EvalPeriodEnd(e));
  }
  // No overtime / draw: tie at end of kind 2 -> game over, for every ruleset; clock running still continues; a win ends.
  for (int rs = 0; rs <= 8; ++rs) {
    EndOverride ov{EndMode::kNoOvertimeDraw, 0};
    TestEnv e; Setup(e, 2, 0, 0, rs, false, false);
    CHECK(EvalPeriodEnd(e, &ov) == kGameOver);
    Setup(e, 2, 7, 0, rs, false, false); CHECK(EvalPeriodEnd(e, &ov) == kContinue);
    Setup(e, 3, 0, 0, rs, false, false); CHECK(EvalPeriodEnd(e, &ov) == EvalPeriodEnd(e));  // OT untouched
  }
  // Shootout after regulation: tie -> 2; win -> 0.
  for (int rs = 0; rs <= 8; ++rs) {
    EndOverride ov{EndMode::kShootoutAfterReg, 0};
    TestEnv e; Setup(e, 2, 0, 0, rs, false, false);
    CHECK(EvalPeriodEnd(e, &ov) == kShootout);
    e.goals[1] = 1; CHECK(EvalPeriodEnd(e, &ov) == kGameOver);
  }
  // First to N: ends the game as soon as a side has N, in any regulation/OT kind, any clock; not for 238/255.
  for (int n = 1; n <= 5; ++n) for (int kind : {0, 1, 2, 3}) for (int g0 = 0; g0 <= 6; ++g0) for (int g1 = 0; g1 <= 6; ++g1) {
    EndOverride ov{EndMode::kFirstToN, n};
    TestEnv e; Setup(e, kind, 600, 5, 0, false, false); e.goals[0] = g0; e.goals[1] = g1;
    const bool hit = g0 >= n || g1 >= n;
    if (hit) CHECK(EvalPeriodEnd(e, &ov) == kGameOver);
    else CHECK(EvalPeriodEnd(e, &ov) == EvalPeriodEnd(e));
  }
  { EndOverride ov{EndMode::kFirstToN, 1}; TestEnv e; Setup(e, 255, 0, 0, 0, false, false); e.goals[0] = 9; e.shoot = 2;
    CHECK(EvalPeriodEnd(e, &ov) == kShootout);
    Setup(e, 238, 0, 0, 0, false, true); CHECK(EvalPeriodEnd(e, &ov) == kRestart); }
  // goals_n <= 0 disables first-to-N.
  { EndOverride ov{EndMode::kFirstToN, 0}; TestEnv e; Setup(e, 1, 600, 0, 0, false, false); e.goals[0] = 9;
    CHECK(EvalPeriodEnd(e, &ov) == kContinue); }
}

static void TestRounds() {
  for (int v = -3; v <= 20; ++v) for (int custom = 0; custom < 2; ++custom) for (int cb : {0, 1, 7, 255}) {
    TestEnv e;
    e.W32(kModePtr, kMode); e.W32(kMode + kModeRulesOff, kRulesLike); e.W8(kRulesLike + kCustomRoundsOff, uint8_t(cb));
    e.W32(kMode + kVariantObjOff, kVarObj); e.W32(kVarObj + kVariantOff, uint32_t(v));
    e.W8(kCustomRoundsFlag, uint8_t(custom ? 1 : 0));
    const bool three = v == 0 || v == 1 || v == 2 || v == 4 || v == 5 || v == 6 || v == 11 || v == 12 || v == 13;
    CHECK(ShootoutRounds(e) == (custom ? cb : (three ? 3 : 5)));
  }
}

static void TestBigEndian() {
  TestEnv e; Setup(e, 3, 0, 0, 3, false, false);
  CHECK(e.U8(kPeriodKind + 3) == 3 && e.U8(kPeriodKind) == 0);
  CHECK(EvalPeriodEnd(e) == kGameOver);  // tie after OT in ruleset 3
}

int main() {
  TestExhaustive(); TestOverrides(); TestRounds(); TestBigEndian();
  std::printf("%s (%ld checks, %d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
