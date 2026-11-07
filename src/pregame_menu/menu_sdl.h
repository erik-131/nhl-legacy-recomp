// Pre-game settings menu: SDL3 window, input and startup glue (design A in docs/menu-design.md).
// Logic lives in menu_core; this file only draws RenderLines() with SDL_RenderDebugText and feeds pad bits.
#pragma once

namespace nhl::pregame {

enum class MenuResult {
  Disabled,   // NHL_MENU not "1": nothing was touched (current launcher behaviour)
  Skipped,    // menu.show=hold without Back held, or no input device: saved settings applied, no window
  Started,    // player pressed Start: settings saved and applied
  Discarded,  // B / Back / window closed: saved settings (not this session's edits) applied
  TimedOut,   // auto-start timer (NHL_MENU_AUTOSTART_S, default 120) ran out: same as Discarded
  Failed,     // SDL or window failure: logged, nothing applied, the game starts as before
};

// Runs the menu if NHL_MENU=1, then applies the settings to the environment / nhl_enhancements.ini and re-applies REX_*
// cvar env overrides. Never throws and never blocks longer than the auto-start timeout. Call before the app is created.
MenuResult RunPregameMenu();

}  // namespace nhl::pregame
