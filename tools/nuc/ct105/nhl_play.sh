#!/bin/sh
# Launcher for playing the Linux build in the Wolf Steam session (Steam shortcut target).
# rtv is the default render path; guest presentation interval 0 (game clock is time-based,
# verified 2026-10-30); readback "fast" (double-buffered, one frame stale, no GPU drain -
# correct equipment textures, unlike the per-destination limit). Extra env/args pass through,
# e.g. NHL_VK_RT_PATH=fsi for the old path.
cd "$(dirname "$0")" || exit 1
export NHL_SWAP_INTERVAL="${NHL_SWAP_INTERVAL:-0}"
export NHL_VK_READBACK_MODE="${NHL_VK_READBACK_MODE:-fast}"
export NHL_NO_GUIDE="${NHL_NO_GUIDE:-1}"  # Guide button opens the Steam menu, not the in-game overlay
# Present every guest frame from the guest output thread (no ImGui overlay/toasts registered);
# otherwise UI-thread painting shows only ~half of the frames.
export NHL_DIRECT_PRESENT="${NHL_DIRECT_PRESENT:-1}"
# REX_PRESENT_THREAD=1 (dedicated present thread) is off: no measurable fps gain over run-to-run
# variance, and play-testing felt laggier. Steam launch options, "<VAR>=<value> %command%":
#   NHL_SWAP_INTERVAL=2           steady 30 fps lock
#   REX_MAX_FRAMES_IN_FLIGHT=1    CPU never ahead of the GPU: ~1 frame (~19 ms) less latency, ~25 % lower fps
# Movies: the VP6 bridge decodes with ffmpeg, which the GoW Steam image lacks (green movies).
# A static ffmpeg is installed next to the binary (not in git).
[ -x ./ffmpeg ] && export NHL_VP6_FFMPEG="${NHL_VP6_FFMPEG:-$PWD/ffmpeg}"
# FPS + frame-time graph top right (MangoHud ships in the GoW Steam image). NHL_HUD=0 turns it off;
# Shift_R+F12 toggles it in game.
if [ "${NHL_HUD:-1}" != 0 ] && command -v mangohud >/dev/null; then
  export MANGOHUD=1
  export MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-position=top-right,fps,frametime,frame_timing,cpu_stats=0,gpu_stats=0,font_size=22,background_alpha=0.3}"
fi
exec ./nhllegacy "$@"
