# tools/lua: file-only Lua scripts for the recompiled game

Original scripts written by us (no game text). How to use them and what is known: `docs/lua-scripting.md`.

- `prelude.lua`: `LOG(msg)` helper (log channel through a failed `io.open`).
- `autostart.lua`: straight-to-match, calls `EnterGameplay(home, away, 0, -1)` from the per-frame hook.
- `tunables.lua`: SetTunableValue example (tested: has NO effect in this build, see the doc).
- `hook_log.lua`: per-frame hook that logs every N calls.
- `scenedef_tail.lua`: the one line to append to a copy of `scrape/scenedef.lua` so the game loads the scripts.
- `stage.sh`: builds a scratch cache root (copy of the game's scenedef plus the scripts) for `REX_CACHE_ROOT`.
