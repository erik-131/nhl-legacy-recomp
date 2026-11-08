#!/bin/bash
# diskclean.sh [--apply] [--days N] [--logs N] [--worktrees]   Free disk used by bench run products.  Default is a DRY RUN.
#   what it touches (only these):
#     <WolfSteam>/nhl-linux*/logs/*.log           keep the newest --logs (default 40) per install, drop the rest (+ vp6_bridge*.txt copies > N days)
#     <WolfSteam>/nhl-linux*/{shot_*,frame_*}.png, core.*   older than 1 day
#     <WolfSteam>/nhl-bq-*                         bq snapshot dirs with no queued/running job
#     nhl-work/**/*.{png,jpg} (shots*), nhl-work/shots/*   older than --days (default 3) (dirs named shots*, *-shots*)
#     nhl-work/*/**/*.log and freq-*/therm-* files > 3 MB older than --days
#     /tmp/vel*, /tmp/bqt*                          tool scratch
#     --worktrees: out/ build dirs of worktrees under nhl-work untouched for --days (rebuilds via ccache in ~1 min)
#   never touched: nhl-backup, WolfSteam/nhl (game data), WolfSteam/.steam, any file not matching the above, anything newer than the cut-off.
W=${NHL_WORK:-$HOME/nhl-work}; WOLF=${WOLF_PROFILE:-/etc/wolf/profile-data/user/WolfSteam}
APPLY=0; DAYS=3; KEEP=40; WT=0
while [ $# -gt 0 ]; do case "$1" in --apply) APPLY=1;; --days) DAYS=$2; shift;; --logs) KEEP=$2; shift;; --worktrees) WT=1;; *) echo "unknown $1"; exit 2;; esac; shift; done
TOT=0
del() { # del PATH...  : size, then remove when --apply.  Refuse protected paths.
  local p r
  for p in "$@"; do r=$(readlink -f "$p" 2>/dev/null) || continue
    case "$r" in */nhl-backup*|"$WOLF"/nhl|"$WOLF"/nhl/*|"$WOLF"/.steam*|/|$HOME|$HOME/nhl-legacy-recomp) echo "REFUSE protected: $p" >&2; continue;; esac
    local s; s=$(du -sk "$p" 2>/dev/null | cut -f1); TOT=$((TOT+${s:-0}))
    [ $APPLY = 1 ] && rm -rf -- "$p"
  done; }
section() { echo "-- $1"; }
before=$(df -m / | awk 'NR==2{print $4}')
section "install logs beyond newest $KEEP / old screenshots / cores"
for d in "$WOLF"/nhl-linux*; do [ -d "$d/logs" ] || continue
  n=$(ls -1t "$d/logs"/*.log 2>/dev/null | tail -n +$((KEEP+1)) | tee /tmp/.dc.$$ | wc -l); [ "$n" -gt 0 ] && { echo "$d/logs: $n old logs"; while read -r f; do del "$f"; done < /tmp/.dc.$$; }
  find "$d" -maxdepth 1 \( -name 'shot_*.png' -o -name 'frame_*.png' -o -name 'core.*' \) -mtime +0 2>/dev/null | while read -r f; do echo "$f"; done > /tmp/.dc.$$
  [ -s /tmp/.dc.$$ ] && { echo "$d: $(wc -l < /tmp/.dc.$$) screenshots/cores older than 1 day"; while read -r f; do del "$f"; done < /tmp/.dc.$$; }
done; rm -f /tmp/.dc.$$
section "bq snapshots without a live job"
for d in "$WOLF"/nhl-bq-*; do [ -d "$d" ] || continue; id=${d##*nhl-bq-}; s=$(cat "$W/bq/jobs/$id/state" 2>/dev/null)
  case "$s" in queued|running) ;; *) echo "$d (job state: ${s:-unknown})"; del "$d";; esac; done
section "nhl-work screenshots / big logs older than $DAYS days"
find "$W" -mindepth 1 -type f \( -name '*.png' -o -name '*.jpg' \) -mtime +$DAYS -not -path '*/wt/*' -not -path '*/wt-*' -not -path '*/docs/*' 2>/dev/null > /tmp/.dc.$$
find "$W" -mindepth 2 -type f \( -name '*.log' -o -name 'freq-*' -o -name 'therm-*' -o -name '*.bt' -o -name '*.maps' \) -size +3M -mtime +$DAYS -not -path '*/wt/*' -not -path '*/wt-*' 2>/dev/null >> /tmp/.dc.$$
echo "$(wc -l < /tmp/.dc.$$) files"; while read -r f; do del "$f"; done < /tmp/.dc.$$; rm -f /tmp/.dc.$$
section "scratch"
for p in /tmp/vel* /tmp/bqt*; do [ -e "$p" ] && { echo "$p"; del "$p"; }; done
if [ $WT = 1 ]; then section "worktree build dirs idle > $DAYS days"
  for o in "$W"/*/out "$W"/*/wt/out; do [ -d "$o" ] || continue
    [ -n "$(find "$o" -maxdepth 2 -mtime -$DAYS -print -quit 2>/dev/null)" ] && continue
    pgrep -f "ninja.*${o%/out}" >/dev/null && continue
    echo "$o"; del "$o"; done; fi
if [ $APPLY = 1 ]; then echo "freed ~$((TOT/1024)) MB; free now $(df -m / | awk 'NR==2{print $4}') MB (was $before)"; else echo "DRY RUN: would free ~$((TOT/1024)) MB (free $before MB). Re-run with --apply."; fi
