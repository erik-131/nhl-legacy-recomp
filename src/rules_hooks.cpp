// Rule hook layer (opt-in, default off): game-rule overrides applied from guest hooks. Findings and addresses:
// docs/rule-hooks.md.
//   nhl_skaters_per_side  0 = off, 3 = 3v3 for the whole game, 4 = 4v4
//   nhl_period_length     0 = off, N = the game's periodlength rule value (a period is always 20 virtual minutes,
//                         the rule only scales the clock: N minutes per period = the clock runs at 20/N)
// Env: REX_NHL_SKATERS_PER_SIDE=3 REX_NHL_PERIOD_LENGTH=5
#include "generated/default/nhllegacy_init.h"

#include <atomic>
#include <cstdlib>

#define Once(what)                                                  \
  do {                                                              \
    static std::atomic<bool> once_logged{false};                    \
    if (!once_logged.exchange(true)) REXLOG_INFO("[nhl-rules] hook first call: {}", what); \
  } while (0)

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nhl_skaters_per_side, 0, "NHL", "Skaters per side on the ice for the whole game: 0 = stock (5), 3 or 4");
REXCVAR_DEFINE_BOOL(nhl_manual_lines, false, "NHL", "Every match starts with manual line changes for both sides (docs/line-change.md)");
REXCVAR_DEFINE_BOOL(nhl_skip_language, false, "NHL", "Leave out the language selection screen in the boot flow (docs/boot-flow.md)");
REXCVAR_DEFINE_INT32(nhl_period_length, 0, "NHL", "periodlength rule value (clock runs at 20/N): 0 = leave the game's setting");

namespace {

constexpr uint32_t kSkaterCount = 0x83C876C4;  // int[2] home, away: GameData + 444
constexpr uint32_t kRulesPtr = 0x83C1D218;     // pointer to the runtime GameRules struct, periodlength at +72

int Skaters() {
  const int n = REXCVAR_GET(nhl_skaters_per_side);
  return n >= 3 && n <= 4 ? n : 0;
}

// Writes the period length into the runtime rules struct (idempotent).
void ApplyPeriod(uint8_t* base) {
  const int n = REXCVAR_GET(nhl_period_length);
  if (n <= 0 || n > 60) return;
  const uint32_t rules = REX_LOAD_U32(kRulesPtr);
  if (!rules) return;
  static std::atomic<bool> logged{false};
  if (REX_LOAD_U32(rules + 72) != uint32_t(n) && !logged.exchange(true)) {
    REXLOG_INFO("[nhl-rules] periodlength {} -> {}", REX_LOAD_U32(rules + 72), n);
  }
  REX_STORE_U32(rules + 72, uint32_t(n));
}

// Line changes (docs/line-change.md): the per-match settings array (pointer word 0x83C1D610, two records of 38520 bytes,
// one per side) holds the line change mode as a u16 at +44: 1 = manual, 2 = auto (the stock value, also what the settings
// reset writes). The game copies the profile into the records again during a match start, so the value is forced at the
// entry of every reader (and right before the stoppage function reads it), not once.
constexpr uint32_t kSettingsPtr = 0x83C1D610;
constexpr uint32_t kSettingsStride = 38520;
constexpr uint32_t kLineMode = 44;

// Writes the mode into both records (idempotent, cheap). Called at the entry of every function that reads it, so that
// all readers agree whatever the in-game option says. The saved option and the settings screens are not touched.
void ApplyManualLines(uint8_t* base) {
  if (!REXCVAR_GET(nhl_manual_lines)) return;
  static const uint32_t want = [] {
    const char* e = std::getenv("NHL_LINE_VALUE");  // dev: try another value
    return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 1u;
  }();
  const uint32_t arr = REX_LOAD_U32(kSettingsPtr);
  if (!arr) return;
  for (uint32_t side = 0; side < 2; ++side) {
    const uint32_t at = arr + side * kSettingsStride + kLineMode;
    const uint32_t have = REX_LOAD_U16(at);
    if (have == want) continue;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) REXLOG_INFO("[nhl-lines] side {} line change mode {} -> {}", side, have, want);
    REX_STORE_U16(at, uint16_t(want));
  }
}

}  // namespace

// Readers of the line change mode (the accessor sub_83514D50(side) and six functions that read the field themselves).
#define NHL_LINE_READER(addr)                                         \
  REX_EXTERN(__imp__sub_##addr);                                      \
  extern "C" REX_FUNC(sub_##addr) {                                   \
    Once("line change reader sub_" #addr);                            \
    ApplyManualLines(base);                                            \
    __imp__sub_##addr(ctx, base);                                     \
  }
NHL_LINE_READER(83514D50)
NHL_LINE_READER(8338F030)
NHL_LINE_READER(833C39B8)
NHL_LINE_READER(833DE698)
NHL_LINE_READER(8353A418)
NHL_LINE_READER(835840B0)
#undef NHL_LINE_READER

// The stoppage line changes: the only reader that decides per side whether the game changes the lines itself. Dev logging
// (NHL_LOG_LINE_CHANGES=1) shows what it saw: the controller's user byte and the mode of each side.
REX_EXTERN(__imp__sub_83315EE8);
extern "C" REX_FUNC(sub_83315EE8) {
  Once("line change reader sub_83315EE8");
  ApplyManualLines(base);
  static const bool log = std::getenv("NHL_LOG_LINE_CHANGES") != nullptr;
  if (log) {
    const uint32_t ctl = REX_LOAD_U32(0x83BFE99C), arr = REX_LOAD_U32(kSettingsPtr);
    for (uint32_t side = 0; side < 2 && ctl && arr; ++side) {
      const uint32_t c = REX_LOAD_U32(ctl + (side + 26) * 4);
      REXLOG_INFO("[nhl-lines] stoppage side {} user byte {} mode {}", side, c ? int(int8_t(REX_LOAD_U8(c + 288))) : -99,
                  REX_LOAD_U16(arr + side * kSettingsStride + kLineMode));
    }
  }
  __imp__sub_83315EE8(ctx, base);
}

// The two user tests inside the stoppage function (return addresses 0x83315F4C for side 0 and 0x83315F78 for side 1) come right
// before it reads the mode of that side, so the mode is forced again here. Dev: NHL_LOG_LINE_CHANGES=1 logs what they return.
REX_EXTERN(__imp__sub_83512740);
extern "C" REX_FUNC(sub_83512740) {
  static const bool log = std::getenv("NHL_LOG_LINE_CHANGES") != nullptr;
  const uint32_t lr = uint32_t(ctx.lr), side = ctx.r3.u32;
  const bool stoppage = lr == 0x83315F4C || lr == 0x83315F78;
  if (stoppage) {
    if (log) {
      const uint32_t arr = REX_LOAD_U32(kSettingsPtr);
      REXLOG_INFO("[nhl-lines] stoppage user test side {}: mode before {}", side, arr ? int(REX_LOAD_U16(arr + side * kSettingsStride + kLineMode)) : -1);
    }
    ApplyManualLines(base);
  }
  __imp__sub_83512740(ctx, base);
  if (stoppage && log) REXLOG_INFO("[nhl-lines] stoppage user test side {} -> {}", side, int32_t(ctx.r3.u32));
}

// Dev: NHL_LOG_LINE_CHANGES=1 logs every executed line change (side, lines), to compare stock and forced manual runs.
REX_EXTERN(__imp__sub_833DCAE8);
extern "C" REX_FUNC(sub_833DCAE8) {
  static const bool log = std::getenv("NHL_LOG_LINE_CHANGES") != nullptr;
  if (log) REXLOG_INFO("[nhl-lines] line change event side {} lines {} {} from {:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
  __imp__sub_833DCAE8(ctx, base);
}

// Getters (count by side / by team object): clamp too, whoever wrote the value.
REX_EXTERN(__imp__sub_83381AE8);
extern "C" REX_FUNC(sub_83381AE8) {
  __imp__sub_83381AE8(ctx, base);
  const int n = Skaters();
  if (n && int32_t(ctx.r3.u32) > n) ctx.r3.u64 = uint32_t(n);
}
REX_EXTERN(__imp__sub_83381B18);
extern "C" REX_FUNC(sub_83381B18) {
  __imp__sub_83381B18(ctx, base);
  const int n = Skaters();
  if (n && int32_t(ctx.r3.u32) > n) ctx.r3.u64 = uint32_t(n);
}

// Skater-count setters (sub_8331E470 game start, sub_83311BA8/sub_833163D0 resets, AddSkater, RemoveSkater all end
// here): clamp the value to N, so 5 never gets in. RemoveSkater keeps its floor of 3 and queues the overflow
// penalty, which is what 3v3 needs.
REX_EXTERN(__imp__sub_83381B00);
extern "C" REX_FUNC(sub_83381B00) {  // (side, value)
  const int n = Skaters();
  if (n && int32_t(ctx.r4.u32) > n) ctx.r4.u64 = uint32_t(n);
  Once("sub_83381B00 SetSkaters(side)");
  __imp__sub_83381B00(ctx, base);
}
REX_EXTERN(__imp__sub_83381B38);
extern "C" REX_FUNC(sub_83381B38) {  // (team object, value)
  const int n = Skaters();
  if (n && int32_t(ctx.r4.u32) > n) ctx.r4.u64 = uint32_t(n);
  Once("sub_83381B38 SetSkaters(team)");
  __imp__sub_83381B38(ctx, base);
}

// Game start: the rules struct is loaded by now.
REX_EXTERN(__imp__sub_8331E470);
extern "C" REX_FUNC(sub_8331E470) {
  Once("sub_8331E470 game init");
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("[nhl-rules] skaters_per_side {} period_length {} (rules at {:08X})", REXCVAR_GET(nhl_skaters_per_side),
                REXCVAR_GET(nhl_period_length), REX_LOAD_U32(kRulesPtr));
  }
  ApplyPeriod(base);  // before the init, in case it reads the length
  __imp__sub_8331E470(ctx, base);
  ApplyPeriod(base);
  if (const int n = Skaters()) {
    REX_STORE_U32(kSkaterCount, uint32_t(n));
    REX_STORE_U32(kSkaterCount + 4, uint32_t(n));
  }
}

// Period start (and OT): the rule is applied again in case the struct was rebuilt.
REX_EXTERN(__imp__sub_8331EDB8);
extern "C" REX_FUNC(sub_8331EDB8) {
  Once("sub_8331EDB8 period start");
  ApplyPeriod(base);
  __imp__sub_8331EDB8(ctx, base);
}

// Line-set selection (side, requested line, int* lineOut, int* setOut). Stock has sets for 5v5 (0-3), PP/PK 5 and 4
// (4-9), PK3 (10-11, 3 skaters: 1F + 2D) and OT 4v4 (12-14); a team at 3 skaters against 3 gets none (returns 0).
REX_EXTERN(__imp__sub_833C25B8);
extern "C" REX_FUNC(sub_833C25B8) {
  const uint32_t side = ctx.r3.u32, req = ctx.r4.u32, line_out = ctx.r5.u32, set_out = ctx.r6.u32;
  Once("sub_833C25B8 line set");
  ApplyPeriod(base);
  __imp__sub_833C25B8(ctx, base);
  const int n = Skaters();
  if (!n || side > 1) return;
  const uint32_t own = REX_LOAD_U32(kSkaterCount + 4 * side), opp = REX_LOAD_U32(kSkaterCount + 4 * (side ^ 1));
  int32_t line = int32_t(req);
  if (own == 3) {  // pk3lines, two lines
    line = line < 0 ? 0 : line > 1 ? 1 : line;
    REX_STORE_U32(line_out, uint32_t(line));
    REX_STORE_U32(set_out, uint32_t(10 + line));
    ctx.r3.u64 = 1;
  } else if (own == 4 && opp == 4) {  // ot4lines, three lines
    line = line < 0 ? 0 : line > 2 ? 2 : line;
    REX_STORE_U32(line_out, uint32_t(line));
    REX_STORE_U32(set_out, uint32_t(12 + line));
    ctx.r3.u64 = 1;
  }
}

// Boot flow (docs/boot-flow.md): the UI script asks the native "ION_Bootflow.GetShowLanguageSelection" whether to show the
// language screen and gets the script's true or false object back. With the switch on the answer is always false.
constexpr uint32_t kScriptFalse = 0x83D93714;  // word holding the script runtime's `false` object (true is at +4)
REX_EXTERN(__imp__sub_826F2EC8);
extern "C" REX_FUNC(sub_826F2EC8) {
  if (REXCVAR_GET(nhl_skip_language)) {
    Once("GetShowLanguageSelection -> false");
    ctx.r3.u64 = REX_LOAD_U32(kScriptFalse);
    return;
  }
  __imp__sub_826F2EC8(ctx, base);
}
