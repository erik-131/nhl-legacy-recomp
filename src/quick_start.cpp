// Quick start (REX_NHL_QUICK_START=true): presses through the boot screens that only wait for a button.
//
// What the boot does (screenshots and file opens, 2026-11-06, seconds from the first log line): language screen at 6,
// waits for A and otherwise times out at ~36; legal text 7.4-12.6 and the EA logo movie, neither can be skipped with a
// button; title, takes Start at ~17.5; autosave notice, takes A; main menu background opens at ~24. A is ignored on the
// title and Start on the notice, so alternating them is safe until the main menu, where A would select Play Now. The
// FE files the game opens tell where the boot is. A press from the player ends it at once, and so does the time limit.
// These screens are timed, so NHL_BOOT_CLOCK runs the guest clock faster until then (menu at 14 s instead of 24 at 3x).
// NHL_QUICK_MOVIE_FRAMES serves a short logo movie (10 s). Serving a screen that does not say "legal screen entered" for the legal text
// hangs the boot; nhl_skip_legal serves one that does (docs/boot-flow.md).
#include "quick_start.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nhl_quick_start, false, "NHL", "Press through the boot screens up to the main menu");
REXCVAR_DEFINE_BOOL(nhl_skip_legal, false, "NHL", "Leave out the legal text screen in the boot flow (docs/boot-flow.md)");

namespace nhllegacy {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kButtonA = 0x1000;
constexpr uint16_t kButtonStart = 0x0010;
constexpr auto kPeriod = std::chrono::milliseconds(500);
constexpr auto kHold = std::chrono::milliseconds(120);
constexpr auto kIgnoreRealFor = std::chrono::seconds(3);  // the press that launched the game can still be held
constexpr auto kLimit = std::chrono::seconds(90);
constexpr auto kMovieHold = std::chrono::nanoseconds(std::chrono::seconds(2));

std::atomic<bool> g_language_done{false};
std::atomic<bool> g_title{false};
std::atomic<bool> g_done{false};
std::atomic<bool> g_short_movie{false};
std::atomic<int64_t> g_elapsed{0};   // ns since the first poll
std::atomic<int64_t> g_movie_at{0};  // g_elapsed when the short movie was served

// EA's movie container is little-endian chunks: type, total size. Keeps the header and the first frames with their audio,
// sets the frame count (header +16) and ends with the real end chunk, so that the player sees a whole, short movie.
std::shared_ptr<const std::vector<uint8_t>> ShortMovie(const std::vector<uint8_t>& d, uint32_t frames) {
  auto out = std::make_shared<std::vector<uint8_t>>();
  uint32_t seen = 0;
  for (size_t o = 0; o + 8 <= d.size();) {
    uint32_t size;
    std::memcpy(&size, d.data() + o + 4, 4);
    if (size < 8 || o + size > d.size()) break;
    const bool frame = !std::memcmp(d.data() + o, "MV0K", 4) || !std::memcmp(d.data() + o, "MV0F", 4);
    const bool end = !std::memcmp(d.data() + o, "SCEl", 4);
    if (frame) ++seen;
    if (seen <= frames || end) out->insert(out->end(), d.begin() + o, d.begin() + o + size);
    o += size;
  }
  if (out->size() >= 20) std::memcpy(out->data() + 16, &frames, 4);
  return out;
}

// The movie's cue file: chunks ST00, size, first frame, last frame, ... The last cue, at frame 131 of 365, tells the
// boot flow to go on to the title, and a movie that ends before it just freezes. Moves the cues to frames 0, 1, 2, ...
std::shared_ptr<const std::vector<uint8_t>> EarlyCues(const std::vector<uint8_t>& d) {
  auto out = std::make_shared<std::vector<uint8_t>>(d);
  uint32_t cue = 0;
  for (size_t o = 0; o + 16 <= out->size(); ++cue) {
    uint32_t size, last;
    std::memcpy(&size, out->data() + o + 4, 4);
    if (size < 16 || o + size > out->size()) break;
    std::memcpy(&last, out->data() + o + 12, 4);
    const uint32_t next = cue + 1;
    std::memcpy(out->data() + o + 8, &cue, 4);
    if (last != 0xFFFFFFFFu) std::memcpy(out->data() + o + 12, &next, 4);
    o += size;
  }
  return out;
}

// The legal screen is served as a copy of the "publisher notice" screen (a blank movie with no timeline, so the boot flow goes on at
// once), which says that the legal screen has been entered: the flow waits for exactly that notice (docs/boot-flow.md).
std::shared_ptr<const std::vector<uint8_t>> BlankLegal(const std::filesystem::path& legal_host) {
  std::ifstream f(legal_host.parent_path() / "00_ealogo.big", std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  static const char kFrom[] = "BOOTFLOW_EA_INTHEGAME";
  static const char kTo[] = "BOOTFLOW_LEGAL";
  const auto it = std::search(d.begin(), d.end(), kFrom, kFrom + sizeof(kFrom) - 1);
  if (d.size() < 64 || std::memcmp(d.data(), "BIGF", 4) != 0 || it == d.end() ||
      std::search(it + 1, d.end(), kFrom, kFrom + sizeof(kFrom) - 1) != d.end()) {
    REXLOG_WARN("[nhl-boot] legal screen left in: the blank screen was not found");
    return nullptr;
  }
  std::fill(it, it + sizeof(kFrom) - 1, uint8_t(0));
  std::copy(kTo, kTo + sizeof(kTo) - 1, it);
  REXLOG_INFO("[nhl-boot] legal screen served blank");
  return std::make_shared<std::vector<uint8_t>>(std::move(d));
}

// The boot clock goes on at the first file the game opens (the first poll of the pad can come late when screens are left out).
void EnsureBootClock() {
  static std::atomic<bool> set{false};
  if (!REXCVAR_GET(nhl_quick_start) || g_done.load() || set.exchange(true)) return;
  if (const char* e = std::getenv("NHL_BOOT_CLOCK")) rex::chrono::Clock::set_guest_time_scalar(std::strtod(e, nullptr));
}

void Finish(const char* why) {
  if (!g_done.exchange(true)) {
    rex::chrono::Clock::set_guest_time_scalar(1.0);
    REXLOG_INFO("[nhl-quick] done: {}", why);
  }
}

}  // namespace

void QuickStartNoteFile(const char* path) {
  EnsureBootClock();
  if (!REXCVAR_GET(nhl_quick_start) || g_done.load()) return;
  if (std::strstr(path, "01_legal")) {
    if (!g_language_done.exchange(true)) REXLOG_INFO("[nhl-quick] language done");
  } else if (std::strstr(path, "03_pressstart")) {
    if (!g_title.exchange(true)) REXLOG_INFO("[nhl-quick] title");
  } else if (std::strstr(path, "nhlmainmenubackground")) {
    Finish("main menu");
  }
}

std::shared_ptr<const std::vector<uint8_t>> QuickStartServe(const std::string& path, const std::filesystem::path& host) {
  const auto read = [](const std::filesystem::path& p, size_t limit) {
    std::ifstream f(p, std::ios::binary);
    auto data = std::make_shared<std::vector<uint8_t>>(limit);
    f.read(reinterpret_cast<char*>(data->data()), std::streamsize(limit));
    data->resize(size_t(f.gcount()));
    return data;
  };
  // Opened right after the movie: it must follow what the movie got, whatever has happened since.
  if (path.find("ealogo.sub") != std::string::npos) {
    return g_short_movie.exchange(false) ? EarlyCues(*read(host, 1 << 16)) : nullptr;
  }
  if (REXCVAR_GET(nhl_skip_legal)) {
    const auto ends = [&](const char* tail) {
      const size_t n = std::strlen(tail);
      return path.size() >= n && std::equal(path.end() - long(n), path.end(), tail);
    };
    if (ends("bootflow\\01_legal.big") || ends("bootflow/01_legal.big")) return BlankLegal(host);
  }
  static const char* movie_frames = std::getenv("NHL_QUICK_MOVIE_FRAMES");
  if (movie_frames && QuickStartActive() && path.find("ealogo.vp6") != std::string::npos) {
    REXLOG_INFO("[nhl-quick] short EA movie");
    g_short_movie = true;
    g_movie_at = g_elapsed.load();
    return ShortMovie(*read(host, size_t(1) << 28), uint32_t(std::strtoul(movie_frames, nullptr, 10)));
  }
  return nullptr;
}

bool QuickStartEnabled() { return REXCVAR_GET(nhl_quick_start); }

bool QuickStartActive() {
  return REXCVAR_GET(nhl_quick_start) && !g_done.load();
}

uint16_t QuickStartButtons(uint16_t real_buttons) {
  if (!QuickStartActive()) return 0;
  EnsureBootClock();
  static const Clock::time_point start = Clock::now();
  const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  g_elapsed = since.count();
  if (since > kLimit) {
    Finish("time limit");
    return 0;
  }
  // The short movie only ends on a press once its cue has fired, so a player's press mustn't stop the presses right then.
  const bool in_movie = g_movie_at.load() != 0 && since.count() - g_movie_at.load() < kMovieHold.count();
  if (real_buttons && since > kIgnoreRealFor && !in_movie) {
    Finish("player pressed a button");
    return 0;
  }
  const auto n = since / kPeriod;
  if (since - n * kPeriod >= kHold) return 0;
  if (g_title.load() || g_movie_at.load()) return (n & 1) ? kButtonStart : kButtonA;
  return g_language_done.load() ? 0 : kButtonA;
}

}  // namespace nhllegacy
