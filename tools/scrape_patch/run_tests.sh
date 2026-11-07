#!/bin/sh
# Builds and runs the frame-script patch unit tests (plain clang++/g++, no game files).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/scrape_patch_test"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -Isrc \
  src/scrape_patch.cpp tests/scrape_patch/test_scrape_patch.cpp -o "$OUT"
"$OUT"
