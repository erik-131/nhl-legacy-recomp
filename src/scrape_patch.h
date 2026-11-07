// Pure text transforms of the game's frame script (scrape/default.scrape). No game data lives here.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace nhl {

// Removes the `ifnot renderoptions is_in_gameplay ... endif` block(s) inside `proc RenderShadows` (shadow maps that only run
// outside gameplay: replays, cutscenes). Same result as tools/nuc/bench/scrape_variant.py "noshadow": the block text is cut,
// surrounding whitespace and line endings (LF or CRLF) stay as they are. On failure (no such proc or block) returns the input
// unchanged and sets *ok = false.
std::string RemoveReplayShadows(const std::string& scrape, bool* ok = nullptr);

// Opt-in file hook. `enabled`: read <game_data_root>/_compiled/scrape/default.scrape, patch it and write
// <cache_dir>/scrape/default.scrape plus the sidecar <cache_dir>/scrape/.nhl_patched (only rewritten if content differs).
// Not enabled: delete the patched file and sidecar, but only when the sidecar exists (a default.scrape without it is the
// game's own and is never touched). Messages go to `log` ("[nhl-scrape] ...").
void SyncReplayShadowPatch(bool enabled, const std::filesystem::path& game_data_root, const std::filesystem::path& cache_dir,
                           const std::function<void(const std::string&)>& log);

}  // namespace nhl
