// nhllegacy - ReXGlue Recompiled Project

#include "generated/default/nhllegacy_init.h"

#include "nhllegacy_app.h"
#include "pregame_menu/menu_sdl.h"

namespace {
// The SDK's main() has already parsed the command line and applied REX_* env overrides when it calls this creator, and no
// runtime exists yet: the earliest in-app point. The menu is OFF unless NHL_MENU=1 (RunPregameMenu returns at once).
std::unique_ptr<rex::ui::WindowedApp> CreateWithMenu(rex::ui::WindowedAppContext& ctx) {
  nhl::pregame::RunPregameMenu();
  return NhllegacyApp::Create(ctx);
}
}  // namespace

REX_DEFINE_APP(nhllegacy, CreateWithMenu)
