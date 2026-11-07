// L2 module 2 implementation. Docs: docs/l2-penalty.md.
#include "penalty.h"

namespace nhl::l2::penalty {

void AddSkater(Env& e, uint32_t side) {
  const int32_t n = e.GetSkaters(side);
  if (n < kMaxSkaters) e.SetSkaters(side, n + 1);
}

int RemoveSkater(Env& e, uint32_t side) {
  const int32_t n = e.GetSkaters(side);
  if (n > kMinSkaters) {
    e.SetSkaters(side, n - 1);
    return -1;
  }
  int queued = -1;
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t entry = EntryAddr(side, i);
    if (e.U8(entry + kEntryServeSeq) != 0) continue;  // already queued
    if (e.U8(entry + kEntryActive) == 0) continue;    // empty slot
    const uint8_t seq = static_cast<uint8_t>(e.U8(QueueCounterAddr(side)) + 1);
    e.W8(QueueCounterAddr(side), seq);
    e.W8(entry + kEntryServeSeq, seq);
    queued = static_cast<int>(i);
    break;
  }
  e.SetSkaters(side, kMinSkaters);
  return queued;
}

bool OvertimeReduce(Env& e) {
  const int32_t a = e.GetSkaters(0), b = e.GetSkaters(1);
  const int32_t lo = a < b ? a : b, hi = a < b ? b : a;
  bool reduce = lo > 3 && hi >= 5;
  if (!reduce) reduce = lo == 4 && hi == 4 && static_cast<int32_t>(e.U32(kOtMode)) == 2;
  if (reduce) {
    RemoveSkater(e, 0);
    RemoveSkater(e, 1);
  }
  return reduce;
}

bool RuleGate(Env& e) {
  const int32_t rule = static_cast<int32_t>(e.U32(e.U32(kRulesPtr) + kRulesRuleOff));
  if (rule == 3) return true;
  if (rule != 2) return rule == 7;
  const int32_t v = static_cast<int32_t>(e.U32(e.U32(e.U32(kModePtr) + kModeSubOff) + kModeVariantOff));
  if (v < 4) return true;
  if (v <= 5) return false;
  if (v <= 6) return true;
  return v > 10;
}

namespace {
inline int32_t S8(uint8_t v) { return static_cast<int8_t>(v); }
inline int32_t S32(Env& e, uint32_t a) { return static_cast<int32_t>(e.U32(a)); }
// Player object for a table index, or 0 (also 0 while the gate is unset or the index is out of range).
uint32_t PlayerObj(Env& e, int32_t idx) {
  if (e.U32(kTeamGate) == 0 || idx < 0 || idx >= kPlayerTableSize) return 0;
  return e.U32(kPlayerTable + 4u * static_cast<uint32_t>(idx));
}
}  // namespace

int32_t SlotIndex(int32_t side, int32_t slot) {
  const int32_t s = static_cast<int8_t>(slot);
  if ((side != 0 && side != 1) || s < 0 || s >= 20) return -1;
  return side * 20 + s;
}

int32_t PickBestAvailableSlot(Env& e, int32_t side, int32_t exclude) {
  const uint32_t uside = static_cast<uint32_t>(side);
  const uint32_t side_rec = e.U32(kTeamGate) ? e.U32(kSideStructPtr) + uside * kSideStructStride + kSideStructOff : 0;
  int32_t best = -1, best_score = 100;
  for (int32_t k = 0; k < kRosterSlots; ++k) {
    int32_t cand = k;
    if (e.U8(kPenaltyBase + (uside * kSlots + static_cast<uint32_t>(cand)) * kEntrySize + kEntryBusy) != 0) cand = -1;
    const int32_t count = S8(e.U8(kPenaltyBase + kListACountOff));
    for (int32_t i = 0; i < count && cand != -1; ++i) {
      const uint32_t rec = kPenaltyBase + kListAOff + kListAStride * static_cast<uint32_t>(i);
      if (S8(e.U8(rec)) == side && S8(e.U8(rec + 1)) == cand && e.U8(rec + 3) != 0) cand = -1;
    }
    if (cand == exclude) continue;
    if (cand == 18 || cand == 19) continue;  // cannot happen for k < 18; kept from the original
    if (cand != -1 && side_rec != 0)
      for (uint32_t j = 0; j < 8 && cand != -1; ++j)
        if (S8(e.U8(side_rec + kSideOnIceOff + j)) == cand) cand = -1;
    if (cand == -1) continue;
    const uint32_t obj = PlayerObj(e, SlotIndex(side, k));
    int32_t score = 100;
    if (obj != 0) {
      score = S32(e, obj + kObjScore);
      const uint8_t ridx = e.U8(obj + kObjRosterIdx);
      if (ridx < 128 && S32(e, kRosterInfoBase + kRosterInfoStride * ridx + kRosterFlagOff) > -1) score += 50;
    }
    if (score < best_score) {
      best = cand;
      best_score = score;
    }
  }
  return best;
}

void SyncPenaltyPlayerStates(Env& e) {
  for (int32_t i = 0; i < S8(e.U8(kPenaltyBase + kListBCountOff)); ++i) {
    const uint32_t rec = kPenaltyBase + kListBOff + kListBStride * static_cast<uint32_t>(i);
    const int32_t side = S8(e.U8(rec)), slot = S8(e.U8(rec + 1)), state = S8(e.U8(rec + 3));
    const uint32_t second = uint32_t(e.U8(rec + 4)) << 8 | e.U8(rec + 5);
    const bool has_second = second != 255;
    const uint32_t obj = PlayerObj(e, SlotIndex(side, has_second ? S8(static_cast<uint8_t>(second)) : slot));
    if (state == 32) {
      if (obj != 0) e.W32(obj + kObjState, 3);
      continue;
    }
    if (state == 0) continue;
    if (obj != 0 && S32(e, obj + kObjState) != 3) e.W32(obj + kObjState, 2);
    if (!has_second) continue;
    const uint32_t obj2 = PlayerObj(e, side * 20 + slot);
    if (obj2 == 0 || S32(e, obj2 + kObjField40) == 1 || S32(e, obj2 + kObjState) == 3) continue;
    e.W32(obj2 + kObjState, 2);
  }
}

}  // namespace nhl::l2::penalty
