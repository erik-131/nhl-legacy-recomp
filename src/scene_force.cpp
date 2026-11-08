// Scene forcing for benchmarks (opt-in, default off): starts the game straight in a match and runs scripted calls/pokes
// at fixed frame offsets, from a hook on the game's per-frame function (the one that also runs the Lua RunEachFrame
// string). Findings, env and recipes: docs/bench-scenes.md.
//
//   NHL_SCENE=live|shootout          enter a match without any menu route (live = exhibition match, shootout = shootout)
//   NHL_SCENE_TEAMS=H,A              team ids (default 5,12 = FLA v CAR)
//   NHL_SCENE_PADS=H,A               controller values passed to the start function (default 0,-1; -1,-1 = CPU v CPU)
//   NHL_SCENE_ENTER_FRAME=N          frame (counted from the first hook call) that enters the match when quick start is
//                                    off (default 12000); with quick start on: N frames after "main menu" (default 2500; 800 hangs the load: main menu not ready)
//   NHL_SCENE_REF=enter|play         what f of CALLS/POKE counts from: the match entry (default) or the first frame of live play (flow state 4)
//   NHL_SCENE_CALLS=f:addr:r3[:r4..r8],...   call a guest function f frames after the reference (hex addr, numbers base 0)
//   NHL_SCENE_POKE=f:addr:value[:bytes],...  write guest memory f frames after entering (bytes 1, 2 or 4)
//   NHL_SCENE_TRACE=1                log calls of a fixed list of game-flow/replay/goal functions (args, caller)
//   NHL_SCENE_LOG=0                  no once-a-second state line (flow state, period, clock)
#include "generated/default/nhllegacy_init.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/uio.h>
#include <unistd.h>
#include <cmath>
#include <rex/hook.h>
#include <rex/logging.h>

#include "quick_start.h"

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);
}

namespace {

struct Call {
  int64_t frame;
  uint32_t addr;
  uint32_t arg[6];  // r3..r8
  uint32_t token[6] = {};  // 1 = @ctrl (view controller), 2 = @replay (replay interface object, *(ctrl+1008))
  uint32_t off[6] = {};
  bool twice[6] = {};  // **ADDR+OFF: load the word at ADDR, then the word at that value + OFF
  uint32_t deref[6] = {};  // != 0: the argument is the u32 stored at that guest address, read when the call is made (syntax *0xADDR)
  bool done = false;
};
struct Poke {
  int64_t frame;
  uint32_t addr, value, bytes;
  bool done = false;
};

struct Config {
  std::string scene;
  uint32_t home = 5, away = 12;
  uint32_t pad_a = 0, pad_b = uint32_t(-1);
  int64_t enter_frame = -1;  // -1: derive
  bool log = true;
  std::vector<Call> calls;
  std::vector<Poke> pokes;
  std::vector<int64_t> dump_world;  // NHL_SCENE_DUMPWORLD=f,f,..: log the 44 entries of the object at g_world+3120 (research: find the puck)
  bool active = false;
};

std::vector<std::string> Split(const std::string& s, char c) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    const size_t q = s.find(c, p);
    out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  return out;
}

const Config& Cfg() {
  static const Config cfg = [] {
    Config c;
    const char* s = std::getenv("NHL_SCENE");
    if (!s || !*s) return c;
    c.scene = s;
    c.active = true;
    if (const char* t = std::getenv("NHL_SCENE_TEAMS")) {
      unsigned h, a;
      if (std::sscanf(t, "%u,%u", &h, &a) == 2) { c.home = h; c.away = a; }
    }
    if (const char* t = std::getenv("NHL_SCENE_PADS")) {
      int a, b;
      if (std::sscanf(t, "%d,%d", &a, &b) == 2) { c.pad_a = uint32_t(a); c.pad_b = uint32_t(b); }
    }
    if (const char* t = std::getenv("NHL_SCENE_ENTER_FRAME")) c.enter_frame = std::strtoll(t, nullptr, 0);
    if (const char* t = std::getenv("NHL_SCENE_LOG")) c.log = std::strcmp(t, "0") != 0;
    if (const char* t = std::getenv("NHL_SCENE_CALLS")) {
      for (const auto& item : Split(t, ',')) {
        const auto f = Split(item, ':');
        if (f.size() < 3) continue;
        Call k{};
        k.frame = std::strtoll(f[0].c_str(), nullptr, 0);
        k.addr = uint32_t(std::strtoul(f[1].c_str(), nullptr, 0));
        for (size_t i = 2; i < f.size() && i < 8; ++i) {
          if (f[i] == "@userplayer") k.token[i - 2] = 3;
          else if (f[i].rfind("**", 0) == 0) { k.deref[i - 2] = uint32_t(std::strtoul(f[i].c_str() + 2, nullptr, 0)); k.off[i - 2] = std::strtoul(std::strchr(f[i].c_str(), '+') ? std::strchr(f[i].c_str(), '+') + 1 : "0", nullptr, 0); k.twice[i - 2] = true; }
          else if (f[i] == "@ctrl") k.token[i - 2] = 1;
          else if (f[i] == "@replay") k.token[i - 2] = 2;
          else if (f[i][0] == '*') k.deref[i - 2] = uint32_t(std::strtoul(f[i].c_str() + 1, nullptr, 0));
          else k.arg[i - 2] = uint32_t(std::strtoll(f[i].c_str(), nullptr, 0));
        }
        c.calls.push_back(k);
      }
    }
    if (const char* t = std::getenv("NHL_SCENE_DUMPWORLD"))
      for (const auto& item : Split(t, ',')) c.dump_world.push_back(std::strtoll(item.c_str(), nullptr, 0));
    if (const char* t = std::getenv("NHL_SCENE_POKE")) {
      for (const auto& item : Split(t, ',')) {
        const auto f = Split(item, ':');
        if (f.size() < 3) continue;
        Poke k{};
        k.frame = std::strtoll(f[0].c_str(), nullptr, 0);
        k.addr = uint32_t(std::strtoul(f[1].c_str(), nullptr, 0));
        k.value = uint32_t(std::strtoll(f[2].c_str(), nullptr, 0));
        k.bytes = f.size() > 3 ? uint32_t(std::strtoul(f[3].c_str(), nullptr, 0)) : 4;
        c.pokes.push_back(k);
      }
    }
    return c;
  }();
  return cfg;
}

// Game-data block (docs/l2-rules.md, docs/l2-gameflow.md): clock, period, flow state.
constexpr uint32_t kGameData = 0x83C87508;  // +426 clock sec s16, +428 sub s16, +432 period kind s32, +436 index s32
constexpr uint32_t kFlow = 0x83C1D220;      // +0 flow state, +12 last stoppage reason

uint32_t CallGuest(PPCContext& ctx, uint8_t* base, uint32_t addr, const uint32_t* args, int n, bool quiet = false) {
  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(addr);
  if (!fn) {
    REXLOG_WARN("[nhl-scene] call 0x{:08X}: no such function", addr);
    return 0;
  }
  // Run on the thread's own context (like the Lua bindings do), then restore every register so the frame function goes on unchanged.
  const PPCContext saved = ctx;
  uint64_t* regs[6] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64, &ctx.r7.u64, &ctx.r8.u64};
  for (int i = 0; i < n && i < 6; ++i) *regs[i] = args[i];
  ctx.lr = 0x828ADFD8;
  fn(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  ctx = saved;
  if (!quiet) REXLOG_INFO("[nhl-scene] called 0x{:08X} -> r3=0x{:08X}", addr, ret);
  return ret;
}

std::atomic<uint32_t> g_world{0};  // r3 of the puck.cpp function 0x833676B8: object with 44 entries of 144 bytes at +3120
std::atomic<uint32_t> g_puck{0};  // candidate puck object (2nd arg of the block-shot function 0x83328180); world position at +112 (x across, y up, z along)
std::atomic<uint32_t> g_ctrl{0};  // `this` of the per-frame view/replay controller update (0x834D2A00); replay interface = *(ctrl+1008)

// ---- NHL_SCENE_FINDPUCK=1 (research): find the live puck by diffing float triples in the guest heap -----------------------------
// Stage 1 (faceoff, puck at rest at the centre): every 4-aligned (x,y,z) with |x|,|y|<1.5, 0<z<1.5, not all zero. Stage 2..4 (play): keep those that moved.
struct PuckFinder {
  std::vector<uint32_t> cand;
  std::vector<std::array<float, 3>> prev, orig;
  int stage = 0;
};
PuckFinder g_pf;
float F32(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
bool Mapped(uint8_t* base, uint32_t ga) {
  uint32_t tmp; iovec l{&tmp, 4}, r{base + ga, 4};
  return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 4;
}
void FindPuck(uint8_t* base, int stage) {
  if (stage == 1) {
    size_t pages = 0;
    {  // sanity: raw host read at base+ga must agree with the guest load
      const uint32_t t = REX_LOAD_U32(0x83C1D220), gp = REX_LOAD_U32(0x83C1E89C);
      uint32_t raw = 0, rawgp = 0; iovec l{&raw, 4}, r{base + 0x83C1D220, 4}; process_vm_readv(getpid(), &l, 1, &r, 1, 0);
      iovec l2{&rawgp, 4}, r2{base + 0x83C1E89C, 4}; process_vm_readv(getpid(), &l2, 1, &r2, 1, 0);
      REXLOG_INFO("[nhl-scene] findpuck sanity: flow guest={:08X} raw(bswap)={:08X} gp guest={:08X} raw={:08X}", t, __builtin_bswap32(raw), gp, __builtin_bswap32(rawgp));
    }
    std::vector<uint32_t> buf(1024);
    for (uint64_t pg = 0xA0000000ull; pg < 0xC0000000ull; pg += 4096) {
      iovec l{buf.data(), 4096}, r{base + pg, 4096};
      if (process_vm_readv(getpid(), &l, 1, &r, 1, 0) != 4096) continue;
      ++pages;
      for (uint32_t i = 0; i + 2 < 1024; ++i) {
        const float x = F32(__builtin_bswap32(buf[i])), y = F32(__builtin_bswap32(buf[i + 1])), z = F32(__builtin_bswap32(buf[i + 2]));
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) < 0.5f && y > 0.0005f && y < 0.5f && std::fabs(z) < 0.5f)  // entity position at object+112: (x across, y up, z along the rink)
          { g_pf.cand.push_back(uint32_t(pg) + i * 4); g_pf.prev.push_back({x, y, z}); g_pf.orig.push_back({x, y, z}); }
      }
    }
    REXLOG_INFO("[nhl-scene] findpuck stage1: mapped pages={} candidates={}", pages, g_pf.cand.size());
    return;
  }
  std::vector<uint32_t> keep;
  std::vector<std::array<float, 3>> nprev, norig;
  for (size_t i = 0; i < g_pf.cand.size(); ++i) {
    const uint32_t a = g_pf.cand[i];
    const float x = F32(REX_LOAD_U32(a)), y = F32(REX_LOAD_U32(a + 4)), z = F32(REX_LOAD_U32(a + 8));
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    if (!(std::fabs(x) < 60.f && y > -0.5f && y < 8.f && std::fabs(z) < 110.f)) continue;
    const auto& pv = g_pf.prev[i];
    if (std::fabs(x - pv[0]) + std::fabs(y - pv[1]) + std::fabs(z - pv[2]) < 0.05f) continue;  // did not move: not the puck
    if (stage >= 3 && std::fabs(x - g_pf.orig[i][0]) + std::fabs(z - g_pf.orig[i][2]) < 2.f) continue;  // the puck travels, stays low
    keep.push_back(a); nprev.push_back({x, y, z}); norig.push_back(g_pf.orig[i]);
  }
  g_pf.cand.swap(keep); g_pf.prev.swap(nprev); g_pf.orig.swap(norig);
  REXLOG_INFO("[nhl-scene] findpuck stage{}: {} candidates", stage, g_pf.cand.size());
  if (g_pf.cand.size() <= 1500 || stage >= 4)
    for (size_t i = 0; i < g_pf.cand.size() && i < 1500; ++i)
      REXLOG_INFO("[nhl-scene]   vt={:08X} cand {:08X}: {:.2f} {:.2f} {:.2f} from {:.2f} {:.2f} {:.2f}", REX_LOAD_U32(g_pf.cand[i] - 112), g_pf.cand[i], g_pf.prev[i][0], g_pf.prev[i][1], g_pf.prev[i][2], g_pf.orig[i][0], g_pf.orig[i][1], g_pf.orig[i][2]);
}
int64_t g_frames = 0;
int64_t g_entered_at = -1;
int64_t g_play_at = -1;
int32_t g_last_flow = -1;
bool g_goal_mark = false;
int64_t g_quick_done_at = -1;
std::chrono::steady_clock::time_point g_last_log;

void Tick(PPCContext& ctx, uint8_t* base) {
  const Config& c = Cfg();
  if (!c.active) return;
  ++g_frames;
  const bool quick_on = nhllegacy::QuickStartEnabled();
  if (quick_on && g_quick_done_at < 0 && !nhllegacy::QuickStartActive()) g_quick_done_at = g_frames;
  if (g_entered_at < 0) {
    int64_t at = c.enter_frame;
    bool go;
    if (quick_on) go = g_quick_done_at >= 0 && g_frames - g_quick_done_at >= (at >= 0 ? at : 2500);
    else go = g_frames >= (at >= 0 ? at : 12000);
    if (go) {
      g_entered_at = g_frames;
      const bool shoot = c.scene == "shootout";
      REXLOG_INFO("[nhl-scene] enter {} frame={} home={} away={}", c.scene, g_frames, c.home, c.away);
      // The game's own start function (what the Lua EnterGameplay / EnterShootOutMode bindings call).
      const uint32_t args[6] = {shoot ? 1u : 0u, shoot ? 28u : c.home, shoot ? 28u : c.away, shoot ? 0u : c.pad_a,
                                shoot ? uint32_t(-1) : c.pad_b, uint32_t(-1)};
      CallGuest(ctx, base, 0x829D7078, args, 6);
    }
    return;
  }
  // marks for the bench report: flow-state changes, "play" (first flow 4) and "goal" (stoppage with reason 2)
  const int32_t flow = int32_t(REX_LOAD_U32(kFlow));
  if (flow != g_last_flow) {
    const int32_t prev_flow = g_last_flow;
    REXLOG_INFO("[nhl-scene] flow {}->{} (+{}f, lastreason={})", g_last_flow, flow, g_frames - g_entered_at, int32_t(REX_LOAD_U32(kFlow + 12)));
    g_last_flow = flow;
    if (flow == 4 && g_play_at < 0) { g_play_at = g_frames; REXLOG_INFO("[nhl-scene] reached play"); }
    static bool replay_mark = false;
    if (flow == 10 && prev_flow == 5 && !replay_mark) { replay_mark = true; REXLOG_INFO("[nhl-scene] reached replay"); }  // stoppage wait over -> replay/celebration state
    if (flow == 5 && REX_LOAD_U32(kFlow + 12) == 2 && !g_goal_mark) { g_goal_mark = true; REXLOG_INFO("[nhl-scene] reached goal"); }
  }
  {  // puck finder stages (NHL_SCENE_FINDPUCK=1): faceoff rest, then three times during play
    static const bool on = std::getenv("NHL_SCENE_FINDPUCK") != nullptr;
    static int64_t f3 = -1;
    if (on) {
      if (flow == 3 && f3 < 0) f3 = g_frames;
      if (g_pf.stage == 0 && f3 >= 0 && g_frames - f3 >= 600) FindPuck(base, g_pf.stage = 1);
      else if (g_pf.stage == 1 && g_play_at >= 0 && g_frames - g_play_at >= 150) FindPuck(base, g_pf.stage = 2);
      else if (g_pf.stage == 2 && g_play_at >= 0 && g_frames - g_play_at >= 500) FindPuck(base, g_pf.stage = 3);
      else if (g_pf.stage == 3 && g_play_at >= 0 && g_frames - g_play_at >= 1200) FindPuck(base, g_pf.stage = 4);
    }
  }
  {  // NHL_SCENE_PUCKPTR=0xGLOBAL: the puck object is the pointer stored at that global (research; logs both table candidates every second)
    static const char* e = std::getenv("NHL_SCENE_PUCKPTR");
    if (e) { const uint32_t pp = REX_LOAD_U32(uint32_t(std::strtoul(e, nullptr, 0))); if (pp >= 0xA0000000u && pp < 0xC0000000u) g_puck.store(pp); }
    static std::chrono::steady_clock::time_point last;
    if (std::getenv("NHL_SCENE_LOGTABLE") && std::chrono::steady_clock::now() - last > std::chrono::seconds(1)) {
      last = std::chrono::steady_clock::now();
      for (uint32_t g : {0x83C1D0C8u, 0x83C1D118u, 0x83C1D084u}) {
        const uint32_t pp = REX_LOAD_U32(g);
        if (pp >= 0xA0000000u && pp < 0xC0000000u)
          REXLOG_INFO("[nhl-scene] table {:08X} -> {:08X} flow={} pos=({:.2f},{:.2f},{:.2f})", g, pp, flow, F32(REX_LOAD_U32(pp + 112)), F32(REX_LOAD_U32(pp + 116)), F32(REX_LOAD_U32(pp + 120)));
      }
    }
  }
  {  // NHL_SCENE_PUCKPOKE=x:y:z[:hold]  put the remembered puck object at a world position for `hold` frames (research)
    static const char* env = std::getenv("NHL_SCENE_PUCKPOKE");
    static int64_t started = -1;
    static float px, py, pz; static int hold = 300; static bool parsed = false;
    if (env && !parsed) { parsed = true; std::sscanf(env, "%f:%f:%f:%d", &px, &py, &pz, &hold); }
    const uint32_t pk = g_puck.load();
    static std::vector<uint32_t> copies;  // every heap address holding the puck's exact (x,y,z) at the start: the sim's own copy is one of them
    if (env && pk && flow == 4) {
      if (started < 0) {
        started = g_frames;
        const uint32_t bx = REX_LOAD_U32(pk + 112), by = REX_LOAD_U32(pk + 116), bz = REX_LOAD_U32(pk + 120);
        std::vector<uint32_t> buf(1024);
        for (uint64_t pg = 0xA0000000ull; pg < 0xC0000000ull; pg += 4096) {
          iovec l{buf.data(), 4096}, r{base + pg, 4096};
          if (process_vm_readv(getpid(), &l, 1, &r, 1, 0) != 4096) continue;
          for (uint32_t i = 0; i + 2 < 1024; ++i)
            if (__builtin_bswap32(buf[i]) == bx && __builtin_bswap32(buf[i + 1]) == by && __builtin_bswap32(buf[i + 2]) == bz) copies.push_back(uint32_t(pg) + i * 4);
        }
        REXLOG_INFO("[nhl-scene] puckpoke start puck={:08X} pos=({:.2f},{:.2f},{:.2f}) copies={} -> target ({:.2f},{:.2f},{:.2f})", pk, F32(bx), F32(by), F32(bz), copies.size(), px, py, pz);
        for (size_t i = 0; i < copies.size() && i < 24; ++i) REXLOG_INFO("[nhl-scene]   copy {:08X}", copies[i]);
      }
      if (g_frames - started < hold) {
        auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        for (uint32_t a : copies) { REX_STORE_U32(a, bits(px)); REX_STORE_U32(a + 4, bits(py)); REX_STORE_U32(a + 8, bits(pz)); }
      }
    }
  }
  static const bool ref_play = [] { const char* r = std::getenv("NHL_SCENE_REF"); return r && std::strcmp(r, "play") == 0; }();
  if (ref_play && g_play_at < 0) return;
  const int64_t t = g_frames - (ref_play ? g_play_at : g_entered_at);
  for (auto& k : c.calls) {
    auto& m = const_cast<Call&>(k);
    if (!m.done && t >= m.frame) {
      m.done = true;
      const uint32_t ctrl = g_ctrl.load();
      for (int i = 0; i < 6; ++i) {
        if (m.deref[i]) { m.arg[i] = REX_LOAD_U32(m.deref[i]); if (m.twice[i]) { const uint32_t o = m.arg[i]; m.arg[i] = o ? REX_LOAD_U32(o + m.off[i]) : 0; } }
        if (m.token[i] == 3) {  // the user's active player: gp->(+168) object, vtable slot 60 (what the referee code 0x83502C40 asks)
          const uint32_t gp = REX_LOAD_U32(0x83C1E89C), obj = gp ? REX_LOAD_U32(gp + 168) : 0, vt = obj ? REX_LOAD_U32(obj) : 0, fn = vt ? REX_LOAD_U32(vt + 60) : 0;
          const uint32_t a[1] = {obj};
          m.arg[i] = fn ? CallGuest(ctx, base, fn, a, 1, true) : 0;
          REXLOG_INFO("[nhl-scene] @userplayer gp={:08X} obj={:08X} fn={:08X} -> {:08X}", gp, obj, fn, m.arg[i]);
        }
        if (m.token[i] == 1) m.arg[i] = ctrl;
        if (m.token[i] == 2) m.arg[i] = ctrl ? REX_LOAD_U32(ctrl + 1008) : 0;
      }
      REXLOG_INFO("[nhl-scene] +{} frames: call 0x{:08X}({:X},{:X},{:X},{:X})", t, m.addr, m.arg[0], m.arg[1], m.arg[2], m.arg[3]);
      CallGuest(ctx, base, m.addr, m.arg, 6);
    }
  }
  for (auto& f : const_cast<std::vector<int64_t>&>(c.dump_world)) {
    if (f >= 0 && t >= f && g_world.load()) {
      f = -1;
      const uint32_t w = g_world.load();
      REXLOG_INFO("[nhl-scene] dumpworld +{}f world={:08X}", t, w);
      for (int e = 0; e < 44; ++e) {
        std::string d;
        for (int k = 0; k < 36; ++k) d += fmt::format(" {:08X}", REX_LOAD_U32(w + 3120 + e * 144 + k * 4));
        REXLOG_INFO("[nhl-scene] E{} {}", e, d);
      }
    }
  }
  for (auto& k : c.pokes) {
    auto& m = const_cast<Poke&>(k);
    if (!m.done && t >= m.frame) {
      m.done = true;
      if (m.bytes == 1) REX_STORE_U8(m.addr, uint8_t(m.value));
      else if (m.bytes == 2) REX_STORE_U16(m.addr, uint16_t(m.value));
      else REX_STORE_U32(m.addr, m.value);
      REXLOG_INFO("[nhl-scene] +{} frames: poke 0x{:08X} = 0x{:X} ({} bytes)", t, m.addr, m.value, m.bytes);
    }
  }
  if (c.log) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_log >= std::chrono::seconds(1)) {
      g_last_log = now;
      const uint32_t zero[1] = {0};
      const int32_t gdiff = int32_t(CallGuest(ctx, base, 0x833FADA0, zero, 1, true));  // goals(side0) - goals(side1), pure
      REXLOG_INFO("[nhl-scene] +{}f flow={} lastreason={} kind={} index={} clock={}.{} gdiff={} gp={:08X} w0={:08X} ctrl={:08X} replay={:08X} world={:08X} puck={:08X} ({:.1f},{:.1f},{:.1f})", t, int32_t(REX_LOAD_U32(kFlow)),
                  int32_t(REX_LOAD_U32(kFlow + 12)), int32_t(REX_LOAD_U32(kGameData + 432)), int32_t(REX_LOAD_U32(kGameData + 436)),
                  int16_t(REX_LOAD_U16(kGameData + 426)), int16_t(REX_LOAD_U16(kGameData + 428)), gdiff, REX_LOAD_U32(0x83C1E89C), REX_LOAD_U32(kGameData), g_ctrl.load(), g_ctrl.load() ? REX_LOAD_U32(g_ctrl.load() + 1008) : 0u, g_world.load(), g_puck.load(), g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 112)) : 0.f, g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 116)) : 0.f, g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 120)) : 0.f);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_828ADFD8);
extern "C" REX_FUNC(sub_828ADFD8) {
  __imp__sub_828ADFD8(ctx, base);
  Tick(ctx, base);  // after the frame body: the Lua RunEachFrame call (the other known-good place to call into the game) runs late in the frame too
}

REX_EXTERN(__imp__sub_834D2A00);
extern "C" REX_FUNC(sub_834D2A00) {  // per-frame view/replay controller update: remember `this`
  if (Cfg().active) g_ctrl.store(ctx.r3.u32);
  __imp__sub_834D2A00(ctx, base);
}

#ifdef NHL_SCENE_TRACE_HOOKS  // cmake -DNHL_SCENE_TRACE=ON: replaces ~20 game functions with logging wrappers (a research build, not for benches)
REX_EXTERN(__imp__sub_83328180);
extern "C" REX_FUNC(sub_83328180) {  // block shot (player, puck, ...): remember the puck
  if (Cfg().active && ctx.r4.u32 >= 0xA0000000u && ctx.r4.u32 < 0xC0000000u) {
    static int n = 0;
    if (g_puck.exchange(ctx.r4.u32) != ctx.r4.u32 && n++ < 10)
      REXLOG_INFO("[nhl-scene] puck candidate {:08X} pos=({:.2f},{:.2f},{:.2f})", ctx.r4.u32, F32(REX_LOAD_U32(ctx.r4.u32 + 112)), F32(REX_LOAD_U32(ctx.r4.u32 + 116)), F32(REX_LOAD_U32(ctx.r4.u32 + 120)));
  }
  __imp__sub_83328180(ctx, base);
}

REX_EXTERN(__imp__sub_833676B8);
extern "C" REX_FUNC(sub_833676B8) {  // puck.cpp: remember `this`
  if (Cfg().active) g_world.store(ctx.r3.u32);
  __imp__sub_833676B8(ctx, base);
}

// ---- trace (NHL_SCENE_TRACE=1): which game-flow / replay / goal functions run, with arguments and caller ----
namespace {
void Trace(const char* name, std::atomic<int>& n, PPCContext& ctx, uint8_t* base, int cap) {
  static const bool on = std::getenv("NHL_SCENE_TRACE") != nullptr;
  if (!on) return;
  const int i = n.fetch_add(1);
  if (i >= cap) return;
  const int64_t t = g_entered_at >= 0 ? g_frames - g_entered_at : -1;
  if (std::strncmp(name, "pk_foresult", 11) == 0) {  // globals that point at an object resting at the centre (the puck at the draw)
    for (uint32_t a = 0x83C00000; a < 0x83D00000; a += 4) {
      const uint32_t pp = REX_LOAD_U32(a);
      if (pp < 0xA0000000u || pp >= 0xC0000000u || (pp & 3)) continue;
      const float x = F32(REX_LOAD_U32(pp + 112)), y = F32(REX_LOAD_U32(pp + 116)), z = F32(REX_LOAD_U32(pp + 120));
      if (std::fabs(x) < 1.f && y > 0.f && y < 1.f && std::fabs(z) < 1.f)
        REXLOG_INFO("[nhl-scene-trace] globalptr {:08X} -> {:08X} pos=({:.3f},{:.3f},{:.3f}) vt={:08X}", a, pp, x, y, z, REX_LOAD_U32(pp));
    }
  }
  if (std::strncmp(name, "pk_", 3) == 0) {
    const uint32_t rr[3] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32};
    for (int k = 0; k < 3; ++k) {
      if (rr[k] < 0xA0000000u || rr[k] >= 0xC0000000u) continue;
      std::string d;
      for (int w = 0; w < 32; ++w) d += fmt::format(" {:.2f}", F32(REX_LOAD_U32(rr[k] + 4 * w)));
      REXLOG_INFO("[nhl-scene-trace] {} arg r{}={:08X} floats:{}", name, 3 + k, rr[k], d);
    }
  }
  if (std::strncmp(name, "replay_a", 8) == 0 && ctx.r3.u32) {
    std::string d;
    for (int w = 0; w < 10; ++w) d += fmt::format(" {:08X}", REX_LOAD_U32(ctx.r3.u32 + 4 * w));
    d += " |";
    if (ctx.r5.u32 > 0x10000000) for (int w = 0; w < 8; ++w) d += fmt::format(" {:08X}", REX_LOAD_U32(ctx.r5.u32 + 4 * w));
    REXLOG_INFO("[nhl-scene-trace] replay_a objwords r3:{} ctrl={:08X} ctrl+1008={:08X}", d, g_ctrl.load(), g_ctrl.load() ? REX_LOAD_U32(g_ctrl.load() + 1008) : 0u, g_world.load(), g_puck.load(), g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 112)) : 0.f, g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 116)) : 0.f, g_puck.load() ? F32(REX_LOAD_U32(g_puck.load() + 120)) : 0.f);
  }
  REXLOG_INFO("[nhl-scene-trace] +{}f {} r3={:X} r4={:X} r5={:X} r6={:X} r7={:X} r8={:X} lr={:08X}", t, name, ctx.r3.u32, ctx.r4.u32,
              ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, uint32_t(ctx.lr));
}
}  // namespace

#define NHL_SCENE_TRACE_HOOK_N(addr, name, cap)                     \
  REX_EXTERN(__imp__sub_##addr);                               \
  extern "C" REX_FUNC(sub_##addr) {                            \
    static std::atomic<int> n{0};                              \
    Trace(name "_" #addr, n, ctx, base, cap);                             \
    __imp__sub_##addr(ctx, base);                              \
  }
#define NHL_SCENE_TRACE_HOOK(addr, name) NHL_SCENE_TRACE_HOOK_N(addr, name, 20)

NHL_SCENE_TRACE_HOOK(83326280, "stoppage")
NHL_SCENE_TRACE_HOOK(8330C890, "flow_setstate")
NHL_SCENE_TRACE_HOOK(8331F408, "end_period_event")
NHL_SCENE_TRACE_HOOK(8339B0C8, "start_period")
NHL_SCENE_TRACE_HOOK(83463DB0, "replay_a")
NHL_SCENE_TRACE_HOOK(83464940, "replay_b")
NHL_SCENE_TRACE_HOOK(83468858, "replay_c")
NHL_SCENE_TRACE_HOOK(834687E0, "replay_d")
NHL_SCENE_TRACE_HOOK(83468938, "replay_e")
NHL_SCENE_TRACE_HOOK(83469480, "replay_f")
NHL_SCENE_TRACE_HOOK(83465390, "replay_g")
NHL_SCENE_TRACE_HOOK(834646A8, "replay_h")
NHL_SCENE_TRACE_HOOK(83484320, "stopplay_a")
NHL_SCENE_TRACE_HOOK(83485F90, "stopplay_b")
NHL_SCENE_TRACE_HOOK(83481038, "gameplay_reset")
NHL_SCENE_TRACE_HOOK(829D7078, "enter_match")
NHL_SCENE_TRACE_HOOK(83433680, "set_camera")
NHL_SCENE_TRACE_HOOK_N(833BDD10, "pk_foresult", 4)
NHL_SCENE_TRACE_HOOK_N(833BDB98, "pk_foend", 4)
NHL_SCENE_TRACE_HOOK_N(8332D8E0, "pk_hitplayer", 2)
NHL_SCENE_TRACE_HOOK_N(83420B50, "pk_passstart", 6)
NHL_SCENE_TRACE_HOOK(83323B10, "announce_goal")
NHL_SCENE_TRACE_HOOK(833192F8, "goal_light_b")
NHL_SCENE_TRACE_HOOK(83316038, "whistle")
NHL_SCENE_TRACE_HOOK(833C6398, "faceoff_setup")
NHL_SCENE_TRACE_HOOK(83328CA8, "announce_goal_b")
NHL_SCENE_TRACE_HOOK(8338EAF8, "generic_event")
#endif  // NHL_SCENE_TRACE_HOOKS
