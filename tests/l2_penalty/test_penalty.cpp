// Unit test for src/l2/penalty.{h,cpp} with SYNTHETIC data (no game data): a sparse big-endian memory.
// Build + run: clang++-20 -std=c++20 -I src tests/l2_penalty/test_penalty.cpp src/l2/penalty.cpp -o /tmp/penalty_test && /tmp/penalty_test
#include <cstdio>
#include <map>
#include <random>

#include "l2/penalty.h"

using namespace nhl::l2::penalty;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class TestEnv : public Env {
 public:
  std::map<uint32_t, uint8_t> m;
  uint8_t U8(uint32_t a) override { auto it = m.find(a); return it == m.end() ? 0 : it->second; }
  uint32_t U32(uint32_t a) override { return uint32_t(U8(a)) << 24 | U8(a + 1) << 16 | U8(a + 2) << 8 | U8(a + 3); }
  void W8(uint32_t a, uint8_t v) override { m[a] = v; }
  void W32(uint32_t a, uint32_t v) override { for (int i = 0; i < 4; ++i) m[a + i] = uint8_t(v >> (24 - 8 * i)); }
  void SetSkaters(uint32_t side, int32_t v) override { W32(kSkaterCount + 4 * side, uint32_t(v)); }
};

static void Counts(TestEnv& e, int h, int a) { e.W32(kSkaterCount, h); e.W32(kSkaterCount + 4, a); }


// ---- additions: SlotIndex, PickBestAvailableSlot, SyncPenaltyPlayerStates ------------------------------------------
static void W32(TestEnv& e, uint32_t a, uint32_t v) { e.W32(a, v); }
static constexpr uint32_t kObjBase = 0x100000, kObjStep = 0x1000, kSideRecBase = 0x200000, kRosterBase = kRosterInfoBase;
static uint32_t Obj(int idx) { return kObjBase + kObjStep * idx; }

static void TestSlotIndex() {  // exhaustive: side -3..4 x all 256 byte values (plus the high bits ignored by the s8 view)
  for (int side = -3; side <= 4; ++side)
    for (int s = 0; s < 256; ++s) {
      const int8_t s8 = int8_t(s);
      const int want = ((side == 0 || side == 1) && s8 >= 0 && s8 < 20) ? side * 20 + s8 : -1;
      CHECK(SlotIndex(side, s) == want);
      CHECK(SlotIndex(side, s + 0x100) == want);
    }
}

// World: gate set, 40 player objects with scores, a side struct, roster records.
static void MakeWorld(TestEnv& e, std::mt19937& rng, bool gate) {
  W32(e, kTeamGate, gate ? 1 : 0);
  W32(e, kPlayerTable == 0 ? 0 : kSideStructPtr, kSideRecBase);
  for (int i = 0; i < kPlayerTableSize; ++i) {
    W32(e, kPlayerTable + 4 * i, Obj(i));
    W32(e, Obj(i) + kObjScore, uint32_t(int(rng() % 160) - 10));
    e.m[Obj(i) + kObjRosterIdx] = (rng() % 4 == 0) ? 200 : uint8_t(rng() % 6);
  }
  for (int r = 0; r < 6; ++r) W32(e, kRosterBase + kRosterInfoStride * r + kRosterFlagOff, uint32_t(int(rng() % 3) - 1));
}

// Independent "spec" version: filter by predicates, then pick the lowest score (< 100, first wins ties).
static int PickSpec(TestEnv& e, int side, int exclude) {
  int best = -1, bs = 100;
  for (int k = 0; k < 18; ++k) {
    if (e.U8(EntryAddr(side, k) + kEntryBusy)) continue;
    bool blocked = false;
    for (int i = 0; i < int8_t(e.U8(kPenaltyBase + kListACountOff)); ++i) {
      const uint32_t r = kPenaltyBase + kListAOff + 8 * i;
      if (int8_t(e.U8(r)) == side && int8_t(e.U8(r + 1)) == k && e.U8(r + 3)) blocked = true;
    }
    if (blocked || k == exclude) continue;
    if (e.U32(kTeamGate) == 0) continue;  // no object -> score 100 -> never < 100
    const uint32_t sr = e.U32(kSideStructPtr) + uint32_t(side) * kSideStructStride + kSideStructOff;
    bool onice = false;
    for (int j = 0; j < 8; ++j) onice |= int8_t(e.U8(sr + kSideOnIceOff + j)) == k;
    if (onice) continue;
    const uint32_t o = e.U32(kPlayerTable + 4 * (side * 20 + k));
    int sc = int(e.U32(o + kObjScore));
    const uint8_t ri = e.U8(o + kObjRosterIdx);
    if (ri < 128 && int(e.U32(kRosterBase + kRosterInfoStride * ri + kRosterFlagOff)) >= 0) sc += 50;
    if (sc < bs) { bs = sc; best = k; }
  }
  return best;
}

static void TestPick() {
  { // directed: scores 100 + k*5 for slots, slot 0 busy, slot 1 in list A, slot 2 excluded, slot 3 on ice -> slot 4
    TestEnv e; std::mt19937 rng(1); MakeWorld(e, rng, true);
    for (int k = 0; k < 18; ++k) { W32(e, Obj(k) + kObjScore, 10 + k * 5); e.m[Obj(k) + kObjRosterIdx] = 200; W32(e, Obj(20 + k) + kObjScore, 10 + k * 5); e.m[Obj(20 + k) + kObjRosterIdx] = 200; }
    for (int s = 0; s < 2; ++s) for (int j = 0; j < 8; ++j) e.m[kSideRecBase + s * kSideStructStride + kSideStructOff + kSideOnIceOff + j] = 0x7F;  // nobody on ice
    CHECK(PickBestAvailableSlot(e, 0, -1) == 0);
    e.m[EntryAddr(0, 0) + kEntryBusy] = 1; CHECK(PickBestAvailableSlot(e, 0, -1) == 1);
    e.m[kPenaltyBase + kListACountOff] = 1;
    e.m[kPenaltyBase + kListAOff] = 0; e.m[kPenaltyBase + kListAOff + 1] = 1; e.m[kPenaltyBase + kListAOff + 3] = 1;
    CHECK(PickBestAvailableSlot(e, 0, -1) == 2);
    CHECK(PickBestAvailableSlot(e, 1, -1) == 0);  // list A record is for side 0 only; busy flag is per side
    CHECK(PickBestAvailableSlot(e, 0, 2) == 3);
    e.m[kSideRecBase + kSideStructOff + kSideOnIceOff + 5] = 3; CHECK(PickBestAvailableSlot(e, 0, 2) == 4);
    e.m[kPenaltyBase + kListAOff + 3] = 0; CHECK(PickBestAvailableSlot(e, 0, -1) == 1);  // flag 0 does not block
    // +50 penalty: slot 1 has roster flag >= 0 -> 10+5+50 = 65; slot 2 = 20 wins over it
    e.m[Obj(1) + kObjRosterIdx] = 0; W32(e, kRosterBase + kRosterFlagOff, 0); CHECK(PickBestAvailableSlot(e, 0, -1) == 2);
    W32(e, kRosterBase + kRosterFlagOff, uint32_t(-1)); CHECK(PickBestAvailableSlot(e, 0, -1) == 1);
    // all scores >= 100 -> -1 (strictly below 100 required)
    for (int k = 0; k < 18; ++k) W32(e, Obj(k) + kObjScore, 100);
    CHECK(PickBestAvailableSlot(e, 0, -1) == -1);
    W32(e, Obj(7) + kObjScore, 99); CHECK(PickBestAvailableSlot(e, 0, -1) == 7);
    // tie: first wins
    W32(e, Obj(4) + kObjScore, 99); CHECK(PickBestAvailableSlot(e, 0, -1) == 4);
    // gate 0 -> -1
    W32(e, kTeamGate, 0); CHECK(PickBestAvailableSlot(e, 0, -1) == -1);
  }
  { // randomized against the spec (small value ranges so every filter fires often), both gates, sides 0/1
    std::mt19937 rng(12345);
    int nonneg = 0;
    for (int it = 0; it < 30000; ++it) {
      TestEnv e; MakeWorld(e, rng, rng() % 8 != 0);
      for (int s = 0; s < 2; ++s) for (int k = 0; k < 20; ++k) e.m[EntryAddr(s, k) + kEntryBusy] = (rng() % 5 == 0);
      const int n = rng() % 5; e.m[kPenaltyBase + kListACountOff] = uint8_t(n);
      for (int i = 0; i < n; ++i) {
        const uint32_t r = kPenaltyBase + kListAOff + 8 * i;
        e.m[r] = rng() % 2; e.m[r + 1] = rng() % 18; e.m[r + 3] = rng() % 2;
      }
      for (int j = 0; j < 8; ++j) { e.m[kSideRecBase + kSideStructOff + kSideOnIceOff + j] = uint8_t(int(rng() % 21) - 2);
                                     e.m[kSideRecBase + kSideStructStride + kSideStructOff + kSideOnIceOff + j] = uint8_t(int(rng() % 21) - 2); }
      const int side = rng() % 2, excl = int(rng() % 20) - 1;
      const int got = PickBestAvailableSlot(e, side, excl), want = PickSpec(e, side, excl);
      if (got != want) { CHECK(got == want); break; }
      nonneg += got >= 0;
    }
    CHECK(nonneg > 1000);  // sanity: the test exercises real picks
  }
}

static void TestSync() {
  // one record, full cross product of the inputs that matter
  const int states[] = {0, 32, 1, 5, -1, 10};
  const int types[] = {0, 10, 11};
  const int seconds[] = {255, 0, 3, 19, 20, 0x80, 0x1FF};  // 0x1FF: u16 != 255 whose low byte is 255 (-1)
  const int objstates[] = {0, 2, 3, 4};
  const int f40s[] = {0, 1, 2};
  for (int gate = 0; gate < 2; ++gate)
  for (int side : {0, 1, 2, -1})
  for (int slot : {0, 7, 19, 20, -1})
  for (int state : states) for (int type : types) for (int second : seconds)
  for (int s1 : objstates) for (int s2 : objstates) for (int f40 : f40s) {
    TestEnv e; std::mt19937 rng(7); MakeWorld(e, rng, gate);
    e.m[kPenaltyBase + kListBCountOff] = 1;
    const uint32_t r = kPenaltyBase + kListBOff;
    e.m[r] = uint8_t(side); e.m[r + 1] = uint8_t(slot); e.m[r + 2] = uint8_t(type); e.m[r + 3] = uint8_t(state);
    e.m[r + 4] = uint8_t(second >> 8); e.m[r + 5] = uint8_t(second);
    // primary object index and second-slot object index (as the original computes them)
    const int has2 = second != 255;
    const int idxA = SlotIndex(side, has2 ? int8_t(second & 0xFF) : slot);  // "obj"
    const int idxB = side * 20 + slot;                                       // "obj2" (range check only)
    for (int i = 0; i < kPlayerTableSize; ++i) W32(e, Obj(i) + kObjState, 9), W32(e, Obj(i) + kObjField40, 0);
    if (idxA >= 0 && idxA < 40) W32(e, Obj(idxA) + kObjState, s1);
    if (idxB >= 0 && idxB < 40 && idxB != idxA) { W32(e, Obj(idxB) + kObjState, s2); }
    if (idxB >= 0 && idxB < 40) W32(e, Obj(idxB) + kObjField40, f40);
    // expected, written as plain per-object results
    int want[40]; for (int i = 0; i < 40; ++i) want[i] = int(e.U32(Obj(i) + kObjState));
    if (gate) {
      const bool a_ok = idxA >= 0 && idxA < 40, b_ok = idxB >= 0 && idxB < 40;
      if (state == 32) { if (a_ok) want[idxA] = 3; }
      else if (state != 0) {
        if (a_ok && want[idxA] != 3) want[idxA] = 2;
        if (has2 && b_ok && f40 != 1 && want[idxB] != 3) want[idxB] = 2;
      }
    }
    SyncPenaltyPlayerStates(e);
    bool ok = true;
    for (int i = 0; i < 40; ++i) ok &= int(e.U32(Obj(i) + kObjState)) == want[i];
    if (!ok) { CHECK(ok); std::printf("  sync case gate=%d side=%d slot=%d state=%d second=%d s1=%d s2=%d f40=%d\n", gate, side, slot, state, second, s1, s2, f40); return; }
  }
  { // several records, count honoured as s8 (0 and negative -> nothing), later records see earlier writes
    TestEnv e; std::mt19937 rng(3); MakeWorld(e, rng, true);
    for (int i = 0; i < 40; ++i) W32(e, Obj(i) + kObjState, 0);
    for (int i = 0; i < 3; ++i) { const uint32_t r = kPenaltyBase + kListBOff + 6 * i; e.m[r] = 1; e.m[r + 1] = uint8_t(i); e.m[r + 3] = i == 1 ? 32 : 5; e.m[r + 5] = 255; }
    e.m[kPenaltyBase + kListBCountOff] = 0xFF; SyncPenaltyPlayerStates(e);  // -1
    for (int i = 0; i < 40; ++i) CHECK(e.U32(Obj(i) + kObjState) == 0);
    e.m[kPenaltyBase + kListBCountOff] = 3; SyncPenaltyPlayerStates(e);
    CHECK(e.U32(Obj(20) + kObjState) == 2 && e.U32(Obj(21) + kObjState) == 3 && e.U32(Obj(22) + kObjState) == 2 && e.U32(Obj(0) + kObjState) == 0);
  }
}

int main() {
  // layout constants agree with the documented guest addresses
  static_assert(kSkaterCount == 0x83C876C4 && kPenaltyBase == 0x83C87700 && kOtMode == 0x83C876B8);
  CHECK(EntryAddr(1, 2) == 0x83C87700 + (20 + 2) * 48);
  CHECK(QueueCounterAddr(1) == 0x83C87700 + 2171 + 1);

  { // AddSkater caps at 5
    TestEnv e;
    for (int n = 0; n <= 6; ++n) { Counts(e, n, n); AddSkater(e, 0); CHECK(int(e.U32(kSkaterCount)) == (n < 5 ? n + 1 : n)); CHECK(int(e.U32(kSkaterCount + 4)) == n); }
  }
  { // RemoveSkater above the floor just decrements, no queue change
    TestEnv e; Counts(e, 5, 4);
    e.m[EntryAddr(0, 0) + kEntryActive] = 1;
    CHECK(RemoveSkater(e, 0) == -1); CHECK(e.U32(kSkaterCount) == 4);
    CHECK(RemoveSkater(e, 0) == -1); CHECK(e.U32(kSkaterCount) == 3);
    CHECK(e.U8(EntryAddr(0, 0) + kEntryServeSeq) == 0 && e.U8(QueueCounterAddr(0)) == 0);
  }
  { // at the floor: queues the first active, unqueued entry; skips queued and empty ones; wraps the counter
    TestEnv e; Counts(e, 3, 5);
    e.m[EntryAddr(0, 0) + kEntryActive] = 0;                 // empty
    e.m[EntryAddr(0, 1) + kEntryActive] = 1; e.m[EntryAddr(0, 1) + kEntryServeSeq] = 7;  // already queued
    e.m[EntryAddr(0, 2) + kEntryActive] = 9;                 // the one
    e.m[EntryAddr(0, 3) + kEntryActive] = 1;
    e.m[QueueCounterAddr(0)] = 255;
    CHECK(RemoveSkater(e, 0) == 2);
    CHECK(e.U8(QueueCounterAddr(0)) == 0 && e.U8(EntryAddr(0, 2) + kEntryServeSeq) == 0);  // wrapped; ticket 0 as the original stores
    CHECK(e.U8(EntryAddr(0, 3) + kEntryServeSeq) == 0 && e.U32(kSkaterCount) == 3 && e.U32(kSkaterCount + 4) == 5);
    // quirk kept from the original: ticket 0 (counter wrap) reads as "not queued", so entry 2 is picked again
    e.m[QueueCounterAddr(0)] = 4;
    CHECK(RemoveSkater(e, 0) == 2); CHECK(e.U8(EntryAddr(0, 2) + kEntryServeSeq) == 5);
    CHECK(RemoveSkater(e, 0) == 3); CHECK(e.U8(EntryAddr(0, 3) + kEntryServeSeq) == 6);
    CHECK(RemoveSkater(e, 0) == -1);  // nothing left to queue, count stays 3
    // side 1 uses its own block and counter
    e.m[EntryAddr(1, 19) + kEntryActive] = 1; Counts(e, 3, 3);
    CHECK(RemoveSkater(e, 1) == 19 && e.U8(QueueCounterAddr(1)) == 1 && e.U8(EntryAddr(1, 19) + kEntryServeSeq) == 1);
  }
  { // OvertimeReduce: exhaustive over counts 0..6 and ot_mode 0..3 against the stated rule
    for (int a = 0; a <= 6; ++a) for (int b = 0; b <= 6; ++b) for (int om = 0; om < 4; ++om) {
      TestEnv e; Counts(e, a, b); e.W32(kOtMode, om);
      const int lo = a < b ? a : b, hi = a < b ? b : a;
      const bool want = (lo > 3 && hi >= 5) || (lo == 4 && hi == 4 && om == 2);
      CHECK(OvertimeReduce(e) == want);
      auto after = [&](int n) { return n > 3 ? n - 1 : 3; };
      CHECK(int(e.U32(kSkaterCount)) == (want ? after(a) : a) && int(e.U32(kSkaterCount + 4)) == (want ? after(b) : b));
    }
  }
  { // RuleGate: every rule value and variant against the table
    for (int rule = -1; rule <= 9; ++rule) for (int v = -2; v <= 14; ++v) {
      TestEnv e;
      e.W32(kRulesPtr, 0x1000); e.W32(0x1000 + kRulesRuleOff, uint32_t(rule));
      e.W32(kModePtr, 0x2000); e.W32(0x2000 + kModeSubOff, 0x3000); e.W32(0x3000 + kModeVariantOff, uint32_t(v));
      bool want;
      if (rule == 3) want = true;
      else if (rule == 2) want = v < 4 || v == 6 || v > 10;
      else want = rule == 7;
      CHECK(RuleGate(e) == want);
    }
  }
  TestSlotIndex();
  TestPick();
  TestSync();
  std::printf(g_fail ? "FAILED (%d)\n" : "all pass\n", g_fail);
  return g_fail != 0;
}
