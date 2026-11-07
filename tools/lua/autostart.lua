-- autostart.lua: start a match without any menu input.
-- EnterGameplay(home, away, 0, -1) is called once from the per-frame hook, after the front end is up.
-- home/away are team ids (5 = FLA, 12 = CAR in the stock DB). The two ice-intro screens still need
-- two Start presses (the bench pad script presses Start at 23 s and 31 s after launch).
-- Calling it earlier than the main menu (about 10 s after launch with the quick-start flags) does not work.
START_HOME, START_AWAY = 5, 12
START_AT_CALL = 5200        -- hook calls, about 11.5 s after launch (4400 is too early, 6000 works but 2.5 s slower)
AUTO_N = 0
function AUTO_TICK()
  AUTO_N = AUTO_N + 1
  if AUTO_N == START_AT_CALL then
    LOG("EnterGameplay_" .. START_HOME .. "_" .. START_AWAY)
    EnterGameplay(START_HOME, START_AWAY, 0, -1)
  end
end
RunEachFrame("AUTO_TICK()")   -- note: RunEachFrame keeps ONE string; a second call replaces the first
