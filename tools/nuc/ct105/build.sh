#!/bin/bash
# Build (and optionally deploy into the Wolf profile) inside CT 105. Usage: build.sh [--deploy]
set -euo pipefail
cd /opt/nhl/nhl-legacy-recomp
SDK=$PWD/third_party/rexglue-sdk-main
DEST=/etc/wolf/profile-data/user/WolfSteam/nhl-linux
[ -x $SDK/out/linux-amd64/Release/rexglue ] || { (cd $SDK && cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXGLUE_ENABLE_TRACY=OFF >/dev/null && ninja -C out/build/linux-amd64 -f build-Release.ninja rexglue:Release); }
[ -d generated/default ] || $SDK/out/linux-amd64/Release/rexglue codegen nhllegacy_manifest.toml
[ -f out/linux-rel/build.ninja ] || cmake -S . -B out/linux-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 \
  -DREXSDK_DIR=$SDK -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20
# Low-memory safe: the game + Wolf need ~4 GB, so cap parallel jobs.
ninja -C out/linux-rel -j"${JOBS:-4}" nhllegacy
if [ "${1:-}" = "--deploy" ]; then
  C=$(docker ps --format '{{.Names}}' | grep -i steam | head -1)
  [ -n "$C" ] && docker exec "$C" pkill -x nhllegacy || true
  install -m 755 -o 1000 -g 1000 out/linux-rel/nhllegacy $DEST/nhllegacy
  install -m 755 -o 1000 -g 1000 $SDK/out/linux-amd64/librexruntime.so $SDK/out/linux-amd64/librexgpu-xenos.so $DEST/
  strip $DEST/nhllegacy $DEST/*.so
  sha256sum $DEST/nhllegacy $DEST/*.so
fi
