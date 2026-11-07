// Unit tests for src/scrape_patch. Build and run: tools/scrape_patch/run_tests.sh. Fake script text only.
#include <cstdio>
#include <string>

#include "scrape_patch.h"

static int g_n = 0, g_fail = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string ToCrlf(const std::string& s) {
  std::string o;
  for (char c : s) { if (c == '\n') o += '\r'; o += c; }
  return o;
}

int main() {
  const std::string fake =
      "proc Other\n\tifnot renderoptions is_in_gameplay\n\t\tkeepme\n\tendif\nendproc\n"
      "proc RenderShadows\n\tbegineventmsg Shadows\n\t\n\tifnot renderoptions is_in_gameplay\n\t\tsetgpr 112 16\n"
      "\t\tdispatchshadowbegin\n\tendif\n\t\n\tendeventmsg\nendproc\n"
      "proc RenderShadowsLater\n\tifnot renderoptions is_in_gameplay\n\t\tkeep2\n\tendif\nendproc\n";
  const std::string want =
      "proc Other\n\tifnot renderoptions is_in_gameplay\n\t\tkeepme\n\tendif\nendproc\n"
      "proc RenderShadows\n\tbegineventmsg Shadows\n\t\n\t\n\t\n\tendeventmsg\nendproc\n"
      "proc RenderShadowsLater\n\tifnot renderoptions is_in_gameplay\n\t\tkeep2\n\tendif\nendproc\n";
  bool ok = false;
  CHECK(nhl::RemoveReplayShadows(fake, &ok) == want && ok);
  CHECK(nhl::RemoveReplayShadows(ToCrlf(fake), &ok) == ToCrlf(want) && ok);   // CRLF kept
  CHECK(nhl::RemoveReplayShadows(want, &ok) == want && !ok);                  // second pass: nothing to do, reported
  const std::string none = "proc Foo\n\tnop\nendproc\n";
  CHECK(nhl::RemoveReplayShadows(none, &ok) == none && !ok);                  // no proc
  const std::string noblock = "proc RenderShadows\n\tnop\nendproc\n";
  CHECK(nhl::RemoveReplayShadows(noblock, &ok) == noblock && !ok);
  const std::string broken = "proc RenderShadows\n\tifnot renderoptions is_in_gameplay\n\tnop\nendproc\n";
  CHECK(nhl::RemoveReplayShadows(broken, &ok) == broken && !ok);              // no endif
  const std::string noend = "proc RenderShadows\n\tifnot renderoptions is_in_gameplay\nendif\n";
  CHECK(nhl::RemoveReplayShadows(noend, &ok) == noend && !ok);                // no endproc
  CHECK(nhl::RemoveReplayShadows("", &ok).empty() && !ok);
  CHECK(nhl::RemoveReplayShadows(fake) == want);                              // ok pointer optional
  std::printf("%d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
