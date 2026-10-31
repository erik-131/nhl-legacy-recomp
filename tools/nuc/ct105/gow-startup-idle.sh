#!/bin/bash
# Benchmark lobby without Steam: sway runs an idle process; nhl_bench.sh execs the game into it.
set -e
source /opt/gow/bash-lib/utils.sh
source /opt/gow/launch-comp.sh
export SWAY_STOP_ON_APP_EXIT=yes
launcher sleep infinity
