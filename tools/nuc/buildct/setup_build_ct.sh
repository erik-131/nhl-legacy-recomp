#!/bin/bash
# Run INSIDE the new build CT (Ubuntu 24.04) as root. Installs the toolchain, Claude Code,
# clones the fork, prepares the patched rexglue SDK, fetches default.xex via the gate, and does a first build.
set -euo pipefail
GATE_HOST=${GATE_HOST:-192.168.0.22}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq git curl ca-certificates xz-utils python3 cmake ninja-build pkg-config \
  clang-20 lld-20 llvm-20 libvulkan-dev glslang-tools spirv-tools libsnappy-dev \
  libx11-dev libx11-xcb-dev libxcb1-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxss-dev \
  libxkbcommon-dev libwayland-dev libasound2-dev libpulse-dev libgtk-3-dev libudev-dev libdbus-1-dev \
  libgl-dev libegl-dev libdrm-dev libgbm-dev libibus-1.0-dev libpipewire-0.3-dev libjack-dev libsndio-dev libusb-1.0-0-dev nasm
# Node + Claude Code
if ! command -v claude >/dev/null; then
  curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
  apt-get install -y -qq nodejs
  npm install -g @anthropic-ai/claude-code
fi
# Source
mkdir -p /opt/nhl && cd /opt/nhl
[ -d nhl-legacy-recomp ] || git clone --branch nuc-perf https://github.com/erik-131/nhl-legacy-recomp
cd nhl-legacy-recomp
if [ ! -d third_party/rexglue-sdk-main/.git ]; then
  git clone https://github.com/rexglue/rexglue-sdk third_party/rexglue-sdk-main
  (cd third_party/rexglue-sdk-main && git checkout -q c94f5eb && git submodule update --init --recursive --jobs 8 \
   && git apply ../../sdk/rexglue-macos-fsi-tiling-fixes.patch \
   && git apply ../../sdk/rexglue-rptr-incremental.patch \
   && git apply ../../sdk/rexglue-fence-wait-stats.patch)
fi
# ssh key for the CT 105 gate
[ -f /root/.ssh/nhl_gate ] || ssh-keygen -q -t ed25519 -N "" -C "nhl-build" -f /root/.ssh/nhl_gate
echo "Public key for CT 105 authorized_keys:"; cat /root/.ssh/nhl_gate.pub
echo "Next: install the gate on CT 105 (see tools/nuc/buildct/README.md), then run tools/nuc/buildct/nhl_build.sh"
