#!/bin/bash
# ab.sh [-n RUNS] [-p PRIO] [-o OWNER] [-t SEC] [-c results.csv] A="ENV_A" B="ENV_B" [C="ENV_C"]
#   One command A/B: queues RUNS (default 3) interleaved runs per variant (A,B,A,B,...) through bq.sh, each on its own snapshot of the
#   dev install, waits, then prints per-run + pooled min/p1/p5/median/fraction>=100 and the bootstrap CI of B-A (abstat.py).
#   ENV_x is appended to bench/playenv.txt (so only the difference needs to be written).  Variants that differ in BINARY, not env:
#   give SRC_A=<install dir> / SRC_B=<install dir> (dirs under WolfSteam/, default nhl-linux-dev).
#   Results (per-run rows) go to nhl-work/results/ab-runs.csv; logs to nhl-work/ab/<stamp>/.
# Example: ab.sh -n 3 A="REX_NHL_UPLOAD_PREPASS=false" B="REX_NHL_UPLOAD_PREPASS=true"
H=$(dirname "$(readlink -f "$0")"); W=${NHL_WORK:-$HOME/nhl-work}; N=3; PRIO=5; OWN=${BQ_OWNER:-anon}; TO=420; CSV=$W/results/ab-runs.csv
while getopts n:p:o:t:c: o; do case $o in n) N=$OPTARG;; p) PRIO=$OPTARG;; o) OWN=$OPTARG;; t) TO=$OPTARG;; c) CSV=$OPTARG;; *) exit 2;; esac; done; shift $((OPTIND-1))
STAMP=$(date +%m%d-%H%M%S); OUT=$W/ab/$STAMP; mkdir -p "$OUT"; BASE=$(cat "$H/playenv.txt")
declare -A ENVV JOBS; NAMES=()
for a in "$@"; do k=${a%%=*}; ENVV[$k]=${a#*=}; NAMES+=("$k"); done
[ ${#NAMES[@]} -ge 2 ] || { sed -n 2,12p "$0"; exit 2; }
for i in $(seq 1 $N); do for k in "${NAMES[@]}"; do
  src=SRC_$k; S=${!src:-nhl-linux-dev}; tag=ab$STAMP-$k$i
  id=$("$H/bq.sh" submit -p $PRIO -o "$OWN" -t $TO -e $((TO>300?300:TO)) -s "$S" -n "$tag" -- "$H/bq-bench.sh" "$tag" "$BASE ${ENVV[$k]}") || exit 1
  JOBS[$k]+="$id "; echo "queued $tag as job $id" >&2
done; done
"$H/bq.sh" status >&2
for k in "${NAMES[@]}"; do for id in ${JOBS[$k]}; do "$H/bq.sh" wait $id >/dev/null 2>&1; done; done
args=(); for k in "${NAMES[@]}"; do l=""; for id in ${JOBS[$k]}; do f=$(ls "$W"/bq/jobs/$id/*.log 2>/dev/null | grep -v -E 'out.log|runner.log|daemon' | head -1); [ -n "$f" ] && { cp "$f" "$OUT/"; l+="$OUT/$(basename "$f"),"; }; done; args+=("$k=${l%,}"); done
python3 "$H/abstat.py" --csv "$CSV" "${args[@]}" | tee "$OUT/report.txt"
echo "logs and report: $OUT" >&2
