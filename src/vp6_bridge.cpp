// VP6 host-decode bridge (env NHL_VP6_BRIDGE=1).
//
// The recompiled VP6 decoder has an unfixed arithmetic bug in its VMX128
// float IDCT (docs/vp6-fork-investigation.md): high-AC blocks decode with
// columns crushed to zero (green/black striping). Rather than patch the
// generated math, this bridge replaces the DECODED PIXELS at the movie
// player's publish seam with frames decoded on the host by ffmpeg:
//
//   input : the SDK's NtCreateFile exports the last opened *.vp6 guest path
//           in the NHL_VP6_LAST_OPEN process env var; the movie-setup hook
//           (sub_8277ABB8) maps it to a host file under --game_data_root.
//   decode: ffmpeg.exe (PATH or NHL_VP6_FFMPEG) is spawned per movie with
//           "-f rawvideo -pix_fmt yuv420p pipe:1"; a reader thread buffers
//           the raw frames (bounded).
//   output: the plane-publish hook (sub_8277CC98, see diag_hooks.cpp) calls
//           Vp6BridgePublish once per published frame; we overwrite the ring
//           slot's Y/U/V planes with the next ffmpeg frame. Rows are written
//           bottom-up (the ring renders vertically flipped - verified with a
//           gradient test pattern). On stream EOF the process is respawned
//           from frame 0 so looping menu movies keep playing.
//
// Audio is untouched (the guest EA-audio path has always been correct); sync
// is inherently guest-paced because we only swap pixels at publish time.
//
// Several players publish concurrently (in gameplay the arena-TV environment
// movie keeps looping while a transition wipe plays, both 1024x512). Each
// player has its own 4-slot descriptor ring, so decode state is kept PER RING:
// the publish hook passes the ring-slot descriptor address, a newly opened
// .vp6 is bound to the first ring that starts publishing after the open (a new
// ring, or one whose previous movie has ended), and the other slots of that
// ring follow it. A single shared stream used to hand the transition's frames
// to the arena TV and vice versa, so the wipe starved and fell back to the
// guest decoder's broken (IDCT-miscompiled) frames - the blocky black/white
// wipes - and looped transition frames onto the TV afterwards.

#include "vp6_bridge.h"

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <fstream>
#include <mutex>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>  // _NSGetArgc / _NSGetArgv
#endif
extern char** environ;
#endif

namespace {

// The bridge shells out to ffmpeg and reads raw frames off a pipe. Only the
// spawn/pipe primitives differ per platform; everything below is shared.
#ifdef _WIN32
using PipeHandle = HANDLE;
using ProcHandle = HANDLE;
inline constexpr PipeHandle kNoPipe = nullptr;
inline constexpr ProcHandle kNoProc = nullptr;
inline bool PipeValid(PipeHandle h) { return h != nullptr; }
inline bool ProcValid(ProcHandle h) { return h != nullptr; }
#else
using PipeHandle = int;
using ProcHandle = pid_t;
inline constexpr PipeHandle kNoPipe = -1;
inline constexpr ProcHandle kNoProc = -1;
inline bool PipeValid(PipeHandle h) { return h >= 0; }
inline bool ProcValid(ProcHandle h) { return h > 0; }
#endif

// Milliseconds since process start; stands in for GetTickCount in log lines.
inline unsigned long HostTickMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());
}

#ifndef _WIN32
// GetCommandLineA equivalent: macOS exposes the real argv through _NSGetArgv,
// Linux through /proc/self/cmdline. Only used to find --game_data_root.
std::string HostCommandLine() {
  std::string out;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  for (int i = 0; i < argc; ++i) {
    if (i) out += ' ';
    out += argv[i];
  }
#else
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  for (char c : raw) out += (c == '\0') ? ' ' : c;
#endif
  return out;
}
#endif

bool BridgeOn() {
  // Default ON: the recompiled decoder's IDCT bug corrupts every bright
  // movie frame, and the bridge degrades gracefully (guest frames) when
  // ffmpeg or the loose movie files are absent. NHL_VP6_BRIDGE=0 opts out.
  static const bool on = [] {
    const char* e = std::getenv("NHL_VP6_BRIDGE");
    return !e || !*e || *e != '0';
  }();
  return on;
}

void BridgeLog(const char* fmt, ...) {
  FILE* f = std::fopen("vp6_bridge.txt", "a");
  if (!f) return;
  {
    // Wall-clock stamp so lines can be matched to the host log and screenshots.
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    const long ms = (long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                               now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    std::fprintf(f, "%02d:%02d:%02d.%03ld ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
  }
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

// ---- movie decode state ------------------------------------------------------

struct BridgeState {
  std::mutex m;
  std::condition_variable cv;
  std::string host_path;   // file ffmpeg decodes (the movie, or its remuxed alpha stream)
  std::string orig_path;   // the movie this stream belongs to (wipe/loop decisions)
  bool is_alpha = false;   // alpha stream (written to the alpha ring's Y plane only)
  std::shared_ptr<BridgeState> alpha_peer;  // alpha stream of this colour movie
  uint8_t* prev_alpha_ptr = nullptr;        // alpha plane written at the previous publish
  std::vector<uint8_t> prev_alpha_frame;    // ... and the frame written there
  uint64_t alpha_writes = 0, alpha_skips = 0;
  ProcHandle proc = kNoProc;   // ffmpeg child
  PipeHandle pipe_rd = kNoPipe;
  std::thread reader;
  // Frame-granular FIFO: whole decoded frames as moved vectors. NEVER buffer
  // the raw byte stream in a std::deque<uint8_t> - MSVC's deque uses 16-byte
  // blocks for byte elements, so bulk inserts/erases become millions of tiny
  // allocations and the publish (game) thread stalls for seconds per frame.
  std::deque<std::vector<uint8_t>> frames;
  std::vector<uint8_t> staging;         // partial-frame accumulation
  std::atomic<size_t> frame_size{0};    // w*h*3/2, set by the publish side
  std::atomic<bool> eof{false};
  std::atomic<bool> reader_stop{false};
  uint64_t frames_served = 0;
  std::vector<uint8_t> last;            // last frame written (underrun fallback)
  unsigned long last_publish_ms = 0;
  ~BridgeState();  // joins the reader (a joinable std::thread in a destructor = std::terminate)
};

constexpr size_t kMaxFrames = 24;             // decoded frames queued ahead
constexpr size_t kMaxStaging = 32ull << 20;   // pre-first-publish cap

void ReaderThread(BridgeState* sp, PipeHandle pipe_rd) {
  BridgeState& st = *sp;
  std::vector<uint8_t> chunk(1 << 20);
  for (;;) {
    if (st.reader_stop.load()) break;
    {
      // Backpressure: wait until the publish side drains the FIFO.
      std::unique_lock<std::mutex> lk(st.m);
      st.cv.wait_for(lk, std::chrono::milliseconds(100), [&st] {
        return st.reader_stop.load() || st.frames.size() < kMaxFrames ||
               (st.frame_size.load() == 0 &&
                st.staging.size() < kMaxStaging);
      });
      if (st.reader_stop.load()) break;
      if (st.frames.size() >= kMaxFrames) continue;
      if (st.frame_size.load() == 0 && st.staging.size() >= kMaxStaging)
        continue;
    }
    size_t got = 0;
#ifdef _WIN32
    DWORD win_got = 0;
    const bool read_ok =
        ReadFile(pipe_rd, chunk.data(), DWORD(chunk.size()), &win_got, nullptr) &&
        win_got != 0;
    got = win_got;
#else
    ssize_t n = ::read(pipe_rd, chunk.data(), chunk.size());
    while (n < 0 && errno == EINTR) n = ::read(pipe_rd, chunk.data(), chunk.size());
    const bool read_ok = n > 0;
    if (n > 0) got = static_cast<size_t>(n);
#endif
    if (!read_ok) {
      st.eof.store(true);
      std::lock_guard<std::mutex> lk(st.m);
      st.cv.notify_all();
      break;
    }
    std::lock_guard<std::mutex> lk(st.m);
    BridgeState& s = st;
    s.staging.insert(s.staging.end(), chunk.data(), chunk.data() + got);
    size_t fs = s.frame_size.load();
    if (fs) {
      size_t off = 0;
      while (s.staging.size() - off >= fs) {
        s.frames.emplace_back(s.staging.begin() + off,
                              s.staging.begin() + off + fs);
        off += fs;
      }
      if (off) s.staging.erase(s.staging.begin(), s.staging.begin() + off);
    }
    s.cv.notify_all();
  }
}

void StopMovie(BridgeState& s) {
  s.reader_stop.store(true);
  s.cv.notify_all();
#ifdef _WIN32
  if (PipeValid(s.pipe_rd)) CancelIoEx(s.pipe_rd, nullptr);
  if (s.reader.joinable()) s.reader.join();
  if (PipeValid(s.pipe_rd)) CloseHandle(s.pipe_rd);
  if (ProcValid(s.proc)) {
    TerminateProcess(s.proc, 0);
    CloseHandle(s.proc);
  }
#else
  // Kill the child FIRST: that closes the write end, so a reader blocked in
  // read() sees EOF and exits. Closing the fd out from under it would race.
  if (ProcValid(s.proc)) ::kill(s.proc, SIGKILL);
  if (s.reader.joinable()) s.reader.join();
  if (PipeValid(s.pipe_rd)) ::close(s.pipe_rd);
  if (ProcValid(s.proc)) {
    int status = 0;
    while (::waitpid(s.proc, &status, 0) < 0 && errno == EINTR) {
    }
  }
#endif
  s.pipe_rd = kNoPipe;
  s.proc = kNoProc;
  s.reader_stop.store(false);
  s.eof.store(false);
  std::lock_guard<std::mutex> lk(s.m);
  s.frames.clear();
  s.staging.clear();
}

// A stream can be destroyed without StopMovie having run on it: the alpha peer
// of a reaped colour stream (ReapIdleLocked only stopped the colour one), or any
// stream whose reader thread already ended on EOF (it stays joinable until
// joined). std::~thread on a joinable thread calls std::terminate, which the
// crash tap reported as "null indirect call lr=8278AF8C" (that lr is just the
// stale guest lr of the sub_8277CC98 call that published the frame). See
// docs/crash-nullcall.md.
BridgeState::~BridgeState() {
  if (reader.joinable() || ProcValid(proc) || PipeValid(pipe_rd)) {
    static std::atomic<unsigned> n{0};
    if (n.fetch_add(1) < 50)
      BridgeLog("dtor: stopping leftover stream %s (reader joinable=%d)", host_path.c_str(),
                reader.joinable() ? 1 : 0);
    StopMovie(*this);
  }
}

std::string FfmpegPath() {
  if (const char* e = std::getenv("NHL_VP6_FFMPEG"); e && *e) return e;
#ifdef _WIN32
  return "ffmpeg.exe";  // PATH
#else
  return "ffmpeg";  // PATH
#endif
}

bool StartMovie(BridgeState& s, const std::string& host_path) {
  StopMovie(s);
  if (s.orig_path.empty()) s.orig_path = host_path;
  const std::string ffmpeg = FfmpegPath();
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 8 << 20)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  std::string cmd = "\"" + ffmpeg +
                    "\" -v error -i \"" + host_path +
                    "\" -map 0:v:0 -f rawvideo -pix_fmt yuv420p pipe:1";
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<char> cmdbuf(cmd.begin(), cmd.end());
  cmdbuf.push_back(0);
  BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    BridgeLog("spawn FAILED err=%lu cmd=%s", GetLastError(), cmd.c_str());
    // Remember the path so the publish hook doesn't retry the spawn per
    // frame (e.g. ffmpeg not installed) - the guest's own frames show.
    s.host_path = host_path;
    return false;
  }
  CloseHandle(pi.hThread);
  s.proc = pi.hProcess;
  s.pipe_rd = rd;
#else
  // posix_spawnp takes an argv vector, so no quoting/escaping is needed (and a
  // path with spaces cannot break the command line the way it can on Windows).
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return false;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  const char* argv[] = {ffmpeg.c_str(),
                        "-v", "error",
                        "-i", host_path.c_str(),
                        "-map", "0:v:0",
                        "-f", "rawvideo",
                        "-pix_fmt", "yuv420p",
                        "pipe:1",
                        nullptr};
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, ffmpeg.c_str(), &fa, nullptr,
                              const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);  // parent keeps only the read end
  if (rc != 0) {
    ::close(fds[0]);
    BridgeLog("spawn FAILED rc=%d ffmpeg=%s movie=%s", rc, ffmpeg.c_str(),
              host_path.c_str());
    // Remember the path so the publish hook doesn't retry the spawn per
    // frame (e.g. ffmpeg not installed) - the guest's own frames show.
    s.host_path = host_path;
    return false;
  }
  s.proc = pid;
  s.pipe_rd = fds[0];
#endif
  s.host_path = host_path;
  s.frames_served = 0;
  s.reader = std::thread(ReaderThread, &s, s.pipe_rd);
  BridgeLog("spawned ffmpeg for %s", host_path.c_str());
  return true;
}

// Map a guest VFS path (game:\..., cache:\..., update:\...) to a host file
// under --game_data_root (parsed from the command line).
std::string GameDataRoot() {
  static const std::string root = [] {
#ifdef _WIN32
    std::string cl = GetCommandLineA();
#else
    std::string cl = HostCommandLine();
#endif
    const char* key = "--game_data_root";
    size_t p = cl.find(key);
    if (p == std::string::npos) return std::string();
    p += std::strlen(key);
    while (p < cl.size() && (cl[p] == ' ' || cl[p] == '=')) ++p;
    std::string v;
    if (p < cl.size() && cl[p] == '"') {
      size_t q = cl.find('"', p + 1);
      v = cl.substr(p + 1, q - p - 1);
    } else {
      size_t q = cl.find(' ', p);
      v = cl.substr(p, q - p);
    }
    return v;
  }();
  return root;
}

std::string MapGuestPath(std::string g) {
  for (auto& c : g)
    if (c == '/') c = '\\';
  auto strip = [&](const char* pfx) -> bool {
    size_t n = std::strlen(pfx);
    #ifdef _WIN32
    const int cmp = _strnicmp(g.c_str(), pfx, n);
#else
    const int cmp = ::strncasecmp(g.c_str(), pfx, n);
#endif
    if (g.size() > n && cmp == 0) {
      g = g.substr(n);
      return true;
    }
    return false;
  };
  std::string root = GameDataRoot();
  if (root.empty()) return {};
  if (strip("game:\\")) return root + "\\" + g;
  // cache:\X overlays game_data_root\_compiled\X (loose-asset tree).
  if (strip("cache:\\")) return root + "\\_compiled\\" + g;
  if (strip("update:\\")) return root + "\\" + g;
  // Raw device path - try as-is under the root.
  return root + "\\" + g;
}

}  // namespace

// Every in-game movie (wipes, arena TV) carries a second VP6 stream for alpha in the same
// file: chunks ALhd/AL0K/AL0F next to MVhd/MV0K/MV0F. ffmpeg's EA demuxer skips them and the
// guest decodes them with the miscompiled IDCT, so the alpha is broken unless we replace it too.
// Write a stand-alone EA stream (AL* renamed MV*) that ffmpeg decodes as ordinary video (Y = alpha).
// Returns "" when the file has no alpha stream.
std::string MakeAlphaFile(const std::string& host_path) {
  std::ifstream in(host_path, std::ios::binary);
  if (!in) return {};
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string out;
  size_t o = 0;
  bool have = false;
  while (o + 8 <= data.size()) {
    uint32_t n;
    std::memcpy(&n, data.data() + o + 4, 4);  // little endian size, includes the 8-byte header
    if (n < 8 || o + n > data.size()) break;
    const char* t = data.data() + o;
    if (t[0] == 'A' && t[1] == 'L' && (std::memcmp(t + 2, "hd", 2) == 0 || std::memcmp(t + 2, "0K", 2) == 0 ||
                                       std::memcmp(t + 2, "0F", 2) == 0)) {
      out.append("MV", 2);
      out.append(t + 2, 2);
      out.append(data, o + 4, n - 4);
      have = true;
    }
    o += n;
  }
  if (!have) return {};
  std::error_code ec;
  std::filesystem::create_directories("vp6_alpha", ec);
  std::string name = std::filesystem::path(host_path).filename().string();
  std::string dst = "vp6_alpha/" + name;
  {
    std::ofstream f(dst, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    f.write(out.data(), (std::streamsize)out.size());
  }
  return dst;
}

// ---- per-ring stream registry -------------------------------------------------

namespace {

struct Registry {
  std::mutex m;
  std::map<uint32_t, std::shared_ptr<BridgeState>> by_ring;  // slot descriptor -> stream
  std::string last_env_path;    // last .vp6 the guest opened (host path)
  std::string pending;          // opened but not yet bound to a ring
  uint64_t pending_since = 0;   // publish counter when it became pending
  std::shared_ptr<BridgeState> recent;  // most recently bound stream
  uint64_t recent_at = 0;       // publish counter when it was bound

  uint64_t publishes = 0;
};

Registry& R() {
  static Registry r;
  return r;
}

// Record a newly opened movie (env vars set by the loose-tree device / the
// SDK's NtCreateFile fallback). Caller holds R().m.
void NoteOpenLocked(const char* who) {
  std::string host;
  if (const char* hp = std::getenv("NHL_VP6_LAST_OPEN_HOST"); hp && *hp) {
    host = hp;  // loose-tree open: already a host path
  } else {
    const char* guest = std::getenv("NHL_VP6_LAST_OPEN");
    if (!guest || !*guest) return;
    host = MapGuestPath(guest);
  }
  Registry& r = R();
  if (host == r.last_env_path) {
    static unsigned dup = 0;
    if (++dup <= 400) BridgeLog("%s: (same as last open, ignored) %s", who, host.c_str());
    return;
  }
  r.last_env_path = host;
  std::error_code exists_ec;
  const bool exists = std::filesystem::exists(host, exists_ec) && !exists_ec;
  BridgeLog("%s: host='%s' exists=%d", who, host.c_str(), exists ? 1 : 0);
  if (!exists) return;
  r.pending = host;
  r.pending_since = r.publishes;
}

// Transition wipes play once; every other movie (menu attract, arena TV) loops. A looping
// replay of a wipe shows it again after it has ended, and a reused ring must take the next
// wipe at once instead of replaying the old one.
bool IsWipe(const std::string& path) {
  return path.find("/transition/") != std::string::npos ||
         path.find("wipe") != std::string::npos;
}

// Returns the stream the given ring slot should be fed from (null: leave the
// guest's frame). Caller holds R().m.
std::shared_ptr<BridgeState> StreamForRingLocked(uint32_t ring) {
  Registry& r = R();
  auto it = r.by_ring.find(ring);
  std::shared_ptr<BridgeState> cur = it != r.by_ring.end() ? it->second : nullptr;
  const bool cur_ended = cur && cur->eof.load() && cur->frames.empty();
  // Bind a pending movie to the first ring that starts publishing: a ring we
  // have not seen yet, or (after a few publishes, so a fresh ring wins the
  // race) a ring whose movie ended - players reuse their ring for the next movie.
  // A wipe ring takes the next open only if it is the same movie (two wipes have their own rings).
  const bool wipe_reuse = cur && IsWipe(r.pending) && r.pending == cur->orig_path;
  if (!r.pending.empty() &&
      (!cur || wipe_reuse ||
       (cur_ended && !IsWipe(cur->orig_path) && r.publishes - r.pending_since >= 4))) {
    auto s = std::make_shared<BridgeState>();
    StartMovie(*s, r.pending);
    std::shared_ptr<BridgeState> na;
    static const bool alpha_on = [] { const char* e = std::getenv("NHL_VP6_ALPHA"); return !e || *e != '0'; }();
    const std::string apath = alpha_on ? MakeAlphaFile(r.pending) : std::string();
    if (!apath.empty()) {
      na = std::make_shared<BridgeState>();
      na->is_alpha = true;
      na->orig_path = r.pending;
      StartMovie(*na, apath);
      s->alpha_peer = na;
    }
    if (cur) {
      // Move the whole ring (all slots that fed from the old movie).
      for (auto& kv : r.by_ring)
        if (kv.second == cur) kv.second = s;
      if (cur->alpha_peer) StopMovie(*cur->alpha_peer);
      StopMovie(*cur);
    }
    BridgeLog("bind ring=%08X%s -> %s", ring, cur ? " (reused)" : "", r.pending.c_str());
    r.by_ring[ring] = s;
    r.recent = s;
    r.recent_at = r.publishes;
    r.pending.clear();
    return s;
  }
  if (cur) return cur;
  // The other slots of a ring that was just bound show up within a few
  // publishes.
  if (r.recent && r.publishes - r.recent_at < 16) {
    r.by_ring[ring] = r.recent;
    return r.recent;
  }
  return nullptr;
}

// Stop streams no ring has published for a while (a finished transition's
// respawned decoder would otherwise idle forever). Caller holds R().m.
void ReapIdleLocked(unsigned long now_ms) {
  Registry& r = R();
  std::vector<std::shared_ptr<BridgeState>> idle;
  for (auto it = r.by_ring.begin(); it != r.by_ring.end();) {
    if (now_ms - it->second->last_publish_ms > 5000) {
      idle.push_back(it->second);
      it = r.by_ring.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& s : idle) {
    if (s->alpha_peer) StopMovie(*s->alpha_peer);
    if (ProcValid(s->proc) || s->reader.joinable()) StopMovie(*s);
  }
}

}  // namespace

static std::atomic<uint32_t> g_burst_count{0};
uint32_t Vp6BridgeBurstCount() { return g_burst_count.load(); }

void Vp6BridgeNoteOpen(const char* host_path) {
  if (!BridgeOn() || !host_path) return;
  static std::atomic<unsigned> seq{0};
  BridgeLog("OPEN #%u %s", seq.fetch_add(1) + 1, host_path);
  // Every open of a wipe re-arms it (the same wipe is played again and again; the old
  // "same path as last time" dedup left later plays on the first play's ended stream).
  if (IsWipe(host_path)) {
    std::lock_guard<std::mutex> lk(R().m);
    R().last_env_path = host_path;
    R().pending = host_path;
    R().pending_since = R().publishes;
  }
}

void Vp6BridgeOnMovieSetup() {
  if (!BridgeOn()) return;
  std::lock_guard<std::mutex> lk(R().m);
  NoteOpenLocked("setup");
}

// Next host frame for a stream: the next decoded frame; a held last frame (ended wipe, underrun);
// or false when there is nothing to show (leave the guest's pixels).
bool FetchFrame(BridgeState& s, size_t fsz, std::vector<uint8_t>& frame, bool& served_new) {
  const size_t ysz = fsz * 2 / 3;
  (void)ysz;
  {
    std::unique_lock<std::mutex> lk(s.m);
    if (s.frame_size.load() != fsz) {
      // First publish (or dimension change): teach the reader the frame size
      // and slice whatever it staged so far.
      s.frame_size.store(fsz);
      size_t off = 0;
      while (s.staging.size() - off >= fsz) {
        s.frames.emplace_back(s.staging.begin() + off,
                              s.staging.begin() + off + fsz);
        off += fsz;
      }
      if (off) s.staging.erase(s.staging.begin(), s.staging.begin() + off);
    }
    // The very first frame of a movie: ffmpeg needs a moment after the spawn.
    // Wait briefly instead of showing the guest decoder's frame (the green
    // flash at the start of the intro movie).
    if (s.frames.empty() && s.frames_served == 0 && !s.eof.load()) {
      s.cv.wait_for(lk, std::chrono::milliseconds(200),
                    [&] { return !s.frames.empty() || s.eof.load(); });
    }
    if (!s.frames.empty()) {
      frame = std::move(s.frames.front());
      s.frames.pop_front();
      s.cv.notify_all();
    }
  }
  if (frame.empty()) {
    if (s.eof.load() && IsWipe(s.orig_path)) {
      // A wipe that has ended: hold its last frame, don't play it again.
      if (s.last.size() != fsz) return false;
      frame = s.last;
      served_new = false;
    } else if (s.eof.load()) {
      // Movie looped (menu attract, arena TV) - restart the decode from frame 0.
      std::string path = s.host_path;
      uint64_t served = s.frames_served;
      BridgeLog("eof after %llu frames - respawning %s",
                (unsigned long long)served, path.c_str());
      StartMovie(s, path);
    }
    // Underrun: repeat the last host frame rather than show the guest
    // decoder's (broken) one.
    if (s.last.size() != fsz) return false;
    frame = s.last;
    served_new = false;
  } else {
    ++s.frames_served;
  }
  return true;
}

void Vp6BridgePublish(uint32_t ring, uint8_t* y, uint8_t* u, uint8_t* v, uint32_t w,
                      uint32_t h, uint32_t y_pitch, uint32_t c_pitch, uint32_t caller_lr,
                      const uint32_t* desc, uint8_t* alpha, uint32_t alpha_size) {
  if (!BridgeOn()) return;
  {
    static const bool trace = std::getenv("NHL_VP6_TRACE") != nullptr;
    static std::atomic<int> tn{0};
    if (trace && w == 1024 && tn.fetch_add(1) < 600) {
      BridgeLog("pub lr=%08X ring=%08X %ux%u yp=%u cp=%u y=%p u=%p v=%p desc7=%08X d8=%08X d9=%08X d10=%08X d11=%08X", caller_lr,
                ring, w, h, y_pitch, c_pitch, (void*)y, (void*)u, (void*)v,
                desc ? desc[7] : 0, desc ? desc[8] : 0, desc ? desc[9] : 0,
                desc ? desc[10] : 0, desc ? desc[11] : 0);
    }
  }
  const unsigned long now_ms = HostTickMs();
  std::shared_ptr<BridgeState> sp;
  {
    std::lock_guard<std::mutex> lk(R().m);
    {
      // Diagnostics: per ring, log the start of each publish burst (a movie playing)
      // with size and the stream it is bound to.
      struct RingDiag { unsigned long last = 0; unsigned n = 0; };
      static std::map<uint32_t, RingDiag> diag;
      RingDiag& d = diag[ring];
      if (d.n == 0 || now_ms - d.last > 400) {
        auto it = R().by_ring.find(ring);
        BridgeLog("burst ring=%08X %ux%u pitch=%u/%u y=%p prev=%u bound=%s pending=%s", ring, w, h, y_pitch, c_pitch, (void*)y, d.n,
                  it != R().by_ring.end() ? it->second->host_path.c_str() : "-",
                  R().pending.empty() ? "-" : R().pending.c_str());
        d.n = 0;
        if (it == R().by_ring.end() ||
            it->second->host_path.find("/movies/environment_") == std::string::npos)
          g_burst_count.fetch_add(1);
      }
      ++d.n;
      d.last = now_ms;
    }
    NoteOpenLocked("publish");
    sp = StreamForRingLocked(ring);
    ++R().publishes;
    if (sp) sp->last_publish_ms = now_ms;
    if (R().publishes % 64 == 0) ReapIdleLocked(now_ms);
  }
  if (!sp || !ProcValid(sp->proc)) return;
  BridgeState& s = *sp;
  const size_t ysz = size_t(w) * h;
  const size_t csz = size_t(w / 2) * (h / 2);
  const size_t fsz = ysz + 2 * csz;
  std::vector<uint8_t> frame;
  bool served_new = true;
  if (!FetchFrame(s, fsz, frame, served_new)) return;
  // The ring renders vertically flipped relative to write order: write frame
  // row r into plane row (h-1-r).
  const uint8_t* fy = frame.data();
  const uint8_t* fu = frame.data() + ysz;
  const uint8_t* fv = frame.data() + ysz + csz;
  if (y) {
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(y + size_t(h - 1 - r) * y_pitch, fy + size_t(r) * w, w);
  }
  const uint32_t cw = w / 2, ch = h / 2;
  if (u) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(u + size_t(ch - 1 - r) * c_pitch, fu + size_t(r) * cw, cw);
  }
  if (v) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(v + size_t(ch - 1 - r) * c_pitch, fv + size_t(r) * cw, cw);
  }
  s.last = std::move(frame);
  // Alpha: the descriptor's 4th plane (+28, size +44) holds the movie's alpha, decoded by the
  // guest from the file's second (AL*) stream with the same broken IDCT. Replace it with the
  // host decode of that stream (one alpha frame per colour frame).
  if (s.alpha_peer) {
    static const bool reassert = [] { const char* e = std::getenv("NHL_VP6_ALPHA_REASSERT"); return !e || *e != '0'; }();
    // Did the guest overwrite the alpha plane we wrote at the previous publish (a later
    // writer than this hook)? Count differing sampled rows, then optionally write it again.
    if (s.prev_alpha_ptr && !s.prev_alpha_frame.empty()) {
      uint32_t bad = 0;
      for (uint32_t r = 0; r < h; r += 8)
        if (std::memcmp(s.prev_alpha_ptr + size_t(h - 1 - r) * y_pitch,
                        s.prev_alpha_frame.data() + size_t(r) * w, w) != 0)
          ++bad;
      static std::atomic<int> vn{0};
      if (vn.fetch_add(1) < 60)
        BridgeLog("alpha-verify prev slot: %u of %u sampled rows differ (guest wrote after us)", bad, h / 8);
      if (reassert && bad)
        for (uint32_t r = 0; r < h; ++r)
          std::memcpy(s.prev_alpha_ptr + size_t(h - 1 - r) * y_pitch,
                      s.prev_alpha_frame.data() + size_t(r) * w, w);
    }
    if (alpha && ProcValid(s.alpha_peer->proc)) {
      std::vector<uint8_t> af;
      bool an = true;
      if (FetchFrame(*s.alpha_peer, fsz, af, an)) {
        for (uint32_t r = 0; r < h; ++r)
          std::memcpy(alpha + size_t(h - 1 - r) * y_pitch, af.data() + size_t(r) * w, w);
        s.prev_alpha_ptr = alpha;
        s.prev_alpha_frame.assign(af.begin(), af.begin() + ysz);
        s.alpha_peer->last = std::move(af);
        ++s.alpha_writes;
      } else {
        ++s.alpha_skips;
      }
    } else {
      ++s.alpha_skips;
      static std::atomic<int> sn{0};
      if (sn.fetch_add(1) < 20)
        BridgeLog("alpha skip: ptr=%p size=%u need=%zu proc=%d", (void*)alpha, alpha_size, ysz,
                  ProcValid(s.alpha_peer->proc) ? 1 : 0);
    }
  }
  if (served_new && s.frames_served % 120 == 0) {
    BridgeLog("served %llu frames ring=%08X tick=%lu queue=%zu %s",
              (unsigned long long)s.frames_served, ring,
              (unsigned long)HostTickMs(), s.frames.size(), s.host_path.c_str());
  }
}
