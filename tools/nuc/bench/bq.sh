#!/bin/bash
# bq.sh - fair bench queue for the NUC (replaces polling on nhl-work/bench.lock).
#
#   bq.sh run    [opts] -- CMD...   submit, wait, stream the output, exit with the job's exit code
#   bq.sh submit [opts] -- CMD...   submit and print the job id
#   bq.sh status [-a]               queue, running jobs, lock holder, ETA (-a: also recent finished jobs)
#   bq.sh wait ID | log ID [-f] | cancel ID | results [N]
#   bq.sh deploy [-d DESTDIR] [-w SEC] [-f]   guarded, atomic install of out/linux-rel into nhl-linux-dev (see below)
#   bq.sh guard [-w SEC]            exit 0 if a deploy into the dev install is safe now (used by build.sh --deploy)
#   bq.sh daemon                    scheduler loop (started automatically by submit)
#
# Options for submit/run:
#   -p N     priority 0..9 (0 = most urgent, default 5); FIFO within a priority
#   -o NAME  owner shown in status (default $BQ_OWNER or "anon"); NAME "install" is lifted above everything while
#            nhl-work/install-done.flag is missing (see $BQ/priority.conf)
#   -t SEC   timeout of the command (default 900); the job is killed (and its game) when it expires
#   -e SEC   expected run time for the ETA in status (default min(timeout, 300))
#   -l LANE  bench (default): exclusive, takes bench.lock, nothing else runs; needs the GPU undisturbed (timing)
#            func : functional test (does it crash / does X appear), up to $BQ_MAXFUNC (2) at once, never together with a bench job
#            free : no game / no GPU (python, unit tests); starts immediately
#   -s SRC   install dir under WolfSteam/ to snapshot at submit time (default nhl-linux-dev; "none" = no snapshot)
#            The job runs on its own private copy (NHL_DIR=nhl-bq-<id>), so later deploys never touch it.
#   -n LABEL free text label (shown in status/results)
#   -k       keep the snapshot dir after the job (default: removed)
#   -N       job needs no Wolf/game container (skip the session check)
# Environment given to the job: NHL_DIR (snapshot name), BQ_JOB, BQ_OWNER, BQ_OUT (job dir with out.log), NHL_WORK.
# erik-hold: while nhl-work/erik-hold.flag exists no bench/func job is started (erik-hold.sh also takes bench.lock itself).
set -u
W=${NHL_WORK:-$HOME/nhl-work}
BQ=${BQ_DIR:-$W/bq}
WOLF=${WOLF_PROFILE:-/etc/wolf/profile-data/user/WolfSteam}
LOCK=$W/bench.lock
HOLD=$W/erik-hold.flag
MAXFUNC=${BQ_MAXFUNC:-2}
SELF=$(readlink -f "$0")
HERE=$(dirname "$SELF")
mkdir -p "$BQ/jobs"
[ -f "$BQ/priority.conf" ] || printf '# owner  flagfile : jobs of OWNER go first while FLAGFILE does not exist\ninstall %s/install-done.flag\n' "$W" > "$BQ/priority.conf"

now() { date +%s; }
hms() { date -d "@$1" +%H:%M:%S; }
get() { sed -n "s/^$2=//p" "$BQ/jobs/$1/meta" 2>/dev/null | head -1; }          # get ID KEY
setv() { local f=$BQ/jobs/$1/meta; grep -v "^$2=" "$f" > "$f.t" 2>/dev/null; echo "$2=$3" >> "$f.t"; mv "$f.t" "$f"; }
state() { cat "$BQ/jobs/$1/state" 2>/dev/null; }
setstate() { echo "$2" > "$BQ/jobs/$1/state.t" && mv "$BQ/jobs/$1/state.t" "$BQ/jobs/$1/state"; }
alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
fmt_d() { local s=$1; [ "$s" -lt 0 ] && s=0; if [ "$s" -ge 3600 ]; then printf '%dh%02dm' $((s/3600)) $((s%3600/60)); elif [ "$s" -ge 60 ]; then printf '%dm%02ds' $((s/60)) $((s%60)); else printf '%ds' "$s"; fi; }

# ---------------------------------------------------------------- snapshot
snap_create() { # snap_create ID SRC  -> copies the install into WOLF/nhl-bq-ID (big files hardlinked, hash recorded)
  local id=$1 src=$WOLF/$2 dst=$WOLF/nhl-bq-$1 f b
  [ -d "$src" ] || { echo "bq: no such install dir $src" >&2; return 1; }
  mkdir -p "$dst"
  for f in "$src"/* "$src"/.[!.]*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    b=$(basename "$f")
    case "$b" in logs|shot_*|frame_*|core.*|pxdump|vl|measure*|tun_*|vp6_bridge.txt|stalldump|*.png) continue;; esac
    if [ -L "$f" ]; then cp -P "$f" "$dst/"
    elif [ -d "$f" ]; then cp -a "$f" "$dst/"            # copy, not link: caches grow during play
    elif [ "$(stat -c %s "$f")" -gt 10000000 ]; then ln -f "$f" "$dst/$b" 2>/dev/null || cp -a "$f" "$dst/"   # binaries: hardlink, hash checked at start
    else cp -a "$f" "$dst/"; fi
  done
  mkdir -p "$dst/logs"
  ( cd "$dst" && sha256sum nhllegacy librexruntime.so librexgpu-xenos.so 2>/dev/null ) > "$BQ/jobs/$id/snap.sha"
}
snap_verify() { ( cd "$WOLF/nhl-bq-$1" 2>/dev/null && sha256sum -c --quiet "$BQ/jobs/$1/snap.sha" ) ; }

# ---------------------------------------------------------------- submit
cmd_submit() {
  local prio=5 owner=${BQ_OWNER:-anon} to=900 est= lane=bench src=nhl-linux-dev label= keep=0 nogame=0
  while [ $# -gt 0 ]; do case "$1" in
    -p) prio=$2; shift 2;; -o) owner=$2; shift 2;; -t) to=$2; shift 2;; -e) est=$2; shift 2;; -l) lane=$2; shift 2;;
    -s) src=$2; shift 2;; -n) label=$2; shift 2;; -k) keep=1; shift;; -N) nogame=1; shift;; --) shift; break;;
    *) echo "bq submit: unknown option $1" >&2; return 2;; esac; done
  [ $# -gt 0 ] || { echo "bq submit: no command after --" >&2; return 2; }
  # Warn (not block) on labels naming experiments already measured as "not worth repeating" (docs/next-session-plan.md).
  case "$(printf '%s' "$label" | tr 'A-Z' 'a-z')" in
    *robust*|*nohiz*|*norbc*|*branchless*|*contract*|*deferread*|*defer_read*|*defer-read*|*binddedupe*|*bind_dedupe*|*bind-dedupe*|*full32*|*mesa26*|*mesa-26*|*mesa_26*)
      echo "bq submit: WARNING label '$label' names an experiment listed under 'Not worth repeating' in docs/next-session-plan.md (robustBufferAccess, INTEL_DEBUG nohiz/norbc, branchless endian, VS contract, defer read, bind dedupe, full32 fuse, Mesa 26.2.6). Submitting anyway." >&2;;
  esac
  case "$lane" in bench|func|free) ;; *) echo "bq: lane must be bench, func or free" >&2; return 2;; esac
  [ "$lane" = free ] && { nogame=1; [ "$src" = nhl-linux-dev ] && src=none; }
  [ -n "$est" ] || est=$(( to < 300 ? to : 300 ))
  local id; id=$( ( flock 8; n=$(cat "$BQ/seq" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$BQ/seq"; printf '%05d' $n ) 8>"$BQ/seq.lock" )
  local d=$BQ/jobs/$id; mkdir -p "$d"
  { echo "id=$id"; echo "owner=$owner"; echo "prio=$prio"; echo "lane=$lane"; echo "timeout=$to"; echo "est=$est"
    echo "submitted=$(now)"; echo "label=$label"; echo "cwd=$PWD"; echo "keep=$keep"; echo "nogame=$nogame"
    echo "snap=$([ "$src" = none ] && echo '' || echo nhl-bq-$id)"; echo "seq=$((10#$id))"; } > "$d/meta"
  { echo '#!/bin/bash'; printf '%q ' "$@"; echo; } > "$d/cmd.sh"
  if [ "$src" != none ]; then snap_create "$id" "$src" || { rm -rf "$d"; return 1; }; fi
  setstate "$id" queued
  ensure_daemon
  echo "$id"
}

ensure_daemon() {
  if ! ( flock -n 9 ) 9>"$BQ/daemon.lock" 2>/dev/null; then return 0; fi   # lock free -> no daemon
  ( setsid nohup "$SELF" daemon >> "$BQ/daemon.log" 2>&1 < /dev/null & )
  sleep 0.3
}

# ---------------------------------------------------------------- scheduler
effprio() { # effective priority: -1 for owners whose gate flag is still missing
  local id=$1 o p; o=$(get "$id" owner); p=$(get "$id" prio)
  while read -r ow fl; do case "$ow" in ''|\#*) continue;; esac
    [ "$o" = "$ow" ] && [ ! -e "$fl" ] && { echo -1; return; }; done < "$BQ/priority.conf"
  echo "$p"
}
running_ids() { local j; for j in "$BQ"/jobs/*/; do j=$(basename "$j"); [ "$(state "$j")" = running ] && echo "$j"; done; }
queued_sorted() { local j; for j in "$BQ"/jobs/*/; do j=$(basename "$j"); [ "$(state "$j")" = queued ] && echo "$(effprio "$j") $(get "$j" seq) $j"; done | sort -n -k1,1 -k2,2 | awk '{print $3}'; }

reap() { # running jobs whose runner died
  local j pid
  for j in $(running_ids); do pid=$(get "$j" pid)
    if ! alive "$pid"; then finish_job "$j" 137 crashed; fi; done
  # stale bq lock whose job is no longer running
  if [ -f "$LOCK" ] && [[ "$(cat "$LOCK" 2>/dev/null)" == "bq "* ]]; then
    local lj; lj=$(cut -d' ' -f2 "$LOCK"); [ "$(state "$lj")" = running ] || rm -f "$LOCK"; fi
}

finish_job() { # finish_job ID RC STATE
  local id=$1 rc=$2 st=$3 end; end=$(now)
  [ "$(state "$id")" = running ] || [ "$st" = cancelled ] || true
  setv "$id" end "$end"; setv "$id" rc "$rc"; setstate "$id" "$st"
  if [ -f "$LOCK" ] && [ "$(cat "$LOCK" 2>/dev/null)" = "bq $id $(get "$id" owner)" ]; then rm -f "$LOCK"; fi
  local snap; snap=$(get "$id" snap)
  [ -n "$snap" ] && [ "$(get "$id" keep)" != 1 ] && rm -rf "${WOLF:?}/$snap"
  local csv=$BQ/results.csv; [ -f "$csv" ] || echo "id,owner,lane,label,state,rc,submitted,start,end,wait_s,run_s,sha_nhllegacy" > "$csv"
  local sub st0; sub=$(get "$id" submitted); st0=$(get "$id" start); st0=${st0:-$end}
  echo "$id,$(get "$id" owner),$(get "$id" lane),\"$(get "$id" label)\",$st,$rc,$(hms "$sub"),$(hms "$st0"),$(hms "$end"),$((st0-sub)),$((end-st0)),$(awk '/ nhllegacy$/{print substr($1,1,12)}' "$BQ/jobs/$id/snap.sha" 2>/dev/null)" >> "$csv"
}

start_job() { # start_job ID  (lock already taken for bench lane)
  local id=$1
  setv "$id" start "$(now)"
  setstate "$id" running
  ( setsid nohup "$SELF" _run "$id" >> "$BQ/jobs/$id/runner.log" 2>&1 < /dev/null & )
}

cmd_daemon() {
  exec 9>"$BQ/daemon.lock"; flock -n 9 || { echo "daemon already running"; exit 0; }
  echo "$(date +%T) daemon up pid $$" >> "$BQ/daemon.log"
  local idle=0
  while :; do
    reap
    local nrun=0 nfunc=0 j l
    for j in $(running_ids); do l=$(get "$j" lane); [ "$l" = func ] && nfunc=$((nfunc+1)); [ "$l" = bench ] && nrun=$((nrun+1)); done
    local lockheld=0; [ -f "$LOCK" ] && lockheld=1
    local blocked=0 any=0
    for j in $(queued_sorted); do any=1; l=$(get "$j" lane)
      case "$l" in
        free) start_job "$j";;
        func) if [ $lockheld = 0 ] && [ $blocked = 0 ] && [ ! -e "$HOLD" ] && [ $nfunc -lt "$MAXFUNC" ]; then start_job "$j"; nfunc=$((nfunc+1)); fi;;
        bench) if [ $blocked = 1 ]; then continue; fi
               if [ -e "$HOLD" ] || [ $lockheld = 1 ] || [ $nfunc -gt 0 ]; then blocked=1; continue; fi   # reserve: no lower job overtakes (func drains first)
               if ( set -o noclobber; echo "bq $j $(get "$j" owner)" > "$LOCK" ) 2>/dev/null; then start_job "$j"; lockheld=1; blocked=1
               else blocked=1; fi;;
      esac
    done
    if [ $any = 0 ] && [ -z "$(running_ids)" ]; then idle=$((idle+1)); else idle=0; fi
    [ $idle -gt 1800 ] && { echo "$(date +%T) idle, daemon exits" >> "$BQ/daemon.log"; exit 0; }   # 30 min at 1 s ticks
    sleep 1
  done
}

# ---------------------------------------------------------------- runner (one per job)
ensure_session() { # one Wolf container, started once even when several jobs begin together
  ( flock 7
    docker ps --format '{{.Names}}' | grep -q '^WolfSteam_' && exit 0
    WOLF_NOSTEAM=1 "$HERE/../ct105/wolf_session.sh" start
    for _ in $(seq 40); do docker ps --format '{{.Names}}' | grep -q '^WolfSteam_' && exit 0; sleep 3; done; exit 1
  ) 7>"$W/wolf-start.lock"
}
kill_game() { # kill_game SNAP : only games whose cwd is that snapshot (bench lane: all of them)
  local p
  for p in $(pgrep -x nhllegacy); do
    if [ -z "${1:-}" ] || [ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$WOLF/$1" ] || [ "$(get "$BQ_JOB" lane)" = bench ]; then kill "$p" 2>/dev/null; fi; done
  sleep 2; for p in $(pgrep -x nhllegacy); do [ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$WOLF/${1:-x}" ] && kill -9 "$p" 2>/dev/null; done
}
cmd_run_internal() {
  local id=$1 d=$BQ/jobs/$1; BQ_JOB=$id
  setv "$id" pid $$
  local snap; snap=$(get "$id" snap)
  if [ -n "$snap" ] && ! snap_verify "$id" > "$d/verify.log" 2>&1; then
    echo "bq: snapshot modified or missing before start: $(cat "$d/verify.log")" > "$d/out.log"; finish_job "$id" 125 failed; return; fi
  if [ "$(get "$id" nogame)" != 1 ] && ! ensure_session >> "$d/runner.log" 2>&1; then
    echo "bq: Wolf session could not be started" > "$d/out.log"; finish_job "$id" 126 failed; return; fi
  cd "$(get "$id" cwd)" 2>/dev/null || cd "$HOME"
  export BQ_JOB=$id BQ_OWNER=$(get "$id" owner) BQ_OUT=$d NHL_WORK=$W
  [ -n "$snap" ] && export NHL_DIR=$snap
  timeout -k 15 "$(get "$id" timeout)" bash "$d/cmd.sh" > "$d/out.log" 2>&1
  local rc=$? st=done
  [ $rc = 124 ] || [ $rc = 137 ] && st=timeout
  [ $rc != 0 ] && [ $st = done ] && st=failed
  [ "$(get "$id" nogame)" != 1 ] && { [ $st = timeout ] && kill_game "$snap"; }
  [ "$st" = timeout ] && echo "bq: killed after $(get "$id" timeout) s timeout" >> "$d/out.log"
  [ "$(state "$id")" = cancelled ] && st=cancelled
  finish_job "$id" "$rc" "$st"
}

# ---------------------------------------------------------------- user commands
cmd_status() {
  local all=0; [ "${1:-}" = -a ] && all=1
  local t; t=$(now)
  if [ -f "$LOCK" ]; then local h; h=$(cat "$LOCK" 2>/dev/null); echo "bench.lock: $h  (age $(fmt_d $((t-$(stat -c %Y "$LOCK")))))$([[ "$h" == "bq "* ]] || echo '  <- not a bq job (legacy/manual or erik-hold)')"; else echo "bench.lock: free"; fi
  [ -e "$HOLD" ] && echo "erik-hold: ACTIVE (no bench/func job starts)"
  ( flock -n 9 ) 9>"$BQ/daemon.lock" 2>/dev/null && echo "daemon: NOT running (submit starts it)"
  printf '%-6s %-9s %-3s %-6s %-12s %-8s %-8s %s\n' ID STATE PRI LANE OWNER WAITED RUN LABEL/ETA
  local j s cum=0
  for j in $(running_ids); do
    local el=$((t-$(get "$j" start))) tl; tl=$(get "$j" timeout)
    printf '%-6s %-9s %-3s %-6s %-12s %-8s %-8s %s\n' "$j" running "$(get "$j" prio)" "$(get "$j" lane)" "$(get "$j" owner)" "$(fmt_d $(( $(get "$j" start)-$(get "$j" submitted) )))" "$(fmt_d $el)" "$(get "$j" label) (timeout in $(fmt_d $((tl-el))))"
    [ "$(get "$j" lane)" = bench ] && { local rem=$(( $(get "$j" est)-el )); [ $rem -lt 0 ] && rem=0; cum=$((cum+rem)); }
  done
  for j in $(queued_sorted); do
    local eta="" ; if [ "$(get "$j" lane)" = bench ]; then eta="start ~$(hms $((t+cum)))"; cum=$((cum+$(get "$j" est))); fi
    printf '%-6s %-9s %-3s %-6s %-12s %-8s %-8s %s\n' "$j" queued "$(get "$j" prio)" "$(get "$j" lane)" "$(get "$j" owner)" "$(fmt_d $((t-$(get "$j" submitted))))" "-" "$(get "$j" label) $eta"
  done
  if [ $all = 1 ]; then
    for j in $(ls -1 "$BQ/jobs" | tail -12); do s=$(state "$j"); case "$s" in queued|running) continue;; esac
      printf '%-6s %-9s %-3s %-6s %-12s %-8s %-8s %s\n' "$j" "$s" "$(get "$j" prio)" "$(get "$j" lane)" "$(get "$j" owner)" "" "$(fmt_d $(( $(get "$j" end)-$(get "$j" start) )))" "$(get "$j" label) rc=$(get "$j" rc)"; done
  fi
}
cmd_wait() { local id=$1; while :; do case "$(state "$id")" in queued|running) sleep 2;; *) break;; esac; done
  local s; s=$(state "$id"); echo "job $id: $s rc=$(get "$id" rc)" >&2; [ "$s" = done ]; }
cmd_log() { local id=$1; if [ "${2:-}" = -f ]; then
    while [ ! -f "$BQ/jobs/$id/out.log" ] && [ "$(state "$id")" = queued ]; do sleep 2; done
    tail -n +1 -f "$BQ/jobs/$id/out.log" & local tp=$!
    while :; do case "$(state "$id")" in queued|running) sleep 2;; *) break;; esac; done; sleep 1; kill $tp 2>/dev/null
  else cat "$BQ/jobs/$id/out.log"; fi; }
cmd_cancel() { local id=$1 s; s=$(state "$id")
  case "$s" in queued) setstate "$id" cancelled; setv "$id" end "$(now)"; setv "$id" rc 1; rm -rf "${WOLF:?}/$(get "$id" snap)"; echo "cancelled $id";;
    running) setstate "$id" cancelled; pkill -TERM -P "$(get "$id" pid)" 2>/dev/null; kill "$(get "$id" pid)" 2>/dev/null; echo "cancelling $id";;
    *) echo "job $id is $s";; esac; }
cmd_results() { tail -n "${1:-20}" "$BQ/results.csv"; }

cmd_run() { local id; id=$(cmd_submit "$@") || exit $?; echo "bq: job $id submitted ($(cmd_status | grep -c ' queued ') queued)" >&2
  cmd_log "$id" -f; cmd_wait "$id"; }

# ---------------------------------------------------------------- deploy guard
# A deploy into the dev install is unsafe only while a NON-bq holder owns bench.lock (it runs straight from the dev dir).
# bq jobs run from snapshots, so they never conflict.  BQ_OWNER matching the lock text = caller holds it itself.
cmd_guard() { local wait=0; [ "${1:-}" = -w ] && wait=$2
  local t0; t0=$(now)
  while :; do
    if [ ! -f "$LOCK" ]; then return 0; fi
    local h; h=$(cat "$LOCK" 2>/dev/null)
    [[ "$h" == "bq "* ]] && return 0
    [ -n "${BQ_OWNER:-}" ] && [[ "$h" == "$BQ_OWNER"* ]] && return 0
    [ -n "${BENCH_TAG:-}" ] && [[ "$h" == "$BENCH_TAG"* ]] && return 0
    [ $(( $(now)-t0 )) -ge "$wait" ] && { echo "bq: deploy refused, bench.lock held by '$h' (age $(fmt_d $(( $(now)-$(stat -c %Y "$LOCK") )))) - that run uses the dev install directly. Use 'bq.sh run' (own snapshot), wait (-w SEC / BQ_DEPLOY_WAIT), or BQ_FORCE=1." >&2; return 3; }
    sleep 3
  done; }
cmd_deploy() { local dest=nhl-linux-dev force=${BQ_FORCE:-0} wait=${BQ_DEPLOY_WAIT:-0} src=
  while [ $# -gt 0 ]; do case "$1" in -d) dest=$2; shift 2;; -w) wait=$2; shift 2;; -f) force=1; shift;; -b) src=$2; shift 2;; *) shift;; esac; done
  src=${src:-$PWD/out/linux-rel}; local sdk=${SDK:-$(readlink -f third_party)/rexglue-sdk-main}
  if [ "$dest" = nhl-linux-dev ] && [ "$force" != 1 ]; then cmd_guard -w "$wait" || return $?; fi
  local D=$WOLF/$dest; exec 8>"$W/deploy.lock"; flock 8     # one deploy at a time
  local f tmp
  for f in "$src/nhllegacy" "$sdk/out/linux-amd64/librexruntime.so" "$sdk/out/linux-amd64/librexgpu-xenos.so"; do
    tmp="$D/.$(basename "$f").new"; install -m 755 "$f" "$tmp" && { strip "$tmp" 2>/dev/null || true; } && mv -f "$tmp" "$D/$(basename "$f")"   # rename = atomic, a snapshot hardlink keeps the old inode
  done
  sha256sum "$D/nhllegacy" "$D"/*.so | awk '{print substr($1,1,12), $2}'; }

case "${1:-status}" in
  submit) shift; cmd_submit "$@";;
  run) shift; cmd_run "$@";;
  status|ls) shift; cmd_status "$@";;
  wait) cmd_wait "$2";;
  log) shift; cmd_log "$@";;
  cancel) cmd_cancel "$2";;
  results) shift; cmd_results "$@";;
  daemon) cmd_daemon;;
  _run) cmd_run_internal "$2";;
  guard) shift; cmd_guard "$@";;
  deploy) shift; cmd_deploy "$@";;
  -h|--help|help) sed -n '2,30p' "$0";;
  *) echo "bq.sh: unknown command $1 (try help)" >&2; exit 2;;
esac
