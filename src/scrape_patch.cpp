#include "scrape_patch.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <system_error>

namespace nhl {
namespace {

bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Start of the first line that begins with `word` (as a whole word) at or after `from`; npos if none.
size_t FindLine(const std::string& s, const std::string& word, size_t from) {
  for (size_t p = from; (p = s.find(word, p)) != std::string::npos; p += word.size()) {
    const bool line_start = p == 0 || s[p - 1] == '\n';
    const size_t e = p + word.size();
    if (line_start && (e >= s.size() || !IsWordChar(s[e]))) return p;
  }
  return std::string::npos;
}

}  // namespace

std::string RemoveReplayShadows(const std::string& scrape, bool* ok) {
  if (ok) *ok = false;
  const size_t proc_at = FindLine(scrape, "proc RenderShadows", 0);
  if (proc_at == std::string::npos) return scrape;
  const size_t end_at = FindLine(scrape, "endproc", proc_at);
  if (end_at == std::string::npos) return scrape;
  const size_t end_of_proc = end_at + 7;  // like the python regex, the proc spans up to and including "endproc"

  static const std::string kBegin = "ifnot renderoptions is_in_gameplay", kEnd = "endif";
  std::string body = scrape.substr(proc_at, end_of_proc - proc_at);
  bool cut = false;
  for (size_t b = 0; (b = body.find(kBegin)) != std::string::npos;) {
    const size_t e = body.find(kEnd, b + kBegin.size());
    if (e == std::string::npos) return scrape;  // unterminated block: do not guess
    body.erase(b, e + kEnd.size() - b);
    cut = true;
  }
  if (!cut) return scrape;
  if (ok) *ok = true;
  return scrape.substr(0, proc_at) + body + scrape.substr(end_of_proc);
}

void SyncReplayShadowPatch(bool enabled, const std::filesystem::path& game_data_root, const std::filesystem::path& cache_dir,
                           const std::function<void(const std::string&)>& log) {
  namespace fs = std::filesystem;
  const fs::path dir = cache_dir / "scrape", out = dir / "default.scrape", side = dir / ".nhl_patched";
  std::error_code ec;
  if (!enabled) {
    if (!fs::exists(side, ec)) return;  // not ours (or never patched): leave the file alone
    fs::remove(out, ec);
    fs::remove(side, ec);
    log("[nhl-scrape] replay shadows back on: removed the patched frame script");
    return;
  }
  const fs::path src = game_data_root / "_compiled" / "scrape" / "default.scrape";
  std::ifstream in(src, std::ios::binary);
  if (!in) { log("[nhl-scrape] no replay-shadow patch: cannot read " + src.string()); return; }
  std::stringstream ss;
  ss << in.rdbuf();
  bool ok = false;
  const std::string patched = RemoveReplayShadows(ss.str(), &ok);
  if (!ok) { log("[nhl-scrape] no replay-shadow patch: RenderShadows block not found in " + src.string()); return; }
  // A default.scrape in the cache without our sidecar is somebody else's (the game's own, a test); do not overwrite it.
  if (fs::exists(out, ec) && !fs::exists(side, ec)) {
    log("[nhl-scrape] no replay-shadow patch: " + out.string() + " exists and is not ours");
    return;
  }
  std::string old;
  {
    std::ifstream cur(out, std::ios::binary);
    if (cur) { std::stringstream cs; cs << cur.rdbuf(); old = cs.str(); }
  }
  if (old == patched && fs::exists(side, ec)) { log("[nhl-scrape] replay shadows off (frame script already patched)"); return; }
  fs::create_directories(dir, ec);
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  f.write(patched.data(), std::streamsize(patched.size()));
  f.close();
  if (!f) { log("[nhl-scrape] could not write " + out.string()); fs::remove(out, ec); return; }
  std::ofstream(side, std::ios::trunc) << "written by nhllegacy (REX_NHL_NO_REPLAY_SHADOWS=1); deleted when the option is off\n";
  log("[nhl-scrape] replay shadows off: wrote patched frame script (" + std::to_string(ss.str().size() - patched.size()) +
      " bytes removed) to " + out.string());
}

}  // namespace nhl
