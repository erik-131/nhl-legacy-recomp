// Unit tests for src/mod_layers.cpp. Build+run (no engine needed):
//   clang++-20 -std=c++20 -Isrc tests/mod_layers/test_mod_layers.cpp src/mod_layers.cpp -o /tmp/t && /tmp/t
#include "mod_layers.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace nhllegacy::mods;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static fs::path g_root;
static void Put(const std::string& rel, const std::string& data = "x") {
  fs::path p = g_root / rel;
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}
static void Fresh() {
  fs::remove_all(g_root);
  fs::create_directories(g_root);
}
static std::string Winner(const ModIndex& i, const char* path) {
  auto* h = i.Resolve(path);
  return h ? i.mods()[h->mod].id : "";
}

int main() {
  g_root = fs::temp_directory_path() / ("modlayers_test_" + std::to_string(getpid()));

  { // empty / missing dir
    Fresh();
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(i.empty() && i.mods().empty() && i.conflicts().empty());
    CHECK(i.Resolve("db/nhlng.db") == nullptr);
    CHECK(i.Manifest()[0] == "[nhl-mods] 0 mods, 0 files, 0 conflicts");
    ModIndex j = ModIndex::Scan(g_root / "nope");
    CHECK(j.empty());
  }
  { // basic + case-insensitive + separators
    Fresh();
    Put("a/db/NHLNG.db");
    Put("a/fe/Menu/x.bin");
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(i.file_count() == 2);
    CHECK(Winner(i, "db/nhlng.db") == "a");
    CHECK(Winner(i, "DB\\NhlNg.DB") == "a");
    CHECK(Winner(i, "\\\\db//nhlng.db") == "a");
    CHECK(Winner(i, "./fe/menu/X.BIN") == "a");
    CHECK(i.Resolve("db/other.db") == nullptr);
    CHECK(i.HasDir("FE\\menu") && i.HasDir("db") && !i.HasDir("rendering"));
    CHECK(i.Resolve("db/nhlng.db")->host.is_absolute());
  }
  { // default order alphabetical; later wins; conflict report
    Fresh();
    Put("alpha/db/nhlng.db"); Put("beta/db/nhlng.db"); Put("beta/only_b.txt"); Put("alpha/only_a.txt");
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(Winner(i, "db/nhlng.db") == "beta");
    CHECK(i.conflicts().size() == 1);
    CHECK(i.conflicts()[0].winner == "beta" && i.conflicts()[0].losers.size() == 1 && i.conflicts()[0].losers[0] == "alpha");
    CHECK(Winner(i, "only_a.txt") == "alpha");
    CHECK(i.Manifest()[0] == "[nhl-mods] 2 mods, 3 files, 1 conflicts");
  }
  { // load_order: explicit order, unknown appended alphabetically, -disable, comments
    Fresh();
    Put("alpha/f"); Put("beta/f"); Put("gamma/f"); Put("delta/f");
    { std::ofstream(g_root / "load_order.txt") << "# c\nGamma\n\nalpha\n-delta\n"; }
    ModIndex i = ModIndex::Scan(g_root);
    // gamma < alpha < beta(unknown); delta disabled
    CHECK(Winner(i, "f") == "beta");
    CHECK(i.enabled_mods() == 3);
    CHECK(i.conflicts().size() == 1 && i.conflicts()[0].losers.size() == 2);
    CHECK(i.conflicts()[0].losers[0] == "gamma" && i.conflicts()[0].losers[1] == "alpha");
    { std::ofstream(g_root / "load_order.txt") << "beta\nalpha\n"; }
    ModIndex k = ModIndex::Scan(g_root);
    CHECK(Winner(k, "f") == "gamma"); // gamma unknown -> after listed, but delta before it alphabetically
  }
  { // disabled via mod.toml; priority
    Fresh();
    Put("a/f"); Put("b/f"); Put("c/f");
    { std::ofstream(g_root / "c/mod.toml") << "name = \"Cee\"\nenabled = false\n"; }
    { std::ofstream(g_root / "a/mod.toml") << "[mod]\nname='Aye' # n\ndescription = \"d\"\npriority = 10\n"; }
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(Winner(i, "f") == "a"); // priority beats alphabetical
    CHECK(i.enabled_mods() == 2);
    CHECK(i.file_count() == 1); // mod.toml is not game data
    CHECK(i.Resolve("mod.toml") == nullptr);
    bool saw = false;
    for (auto& m : i.mods()) if (m.id == "a") { saw = m.name == "Aye" && m.description == "d" && m.priority == 10; }
    CHECK(saw);
    for (auto& m : i.mods()) if (m.id == "c") CHECK(!m.enabled && m.disabled_by == "mod.toml" && m.files == 0);
  }
  { // path traversal
    std::string o;
    CHECK(!NormalizeKey("../x", o) && o.empty());
    CHECK(!NormalizeKey("a/../../x", o));
    CHECK(!NormalizeKey("a\\..\\x", o));
    CHECK(!NormalizeKey("C:\\x", o));
    CHECK(!NormalizeKey("", o) && !NormalizeKey("///", o));
    CHECK(!NormalizeKey(std::string("a\0b", 3), o));
    CHECK(NormalizeKey("A//B\\\\c.D", o) && o == "a/b/c.d");
    Fresh();
    Put("a/db/f");
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(i.Resolve("../a/db/f") == nullptr);
    CHECK(i.Resolve("db/../db/f") == nullptr);
    // hostile names on disk are rejected, not indexed
    Put("a/bad:name"); Put("a/back\\slash");
    ModIndex j = ModIndex::Scan(g_root);
    CHECK(j.file_count() == 1);
    CHECK(j.mods()[0].rejected == 2);
  }
  { // symlinks: file, dir, whole mod, load_order target
    Fresh();
    fs::path secret = g_root.parent_path() / ("modlayers_secret_" + std::to_string(getpid()));
    fs::create_directories(secret);
    std::ofstream(secret / "game.dat") << "secret";
    Put("a/real");
    fs::create_symlink(secret / "game.dat", g_root / "a/linkfile");
    fs::create_directory_symlink(secret, g_root / "a/linkdir");
    fs::create_directory_symlink(secret, g_root / "linkmod");
    ModIndex i = ModIndex::Scan(g_root);
    CHECK(i.mods().size() == 1);
    CHECK(i.file_count() == 1 && i.Resolve("real") != nullptr);
    CHECK(i.Resolve("linkfile") == nullptr);
    CHECK(i.Resolve("linkdir/game.dat") == nullptr);
    CHECK(i.mods()[0].rejected == 2);
    fs::remove_all(secret);
  }
  fs::remove_all(g_root);
  std::printf(g_fail ? "FAILED (%d)\n" : "all tests passed\n", g_fail);
  return g_fail ? 1 : 0;
}
