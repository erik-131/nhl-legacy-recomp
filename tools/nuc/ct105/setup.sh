#!/bin/bash
# One-time setup for running Claude Code + builds directly inside CT 115 (Games on Whales; formerly CT 105).
# Usage (as root in CT 115): bash setup.sh   -> repo in /opt/nhl/nhl-legacy-recomp
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# Debian 13 (trixie) ships no clang-20: take it from apt.llvm.org.
if ! apt-cache show clang-20 >/dev/null 2>&1; then
  curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc
  echo "deb http://apt.llvm.org/$(. /etc/os-release; echo $VERSION_CODENAME)/ llvm-toolchain-$(. /etc/os-release; echo $VERSION_CODENAME)-20 main" > /etc/apt/sources.list.d/llvm-20.list
  apt-get update -qq
fi
apt-get install -y -qq git curl ca-certificates xz-utils python3 cmake ninja-build pkg-config \
  clang-20 lld-20 llvm-20 libvulkan-dev glslang-tools spirv-tools libsnappy-dev \
  libx11-dev libx11-xcb-dev libxcb1-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxss-dev \
  libxkbcommon-dev libwayland-dev libasound2-dev libpulse-dev libgtk-3-dev libudev-dev libdbus-1-dev \
  libgl-dev libegl-dev libdrm-dev libgbm-dev libibus-1.0-dev libpipewire-0.3-dev libjack-dev libsndio-dev libusb-1.0-0-dev nasm
if ! command -v claude >/dev/null; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y -qq nodejs
  npm install -g @anthropic-ai/claude-code
fi
mkdir -p /opt/nhl && cd /opt/nhl
[ -d nhl-legacy-recomp ] || git clone -b nuc-perf https://github.com/erik-131/nhl-legacy-recomp.git
cd nhl-legacy-recomp
SDK=third_party/rexglue-sdk-main
if [ ! -d $SDK/.git ]; then
  git clone https://github.com/rexglue/rexglue-sdk.git $SDK
  git -C $SDK checkout -q c94f5eb
  git -C $SDK submodule update --init --recursive --jobs 8
  git -C $SDK checkout -q -b nhl-nuc
  git -C $SDK apply --index "$PWD/sdk/rexglue-nhl-nuc-combined.patch"
  git -C $SDK -c user.name=nhl -c user.email=nhl@localhost commit -qm "NHL NUC: Redth FSI/tiling fixes + all NUC diagnostics (sdk/rexglue-nhl-nuc-combined.patch)"
  grep -qx "third_party/rexglue-sdk-main" .git/info/exclude || echo "third_party/rexglue-sdk-main" >> .git/info/exclude
fi
# Vanilla xex for codegen comes from the game install on this CT (never commit it).
mkdir -p game
[ -f game/default.xex ] || cp /etc/wolf/profile-data/user/WolfSteam/nhl/game/default.xex game/
echo "d818eb15775c1fee34b057b0fc494b211535d07123230ee45bd7d40991c55624  game/default.xex" | sha256sum -c -
# Static ffmpeg for the VP6 movie bridge (the GoW Steam image has no ffmpeg -> green movies).
NL=/etc/wolf/profile-data/user/WolfSteam/nhl-linux
if [ -d $NL ] && [ ! -x $NL/ffmpeg ]; then
  T=$(mktemp -d); F=ffmpeg-n8.1-latest-linux64-gpl-8.1.tar.xz; U=https://github.com/BtbN/FFmpeg-Builds/releases/download/latest
  (cd $T && curl -fsSLO $U/$F && curl -fsSLO $U/checksums.sha256 && grep " $F\$" checksums.sha256 | sha256sum -c - && tar xf $F)
  install -m 755 -o 1000 -g 1000 $T/*/bin/ffmpeg $NL/ffmpeg; rm -rf $T
fi
install -m 755 -o 1000 -g 1000 tools/nuc/ct105/nhl_play.sh $NL/nhl_play.sh 2>/dev/null || true
mkdir -p /root/nhlbench && cp tools/nuc/nhl_bench.sh /root/nhlbench/ && chmod +x /root/nhlbench/nhl_bench.sh
cp tools/nuc/ct105/CLAUDE.md CLAUDE.local.md
grep -qx "CLAUDE.local.md" .git/info/exclude || echo "CLAUDE.local.md" >> .git/info/exclude
echo "OK. Build: tools/nuc/ct105/build.sh [--deploy]; start Claude Code here: cd /opt/nhl/nhl-legacy-recomp && claude"
