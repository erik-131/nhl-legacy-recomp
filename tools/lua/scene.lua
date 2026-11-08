-- scene.lua: forced scenes for benchmarking (docs/bench-scenes.md). Generated config comes first (scene_cfg.lua):
--   SC_HOME, SC_AWAY (team ids), SC_AT (hook call that enters the match), SC_MODE ("gameplay"|"shootout"),
--   SC_TL = timeline: list of { call_after_entry, "label", function() ... end }.
-- The hook runs once per presented frame (~500/s in menus, ~700/s in a match), so call counts are a rough clock.
SC_N = 0
SC_T0 = nil
SC_I = 1
function SC_TICK()
  SC_N = SC_N + 1
  if SC_N == SC_AT then
    LOG("SC_ENTER_" .. SC_MODE .. "_" .. SC_HOME .. "_" .. SC_AWAY)
    if SC_MODE == "shootout" then EnterShootOutMode() else EnterGameplay(SC_HOME, SC_AWAY, SC_C, SC_D) end
    SC_T0 = SC_N
  end
  if SC_T0 and SC_TL then
    local e = SC_TL[SC_I]
    if e and SC_N - SC_T0 >= e[1] then
      SC_I = SC_I + 1
      LOG("SC_" .. e[2])
      SAFE(e[3])
    end
  end
end
RunEachFrame("SC_TICK()")
