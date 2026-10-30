#!/bin/bash
# Forced-command gate on CT 105 (Games on Whales) for the nhl-build CT.
# Installed as /root/nhlbench/nhl_gate.sh; authorized_keys line:
#   restrict,from="<BUILD_CT_IP>",command="/root/nhlbench/nhl_gate.sh" ssh-ed25519 AAAA... nhl-build
# Only the verbs below are possible; everything else is refused.
set -u
W=/etc/wolf/profile-data/user/WolfSteam
B=$W/nhl-linux
OUT=/root/nhlbench
read -r -a A <<< "${SSH_ORIGINAL_COMMAND:-}"
VERB=${A[0]:-help}
ok_name() { [[ "$1" =~ ^[A-Za-z0-9._-]{1,64}$ ]]; }
case "$VERB" in
  install)   # stdin: nhl-linux.tar.xz (top dir nhl-linux/)
    H=$(stat -c '%u:%g' $W)
    T=$(mktemp -d); xz -dc | tar -xf - -C "$T" || { echo "bad archive"; exit 1; }
    [ -x "$T/nhl-linux/nhllegacy" ] || { echo "missing nhl-linux/nhllegacy"; exit 1; }
    mkdir -p $B/logs
    cp -f "$T"/nhl-linux/{nhllegacy,librexruntime.so,librexgpu-xenos.so} $B/ 2>/dev/null
    [ -f "$T/nhl-linux/libsnappy.so.1" ] && cp -f "$T/nhl-linux/libsnappy.so.1" $B/
    ln -sfn ../nhl/game $B/game; chown -R $H $B; rm -rf "$T"
    sha256sum $B/nhllegacy $B/librexgpu-xenos.so $B/librexruntime.so; echo INSTALL-OK ;;
  bench)     # bench <tag> [NHL_X=v REX_Y=v ...]
    TAG=${A[1]:-}; ok_name "$TAG" || { echo "bad tag"; exit 1; }
    ENVS=""
    for e in "${A[@]:2}"; do
      [[ "$e" =~ ^(NHL|REX)_[A-Z0-9_]{1,48}=[A-Za-z0-9_.,:-]{0,64}$ ]] || { echo "refused env: $e"; exit 1; }
      ENVS="$ENVS $e"
    done
    pgrep -f nhl_bench.sh >/dev/null && { echo "a benchmark is already running"; exit 1; }
    nohup $OUT/nhl_bench.sh "$TAG" "${ENVS# }" > $OUT/last_run.out 2>&1 &
    echo "started $TAG ($ENVS)" ;;
  status)    cat $OUT/last_run.out 2>/dev/null; pgrep -af "nhl_bench|nhllegacy" | cut -c1-120; free -m | head -2 ;;
  result)    ok_name "${A[1]:-}" && cat "$OUT/${A[1]}.txt" "$OUT/${A[1]}.threads" 2>/dev/null ;;
  shot)      [[ "${A[1]:-}" =~ ^shot_[0-9]{1,4}s\.png$ ]] && cat "$B/${A[1]}" ;;
  log)       L=$(ls -t $B/logs/*.log 2>/dev/null | head -1); [ -n "$L" ] && grep -E "nhl-|error|critical|Vulkan device" "$L" | tail -n "${A[1]:-200}" | cut -c1-240 ;;
  xex)       cat $W/nhl/game/default.xex ;;
  stop)      C=$(docker ps --format '{{.Names}}' | grep -i steam | head -1); [ -n "$C" ] && docker exec "$C" pkill -x nhllegacy; pkill -f nhl_bench.sh; echo stopped ;;
  *)         echo "verbs: install | bench <tag> [NHL_*/REX_* env] | status | result <tag> | shot shot_<n>s.png | log [n] | xex | stop" ;;
esac
