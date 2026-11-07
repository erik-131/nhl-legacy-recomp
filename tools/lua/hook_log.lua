-- hook_log.lua: per-frame hook that logs every HOOK_EVERY calls.
-- The hook runs once per guest frame (about 270-500 calls/s in the menus, ~700/s in a match on the
-- bench with swap interval 0, fewer when vsync-limited). There is no clock in the Lua state (no os,
-- debug, package), so use the host log timestamps to turn calls into seconds.
HOOK_EVERY = 1200
HOOK_N = 0
function HOOK_TICK()
  HOOK_N = HOOK_N + 1
  if HOOK_N % HOOK_EVERY == 0 then LOG("hook_" .. HOOK_N) end
end
RunEachFrame("HOOK_TICK()")
