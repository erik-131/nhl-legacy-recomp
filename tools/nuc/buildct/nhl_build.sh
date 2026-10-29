#!/bin/bash
# Build + package + deploy. Usage: nhl_build.sh [--deploy]
set -euo pipefail
cd /opt/nhl/nhl-legacy-recomp
G="ssh -i /root/.ssh/nhl_gate -o BatchMode=yes root@${GATE_HOST:-192.168.0.22}"
mkdir -p game
[ -f game/default.xex ] || $G xex > game/default.xex
echo "d818eb15775c1fee34b057b0fc494b211535d07123230ee45bd7d40991c55624  game/default.xex" | sha256sum -c -
SDK=$PWD/third_party/rexglue-sdk-main
[ -x $SDK/out/linux-amd64/Release/rexglue ] || { (cd $SDK && cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXGLUE_ENABLE_TRACY=OFF >/dev/null && ninja -C out/build/linux-amd64 -f build-Release.ninja rexglue:Release); }
[ -d generated/default ] || $SDK/out/linux-amd64/Release/rexglue codegen nhllegacy_manifest.toml
[ -f out/linux-rel/build.ninja ] || cmake -S . -B out/linux-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 \
  -DREXSDK_DIR=$SDK -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_EXE_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20 -DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/usr/bin/ld.lld-20
ninja -C out/linux-rel -j"${JOBS:-$(nproc)}" nhllegacy
P=$(mktemp -d)/nhl-linux; mkdir -p $P
strip -o $P/nhllegacy out/linux-rel/nhllegacy
cp $SDK/out/linux-amd64/librexruntime.so $SDK/out/linux-amd64/librexgpu-xenos.so $P/ && strip $P/*.so
cp -L /lib/x86_64-linux-gnu/libsnappy.so.1 $P/
tar -C $(dirname $P) -cf - nhl-linux | xz -T0 -3 > /opt/nhl/nhl-linux.tar.xz
ls -la /opt/nhl/nhl-linux.tar.xz
[ "${1:-}" = "--deploy" ] && $G install < /opt/nhl/nhl-linux.tar.xz
