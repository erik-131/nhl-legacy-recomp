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
# X11 window via Xwayland instead of a native Wayland surface: the Steam overlay hooks X11
# (it works in other games in the same session). NHL_WAYLAND=1 keeps the Wayland window.
if [ "${NHL_WAYLAND:-0}" != 1 ]; then export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-x11}"; export DISPLAY="${DISPLAY:-:0}"; fi
# Present every guest frame from the guest output thread (no ImGui overlay/toasts registered);
# otherwise UI-thread painting shows only ~half of the frames.
export NHL_DIRECT_PRESENT="${NHL_DIRECT_PRESENT:-1}"
# Don't re-upload every guest buffer each frame (the Simpsons recomp's first memory step):
# visible fps median 46 -> 61 over 5 interleaved runs each. Set to true if stale geometry shows up.
export REX_CLEAR_MEMORY_PAGE_STATE="${REX_CLEAR_MEMORY_PAGE_STATE:-false}"
# Skip EDRAM ownership transfers into areas the next draw provably overwrites (the XDK's 4x MSAA
# clear quads): presented fps p5 36 -> 60 over 2 interleaved runs each. Set to false if a pass looks wrong.
export REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS="${REX_NHL_RT_SKIP_OVERWRITTEN_TRANSFERS:-true}"
# Record shared-memory uploads into a pre-pass instead of ending the render pass for each one
# (~97 % of uploads in gameplay): frames <=17 ms 92.7/78.9 % -> 99.7/91.5 % over 2 interleaved rounds
# with P-core pinning, 82.1 -> 90.2 % in one in-process A/B; reaction time unchanged. Set to false if
# geometry looks stale or wrong.
export REX_NHL_UPLOAD_PREPASS="${REX_NHL_UPLOAD_PREPASS:-true}"
# Fence wait: pause for 50 us, then short sleeps instead of spinning. Saves ~0.8 of a core; on two E-cores
# median 63.8 -> 89.9 and 75.7 -> 86.4 fps (A/B in one match, both orders). Set to 0 to spin as recompiled.
export REX_NHL_FENCE_POLL="${REX_NHL_FENCE_POLL:-3}"
# Picture-identical GPU wins, on by default (stacked measurement 2026-11-07, docs/nuc-perf-notes.md "Stacked"): paused replay +40 %, live
# play p5 +45 fps. NHL_FAST=0 turns the whole block off; each one can also be turned off alone, e.g. REX_NHL_PS_IMPLICIT_LOD=false.
# ps_implicit_lod + anisotropic_override=0 give a slightly sharper ice (approved); readback_skip_depth skips the depth readback (nothing reads it).
if [ "${NHL_FAST:-1}" != 0 ]; then
  export REX_NHL_TEXTURE_UNSIGNED_SPEC="${REX_NHL_TEXTURE_UNSIGNED_SPEC:-true}"
  export REX_NHL_REGISTER_VARS="${REX_NHL_REGISTER_VARS:-true}"
  export REX_NHL_DIRECT_RESOLVE_FUSED="${REX_NHL_DIRECT_RESOLVE_FUSED:-true}"
  export REX_NHL_READBACK_BATCH="${REX_NHL_READBACK_BATCH:-true}"
  export REX_NHL_READBACK_SKIP_DEPTH="${REX_NHL_READBACK_SKIP_DEPTH:-true}"
  export REX_NHL_RT_CLEAR_THROUGH="${REX_NHL_RT_CLEAR_THROUGH:-true}"
  export REX_NHL_RT_CLEAR_THROUGH_DEPTH="${REX_NHL_RT_CLEAR_THROUGH_DEPTH:-true}"
  export REX_NHL_RT_CLEAR_VERTEX_SHADER_HASH="${REX_NHL_RT_CLEAR_VERTEX_SHADER_HASH:-1E6883FCCDE1F688}"
  export REX_NHL_RT_CLEAR_PIXEL_SHADER_HASH="${REX_NHL_RT_CLEAR_PIXEL_SHADER_HASH:-A4A965C189287B99}"
  export REX_NHL_PS_IMPLICIT_LOD="${REX_NHL_PS_IMPLICIT_LOD:-true}"
  export REX_ANISOTROPIC_OVERRIDE="${REX_ANISOTROPIC_OVERRIDE:-0}"
  # Vertex shader (skinning) wins, picture identical (branch replay100, SDK 1af2842): bone-matrix fetches sample one image instead of
  # two (nhl_vs_texture_unsigned_spec) + fast vertex math: paused replay +4.5 %.
  export REX_NHL_VS_TEXTURE_UNSIGNED_SPEC="${REX_NHL_VS_TEXTURE_UNSIGNED_SPEC:-true}"
  export REX_NHL_FAST_VERTEX_MATH="${REX_NHL_FAST_VERTEX_MATH:-true}"
fi
# Press through the boot screens up to the main menu; any button takes over. NHL_QUICK_START=0 turns it off.
# The game's clock runs 3x until then, for its timed screens (menu at ~16 s instead of ~26); NHL_BOOT_CLOCK=1 turns that off.
# A 12-frame logo movie instead of the whole one (menu at ~12 s); NHL_QUICK_MOVIE_FRAMES=365 plays all of it.
if [ "${NHL_QUICK_START:-1}" != 0 ]; then
  export REX_NHL_QUICK_START="${REX_NHL_QUICK_START:-true}"
  export NHL_BOOT_CLOCK="${NHL_BOOT_CLOCK:-3}"
  export NHL_QUICK_MOVIE_FRAMES="${NHL_QUICK_MOVIE_FRAMES:-12}"
fi
# REX_PRESENT_THREAD=1 (dedicated present thread) is off: no measurable fps gain over run-to-run
# variance, and play-testing felt laggier. Steam launch options, "<VAR>=<value> %command%":
#   NHL_SWAP_INTERVAL=2           steady 30 fps lock
#   REX_MAX_FRAMES_IN_FLIGHT=1    CPU never ahead of the GPU: ~1 frame (~19 ms) less latency, ~25 % lower fps
#   NHL_WAYLAND=1                 native Wayland window (if X11 misbehaves)
# Movies: the VP6 bridge decodes with ffmpeg, which the GoW Steam image lacks (green movies).
# A static ffmpeg is installed next to the binary (not in git).
[ -x ./ffmpeg ] && export NHL_VP6_FFMPEG="${NHL_VP6_FFMPEG:-$PWD/ffmpeg}"
# FPS + frame-time graph top right (MangoHud ships in the GoW Steam image). NHL_HUD=0 turns it off;
# Shift_R+F12 toggles it in game.
if [ "${NHL_HUD:-1}" != 0 ] && command -v mangohud >/dev/null; then
  export MANGOHUD=1
  export MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-position=top-right,fps,frametime,frame_timing,cpu_stats=0,gpu_stats=0,font_size=22,background_alpha=0.3}"
fi
# The game places its own threads on hybrid CPUs with an integrated GPU (src/cpu_placement.cpp).
# NHL_CPU_CLUSTER=0 turns that off and keeps the main guest thread and the GPU command processor on the
# P-cores instead (nhl_pin.sh; $$ becomes the game's pid after exec). NHL_PIN_PCORES=0 leaves it to the scheduler.
if [ "${NHL_CPU_CLUSTER:-1}" = 0 ]; then
  export REX_NHL_CPU_CLUSTER=off
  [ "${NHL_PIN_PCORES:-1}" != 0 ] && [ -x ./nhl_pin.sh ] && ./nhl_pin.sh $$ >/dev/null 2>&1 &
fi
# Pre-game menu (controller; waits up to NHL_MENU_PAD_WAIT_S for the pad, auto-starts after 120 s). NHL_MENU=0 turns it off.
export NHL_MENU="${NHL_MENU:-1}"
exec ./nhllegacy "$@"
