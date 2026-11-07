// L2 module 2: skaters-on-ice bookkeeping and the penalty queue, as readable C++.
//
// Pure functions over an `Env` (guest memory + the two skater-count accessors), so they run against the real
// guest memory in the hooks (penalty_hooks.cpp) and against a synthetic memory in the unit test
// (tests/l2_penalty/test_penalty.cpp). Docs: docs/l2-penalty.md.
//
// Guest functions covered ("read" = from the disassembly, not guessed):
//   0x8330BEA0 AddSkater(side)        -> AddSkater()
//   0x83311140 RemoveSkater(side)     -> RemoveSkater()
//   0x8331B508 OvertimeReduce()       -> OvertimeReduce()   (5v5 -> 4v4, 4v4 stays 4v4 in one OT mode)
//   0x833159B0 RuleGate()             -> RuleGate()
//   0x83310718 PickBestAvailableSlot(side, exclude) -> PickBestAvailableSlot()   (read-only chooser, returns slot or -1)
//   0x833104D0 SyncPenaltyPlayerStates() -> SyncPenaltyPlayerStates()           (writes player-object state words)
#pragma once
#include <cstdint>

namespace nhl::l2::penalty {

// ---- guest layout (read) -----------------------------------------------------------------------------------
// Everything below hangs off one global game-data block (guest VA kGameData). All data is big-endian.
constexpr uint32_t kGameData = 0x83C87508;
constexpr uint32_t kOtMode = kGameData + 432;           // int; 2 = the OT format that also reduces 4v4 to 3v3
constexpr uint32_t kSkaterCount = kGameData + 444;      // int[2], index = side (0 home, 1 away); accessors 0x83381AE8/0x83381B00
constexpr uint32_t kPenaltyBase = kGameData + 504;      // start of the penalty block (what 0x833FBE90 returns)

// Penalty block: 2 sides x kSlots entries of kEntrySize bytes, entry (side, i) at kPenaltyBase + (side*kSlots+i)*kEntrySize.
constexpr uint32_t kSlots = 20;
constexpr uint32_t kEntrySize = 48;
constexpr uint32_t kEntryActive = 149;   // u8 != 0: the entry holds a penalty [read; role of the value guessed]
constexpr uint32_t kEntryServeSeq = 151; // u8: 0 = not yet queued; else the queue ticket (counter value at queue time)
// Per-side queue counters: u8 at kPenaltyBase + 2171 + side. Incremented (wraps at 256) each time a penalty is queued.
constexpr uint32_t kQueueCounterOff = 2171;

// Rules block pointer chain used by RuleGate.
constexpr uint32_t kRulesPtr = 0x83C1D218;        // -> rules struct; +36 = penalty rule value (int)
constexpr uint32_t kRulesRuleOff = 36;
constexpr uint32_t kModePtr = 0x83BFE99C;         // -> object; +24 -> sub-object; +16 -> int `variant`
constexpr uint32_t kModeSubOff = 24;
constexpr uint32_t kModeVariantOff = 16;

// Extra layout read by the chooser / sync functions (all [read]; roles of the fields are [guess]).
constexpr uint32_t kEntryBusy = 150;          // u8 != 0: roster slot is unavailable to the chooser
constexpr uint32_t kListACountOff = 2168;     // s8 count of 8-byte records at penalty base + 8
constexpr uint32_t kListAOff = 8, kListAStride = 8;   // rec: s8 side, s8 slot, ?, u8 flag (!=0 = blocks the slot)
constexpr uint32_t kListBCountOff = 2169;     // s8 count of 6-byte records at penalty base + 88
constexpr uint32_t kListBOff = 88, kListBStride = 6;  // rec: s8 side, s8 slot, s8 type(10/11 special), s8 state(0/32/other), u16 second-slot (255 = none)
constexpr uint32_t kTeamGate = 0x83C1E89C;    // u32; 0 = the player-object system is not set up (everything below is skipped)
constexpr uint32_t kPlayerTable = 0x83C1D2F8; // u32[40] pointers to player objects, index = side*20 + roster slot
constexpr uint32_t kSideStructPtr = 0x83C1D590;     // u32 -> array of per-side structs
constexpr uint32_t kSideStructStride = 38520;       // bytes per side
constexpr uint32_t kSideStructOff = 38268;          // offset (65536-27268) of the side struct used by the chooser
constexpr uint32_t kSideOnIceOff = 36;              // 8 x s8 slots inside it that the chooser skips ("on ice / locked")
constexpr uint32_t kRosterInfoBase = 0x83C86230;    // table of 160-byte records indexed by player-object +556 (s8)
constexpr uint32_t kRosterInfoStride = 160;
constexpr uint32_t kRosterFlagOff = 148;            // s32 in the roster record; >= 0 adds 50 to the chooser score
constexpr uint32_t kObjField40 = 40;                // s32 in player object; 1 = skipped by the sync's second-player step
constexpr uint32_t kObjState = 428;                 // s32 in player object; sync sets 3 or 2
constexpr uint32_t kObjRosterIdx = 556;             // u8 in player object (>= 128 = no roster record)
constexpr uint32_t kObjScore = 1352;                // s32 in player object; chooser's base score (lower wins)
constexpr int kPlayerTableSize = 40;
constexpr int kRosterSlots = 18;                    // chooser scans slots 0..17

constexpr int kMaxSkaters = 5;   // AddSkater never goes above this
constexpr int kMinSkaters = 3;   // RemoveSkater never goes below this; the overflow is queued instead

constexpr uint32_t EntryAddr(uint32_t side, uint32_t i) { return kPenaltyBase + (side * kSlots + i) * kEntrySize; }
constexpr uint32_t QueueCounterAddr(uint32_t side) { return kPenaltyBase + kQueueCounterOff + side; }

// ---- environment ---------------------------------------------------------------------------------------------
class Env {
 public:
  virtual ~Env() = default;
  virtual uint8_t U8(uint32_t addr) = 0;
  virtual uint32_t U32(uint32_t addr) = 0;
  virtual void W8(uint32_t addr, uint8_t v) = 0;
  virtual void W32(uint32_t addr, uint32_t v) {  // big-endian, via W8 by default
    for (int i = 0; i < 4; ++i) W8(addr + i, static_cast<uint8_t>(v >> (24 - 8 * i)));
  }
  // The skater-count accessors. Default = the raw int array; the guest env routes through the (possibly hooked)
  // guest accessors so rule overrides (nhl_skaters_per_side) keep working in replace mode.
  virtual int32_t GetSkaters(uint32_t side) { return static_cast<int32_t>(U32(kSkaterCount + 4 * side)); }
  virtual void SetSkaters(uint32_t side, int32_t v) = 0;
};

// ---- functions -----------------------------------------------------------------------------------------------
// AddSkater: count < 5 -> count + 1.
void AddSkater(Env& e, uint32_t side);

// RemoveSkater: count > 3 -> count - 1. Otherwise the count becomes 3 (floor) and the first not-yet-queued active
// penalty entry of that side is queued: queue counter ++, entry's serve-seq = new counter (low byte).
// Returns the index of the queued entry, or -1 if none was queued (also when count > 3).
int RemoveSkater(Env& e, uint32_t side);

// OvertimeReduce: with lo/hi the smaller/larger side count: if lo > 3 && hi >= 5 (5v5, or 5 vs 4), or
// lo == 4 && hi == 4 && ot_mode == 2, then RemoveSkater for both sides. Returns true if it reduced.
bool OvertimeReduce(Env& e);

// RuleGate: rule = rules->+36. 3 -> true. 2 -> depends on `variant` (mode->+24->+16): <4 true, 4..5 false,
// 6 true, 7..10 false, >10 true. Otherwise true iff rule == 7. (Purpose: a penalty/rule-set predicate; callers
// not analysed [guess].) Reads null pointers as 0 like the original would fault, so a null chain returns the
// result for 0 only in the unit test; the guest never has null here.
bool RuleGate(Env& e);

// SlotIndex (guest 0x8330AB40): side in {0,1} and slot (as s8) in 0..19 -> side*20 + slot, else -1. Pure.
int32_t SlotIndex(int32_t side, int32_t slot);

// PickBestAvailableSlot (guest 0x83310718; r3 = side, r4 = exclude): scans roster slots 0..17 of `side` and returns the
// slot with the strictly lowest score below 100 (first one wins ties), or -1. A slot is skipped if its entry's
// +150 byte != 0, if a list-A record (side, slot, flag != 0) exists, if `slot == exclude`, or if it is in the side
// struct's 8-byte "on ice" list. Score = player object +1352 (100 if no object), +50 if its roster record's +148 >= 0.
// If kTeamGate is 0 no object is ever found, every score is 100 and the result is -1. Writes nothing.
int32_t PickBestAvailableSlot(Env& e, int32_t side, int32_t exclude);

// SyncPenaltyPlayerStates (guest 0x833104D0, no args): for each list-B record, `obj` = player object of (side, second
// slot if the record has one (u16 != 255) else primary slot): state 32 -> obj state = 3 (done); state 0 -> nothing;
// other state -> obj state = 2 unless it is already 3. Then, if the record has a second slot, the primary object
// (index side*20+slot, only range-checked 0..39) also gets state 2 unless its +40 == 1 or its state is already 3.
// The original also builds a local de-dup list that is never read afterwards (dead), omitted here.
void SyncPenaltyPlayerStates(Env& e);

}  // namespace nhl::l2::penalty
