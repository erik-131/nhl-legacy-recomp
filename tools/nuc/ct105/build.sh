#!/bin/bash
# Build (and optionally deploy into the Wolf profile) inside the container. Usage: build.sh [--deploy]
set -euo pipefail
# Root = the checkout this script lives in (a worktree builds itself, not the main tree); NHL_ROOT overrides.
cd "${NHL_ROOT:-$(dirname "$(readlink -f "$0")")/../../..}"
SDK=$(readlink -f third_party)/rexglue-sdk-main   # real path: identical ccache keys in every worktree
# DEST_DIR=nhl-linux-dev deploys experiments without touching the playable nhl-linux install.
DEST=/etc/wolf/profile-data/user/WolfSteam/${DEST_DIR:-nhl-linux}
[ -x $SDK/out/linux-amd64/Release/rexglue ] || { (cd $SDK && cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXGLUE_ENABLE_TRACY=OFF >/dev/null && ninja -C out/build/linux-amd64 -f build-Release.ninja rexglue:Release); }
[ -d generated/default ] || $SDK/out/linux-amd64/Release/rexglue codegen nhllegacy_manifest.toml
[ -f out/linux-rel/build.ninja ] || cmake -S . -B out/linux-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 \
  -DREXSDK_DIR=$SDK -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 \
  $(command -v ccache >/dev/null && echo -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache) -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DNHL_SKIP_CODEGEN=$([ -f generated/.skip-codegen ] && echo ON || echo OFF)
# CCACHE=1 on an existing build dir turns the launcher on (one full rebuild, then ~1 min for the same sources anywhere).
[ "${CCACHE:-}" = 1 ] && cmake -S . -B out/linux-rel -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON >/dev/null
# Parallelism: the game + Wolf need ~4 GB; a clang job needs <= ~1 GB, so JOBS follows MemAvailable (override with JOBS=n).
AVAIL=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); AUTO=$(( AVAIL / 1000 )); [ $AUTO -gt 10 ] && AUTO=10; [ $AUTO -lt 2 ] && AUTO=2
export CCACHE_CONFIGPATH=${CCACHE_CONFIGPATH:-$HOME/nhl-work/ccache.conf}
ninja -C out/linux-rel -j"${JOBS:-$AUTO}" nhllegacy
if [ "${1:-}" = "--deploy" ]; then
  C=$(docker ps --format '{{.Names}}' | { grep -i steam || true; } | head -1)
  # Only stop a running game when replacing the playable install (experiments go elsewhere).
  [ "${DEST_DIR:-nhl-linux}" = nhl-linux ] && [ -n "$C" ] && docker exec "$C" pkill -x nhllegacy || true
  if [ "${DEST_DIR:-nhl-linux}" != nhl-linux ]; then
    # experiments: guarded (refuses while a non-bq run holds bench.lock on the dev dir, BQ_FORCE=1 overrides) and atomic (rename), so a
    # bq job's private snapshot or a running game never sees a half-written binary.
    DEST_DIR=$DEST_DIR "$PWD/tools/nuc/bench/bq.sh" deploy -d "$DEST_DIR" -b "$PWD/out/linux-rel"
  else
    install -m 755 -o 1000 -g 1000 out/linux-rel/nhllegacy $DEST/nhllegacy
    install -m 755 -o 1000 -g 1000 $SDK/out/linux-amd64/librexruntime.so $SDK/out/linux-amd64/librexgpu-xenos.so $DEST/
    strip $DEST/nhllegacy $DEST/*.so
    sha256sum $DEST/nhllegacy $DEST/*.so
  fi
fi
