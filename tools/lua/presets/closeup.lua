-- preset closeup: player close-up camera right after the faceoff (call counts after EnterGameplay, ~760/s: 8800 = about +23 s from process start)
SC_TL = { {8800, "CAM_CloseOnPlayer", function() SetCamera("CloseOnPlayer") end} }
