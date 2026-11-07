#!/bin/bash
# Builds event_wakeups against the SDK headers and links librexruntime.so from the SDK build. Output: $1 (default ./event_wakeups).
R=$(cd "$(dirname "$0")/../../.." && pwd); S=$R/third_party/rexglue-sdk-main
clang++-20 -std=c++23 -O1 -I"$S/include" $(find "$S/thirdparty" -maxdepth 2 -type d -name include | sed 's/^/-I/') -I"$S/thirdparty/fmt/include" -I"$S/thirdparty/spdlog/include" \
  "$R/tools/nuc/tests/event_wakeups.cpp" -o "${1:-event_wakeups}" -L"$S/out/linux-amd64" -lrexruntime -lpthread -fuse-ld=lld-20
