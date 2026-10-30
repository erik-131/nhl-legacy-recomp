#!/bin/sh
# Launcher for playing the Linux build in the Wolf Steam session (Steam shortcut target).
# rtv is the default render path; guest presentation interval 0 (game clock is time-based,
# verified 2026-10-30); per-destination readback limit (as fast as readback=some, but keeps
# copying). Extra env/args pass through, e.g. NHL_VK_RT_PATH=fsi for the old path.
cd "$(dirname "$0")" || exit 1
export NHL_SWAP_INTERVAL="${NHL_SWAP_INTERVAL:-0}"
export NHL_VK_RB_MAX_PER_DEST="${NHL_VK_RB_MAX_PER_DEST:-2}"
exec ./nhllegacy "$@"
