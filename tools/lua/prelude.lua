-- LOG(msg): write a line into the host log from Lua.
-- A failed io.open shows up as  [NtCreateFile] FAILED: path='LUAMARK_<msg>'  with a timestamp.
-- Characters outside [%w_.-] are replaced, otherwise the game rejects the name and logs nothing.
function LOG(msg)
  io.open("LUAMARK_" .. string.gsub(tostring(msg), "[^%w_%.%-]", "_"), "r")
end

-- SAFE(f, ...): call f, report a Lua error through LOG instead of losing it silently.
function SAFE(f, ...)
  local ok, err = pcall(f, ...)
  if not ok then LOG("ERR_" .. tostring(err)) end
  return ok
end
