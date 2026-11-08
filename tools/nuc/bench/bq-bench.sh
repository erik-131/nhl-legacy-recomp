#!/bin/bash
# bq-bench.sh TAG "ENV" : the standard bench body for a bq job: abrun.sh on the job's private snapshot (NHL_DIR), then keeps the log
# in the job dir ($BQ_OUT/TAG.log, survives the snapshot being removed) and prints the crash triage verdict.
#   bq.sh run -t 400 -- tools/nuc/bench/bq-bench.sh mytag "$(cat tools/nuc/bench/playenv.txt) REX_X=1"
# ROUTE (default quick) and DUR (default 165) as for abrun.sh.
H=$(dirname "$(readlink -f "$0")"); T=$1; E=${2:-}
D=${WOLF_PROFILE:-/etc/wolf/profile-data/user/WolfSteam}/${NHL_DIR:-nhl-linux-dev}
ROUTE=${ROUTE:-quick} DUR=${DUR:-165} "${ABRUN:-$H/abrun.sh}" "$T" "$E"
L=$(ls -t "$D"/logs/*.log 2>/dev/null | head -1)
if [ -n "$L" ]; then OUT=${BQ_OUT:-${NHL_WORK:-$HOME/nhl-work}}; cp "$L" "$OUT/$T.log"; cp "$(ls -t "$D"/shot_*.png 2>/dev/null | head -1)" "$OUT/$T.png" 2>/dev/null
  python3 "$H/triage.py" "$OUT/$T.log" --dur "${DUR:-165}" | tail -1; python3 "$H/abstat.py" "$OUT/$T.log" | tail -1; fi
