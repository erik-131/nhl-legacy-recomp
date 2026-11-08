// clang++-20 -std=c++20 -Isrc tests/roster_choice/test_roster_choice.cpp src/roster_choice.cpp src/mod_layers.cpp -o /tmp/trc && /tmp/trc
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include "mod_layers.h"
#include "roster_choice.h"

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static void Put(const fs::path& p, size_t n, char fill, char first = 'T') {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  std::string s(n, fill);
  if (n) s[0] = first;
  f << s;
}

int main() {
  using namespace nhllegacy::rosters;
  fs::path root = fs::temp_directory_path() / ("rc_test_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::path mods = root / "mods", stock = root / "stock" / "nhlng.db";
  Put(stock, 2 << 20, 'a');
  Put(mods / "roster-2026" / "db" / "nhlng.db", 2 << 20, 'a');   // same header as stock
  Put(mods / "roster-tiny" / "db" / "nhlng.db", 100, 'a');       // too small
  Put(mods / "roster-badhdr" / "db" / "nhlng.db", 2 << 20, 'a', 'X');
  fs::create_directories(mods / "roster-empty");
  fs::create_directories(mods / "other-mod");
  CHECK((ListSets(mods) == std::vector<std::string>{"2026", "badhdr", "empty", "tiny"}));

  auto d = Decide(mods, stock, "");
  CHECK(d.active == "original" && d.note.empty() && d.force_disabled.size() == 4);
  d = Decide(mods, stock, "Original");
  CHECK(d.active == "original" && d.note.empty());
  d = Decide(mods, stock, " 2026\n");
  CHECK(d.active == "2026" && d.note.empty() && d.force_disabled.size() == 3);
  for (auto& s : d.force_disabled) CHECK(s != "roster-2026");
  for (const char* bad : {"tiny", "badhdr", "empty", "nope", "../x"}) {
    d = Decide(mods, stock, bad);
    CHECK(d.active == "original" && !d.note.empty() && d.force_disabled.size() == 4);
  }
  // stock unreadable: header check skipped, size check still applies
  d = Decide(mods, root / "nostock", "badhdr");
  CHECK(d.active == "badhdr");
  d = Decide(root / "nomods", stock, "2026");
  CHECK(d.active == "original" && !d.note.empty());

  // integration with ModIndex: only the active set serves the DB
  d = Decide(mods, stock, "2026");
  auto idx = nhllegacy::mods::ModIndex::Scan(mods, {}, d.force_disabled);
  const auto* hit = idx.Resolve("db\\nhlng.db");
  CHECK(hit && hit->host.string().find("roster-2026") != std::string::npos);
  d = Decide(mods, stock, "original");
  idx = nhllegacy::mods::ModIndex::Scan(mods, {}, d.force_disabled);
  CHECK(idx.Resolve("db/nhlng.db") == nullptr);
  d = Decide(mods, stock, "tiny");  // rejected -> nothing served, not the broken file
  idx = nhllegacy::mods::ModIndex::Scan(mods, {}, d.force_disabled);
  CHECK(idx.Resolve("db/nhlng.db") == nullptr);

  fs::remove_all(root);
  std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
