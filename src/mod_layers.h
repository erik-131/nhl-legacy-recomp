// nhllegacy - user mod layers (`mods/<name>/<game-relative path>`).
//
// Pure logic, no engine dependencies: scans a mods directory once at startup into an
// immutable in-memory index (normalised path -> winning host file + mod). Resolve() is
// a single hash lookup with no directory probing and no locking. See docs/mod-layers.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nhllegacy::mods {

// Normalise a game-relative path to the index key: '\' -> '/', ASCII lowercase,
// leading separators and "./" stripped, duplicate separators collapsed. Returns false
// (and leaves `out` empty) for anything that could escape or is not a plain relative
// path: "..", absolute/drive paths (':'), NUL/control chars, empty.
bool NormalizeKey(std::string_view path, std::string& out);

struct ModInfo {
  std::string id;           // directory name (what load_order.txt refers to)
  std::string name;         // mod.toml name, else id
  std::string description;  // mod.toml description
  std::filesystem::path dir;
  bool enabled = true;      // false if mod.toml enabled=false or "-id" in load_order.txt
  std::string disabled_by;  // "mod.toml" / "load_order.txt" / "symlink" when not enabled
  int priority = 0;         // mod.toml priority; higher wins regardless of load order
  size_t files = 0;         // indexed files (0 for disabled mods)
  size_t rejected = 0;      // entries skipped: symlinks, bad names
  size_t rank = 0;          // final precedence position, higher wins (enabled mods only)
};

struct FileHit {
  std::filesystem::path host;  // absolute host file
  uint32_t mod = 0;            // index into mods()
};

struct Conflict {
  std::string path;                // normalised key
  std::string winner;              // mod id
  std::vector<std::string> losers; // mod ids, lowest precedence first
};

class ModIndex {
 public:
  using LogFn = std::function<void(const std::string&)>;

  // Never throws. A missing/empty/unreadable root yields an empty index.
  static ModIndex Scan(const std::filesystem::path& root, const LogFn& log = {});

  // O(1). `path` may use '\' or '/', any case, leading separators. nullptr = no mod
  // provides it (or the path is invalid).
  const FileHit* Resolve(std::string_view path) const;

  const std::vector<ModInfo>& mods() const { return mods_; }  // precedence order, low -> high
  const std::vector<Conflict>& conflicts() const { return conflicts_; }
  size_t file_count() const { return files_.size(); }
  size_t enabled_mods() const;
  bool empty() const { return files_.empty(); }

  // True if any enabled mod has a file under directory `dir` (normalised like paths).
  // Lets a device decide whether a directory exists only through mods.
  bool HasDir(std::string_view dir) const;

  // "[nhl-mods] N mods, M files, K conflicts" followed by per-mod and per-conflict lines.
  std::vector<std::string> Manifest() const;

 private:
  std::vector<ModInfo> mods_;
  std::unordered_map<std::string, FileHit> files_;
  std::unordered_map<std::string, uint32_t> dirs_;  // normalised dir -> file count
  std::vector<Conflict> conflicts_;
};

// Exposed for tests.
struct ModToml {
  std::string name, description;
  bool has_enabled = false, enabled = true;
  bool has_priority = false;
  int priority = 0;
};
ModToml ParseModToml(std::string_view text);

struct LoadOrder {
  std::vector<std::string> order;     // lowercase ids, in file order
  std::vector<std::string> disabled;  // lowercase ids ("-id")
};
LoadOrder ParseLoadOrder(std::string_view text);

}  // namespace nhllegacy::mods
