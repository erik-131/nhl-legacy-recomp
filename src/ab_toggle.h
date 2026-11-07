// In-process A/B switching of runtime cvars (NHL_AB_TOGGLE), for faster benchmarks.
//
// A bench run spends ~3 of its 5 minutes booting and navigating menus, and two runs land on
// different matchups, camera paths and temperatures. Alternating a runtime cvar inside one
// match compares both settings on the same scene and costs one run instead of two or more.
//
// Format: <cvar>=<a>,<b>[;<cvar>=<a>,<b>...]@<seconds>, e.g.
//   NHL_AB_TOGGLE="nhl_upload_prepass=false,true@20"
// Phase A applies every <a> value, phase B every <b> value; the phases alternate every <seconds>
// starting with A, and each switch logs "[nhl-ab] phase=A|B" (tools/nuc/bench/abphase.py splits
// the per-second fps/present lines by phase). Only cvars that are read at run time take effect.

#pragma once

namespace nhllegacy {

// No-op unless NHL_AB_TOGGLE is set. Spawns a detached thread.
void StartAbToggle();

}  // namespace nhllegacy
