#!/bin/sh
# Builds and runs the pre-game menu unit tests (plain g++, no SDK, no game files).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/pregame_menu_test"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g \
  src/pregame_menu/menu_core.cpp tests/pregame_menu/test_menu_core.cpp -o "$OUT"
"$OUT"
