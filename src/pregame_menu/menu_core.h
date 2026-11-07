// Pre-game settings menu: game-independent logic (no SDK, SDL or ImGui includes).
//
//   options   what can be chosen, with the env var / ini key each choice sets      (DefaultOptions)
//   settings  chosen values, load/save of nhl_pregame.ini                          (Settings)
//   apply     choices -> env vars and ini keys, through an Applier the app provides (ApplyAll)
//   nav       gamepad state machine: cursor, pages, edit, Start/Back                (Menu)
//   view      what to draw, as plain rows of text                                   (BuildView, RenderLines)
//
// A renderer (SDL_RenderDebugText, ImGui, a terminal) only draws View/RenderLines and feeds pad bits to Menu::Update.
// Design and open questions: docs/menu-design.md.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nhl::pregame {

// ---- options ---------------------------------------------------------------------------------------------------------

enum class Target {
  Env,   // environment variable; skipped if the process already has it (launcher / dev override wins)
  Ini,   // key in nhl_enhancements.ini (the overlay's file, read by OnPreSetup); empty value erases the key
  Menu,  // read by the menu itself (e.g. menu.show)
};

struct Assign {
  Target target;
  std::string name;
  std::string value;
};

struct Choice {
  std::string value;  // stable id stored in the settings file
  std::string label;  // shown to the player
  std::vector<Assign> assigns;
  std::string warn;   // non-empty: shown in the hint panel when this choice is focused (quality drops, risks)
};

enum class Kind { Choice, Action };
enum class ActionId { None, ResetAll };

struct Option {
  std::string id;      // key in nhl_pregame.ini
  std::string page;    // tab name
  std::string label;
  std::string help;    // one line
  Kind kind = Kind::Choice;
  ActionId action = ActionId::None;
  std::vector<Choice> choices;
  int def = 0;                 // index of the default choice
  bool restart = false;        // cannot change while the game runs (matters once the menu is reused in game)
  std::string measured;        // evidence or "GUESS"; shown in the doc and hint panel
};

const std::vector<Option>& DefaultOptions();
std::vector<std::string> PageNames(const std::vector<Option>& opts);  // in order of first use

// ---- settings --------------------------------------------------------------------------------------------------------

class Settings {
 public:
  explicit Settings(const std::vector<Option>& opts) : opts_(&opts) {}
  int Index(const Option& o) const;              // chosen choice index (default if unset/invalid)
  void SetIndex(const Option& o, int idx);       // out of range ignored
  bool IsDefault(const Option& o) const { return Index(o) == o.def; }
  void ResetAll();
  bool operator==(const Settings& r) const { return values_ == r.values_; }

  // Text format: "# comment", "key=value" lines; only non-default options are stored, unknown keys are kept.
  // Returns number of lines that were ignored (bad value for a known id, junk). Missing file = defaults, 0.
  int ParseText(const std::string& text);
  std::string ToText() const;
  int Load(const std::string& path);
  bool Save(const std::string& path) const;      // writes path.tmp then renames; false on failure
  int warnings() const { return warnings_; }

 private:
  const std::vector<Option>* opts_;
  std::map<std::string, std::string> values_;    // known id -> choice value (non-default only)
  std::map<std::string, std::string> unknown_;   // kept so that newer/older builds do not lose keys
  int warnings_ = 0;
};

// ---- apply -----------------------------------------------------------------------------------------------------------

class Applier {
 public:
  virtual ~Applier() = default;
  virtual bool EnvIsSet(const std::string& name) = 0;
  virtual void SetEnv(const std::string& name, const std::string& value) = 0;
  virtual void SetIni(const std::string& name, const std::string& value) = 0;  // empty value: erase
};

struct ApplyReport {
  int env_set = 0, env_kept = 0, ini_set = 0;
};
// Applies every option, defaults included, so that the menu can replace the env defaults in nhl_play.sh.
// Env: always (an env var already set wins). Ini: only for non-default choices; a default choice leaves the ini key alone,
// unless `initial` (the settings when the menu opened) had that option non-default, i.e. the player moved it back: then erase.
ApplyReport ApplyAll(const std::vector<Option>& opts, const Settings& s, Applier& out, const Settings* initial = nullptr);

// ---- navigation ------------------------------------------------------------------------------------------------------

enum Pad : uint32_t {  // same meaning as XInput; the renderer maps its pad to these bits
  kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kX = 64, kY = 128,
  kLB = 256, kRB = 512, kStart = 1024, kBack = 2048,
};
uint32_t StickToPad(float lx, float ly, float deadzone = 0.5f);  // +y is up

enum class Outcome {
  Running,
  StartGame,   // Start: settings saved by caller (Menu::settings()), game starts
  Discard,     // B / Back: start the game with the settings as they were when the menu opened
  TimedOut,    // auto-start timer ran out with no input; same as Discard but flagged for the log
};

struct MenuConfig {
  int visible_rows = 9;
  uint32_t repeat_first_ms = 400, repeat_ms = 120;
  uint32_t auto_start_ms = 0;  // 0 = wait forever. Any input restarts the wait.
};

class Menu {
 public:
  Menu(const std::vector<Option>& opts, Settings initial, MenuConfig cfg = {});
  // Call once per frame with the pad's current bits and a monotonic ms clock. Buttons already held when the menu opens
  // (the press that launched the game) are ignored until released.
  Outcome Update(uint32_t pad, uint64_t now_ms);

  const Settings& settings() const { return cur_; }
  const Settings& initial() const { return initial_; }
  bool dirty() const { return !(cur_ == initial_); }
  size_t page() const { return page_; }
  size_t cursor() const { return cursor_; }   // row within the page
  size_t scroll() const { return scroll_; }
  std::vector<const Option*> PageOptions() const;
  const std::vector<Option>& options() const { return *opts_; }
  const std::vector<std::string>& pages() const { return pages_; }
  const MenuConfig& config() const { return cfg_; }
  uint64_t idle_ms(uint64_t now_ms) const { return now_ms - last_input_ms_; }

 private:
  void Press(uint32_t bit);
  void Move(int d);
  void Page(int d);
  void Change(int d, bool wrap);
  void Activate();
  void Fix();

  const std::vector<Option>* opts_;
  Settings initial_, cur_;
  MenuConfig cfg_;
  std::vector<std::string> pages_;
  size_t page_ = 0, cursor_ = 0, scroll_ = 0;
  uint32_t prev_ = 0, ignore_ = 0;
  bool started_ = false;
  uint64_t last_input_ms_ = 0, repeat_at_ = 0;
  uint32_t repeating_ = 0;
  Outcome out_ = Outcome::Running;
};

// ---- view ------------------------------------------------------------------------------------------------------------

struct ViewRow {
  std::string label, value;
  bool selected = false, changed = false, action = false;
};
struct View {
  std::string title;
  std::vector<std::string> tabs;
  size_t tab = 0;
  std::vector<ViewRow> rows;
  bool more_above = false, more_below = false;
  std::string hint, warn, note;   // help of the focused row; warning if the focused choice lowers quality; restart note
  std::string footer;
  int countdown_s = -1;           // seconds until auto start, -1 if none
};
View BuildView(const Menu& m, uint64_t now_ms = 0);
// Plain text frame of at most `cols` columns, for terminals, logs, tests and as the text source of SDL_RenderDebugText.
std::vector<std::string> RenderLines(const View& v, int cols = 64);

}  // namespace nhl::pregame
