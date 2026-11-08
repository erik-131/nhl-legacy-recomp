// nhllegacy - user mod layers. See mod_layers.h and docs/mod-layers.md.
#include "mod_layers.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace nhllegacy::mods {

namespace fs = std::filesystem;

namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

std::string LowerStr(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = Lower(c);
  return r;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool ReadFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// A single path component acceptable inside a mod (no separators, no drive colon,
// no control chars, not "." / "..", not hidden).
bool GoodComponent(const std::string& n) {
  if (n.empty() || n == "." || n == ".." || n[0] == '.') return false;
  for (unsigned char c : n)
    if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == 0x7f) return false;
  return true;
}

bool IsSymlink(const fs::path& p) {
  std::error_code ec;
  auto st = fs::symlink_status(p, ec);
  return ec || fs::is_symlink(st);
}

}  // namespace

bool NormalizeKey(std::string_view path, std::string& out) {
  out.clear();
  std::string r;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
    size_t j = i;
    while (j < path.size() && path[j] != '/' && path[j] != '\\') ++j;
    std::string_view comp = path.substr(i, j - i);
    i = j;
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") return false;
    for (unsigned char c : comp)
      if (c < 0x20 || c == ':' || c == 0x7f) return false;
    if (!r.empty()) r += '/';
    for (char c : comp) r += Lower(c);
  }
  if (r.empty()) return false;
  out = std::move(r);
  return true;
}

ModToml ParseModToml(std::string_view text) {
  ModToml t;
  std::istringstream in{std::string(text)};
  std::string raw;
  while (std::getline(in, raw)) {
    std::string_view line = Trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string key = LowerStr(Trim(line.substr(0, eq)));
    std::string_view val = Trim(line.substr(eq + 1));
    std::string sval;
    if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
      char q = val[0];
      size_t e = val.find(q, 1);
      sval = std::string(val.substr(1, e == std::string_view::npos ? std::string_view::npos : e - 1));
    } else {
      size_t h = val.find('#');
      sval = std::string(Trim(val.substr(0, h)));
    }
    if (key == "name") t.name = sval;
    else if (key == "description") t.description = sval;
    else if (key == "enabled") {
      std::string l = LowerStr(sval);
      t.has_enabled = true;
      t.enabled = !(l == "false" || l == "0" || l == "no" || l == "off");
    } else if (key == "priority") {
      char* end = nullptr;
      long v = std::strtol(sval.c_str(), &end, 10);
      if (end != sval.c_str()) {
        t.has_priority = true;
        t.priority = int(std::clamp(v, -1000000L, 1000000L));
      }
    }
  }
  return t;
}

LoadOrder ParseLoadOrder(std::string_view text) {
  LoadOrder lo;
  std::istringstream in{std::string(text)};
  std::string raw;
  while (std::getline(in, raw)) {
    std::string_view line = Trim(raw);
    if (line.empty() || line[0] == '#') continue;
    bool dis = false;
    if (line[0] == '-') {
      dis = true;
      line = Trim(line.substr(1));
    }
    if (line.empty()) continue;
    std::string id = LowerStr(line);
    (dis ? lo.disabled : lo.order).push_back(id);
  }
  return lo;
}

namespace {

// Recursively index regular files of an enabled mod. Never follows symlinks.
void WalkMod(const fs::path& dir, const std::string& rel_prefix, size_t depth,
             std::vector<std::pair<std::string, fs::path>>& out, size_t& rejected) {
  if (depth > 32) { ++rejected; return; }
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::none, ec), end;
  if (ec) return;
  std::vector<fs::directory_entry> ents;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    ents.push_back(*it);
  }
  std::sort(ents.begin(), ents.end());  // deterministic
  for (const auto& de : ents) {
    const std::string name = de.path().filename().string();
    std::error_code e2;
    auto st = de.symlink_status(e2);
    if (e2) { ++rejected; continue; }
    if (fs::is_symlink(st)) { ++rejected; continue; }  // never traverse or serve links
    if (!name.empty() && name[0] == '.') continue;      // hidden: silently ignored
    if (!GoodComponent(name)) { ++rejected; continue; }
    const std::string rel = rel_prefix.empty() ? name : rel_prefix + "/" + name;
    if (fs::is_directory(st)) {
      WalkMod(de.path(), rel, depth + 1, out, rejected);
    } else if (fs::is_regular_file(st)) {
      if (depth == 0 && LowerStr(name) == "mod.toml") continue;  // metadata, not game data
      out.emplace_back(rel, de.path());
    } else {
      ++rejected;
    }
  }
}

}  // namespace

ModIndex ModIndex::Scan(const fs::path& root, const LogFn& log, const std::vector<std::string>& force_disabled) {
  ModIndex idx;
  try {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return idx;

    // 1. discover candidate mods (one level, real directories only)
    struct Cand { ModInfo info; size_t base = 0; };
    std::vector<Cand> cands;
    std::vector<fs::directory_entry> top;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) top.push_back(*it);
    std::sort(top.begin(), top.end());
    for (const auto& de : top) {
      std::error_code e2;
      const std::string id = de.path().filename().string();
      auto st = de.symlink_status(e2);
      if (e2 || id.empty() || id[0] == '.') continue;
      if (fs::is_symlink(st)) {
        if (log) log("[nhl-mods] ignoring symlink '" + id + "' in mods dir");
        continue;
      }
      if (!fs::is_directory(st)) continue;
      if (!GoodComponent(id)) continue;
      Cand c;
      c.info.id = id;
      c.info.name = id;
      c.info.dir = de.path();
      std::string toml;
      if (ReadFile(de.path() / "mod.toml", toml) && !IsSymlink(de.path() / "mod.toml")) {
        ModToml t = ParseModToml(toml);
        if (!t.name.empty()) c.info.name = t.name;
        c.info.description = t.description;
        if (t.has_priority) c.info.priority = t.priority;
        if (t.has_enabled && !t.enabled) { c.info.enabled = false; c.info.disabled_by = "mod.toml"; }
      }
      cands.push_back(std::move(c));
    }

    // 2. load order: listed mods first (file order), unknown alphabetically after
    LoadOrder lo;
    std::string lotxt;
    if (ReadFile(root / "load_order.txt", lotxt) && !IsSymlink(root / "load_order.txt")) lo = ParseLoadOrder(lotxt);
    auto pos_of = [&](const std::string& lid) -> size_t {
      for (size_t i = lo.order.size(); i-- > 0;)  // last mention wins
        if (lo.order[i] == lid) return i;
      return lo.order.size();
    };
    std::vector<std::string> cand_lower;
    for (auto& c : cands) {
      const std::string lid = LowerStr(c.info.id);
      c.base = pos_of(lid);
      if (std::find(lo.disabled.begin(), lo.disabled.end(), lid) != lo.disabled.end()) {
        c.info.enabled = false;
        c.info.disabled_by = "load_order.txt";
      }
      for (const auto& f : force_disabled) {
        if (LowerStr(f) == lid) { c.info.enabled = false; c.info.disabled_by = "not selected"; }
      }
    }
    // cands is alphabetical (sorted above), stable_sort keeps that for unknowns
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      if (a.info.priority != b.info.priority) return a.info.priority < b.info.priority;
      return a.base < b.base;
    });

    // 3. index, lowest precedence first so later mods overwrite
    std::unordered_map<std::string, std::vector<uint32_t>> chains;  // only for collisions
    size_t rank = 0;
    for (auto& c : cands) {
      idx.mods_.push_back(c.info);
      const uint32_t mi = uint32_t(idx.mods_.size() - 1);
      ModInfo& mi_ref = idx.mods_.back();
      if (!mi_ref.enabled) continue;
      mi_ref.rank = rank++;
      std::vector<std::pair<std::string, fs::path>> found;
      WalkMod(mi_ref.dir, "", 0, found, mi_ref.rejected);
      for (auto& [rel, host] : found) {
        std::string key;
        if (!NormalizeKey(rel, key)) { ++mi_ref.rejected; continue; }
        std::error_code e3;
        fs::path abs = fs::absolute(host, e3);
        auto [it, inserted] = idx.files_.try_emplace(key, FileHit{abs, mi});
        ++mi_ref.files;
        if (inserted) {
          size_t cut = key.size();
          while ((cut = key.rfind('/', cut - 1)) != std::string::npos && cut > 0) {
            ++idx.dirs_[key.substr(0, cut)];
            if (cut == 0) break;
          }
        } else {
          auto& ch = chains[key];
          if (ch.empty()) ch.push_back(it->second.mod);
          ch.push_back(mi);
          it->second = FileHit{abs, mi};
        }
      }
    }
    for (auto& [key, ch] : chains) {
      Conflict cf;
      cf.path = key;
      cf.winner = idx.mods_[ch.back()].id;
      for (size_t i = 0; i + 1 < ch.size(); ++i) cf.losers.push_back(idx.mods_[ch[i]].id);
      idx.conflicts_.push_back(std::move(cf));
    }
    std::sort(idx.conflicts_.begin(), idx.conflicts_.end(),
              [](const Conflict& a, const Conflict& b) { return a.path < b.path; });
  } catch (...) {
    // A broken mods dir must never take the game down; return what we have, cleared.
    ModIndex empty;
    return empty;
  }
  return idx;
}

const FileHit* ModIndex::Resolve(std::string_view path) const {
  if (files_.empty()) return nullptr;
  std::string key;
  if (!NormalizeKey(path, key)) return nullptr;
  auto it = files_.find(key);
  return it == files_.end() ? nullptr : &it->second;
}

bool ModIndex::HasDir(std::string_view dir) const {
  std::string key;
  if (!NormalizeKey(dir, key)) return false;
  return dirs_.count(key) != 0;
}

size_t ModIndex::enabled_mods() const {
  size_t n = 0;
  for (const auto& m : mods_) n += m.enabled;
  return n;
}

std::vector<std::string> ModIndex::Manifest() const {
  std::vector<std::string> v;
  v.push_back("[nhl-mods] " + std::to_string(enabled_mods()) + " mods, " + std::to_string(files_.size()) +
              " files, " + std::to_string(conflicts_.size()) + " conflicts");
  for (const auto& m : mods_) {
    std::string l = "[nhl-mods]   " + (m.enabled ? "#" + std::to_string(m.rank) : std::string("off")) + " " + m.id;
    if (m.name != m.id) l += " (" + m.name + ")";
    if (m.priority) l += " priority=" + std::to_string(m.priority);
    if (m.enabled) l += " files=" + std::to_string(m.files);
    else l += " disabled by " + m.disabled_by;
    if (m.rejected) l += " rejected=" + std::to_string(m.rejected);
    v.push_back(std::move(l));
  }
  constexpr size_t kMaxConflictLines = 50;
  for (size_t i = 0; i < conflicts_.size() && i < kMaxConflictLines; ++i) {
    const auto& c = conflicts_[i];
    std::string l = "[nhl-mods]   conflict " + c.path + ": " + c.winner + " wins over";
    for (const auto& s : c.losers) l += " " + s;
    v.push_back(std::move(l));
  }
  if (conflicts_.size() > kMaxConflictLines)
    v.push_back("[nhl-mods]   ... " + std::to_string(conflicts_.size() - kMaxConflictLines) + " more conflicts");
  return v;
}

}  // namespace nhllegacy::mods
