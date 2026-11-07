-- WARNING: SetTunableValue is a silent no-op in the retail build (its name map pointer at guest 0x83B6F0FC is NULL, see docs/lua-scripting.md).
-- Kept as the exact script that was tested; it changes nothing.
-- tunables.lua: set engine tunables from a file.
-- SetTunableValue(name, value): both arguments are read with lua_tolstring, so numbers and strings work.
-- Unknown names are ignored silently (no error, no log). Values set at boot may be overwritten while a
-- match loads, so they are re-applied every 10th hook call.
TUNABLES = {
  gShowCPUAwayLineWhenUserHome = 0,   -- visible: hides the CPU away line box at the faceoff
}
function TUNE_APPLY()
  for name, value in pairs(TUNABLES) do SetTunableValue(name, value) end
end
TUNE_APPLY()
TUNE_N = 0
function TUNE_TICK()
  TUNE_N = TUNE_N + 1
  if TUNE_N % 10 == 0 then TUNE_APPLY() end
end
