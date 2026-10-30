#!/bin/bash
# Start/stop the Wolf Steam session headless (no Moonlight client) via the Wolf API lobby endpoint —
# the same call Wolf UI makes for "User -> Steam -> Start". Usage: wolf_session.sh start|stop|status
# Needs sudo (the API socket lives inside the wolf container, reached via /proc/<pid>/root).
set -euo pipefail
WPID=$(pgrep -x wolf | head -1) || { echo "wolf not running"; exit 1; }
S=/proc/$WPID/root/run/user/wolf/wolf.sock
api() { sudo curl -s --unix-socket "$S" "$@"; }
steam_c() { docker ps --format '{{.Names}}' | grep -i steam | head -1; }
case "${1:-status}" in
start)
  [ -n "$(steam_c)" ] && { echo "already running: $(steam_c)"; exit 0; }
  # runner_state_folder is mounted directly as /home/retro, so it must be the WolfSteam folder.
  api http://localhost/api/v1/profiles | python3 -c '
import json,sys
p=[p for p in json.load(sys.stdin)["profiles"] if p["id"]=="user"][0]
a=[a for a in p["apps"] if a["title"]=="Steam"][0]
print(json.dumps({"profile_id":"user","name":"Steam","icon_png_path":a.get("icon_png_path"),"multi_user":False,
 "pin":None,"stop_when_everyone_leaves":False,
 "video_settings":{"width":1920,"height":1080,"refresh_rate":60,"wayland_render_node":"/dev/dri/renderD128",
   "runner_render_node":"/dev/dri/renderD128","video_producer_buffer_caps":"video/x-raw(memory:DMABuf)"},
 "audio_settings":{"channel_count":2},"client_settings":None,
 "runner_state_folder":"/etc/wolf/profile-data/user/WolfSteam","runner":a["runner"]}))' |
    api -X POST -H 'Content-Type: application/json' --data @- http://localhost/api/v1/lobbies/create; echo
  for _ in $(seq 60); do C=$(steam_c); [ -n "$C" ] && docker exec "$C" pgrep -f ubuntu12_32/steam >/dev/null && break; sleep 3; done
  echo "container: $(steam_c)";;
stop)
  for id in $(api http://localhost/api/v1/lobbies | python3 -c 'import json,sys; [print(l["id"]) for l in json.load(sys.stdin)["lobbies"]]'); do
    api -X POST -H 'Content-Type: application/json' -d "{\"lobby_id\":\"$id\"}" http://localhost/api/v1/lobbies/stop; echo " $id"
  done;;
status)
  api http://localhost/api/v1/lobbies; echo; echo "container: $(steam_c)";;
esac
