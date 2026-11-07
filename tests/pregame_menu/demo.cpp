// Prints the menu as text after a scripted key string, to look at the layout without a game or a window.
//   keys: u d l r = d-pad, a b x y, [ = LB, ] = RB, s = Start, k = Back. Example: demo "]rd"
#include <cstdio>
#include <string>
#include "../../src/pregame_menu/menu_core.h"
using namespace nhl::pregame;
int main(int argc, char** argv) {
  const auto& opts = DefaultOptions();
  if (argc > 1 && std::string(argv[1]) == "--table") {  // markdown table for docs/menu-design.md
    for (const auto& o : opts) {
      if (o.kind != Kind::Choice) continue;
      std::string set, ch;
      for (size_t i = 0; i < o.choices.size(); ++i) {
        ch += (i ? " / " : "") + o.choices[i].label + (int(i) == o.def ? " (default)" : "");
        for (const auto& a : o.choices[i].assigns)
          if (a.target != Target::Menu) set += (set.empty() ? "" : ", ") + a.name + "=" + (a.value.empty() ? "(erase)" : a.value);
      }
      std::printf("| %s | %s | %s | %s | %s | %s |\n", o.page.c_str(), o.label.c_str(), ch.c_str(), o.help.c_str(),
                  o.restart ? "yes" : "no", set.c_str());
    }
    return 0;
  }
  Menu m(opts, Settings(opts));
  uint64_t t = 0;
  m.Update(0, t);
  for (const char* p = argc > 1 ? argv[1] : ""; *p; ++p) {
    uint32_t b = *p == 'u' ? kUp : *p == 'd' ? kDown : *p == 'l' ? kLeft : *p == 'r' ? kRight : *p == 'a' ? kA :
                 *p == 'b' ? kB : *p == 'x' ? kX : *p == 'y' ? kY : *p == '[' ? kLB : *p == ']' ? kRB :
                 *p == 's' ? kStart : *p == 'k' ? kBack : 0;
    m.Update(b, t += 16); m.Update(0, t += 16);
  }
  for (const auto& l : RenderLines(BuildView(m, t), 64)) std::printf("%s\n", l.c_str());
}
