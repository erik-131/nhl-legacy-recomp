-- camera by name (SetCamera takes a name from the game's camera table, see docs/bench-scenes.md)
local names = {"CloseOnPlayer","FocusOnPlayer","WideStadium_Broadcast","ReplayBroadcastClose","ReplaySlashISO","InsideNet","PostWhistle_Corner","GAMEPLAY_TRUE_BROADCAST","PLAYER_VIEW","VideoReplay_Broadcast","AboveNet","FACEOFF_SIDE_VIEW"}
SC_TL = {}
for i, n in ipairs(names) do
  SC_TL[i] = {9500 + (i - 1) * 1500, "CAM_" .. n, function() SetCamera(n) end}
end
