#include "roster_choice.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace nhllegacy::rosters {
namespace fs = std::filesystem;
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string TrimWs(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool ReadHead(const fs::path& p, char (&buf)[16]) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.read(buf, sizeof buf);
  return f.gcount() == static_cast<std::streamsize>(sizeof buf);
}

}  // namespace

std::vector<std::string> ListSets(const fs::path& mods_root) {
  std::vector<std::string> out;
  try {
    std::error_code ec;
    for (fs::directory_iterator it(mods_root, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      const std::string id = it->path().filename().string();
      if (id.size() <= std::char_traits<char>::length(kPrefix) || Lower(id).rfind(kPrefix, 0) != 0) continue;
      auto st = it->symlink_status(e2);
      if (e2 || fs::is_symlink(st) || !fs::is_directory(st)) continue;
      out.push_back(id.substr(std::char_traits<char>::length(kPrefix)));
    }
  } catch (...) {
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool ValidateDb(const fs::path& db, const fs::path& stock, std::string& why) {
  try {
    std::error_code ec;
    auto st = fs::symlink_status(db, ec);
    if (ec || !fs::exists(st)) { why = "db/nhlng.db missing"; return false; }
    if (fs::is_symlink(st) || !fs::is_regular_file(st)) { why = "db/nhlng.db is not a regular file"; return false; }
    const auto sz = fs::file_size(db, ec);
    if (ec || sz < (1u << 20)) { why = "db/nhlng.db smaller than 1 MiB"; return false; }
    char a[16], b[16];
    if (!ReadHead(db, a)) { why = "db/nhlng.db unreadable"; return false; }
    if (ReadHead(stock, b) && std::equal(a, a + 16, b) == false) { why = "header differs from the stock nhlng.db"; return false; }
    return true;
  } catch (...) {
    why = "exception while checking db/nhlng.db";
    return false;
  }
}

Decision Decide(const fs::path& mods_root, const fs::path& stock_db, const std::string& wanted_raw) {
  Decision d;
  d.wanted = Lower(TrimWs(wanted_raw));
  if (d.wanted.empty()) d.wanted = "original";
  const auto sets = ListSets(mods_root);
  std::string chosen;  // directory-case name of the set, empty = original
  if (d.wanted != "original" && d.wanted != "default" && d.wanted != "stock") {
    for (const auto& s : sets)
      if (Lower(s) == d.wanted) chosen = s;
    if (chosen.empty()) {
      d.note = "no roster set '" + d.wanted + "' (mods/" + kPrefix + d.wanted + "/db/nhlng.db); using the original rosters";
    } else {
      std::string why;
      const fs::path db = mods_root / (std::string(kPrefix) + chosen) / "db" / "nhlng.db";
      if (!ValidateDb(db, stock_db, why)) {
        d.note = "roster set '" + chosen + "' rejected (" + why + "); using the original rosters";
        chosen.clear();
      }
    }
  }
  if (!chosen.empty()) d.active = chosen;
  for (const auto& s : sets)
    if (s != chosen) d.force_disabled.push_back(std::string(kPrefix) + s);
  return d;
}

}  // namespace nhllegacy::rosters
