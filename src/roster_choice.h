// Roster set choice: which player/team database the game boots with. Pure logic, no engine dependencies.
// Docs: docs/roster-choice.md.
//
// A roster set is a mod directory `mods/roster-<name>/` holding `db/nhlng.db` (see docs/mod-layers.md). At most one set is
// active; "original" (or nothing, or any failure) means no set is active and the game reads its stock `_compiled/db/nhlng.db`.
// The choice comes from env NHL_ROSTERS, else the key `rosters` of nhl_enhancements.ini (written by the pregame menu).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nhllegacy::rosters {

constexpr const char* kPrefix = "roster-";

struct Decision {
  std::string wanted;                    // normalised choice ("original" when empty)
  std::string active = "original";      // set actually used ("original" or a set name)
  std::vector<std::string> force_disabled;  // mod ids to pass to ModIndex::Scan (all sets but the active one)
  std::string note;                      // why we fell back, empty if the choice was honoured
};

// Names (without prefix) of the roster-* directories in `mods_root`, sorted. Symlinks and dot names are skipped.
std::vector<std::string> ListSets(const std::filesystem::path& mods_root);

// Is `db` usable as the game's nhlng.db? Regular non-symlink file, >= 1 MiB, first 16 bytes equal to `stock`'s when
// `stock` is readable. On failure `why` says what is wrong.
bool ValidateDb(const std::filesystem::path& db, const std::filesystem::path& stock, std::string& why);

// Never throws. wanted: "", "original", "default", "stock" -> original; otherwise a set name (any case).
Decision Decide(const std::filesystem::path& mods_root, const std::filesystem::path& stock_db, const std::string& wanted);

}  // namespace nhllegacy::rosters
