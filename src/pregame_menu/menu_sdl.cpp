#include "pregame_menu/menu_sdl.h"

#include <cstdio>
#include <SDL3/SDL.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <fmt/format.h>

#include "pregame_menu/menu_core.h"
#include "renderer/core/nhl_settings.h"

// The log file sink doesn't exist yet when the menu runs (it is created with the runtime), so every line is
// also written to stderr.
#define MENU_LOG(level, ...)                                                          \
  do {                                                                                \
    REXLOG_##level(__VA_ARGS__);                                                      \
    std::fprintf(stderr, "%s\n", fmt::format(__VA_ARGS__).c_str());                   \
  } while (0)

namespace nhl::pregame {
namespace {

constexpr int kWinW = 1024, kWinH = 640;  // font 7x13 at 2x: 14 px per char -> 64 columns = 896 px; 28 px per line
constexpr int kScale = 2, kCols = 64;

class EnvIniApplier : public Applier {
 public:
  bool EnvIsSet(const std::string& n) override { return std::getenv(n.c_str()) != nullptr; }
  void SetEnv(const std::string& n, const std::string& v) override {
#ifdef _WIN32
    _putenv_s(n.c_str(), v.c_str());
#else
    ::setenv(n.c_str(), v.c_str(), 1);
#endif
    MENU_LOG(INFO, "[nhl-menu] env {}={}", n, v);
  }
  void SetIni(const std::string& n, const std::string& v) override {
    if (v.empty()) nhl::EraseSetting(n); else nhl::SaveSetting(n, v);
    MENU_LOG(INFO, "[nhl-menu] ini {}={}", n, v.empty() ? "(erased)" : v);
  }
};

struct Pads {
  std::vector<SDL_Gamepad*> open;
  void Refresh() {  // open every attached pad (the Wolf vpad and a client pad may both exist)
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; ++i) {
      SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
      if (!g) g = SDL_OpenGamepad(ids[i]);
      if (g) {
        bool known = false;
        for (auto* o : open) known = known || o == g;
        if (!known) open.push_back(g);
      }
    }
    SDL_free(ids);
  }
  void Drop(SDL_JoystickID id) {
    for (size_t i = 0; i < open.size(); ++i)
      if (SDL_GetGamepadID(open[i]) == id) { SDL_CloseGamepad(open[i]); open.erase(open.begin() + long(i)); return; }
  }
  uint32_t Bits() const {
    static const struct { SDL_GamepadButton b; uint32_t bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
        {SDL_GAMEPAD_BUTTON_SOUTH, kA},          {SDL_GAMEPAD_BUTTON_EAST, kB},
        {SDL_GAMEPAD_BUTTON_WEST, kX},           {SDL_GAMEPAD_BUTTON_NORTH, kY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRB},
        {SDL_GAMEPAD_BUTTON_START, kStart},      {SDL_GAMEPAD_BUTTON_BACK, kBack}};
    uint32_t bits = 0;
    for (auto* g : open) {
      for (const auto& m : map) if (SDL_GetGamepadButton(g, m.b)) bits |= m.bit;
      const float lx = float(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)) / 32768.0f;
      const float ly = -float(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)) / 32768.0f;  // SDL +y is down
      bits |= StickToPad(lx, ly);
    }
    return bits;
  }
  ~Pads() { for (auto* g : open) SDL_CloseGamepad(g); }
};

uint32_t KeyBits() {
  const bool* k = SDL_GetKeyboardState(nullptr);
  if (!k) return 0;
  uint32_t b = 0;
  auto on = [&](SDL_Scancode s, uint32_t bit) { if (k[s]) b |= bit; };
  on(SDL_SCANCODE_UP, kUp);        on(SDL_SCANCODE_DOWN, kDown);
  on(SDL_SCANCODE_LEFT, kLeft);    on(SDL_SCANCODE_RIGHT, kRight);
  on(SDL_SCANCODE_RETURN, kA);     on(SDL_SCANCODE_ESCAPE, kB);
  on(SDL_SCANCODE_BACKSPACE, kY);  on(SDL_SCANCODE_Q, kLB);
  on(SDL_SCANCODE_E, kRB);         on(SDL_SCANCODE_F5, kStart);
  return b;
}

// NHL_MENU_SCRIPT="a,down,a,start": one press per token (2 frames down, 3 up); "wait" = 10 idle frames.
std::vector<uint32_t> ParseScript(const char* text) {
  static const struct { const char* n; uint32_t b; } names[] = {
      {"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight}, {"a", kA}, {"b", kB}, {"x", kX}, {"y", kY},
      {"lb", kLB}, {"rb", kRB}, {"start", kStart}, {"back", kBack}, {"wait", 0}};
  std::vector<uint32_t> frames(3, 0);  // pad bits per frame; idle lead-in, the menu ignores buttons held at its first frame
  std::string t(text ? text : "");
  for (size_t i = 0; i <= t.size();) {
    size_t j = t.find(',', i);
    if (j == std::string::npos) j = t.size();
    const std::string tok = t.substr(i, j - i);
    i = j + 1;
    for (const auto& n : names) {
      if (tok != n.n) continue;
      if (n.b == 0) frames.insert(frames.end(), 10, 0);
      else { frames.insert(frames.end(), 2, n.b); frames.insert(frames.end(), 3, 0); }
    }
  }
  return frames;
}

bool EnvEquals(const char* name, const char* v) {
  const char* e = std::getenv(name);
  return e && std::string(e) == v;
}

// ---- software text: ImGui's built-in default font (ProggyClean 13 px), copied out of a throwaway context -------------------
struct Glyph { int w = 0, h = 0, ox = 0, oy = 0; std::vector<uint8_t> a; };
struct Font {
  Glyph g[128];
  int adv = 7, line = 13;
  bool ok = false;
};

Font BakeFont() {
  Font f;
  IMGUI_CHECKVERSION();
  ImGuiContext* ctx = ImGui::CreateContext();
  if (!ctx) return f;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.Fonts->AddFontDefault();
  unsigned char* px = nullptr;
  int tw = 0, th = 0;
  io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
  ImFont* font = io.Fonts->Fonts.Size ? io.Fonts->Fonts[0] : nullptr;
  ImFontBaked* baked = font ? font->GetFontBaked(13.0f) : nullptr;
  if (px && baked) {
    f.adv = int(baked->FindGlyph('M')->AdvanceX + 0.5f);
    for (int c = 32; c < 127; ++c) {
      const ImFontGlyph* gl = baked->FindGlyph(ImWchar(c));
      if (!gl) continue;
      const int x0 = int(gl->U0 * float(tw) + 0.5f), x1 = int(gl->U1 * float(tw) + 0.5f);
      const int y0 = int(gl->V0 * float(th) + 0.5f), y1 = int(gl->V1 * float(th) + 0.5f);
      Glyph& o = f.g[c];
      o.w = x1 - x0; o.h = y1 - y0; o.ox = int(gl->X0); o.oy = int(gl->Y0);
      if (o.w <= 0 || o.h <= 0 || x0 < 0 || y0 < 0 || x1 > tw || y1 > th) { o.w = o.h = 0; continue; }
      o.a.resize(size_t(o.w) * size_t(o.h));
      for (int y = 0; y < o.h; ++y) for (int x = 0; x < o.w; ++x) o.a[size_t(y * o.w + x)] = px[(y0 + y) * tw + x0 + x];
    }
    f.ok = true;
  }
  ImGui::DestroyContext(ctx);
  return f;
}

struct Canvas {
  SDL_Surface* s;
  uint32_t Map(uint8_t r, uint8_t g, uint8_t b) const { return SDL_MapSurfaceRGB(s, r, g, b); }
  uint32_t* Px(int x, int y) const { return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(s->pixels) + y * s->pitch) + x; }
  void Fill(uint8_t r, uint8_t g, uint8_t b) const {
    SDL_FillSurfaceRect(s, nullptr, Map(r, g, b));
  }
  void Rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) const {
    SDL_Rect rc{x, y, w, h};
    SDL_FillSurfaceRect(s, &rc, Map(r, g, b));
  }
  // Alpha-blend one glyph, each source pixel enlarged to kScale x kScale.
  void Glyph(const Font& f, char ch, int x, int y, uint8_t r, uint8_t g, uint8_t b) const {
    const unsigned c = static_cast<unsigned char>(ch);
    if (c < 32 || c >= 127) return;
    const nhl::pregame::Glyph& gl = f.g[c];
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(s->format);
    for (int gy = 0; gy < gl.h; ++gy) for (int gx = 0; gx < gl.w; ++gx) {
      const unsigned a = gl.a[size_t(gy * gl.w + gx)];
      if (!a) continue;
      for (int sy = 0; sy < kScale; ++sy) for (int sx = 0; sx < kScale; ++sx) {
        const int px = x + (gl.ox + gx) * kScale + sx, py = y + (gl.oy + gy) * kScale + sy;
        if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
        uint32_t* p = Px(px, py);
        uint8_t br, bg, bb;
        SDL_GetRGB(*p, d, nullptr, &br, &bg, &bb);
        *p = Map(uint8_t((r * a + br * (255 - a)) / 255), uint8_t((g * a + bg * (255 - a)) / 255), uint8_t((b * a + bb * (255 - a)) / 255));
      }
    }
  }
};

// Draws into the window's own surface (the SDK's bundled SDL3 has no render support). Returns false if there is no surface.
bool Draw(SDL_Window* win, const Font& font, const Menu& m, uint64_t now) {
  SDL_Surface* surf = SDL_GetWindowSurface(win);
  if (!surf || SDL_BYTESPERPIXEL(surf->format) != 4 || !surf->pixels) return false;
  const Canvas cv{surf};
  const View v = BuildView(m, now);
  const std::vector<std::string> lines = RenderLines(v, kCols);
  cv.Fill(20, 22, 30);
  bool in_warn = false;
  const int step = (font.line + 1) * kScale, cell = font.adv * kScale, x0 = 8, y0 = 6;
  for (size_t i = 0; i < lines.size(); ++i) {
    // Line layout of RenderLines: 0 title, 1 tabs, 2 rule, 3 "more above", rows from 4, then the rest.
    uint8_t cr = 200, cg = 205, cb = 215;
    const int y = y0 + int(i) * step;
    if (i == 0) { cr = 255; cg = 255; cb = 255; }
    else if (i >= 4 && i < 4 + v.rows.size()) {
      const ViewRow& row = v.rows[i - 4];
      if (row.selected) { cv.Rect(2, y - 1, surf->w - 4, step, 48, 52, 78); cr = 255; cg = 220; cb = 80; }
      else if (row.changed) { cr = 130; cg = 190; cb = 255; }
    } else if (lines[i].rfind("! ", 0) == 0) { in_warn = true; cr = 255; cg = 150; cb = 60; }
    else if (in_warn && lines[i].rfind("  ", 0) == 0) { cr = 255; cg = 150; cb = 60; }  // wrapped warning line
    else in_warn = false;
    for (size_t c = 0; c < lines[i].size(); ++c) cv.Glyph(font, lines[i][c], x0 + int(c) * cell, y, cr, cg, cb);
  }
  return true;
}

}  // namespace

MenuResult RunPregameMenu() {
  if (!EnvEquals("NHL_MENU", "1")) return MenuResult::Disabled;  // default OFF until tested against the game

  const auto& opts = DefaultOptions();
  Settings saved(opts);
  const std::string ini = (rex::filesystem::GetExecutableFolder() / "nhl_pregame.ini").string();
  const int bad = saved.Load(ini);
  if (bad) MENU_LOG(WARN, "[nhl-menu] {} line(s) ignored in {}", bad, ini);

  const Option* show = nullptr;
  for (const auto& o : opts) if (o.id == "show") show = &o;
  const bool hold_mode = show && saved.Index(*show) != show->def;  // only choice 1 ("hold") differs from the default

  MenuResult result = MenuResult::Skipped;
  Settings final_settings = saved;  // what gets applied

  const bool video_ok = SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO);
  const bool pad_sys = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!video_ok) {
    MENU_LOG(WARN, "[nhl-menu] SDL video unavailable ({}); skipping menu", SDL_GetError());
    if (pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return MenuResult::Failed;
  }
  {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // pad events before the window has focus (Moonlight)
    Pads pads;
    SDL_PumpEvents();
    pads.Refresh();
    const bool have_kb = SDL_HasKeyboard();
    const bool have_input = !pads.open.empty() || have_kb || EnvEquals("NHL_MENU_FORCE", "1");  // FORCE: tests without a device
    const bool back_held = (pads.Bits() & kBack) || (KeyBits() & kB);
    if (!have_input) {
      MENU_LOG(INFO, "[nhl-menu] no pad or keyboard attached; skipping");
    } else if (hold_mode && !back_held) {
      MENU_LOG(INFO, "[nhl-menu] menu.show=hold and Back not held; skipping");
    } else {
      SDL_Window* win = SDL_CreateWindow("NHL Legacy - Settings", kWinW, kWinH, 0);
      const Font font = win ? BakeFont() : Font{};
      if (!win || !font.ok || !SDL_GetWindowSurface(win)) {
        MENU_LOG(WARN, "[nhl-menu] window/font/surface failed (win={} font={} err={}); skipping menu", win != nullptr, font.ok, SDL_GetError());
        result = MenuResult::Failed;
      } else {
        const char* dump = std::getenv("NHL_MENU_DUMP");
        const char* dump_all = std::getenv("NHL_MENU_DUMP_ALL");  // dir: one BMP per tab, then leave as a timeout
        const std::vector<uint32_t> script = ParseScript(std::getenv("NHL_MENU_SCRIPT"));
        size_t script_pos = 0;
        int all_settle = 0, all_hold = 0;
        size_t all_tab = 0;
        bool dumped = false, first_logged = false;
        MenuConfig cfg;
        const char* as = std::getenv("NHL_MENU_AUTOSTART_S");
        const long secs = as ? std::strtol(as, nullptr, 10) : 120;  // never wait forever; 0 = no timeout
        cfg.auto_start_ms = secs > 0 ? uint32_t(secs) * 1000u : 0u;
        Menu menu(opts, saved, cfg);
        Outcome out = Outcome::Running;
        const uint64_t t0 = SDL_GetTicks();
        while (out == Outcome::Running) {
          const uint64_t frame = SDL_GetTicks();
          SDL_Event e;
          while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) out = Outcome::Discard;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) pads.Refresh();
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) pads.Drop(e.gdevice.which);
          }
          uint32_t inject = script_pos < script.size() ? script[script_pos++] : 0;
          if (dump_all && all_hold > 0) { inject |= kRB; --all_hold; }
          if (inject) MENU_LOG(INFO, "[nhl-menu] scripted input bits {:#x} at {} ms", inject, frame - t0);
          if (out == Outcome::Running) out = menu.Update(pads.Bits() | KeyBits() | inject, frame - t0);
          if (!Draw(win, font, menu, frame - t0)) { MENU_LOG(WARN, "[nhl-menu] window surface lost: {}", SDL_GetError()); out = Outcome::Discard; }
          else {
            if (!first_logged && (first_logged = true)) MENU_LOG(INFO, "[nhl-menu] first frame drawn at SDL tick {} ms (window created {} ms earlier)", SDL_GetTicks(), SDL_GetTicks() - t0);
            if (dump && !dumped) { dumped = true; SDL_SaveBMP(SDL_GetWindowSurface(win), dump); MENU_LOG(INFO, "[nhl-menu] dumped first frame to {}", dump); }
            if (dump_all && all_hold == 0 && ++all_settle >= 3) {  // let the page settle, then dump this tab
              all_settle = 0;
              std::string name = menu.pages()[menu.page()];
              for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
              const std::string file = std::string(dump_all) + "/menu_" + name + ".bmp";
              SDL_SaveBMP(SDL_GetWindowSurface(win), file.c_str());
              MENU_LOG(INFO, "[nhl-menu] dumped tab {} to {}", menu.pages()[menu.page()], file);
              if (++all_tab >= menu.pages().size()) out = Outcome::TimedOut; else all_hold = 2;
            }
            SDL_UpdateWindowSurface(win);
          }
          const uint64_t spent = SDL_GetTicks() - frame;
          if (spent < 16) SDL_Delay(uint32_t(16 - spent));  // 60 Hz even if vsync does not block
        }
        // Let the closing press go before the game's quick-start sees input (it also ignores input for 3 s).
        for (uint64_t w = SDL_GetTicks(); SDL_GetTicks() - w < 500 && (pads.Bits() | KeyBits()); SDL_Delay(10)) SDL_PumpEvents();
        if (out == Outcome::StartGame) {
          final_settings = menu.settings();
          result = MenuResult::Started;
          if (menu.dirty() && !final_settings.Save(ini)) MENU_LOG(WARN, "[nhl-menu] could not save {}", ini);
        } else {
          result = out == Outcome::TimedOut ? MenuResult::TimedOut : MenuResult::Discarded;
        }
        MENU_LOG(INFO, "[nhl-menu] closed: {} after {} ms", out == Outcome::StartGame ? "start" : (result == MenuResult::TimedOut ? "timeout" : "discard"),
                    SDL_GetTicks() - t0);
      }
      if (win) SDL_DestroyWindow(win);
    }
  }
  if (pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);

  if (result == MenuResult::Failed) return result;
  EnvIniApplier applier;
  const ApplyReport rep = ApplyAll(opts, final_settings, applier, &saved);
  MENU_LOG(INFO, "[nhl-menu] applied: env set {}, kept (already set) {}, ini {}", rep.env_set, rep.env_kept, rep.ini_set);
  // cvar::ApplyEnvironment already ran before the app is created; run it again so the REX_* values set above take effect.
  if (rep.env_set > 0) rex::cvar::ApplyEnvironment();
  return result;
}

}  // namespace nhl::pregame
