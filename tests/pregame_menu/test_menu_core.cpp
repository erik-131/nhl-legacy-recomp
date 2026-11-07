// Unit tests for src/pregame_menu/menu_core. Build and run: tools/pregame_menu/run_tests.sh
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "../../src/pregame_menu/menu_core.h"

using namespace nhl::pregame;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Rec : Applier {
  std::set<std::string> preset;
  std::map<std::string, std::string> env, ini;
  bool EnvIsSet(const std::string& n) override { return preset.count(n) != 0; }
  void SetEnv(const std::string& n, const std::string& v) override { env[n] = v; }
  void SetIni(const std::string& n, const std::string& v) override { ini[n] = v; }
};

static const Option& Opt(const std::string& id) {
  for (const auto& o : DefaultOptions()) if (o.id == id) return o;
  std::printf("no option %s\n", id.c_str()); std::exit(2);
}

// drives the menu: press = one frame down, one frame up
static Outcome Tap(Menu& m, uint32_t bit, uint64_t& t) {
  if (t == 0) m.Update(0, t);                              // menu opened with nothing held
  m.Update(bit, t += 16);
  return m.Update(0, t += 16);
}

static void TestModel() {
  const auto& opts = DefaultOptions();
  std::set<std::string> ids;
  for (const auto& o : opts) {
    CHECK(ids.insert(o.id).second);                       // unique ids
    CHECK(!o.label.empty() && !o.help.empty());
    if (o.kind == Kind::Choice) {
      CHECK(o.choices.size() >= 2);
      CHECK(o.def >= 0 && o.def < int(o.choices.size()));
      std::set<std::string> vs;
      for (const auto& c : o.choices) { CHECK(vs.insert(c.value).second); CHECK(!c.label.empty()); }
    }
  }
  CHECK(PageNames(opts).front() == "Start");
}

static void TestSettingsRoundTrip() {
  const auto& opts = DefaultOptions();
  Settings s(opts);
  CHECK(s.IsDefault(Opt("msaa")));
  s.SetIndex(Opt("msaa"), 1);
  s.SetIndex(Opt("boot"), 2);
  CHECK(!s.IsDefault(Opt("msaa")));
  Settings t(opts);
  CHECK(t.ParseText(s.ToText()) == 0);
  CHECK(t == s);
  CHECK(t.Index(Opt("msaa")) == 1 && t.Index(Opt("boot")) == 2);
  s.SetIndex(Opt("msaa"), 0);                              // back to default => no longer stored
  CHECK(s.ToText().find("msaa") == std::string::npos);
  s.SetIndex(Opt("msaa"), 99);                             // out of range ignored
  CHECK(s.Index(Opt("msaa")) == 0);
}

static void TestParseRobustness() {
  const auto& opts = DefaultOptions();
  Settings s(opts);
  int w = s.ParseText("# c\n\n  msaa = 1x \nboot=bogus\njunk line\nfuture_key=7\nss=2x\r\nfence=sleep\n");
  CHECK(w == 2);                                           // bogus value + junk line
  CHECK(s.Index(Opt("msaa")) == 1);
  CHECK(s.Index(Opt("boot")) == Opt("boot").def);          // bad value -> default for that option only
  CHECK(s.Index(Opt("ss")) == 1);                          // CRLF tolerated
  CHECK(s.IsDefault(Opt("fence")));                        // explicit default normalised away
  CHECK(s.ToText().find("future_key=7") != std::string::npos);  // unknown key survives a save
  Settings e(opts);
  CHECK(e.ParseText("") == 0 && e.IsDefault(Opt("msaa")));
  CHECK(e.ParseText("\xff\xfe\x00 garbage") >= 0);         // binary junk must not crash
}

static void TestFiles() {
  const auto& opts = DefaultOptions();
  const std::string path = "/tmp/pregame_menu_test.ini";
  std::remove(path.c_str());
  Settings s(opts);
  CHECK(s.Load(path) == 0 && s.IsDefault(Opt("msaa")));    // missing file = defaults
  s.SetIndex(Opt("cap"), 1);
  CHECK(s.Save(path));
  Settings t(opts);
  CHECK(t.Load(path) == 0 && t == s);
  CHECK(!s.Save("/nonexistent-dir/x.ini"));                // failure reported, no crash
  FILE* f = std::fopen((path + ".tmp").c_str(), "rb");
  CHECK(f == nullptr);                                     // temp file renamed away
  if (f) std::fclose(f);
  std::remove(path.c_str());
}

static void TestApply() {
  const auto& opts = DefaultOptions();
  Settings s(opts);
  Rec r;
  ApplyReport rep = ApplyAll(opts, s, r);
  // all defaults reproduce the launcher's exports
  CHECK(r.env["REX_NHL_QUICK_START"] == "true" && r.env["NHL_BOOT_CLOCK"] == "3" && r.env["NHL_QUICK_MOVIE_FRAMES"] == "12");
  CHECK(r.env["REX_NHL_FENCE_POLL"] == "3" && r.env["NHL_SWAP_INTERVAL"] == "0");
  CHECK(r.env["REX_NHL_UPLOAD_PREPASS"] == "true" && r.env["REX_CLEAR_MEMORY_PAGE_STATE"] == "false");
  CHECK(r.env["REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS"] == "true" && r.env["REX_NHL_ASYNC_NO_PLACEHOLDER"] == "true");
  CHECK(r.ini.empty() && rep.ini_set == 0);                // all defaults: nhl_enhancements.ini is left untouched
  CHECK(rep.env_set > 0 && rep.env_kept == 0);

  s.SetIndex(Opt("msaa"), 1);
  s.SetIndex(Opt("ss"), 1);
  Rec r2;
  r2.preset.insert("REX_NHL_FENCE_POLL");                  // launcher/dev override wins
  rep = ApplyAll(opts, s, r2);
  CHECK(r2.env["REX_NHL_FORCE_HOST_MSAA_1X"] == "true");
  CHECK(r2.ini["supersampling"] == "2");
  CHECK(r2.ini.count("fullscreen") == 0 && r2.ini.count("edge_aa_mode") == 0);  // default options write nothing
  {  // the player moved ss back to default in this session: erase just that key
    Settings init(opts); init.SetIndex(Opt("ss"), 1);
    Settings now(opts);
    Rec r4;
    ApplyAll(opts, now, r4, &init);
    CHECK(r4.ini.size() == 1 && r4.ini.count("supersampling") == 1 && r4.ini["supersampling"].empty());
    Rec r5;                                                // nothing changed, ss saved non-default: rewritten, rest untouched
    ApplyAll(opts, init, r5, &init);
    CHECK(r5.ini.size() == 1 && r5.ini["supersampling"] == "2");
  }
  CHECK(r2.env.count("REX_NHL_FENCE_POLL") == 0 && rep.env_kept == 1);
  CHECK(r.env.count("REX_NHL_NO_REPLAY_SHADOWS") == 0);   // default On: nothing exported
  {
    Settings off(opts);
    off.SetIndex(Opt("replay_shadows"), 1);
    Rec ro;
    ApplyAll(opts, off, ro);
    CHECK(ro.env["REX_NHL_NO_REPLAY_SHADOWS"] == "1");
  }
  Rec r3;
  Settings only_menu(opts);
  only_menu.SetIndex(Opt("show"), 1);
  ApplyAll(opts, only_menu, r3);
  CHECK(r3.env.count("menu.show") == 0 && r3.ini.count("menu.show") == 0);  // Target::Menu is not exported
}

static void TestNavigation() {
  const auto& opts = DefaultOptions();
  uint64_t t = 0;
  Menu m(opts, Settings(opts));
  CHECK(m.page() == 0 && m.cursor() == 0);
  CHECK(Tap(m, kDown, t) == Outcome::Running && m.cursor() == 1);
  CHECK(Tap(m, kUp, t) == Outcome::Running && m.cursor() == 0);
  Tap(m, kUp, t);                                          // wraps
  CHECK(m.cursor() == m.PageOptions().size() - 1);
  Tap(m, kDown, t);
  CHECK(m.cursor() == 0);
  // value change: right/A cycle, left goes back, wraps
  const Option& boot = *m.PageOptions()[0];
  Tap(m, kRight, t); CHECK(m.settings().Index(boot) == 1);
  Tap(m, kA, t);     CHECK(m.settings().Index(boot) == 2);
  Tap(m, kRight, t); CHECK(m.settings().Index(boot) == 0);
  Tap(m, kLeft, t);  CHECK(m.settings().Index(boot) == 2);
  Tap(m, kY, t);     CHECK(m.settings().Index(boot) == boot.def);  // Y = default for this option
  CHECK(!m.dirty());
  // pages
  Tap(m, kRB, t); CHECK(m.page() == 1 && m.cursor() == 0);
  Tap(m, kLB, t); CHECK(m.page() == 0);
  Tap(m, kLB, t); CHECK(m.page() == m.pages().size() - 1);  // wraps
  // reset-all action row (Start page, last row)
  Tap(m, kRB, t); CHECK(m.page() == 0);
  Tap(m, kRight, t); CHECK(m.dirty());
  Tap(m, kUp, t);                                          // wrap to last row = reset action
  Tap(m, kA, t); CHECK(!m.dirty());
  Tap(m, kLeft, t); CHECK(!m.dirty());                     // left/right on an action row does nothing
  // Start is ONE press and finishes
  CHECK(Tap(m, kStart, t) == Outcome::StartGame);
  CHECK(m.Update(kDown, t + 16) == Outcome::StartGame);    // stays finished
}

static void TestDiscardAndHeldStart() {
  const auto& opts = DefaultOptions();
  uint64_t t = 0;
  Menu m(opts, Settings(opts));
  // Start was held from the launch: ignored until released, then a new press counts
  CHECK(m.Update(kStart, t += 16) == Outcome::Running);
  CHECK(m.Update(kStart, t += 16) == Outcome::Running);
  CHECK(m.Update(0, t += 16) == Outcome::Running);
  CHECK(m.Update(kStart, t += 16) == Outcome::StartGame);

  Menu d(opts, Settings(opts));
  t = 0;
  Tap(d, kRight, t);
  CHECK(d.dirty());
  CHECK(Tap(d, kB, t) == Outcome::Discard);                // B: caller must not save d.settings()
  CHECK(!(d.settings() == d.initial()));                   // the caller sees what would have been saved
  Menu b(opts, Settings(opts));
  t = 0;
  CHECK(Tap(b, kBack, t) == Outcome::Discard);
}

static void TestRepeatAndStick() {
  const auto& opts = DefaultOptions();
  Menu m(opts, Settings(opts));
  uint64_t t = 1000;
  m.Update(0, t);
  m.Update(kRB, t += 16); m.Update(0, t += 16);            // Performance page: 5 rows
  m.Update(kDown, t += 16); CHECK(m.cursor() == 1);        // first move on press
  m.Update(kDown, t += 100); CHECK(m.cursor() == 1);       // no repeat before 400 ms
  m.Update(kDown, t += 350); CHECK(m.cursor() == 2);       // first repeat
  m.Update(kDown, t += 50);  CHECK(m.cursor() == 2);
  m.Update(kDown, t += 80);  CHECK(m.cursor() == 3);       // then every 120 ms
  m.Update(0, t += 16);
  m.Update(kDown, t += 16);  CHECK(m.cursor() == 4);
  CHECK(StickToPad(0.9f, 0.0f) == kRight);
  CHECK(StickToPad(-0.9f, 0.9f) == (kLeft | kUp));
  CHECK(StickToPad(0.3f, -0.3f) == 0);                     // inside the dead zone
}

static void TestScrollAndTimeout() {
  const auto& opts = DefaultOptions();
  MenuConfig cfg; cfg.visible_rows = 2; cfg.auto_start_ms = 5000;
  Menu m(opts, Settings(opts), cfg);
  uint64_t t = 0;
  Tap(m, kDown, t); Tap(m, kDown, t);                      // third row of the Start page
  CHECK(m.cursor() == 2 && m.scroll() == 1);
  View v = BuildView(m, t);
  CHECK(v.rows.size() == 2 && v.more_above && !v.more_below && v.rows.back().selected);
  CHECK(v.countdown_s >= 4);
  CHECK(m.Update(0, t + 4000) == Outcome::Running);
  CHECK(m.Update(0, t + 6000) == Outcome::TimedOut);
  Menu n(opts, Settings(opts), cfg);                       // any input restarts the wait
  t = 0; n.Update(0, 0); n.Update(kDown, 4000); n.Update(0, 4016);
  CHECK(n.Update(0, 8000) == Outcome::Running);
  CHECK(n.Update(0, 9100) == Outcome::TimedOut);
}

static void TestView() {
  const auto& opts = DefaultOptions();
  uint64_t t = 0;
  Menu m(opts, Settings(opts));
  Tap(m, kRB, t);                                          // Performance
  CHECK(BuildView(m).tabs[BuildView(m).tab] == "Performance");
  Tap(m, kRight, t);                                       // msaa -> 1x, has a warning
  View v = BuildView(m, t);
  CHECK(v.rows[0].changed && v.rows[0].value == "Off (1x)");
  CHECK(!v.warn.empty() && !v.hint.empty() && !v.note.empty());
  auto lines = RenderLines(v, 64);
  bool marker = false;
  for (const auto& l : lines) { CHECK(int(l.size()) <= 64); marker |= l.rfind("> ", 0) == 0; }
  CHECK(marker);
  CHECK(!RenderLines(v, 20).empty());                      // narrow width must not crash or overflow
  for (const auto& l : RenderLines(v, 20)) CHECK(int(l.size()) <= 20);
}

static void TestWrap() {
  const auto& opts = DefaultOptions();
  View v;
  v.hint = "Draw the game's main pass at 1 sample instead of the game's 2x MSAA, which saves a lot of GPU time on weak cards";
  v.warn = "Edges look more jagged and thin lines may shimmer when moving";
  v.note = "Takes effect when the game starts.";
  for (int cols : {64, 40, 20, 8}) {
    auto lines = RenderLines(v, cols);
    std::string all;
    int bang = 0;
    for (const auto& l : lines) { CHECK(int(l.size()) <= cols); all += l + "\n"; bang += l.rfind("! ", 0) == 0; }
    CHECK(bang == 1);                                      // only the first warning line carries the "!"
    if (cols >= 20) {  // no word lost (words are shorter than the width)
      for (const char* w : {"MSAA,", "weak", "shimmer", "moving", "starts."}) CHECK(all.find(w) != std::string::npos);
    }
  }
  CHECK(RenderLines(v, 64).size() > RenderLines(View{}, 64).size() + 1);  // hint and warning wrapped onto extra lines
  v.hint = std::string(150, 'x');                          // a word longer than a line is cut, nothing lost
  size_t xs = 0;
  for (const auto& l : RenderLines(v, 30)) xs += size_t(std::count(l.begin(), l.end(), 'x'));
  CHECK(xs == 150);
  // pad path through the core: A on 'Boot screens' cycles it, Start closes
  uint64_t t = 0;
  Menu m(opts, Settings(opts));
  const Option& boot = *m.PageOptions()[0];
  CHECK(Tap(m, kA, t) == Outcome::Running && m.settings().Index(boot) == 1 && m.dirty());
  CHECK(Tap(m, kStart, t) == Outcome::StartGame);
}

int main() {
  TestModel(); TestSettingsRoundTrip(); TestParseRobustness(); TestFiles(); TestApply();
  TestNavigation(); TestDiscardAndHeldStart(); TestRepeatAndStick(); TestScrollAndTimeout(); TestView(); TestWrap();
  std::printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
