#include "menu_core.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nhl::pregame {
namespace {

Choice C(std::string value, std::string label, std::vector<Assign> a = {}, std::string warn = {}) {
  return Choice{std::move(value), std::move(label), std::move(a), std::move(warn)};
}
Assign Env(const char* n, const char* v) { return {Target::Env, n, v}; }
Assign Ini(const char* n, const char* v) { return {Target::Ini, n, v}; }
std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

// Defaults below mirror what tools/nuc/ct105/nhl_play.sh exports today, so that "all defaults" = the current launcher.
const std::vector<Option>& DefaultOptions() {
  static const std::vector<Option> kOpts = [] {
    std::vector<Option> o;
    auto add = [&](Option x) { o.push_back(std::move(x)); };

    add({"boot", "Start", "Boot screens",
         "Presses through language, title and notice screens up to the main menu. Any real button takes over.",
         Kind::Choice, ActionId::None,
         {C("quick", "Quick start",
            {Env("REX_NHL_QUICK_START", "true"), Env("NHL_BOOT_CLOCK", "3"), Env("NHL_QUICK_MOVIE_FRAMES", "12")}),
          C("quick-intro", "Quick, full intro movie",
            {Env("REX_NHL_QUICK_START", "true"), Env("NHL_BOOT_CLOCK", "3"), Env("NHL_QUICK_MOVIE_FRAMES", "365")}),
          C("normal", "Normal boot",
            {Env("REX_NHL_QUICK_START", "false"), Env("NHL_BOOT_CLOCK", "1"), Env("NHL_QUICK_MOVIE_FRAMES", "365")})},
         0, true,
         "measured 2026-11-06: main menu at ~10-14 s with quick start v ~24-26 s without (docs/nuc-perf-notes.md)"});
    add({"show", "Start", "Show this menu at launch",
         "If off, hold Back while the game starts to bring this menu back.",
         Kind::Choice, ActionId::None,
         {C("always", "Always", {{Target::Menu, "menu.show", "always"}}),
          C("hold", "Only when Back is held", {{Target::Menu, "menu.show", "hold"}},
            "You will not see this screen again unless you hold Back at launch.")},
         0, false, ""});
    add({"reset", "Start", "Reset everything to defaults", "Puts every option back to its default.", Kind::Action,
         ActionId::ResetAll, {}, 0, false, ""});

    add({"msaa", "Performance", "Main pass multisampling",
         "Draw the game's main pass at 1 sample instead of the game's 2x MSAA.", Kind::Choice, ActionId::None,
         {C("game", "Game default (2x)", {Env("REX_NHL_FORCE_HOST_MSAA_1X", "false")}),
          C("1x", "Off (1x)", {Env("REX_NHL_FORCE_HOST_MSAA_1X", "true")}, "Edges look more jagged.")},
         0, true,
         "earlier test: +23 % fps (not found in nuc-perf-notes; the only note on nhl_force_host_msaa_1x is GPU 16.5 -> 15.7 ms, "
         "Oct 31). NUC session: confirm that this cvar is what gave +23 %"});
    add({"ss", "Performance", "Render resolution", "Internal resolution multiplier (720p x N).", Kind::Choice,
         ActionId::None,
         {C("1x", "1x (720p)", {Ini("supersampling", "")}),
          C("2x", "2x (1440p)", {Ini("supersampling", "2")}, "Much more GPU work. Cost on the NUC: GUESS, not measured.")},
         0, true, "cap is 2x (larger scaled buffers break equipment textures, see nhl_settings.h)"});
    add({"fence", "Performance", "Waiting for the GPU", "How the CPU waits for the GPU between frames.", Kind::Choice,
         ActionId::None,
         {C("sleep", "Sleep (saves CPU)", {Env("REX_NHL_FENCE_POLL", "3")}),
          C("spin", "Spin (as recompiled)", {Env("REX_NHL_FENCE_POLL", "0")}, "Uses about 0.8 of a core more.")},
         0, true,
         "measured: ~0.8 core saved; two E-cores median 63.8 -> 89.9 and 75.7 -> 86.4 fps; close-ups maybe ~5 fps lower in one test"});
    add({"cap", "Performance", "Frame limit", "Swap interval of the presenter.", Kind::Choice, ActionId::None,
         {C("free", "No limit", {Env("NHL_SWAP_INTERVAL", "0")}),
          C("30", "30 fps lock", {Env("NHL_SWAP_INTERVAL", "2")}, "Half the smoothness; steadier on a weak GPU."),
          C("60", "60 fps (vsync)", {Env("NHL_SWAP_INTERVAL", "1")},
            "Untested hypothesis: steadier frame delivery to a 60 Hz stream and less GPU load; adds a little latency.")},
         0, true, "30 from nhl_play.sh comments; 60: see worklog 2026-11-07 (Wolf stream path), not measured yet"});
    add({"cluster", "Performance", "Thread placement", "Let the game place its own threads on hybrid CPUs.", Kind::Choice,
         ActionId::None,
         {C("game", "Game places threads", {Env("NHL_CPU_CLUSTER", "1")}),
          C("os", "Pin P-cores / OS decides", {Env("NHL_CPU_CLUSTER", "0")})},
         0, true, "see nuc-perf-notes (cpu_placement); Intel hybrid only"});

    add({"edge_aa", "Look", "Edge anti-aliasing", "The game's own edge-detect AA pass.", Kind::Choice, ActionId::None,
         {C("auto", "Auto (menus only)", {Ini("edge_aa_mode", "")}), C("on", "Always on", {Ini("edge_aa_mode", "1")}),
          C("off", "Always off", {Ini("edge_aa_mode", "2")}, "Edges look more jagged.")},
         0, true, ""});
    add({"post_aa", "Look", "Post anti-aliasing", "Filter on the final image.", Kind::Choice, ActionId::None,
         {C("off", "Off", {Ini("swap_post_effect", "")}), C("fxaa", "FXAA", {Ini("swap_post_effect", "1")}),
          C("fxaa-x", "FXAA extreme", {Ini("swap_post_effect", "2")}, "Softens the picture.")},
         0, true, "GPU cost on the NUC: GUESS, not measured"});
    add({"shadow_soft", "Look", "Soften shadows", "Bilinear filtering of shadow maps.", Kind::Choice, ActionId::None,
         {C("off", "Off", {Ini("shadow_filter_linear", "")}), C("on", "On", {Ini("shadow_filter_linear", "1")})}, 0, true,
         ""});
    add({"replay_shadows", "Look", "Shadows in replays", "Shadow maps in replays and cutscenes (gameplay has none).",
         Kind::Choice, ActionId::None,
         {C("on", "On", {}),
          C("off", "Off", {Env("REX_NHL_NO_REPLAY_SHADOWS", "1")},
            "Measured: slow replay scenes much steadier (p25 69 -> 101 fps); the replay picture loses its shadows; not yet judged visually")},
         0, true, "p25 69 -> 101, p5 52 -> 75-99 at the same median (2 runs, docs/worklog.md 2026-11-06)"});
    add({"hud", "Look", "FPS graph", "MangoHud overlay with fps and frame times.", Kind::Choice, ActionId::None,
         {C("on", "On", {Env("NHL_HUD", "1")}), C("off", "Off", {Env("NHL_HUD", "0")})}, 0, true, ""});

    add({"fullscreen", "Display", "Window", "Borderless fullscreen or a window.", Kind::Choice, ActionId::None,
         {C("window", "Window", {Ini("fullscreen", "")}), C("full", "Borderless fullscreen", {Ini("fullscreen", "1")})},
         0, true, ""});
    add({"monitor", "Display", "Monitor", "Which screen the window opens on.", Kind::Choice, ActionId::None,
         {C("0", "Default", {Ini("monitor", "")}), C("1", "Monitor 1", {Ini("monitor", "1")}),
          C("2", "Monitor 2", {Ini("monitor", "2")}), C("3", "Monitor 3", {Ini("monitor", "3")})},
         0, true, ""});

    add({"transfers", "Advanced", "Skip pointless render-target copies",
         "Skip EDRAM transfers into areas the next draw overwrites.", Kind::Choice, ActionId::None,
         {C("on", "On", {Env("REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS", "true")}),
          C("off", "Off", {Env("REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS", "false")}, "Slower. Use only if a pass looks wrong.")},
         0, true, "measured: presented fps p5 36 -> 60 (2 interleaved runs each)"});
    add({"prepass", "Advanced", "Upload pre-pass", "Record memory uploads into a pre-pass.", Kind::Choice, ActionId::None,
         {C("on", "On", {Env("REX_NHL_UPLOAD_PREPASS", "true")}),
          C("off", "Off", {Env("REX_NHL_UPLOAD_PREPASS", "false")}, "Slower. Use only if geometry looks stale.")},
         0, true, "measured: frames <=17 ms 92.7/78.9 % -> 99.7/91.5 %"});
    add({"pagestate", "Advanced", "Re-upload all guest memory", "Re-upload every guest buffer each frame.", Kind::Choice,
         ActionId::None,
         {C("off", "Off", {Env("REX_CLEAR_MEMORY_PAGE_STATE", "false")}),
          C("on", "On", {Env("REX_CLEAR_MEMORY_PAGE_STATE", "true")}, "Slower: fps median 46 with it v 61 without (measured).")},
         0, true, "measured as above; turn on only if stale geometry shows up"});
    add({"placeholder", "Advanced", "New shaders", "What to draw while a new shader compiles.", Kind::Choice, ActionId::None,
         {C("wait", "Wait for it", {Env("REX_NHL_ASYNC_NO_PLACEHOLDER", "true")}),
          C("placeholder", "Draw a placeholder", {Env("REX_NHL_ASYNC_NO_PLACEHOLDER", "false")},
            "Can show wrong frames; more freezes were seen with this (P17).")},
         0, true, "measured: gaps in gameplay sum 2310/2182 -> 1976/1465 ms"});
    return o;
  }();
  return kOpts;
}

std::vector<std::string> PageNames(const std::vector<Option>& opts) {
  std::vector<std::string> p;
  for (const auto& o : opts)
    if (std::find(p.begin(), p.end(), o.page) == p.end()) p.push_back(o.page);
  return p;
}

// ---- Settings --------------------------------------------------------------------------------------------------------

int Settings::Index(const Option& o) const {
  auto it = values_.find(o.id);
  if (it == values_.end()) return o.def;
  for (size_t i = 0; i < o.choices.size(); ++i)
    if (o.choices[i].value == it->second) return int(i);
  return o.def;
}

void Settings::SetIndex(const Option& o, int idx) {
  if (o.kind != Kind::Choice || idx < 0 || idx >= int(o.choices.size())) return;
  if (idx == o.def) values_.erase(o.id);
  else values_[o.id] = o.choices[size_t(idx)].value;
}

void Settings::ResetAll() { values_.clear(); }

int Settings::ParseText(const std::string& text) {
  values_.clear();
  unknown_.clear();
  warnings_ = 0;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) { ++warnings_; continue; }
    std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
    const Option* known = nullptr;
    for (const auto& o : *opts_)
      if (o.id == k && o.kind == Kind::Choice) known = &o;
    if (!known) { unknown_[k] = v; continue; }
    bool ok = false;
    for (const auto& c : known->choices) ok |= (c.value == v);
    if (!ok) { ++warnings_; continue; }  // bad value: fall back to default for that option only
    values_[k] = v;
  }
  for (auto it = values_.begin(); it != values_.end();) {  // an explicit default is normalised away
    const Option* o = nullptr;
    for (const auto& x : *opts_) if (x.id == it->first) o = &x;
    if (o && o->choices[size_t(o->def)].value == it->second) it = values_.erase(it);
    else ++it;
  }
  return warnings_;
}

std::string Settings::ToText() const {
  std::string s = "# NHL Legacy pre-game menu. Only non-default choices are listed; delete a line to reset it.\n";
  s += "version=1\n";
  for (const auto& [k, v] : values_) s += k + "=" + v + "\n";
  for (const auto& [k, v] : unknown_)
    if (k != "version") s += k + "=" + v + "\n";
  return s;
}

int Settings::Load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { ParseText(""); return 0; }
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseText(ss.str());
}

bool Settings::Save(const std::string& path) const {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << ToText();
    f.flush();
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// ---- apply -----------------------------------------------------------------------------------------------------------

ApplyReport ApplyAll(const std::vector<Option>& opts, const Settings& s, Applier& out, const Settings* initial) {
  ApplyReport r;
  for (const auto& o : opts) {
    if (o.kind != Kind::Choice) continue;
    // Ini keys belong to the player once set, so a default choice leaves the ini untouched, unless the player moved this
    // option back to its default during this session (then the key is erased so the game default applies again).
    const bool ini_wanted = !s.IsDefault(o) || (initial && !initial->IsDefault(o));
    for (const auto& a : o.choices[size_t(s.Index(o))].assigns) {
      if (a.target == Target::Env) {
        if (out.EnvIsSet(a.name)) ++r.env_kept;
        else { out.SetEnv(a.name, a.value); ++r.env_set; }
      } else if (a.target == Target::Ini) {
        if (!ini_wanted) continue;
        out.SetIni(a.name, a.value);
        ++r.ini_set;
      }
    }
  }
  return r;
}

// ---- navigation ------------------------------------------------------------------------------------------------------

uint32_t StickToPad(float lx, float ly, float dz) {
  uint32_t b = 0;
  if (ly > dz) b |= kUp;
  if (ly < -dz) b |= kDown;
  if (lx < -dz) b |= kLeft;
  if (lx > dz) b |= kRight;
  return b;
}

Menu::Menu(const std::vector<Option>& opts, Settings initial, MenuConfig cfg)
    : opts_(&opts), initial_(initial), cur_(std::move(initial)), cfg_(cfg), pages_(PageNames(opts)) {}

std::vector<const Option*> Menu::PageOptions() const {
  std::vector<const Option*> v;
  for (const auto& o : *opts_)
    if (!pages_.empty() && o.page == pages_[page_]) v.push_back(&o);
  return v;
}

void Menu::Fix() {
  const size_t n = PageOptions().size();
  if (n == 0) { cursor_ = scroll_ = 0; return; }
  cursor_ = std::min(cursor_, n - 1);
  const size_t vis = size_t(std::max(1, cfg_.visible_rows));
  if (cursor_ < scroll_) scroll_ = cursor_;
  if (cursor_ >= scroll_ + vis) scroll_ = cursor_ + 1 - vis;
  if (n <= vis) scroll_ = 0;
}

void Menu::Move(int d) {
  const size_t n = PageOptions().size();
  if (n == 0) return;
  cursor_ = size_t((int(cursor_) + d + int(n)) % int(n));
  Fix();
}

void Menu::Page(int d) {
  if (pages_.empty()) return;
  page_ = size_t((int(page_) + d + int(pages_.size())) % int(pages_.size()));
  cursor_ = scroll_ = 0;
}

void Menu::Change(int d, bool wrap) {
  auto v = PageOptions();
  if (v.empty()) return;
  const Option& o = *v[cursor_];
  if (o.kind != Kind::Choice) return;
  int n = int(o.choices.size()), i = cur_.Index(o) + d;
  if (wrap) i = (i + n) % n;
  else i = std::clamp(i, 0, n - 1);
  cur_.SetIndex(o, i);
}

void Menu::Activate() {
  auto v = PageOptions();
  if (v.empty()) return;
  const Option& o = *v[cursor_];
  if (o.kind == Kind::Action) {
    if (o.action == ActionId::ResetAll) cur_.ResetAll();
  } else {
    Change(+1, true);
  }
}

void Menu::Press(uint32_t bit) {
  switch (bit) {
    case kUp: Move(-1); break;
    case kDown: Move(+1); break;
    case kLeft: Change(-1, true); break;
    case kRight: Change(+1, true); break;
    case kA: Activate(); break;
    case kLB: Page(-1); break;
    case kRB: Page(+1); break;
    case kY: {  // reset the focused option
      auto v = PageOptions();
      if (!v.empty() && v[cursor_]->kind == Kind::Choice) cur_.SetIndex(*v[cursor_], v[cursor_]->def);
      break;
    }
    case kStart: out_ = Outcome::StartGame; break;
    case kB: case kBack: out_ = Outcome::Discard; break;
    default: break;
  }
}

Outcome Menu::Update(uint32_t pad, uint64_t now) {
  if (out_ != Outcome::Running) return out_;
  if (!started_) {  // a button still held from the launch press must be let go before it counts
    started_ = true;
    ignore_ = pad;
    last_input_ms_ = now;
  }
  ignore_ &= pad;
  const uint32_t eff = pad & ~ignore_;
  const uint32_t pressed = eff & ~prev_;
  prev_ = eff;
  if (eff) last_input_ms_ = now;
  for (uint32_t bit = 1; bit <= kBack && out_ == Outcome::Running; bit <<= 1)
    if (pressed & bit) Press(bit);
  // hold-to-repeat for d-pad only
  const uint32_t dir = eff & (kUp | kDown | kLeft | kRight);
  if (pressed & dir) { repeating_ = pressed & dir; repeat_at_ = now + cfg_.repeat_first_ms; }
  else if (!dir) repeating_ = 0;
  else if (repeating_ && now >= repeat_at_ && out_ == Outcome::Running) {
    for (uint32_t bit : {uint32_t(kUp), uint32_t(kDown), uint32_t(kLeft), uint32_t(kRight)})
      if (repeating_ & bit & eff) Press(bit);
    repeat_at_ = now + cfg_.repeat_ms;
  }
  if (out_ == Outcome::Running && cfg_.auto_start_ms && now - last_input_ms_ >= cfg_.auto_start_ms)
    out_ = Outcome::TimedOut;
  return out_;
}

// ---- view ------------------------------------------------------------------------------------------------------------

View BuildView(const Menu& m, uint64_t now_ms) {
  View v;
  v.title = "NHL Legacy - Settings";
  v.tabs = m.pages();
  v.tab = m.page();
  auto opts = m.PageOptions();
  const size_t vis = size_t(std::max(1, m.config().visible_rows));
  const size_t a = m.scroll(), b = std::min(opts.size(), a + vis);
  v.more_above = a > 0;
  v.more_below = b < opts.size();
  for (size_t i = a; i < b; ++i) {
    const Option& o = *opts[i];
    ViewRow r;
    r.label = o.label;
    r.selected = (i == m.cursor());
    r.action = o.kind == Kind::Action;
    if (!r.action) {
      r.value = o.choices[size_t(m.settings().Index(o))].label;
      r.changed = !m.settings().IsDefault(o);
    }
    v.rows.push_back(std::move(r));
  }
  if (!opts.empty()) {
    const Option& f = *opts[m.cursor()];
    v.hint = f.help;
    if (f.kind == Kind::Choice) {
      v.warn = f.choices[size_t(m.settings().Index(f))].warn;
      if (f.restart) v.note = "Takes effect when the game starts.";
    }
  }
  v.footer = "A change  Y default  LB/RB page  START play  B skip changes";
  if (m.config().auto_start_ms) {
    const uint64_t idle = m.idle_ms(now_ms);
    v.countdown_s = idle >= m.config().auto_start_ms ? 0 : int((m.config().auto_start_ms - idle + 999) / 1000);
  }
  return v;
}

// Word wrap to `cols` columns; continuation lines get `indent`. Words longer than a line are cut.
static std::vector<std::string> Wrap(const std::string& text, int cols, const std::string& first = "", const std::string& indent = "") {
  std::vector<std::string> out;
  std::string line = first;
  bool has_word = false;
  const size_t width = size_t(std::max(cols, int(indent.size()) + 1));
  std::istringstream is(text);
  std::string w;
  while (is >> w) {
    while (line.size() + (has_word ? 1 : 0) + w.size() > width) {
      if (has_word) { out.push_back(line); line = indent; has_word = false; continue; }
      const size_t room = width - line.size();  // word alone is too long: cut it
      out.push_back(line + w.substr(0, room));
      w = w.substr(room);
      line = indent;
    }
    if (w.empty()) continue;
    line += (has_word ? " " : "") + w;
    has_word = true;
  }
  if (has_word) out.push_back(line);
  return out;
}

std::vector<std::string> RenderLines(const View& v, int cols) {
  auto fit = [&](std::string s) {
    if (int(s.size()) > cols) s = s.substr(0, size_t(cols));
    return s;
  };
  std::vector<std::string> out;
  out.push_back(fit(v.title));
  std::string tabs;
  for (size_t i = 0; i < v.tabs.size(); ++i) tabs += (i == v.tab ? "[" + v.tabs[i] + "]" : " " + v.tabs[i] + " ") + " ";
  out.push_back(fit(tabs));
  out.push_back(std::string(size_t(cols), '-'));
  out.push_back(v.more_above ? fit("   ^ more") : "");
  for (const auto& r : v.rows) {
    std::string left = std::string(r.selected ? "> " : "  ") + r.label;
    std::string right = r.action ? "" : (r.changed ? "*" : " ") + r.value;
    int pad = cols - int(left.size()) - int(right.size());
    out.push_back(fit(left + std::string(size_t(std::max(1, pad)), ' ') + right));
  }
  out.push_back(v.more_below ? fit("   v more") : "");
  out.push_back(std::string(size_t(cols), '-'));
  auto add = [&](const std::vector<std::string>& w) { out.insert(out.end(), w.begin(), w.end()); };
  add(Wrap(v.hint, cols));
  add(Wrap(v.warn, cols, "! ", "  "));
  add(Wrap(v.note, cols));
  if (v.countdown_s >= 0) out.push_back(fit("Starting in " + std::to_string(v.countdown_s) + " s (press any button to stay)"));
  out.push_back(fit(v.footer));
  return out;
}

}  // namespace nhl::pregame
