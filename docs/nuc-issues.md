# NUC / Linux build — issues reported by Erik (play tests 2026-10-30)

Status: **open**, **fixed** (verified in play or screenshots), **fixed?** (changed, not confirmed by Erik).

| # | Issue | Status | Notes |
|---|---|---|---|
| 1 | Low / uneven frame rate in gameplay | **improved** | 10 fps (fsi) → ~61 fps presented median (rtv + direct present + `clear_memory_page_state=false`). |
| 2 | Input feels sluggish ("seigt") | **measured** | Present thread reverted (made it worse). `REX_MAX_FRAMES_IN_FLIGHT=1` and the direct (no Steam) tile: no noticeable difference. Needs an objective measurement (see Next). |
| 3 | Hitching on fast camera pans, not smooth | **improved** | Not shader compilation (6 pipelines per match). Likely uneven frame times (45–65 fps into a 60 Hz stream). Needs steady ≥ 60 + pacing. |
| 4 | Green fields in movies, transitions, arena TV | **fixed** | No ffmpeg in the GoW Steam image → VP6 bridge had no frames; static ffmpeg bundled. |
| 5 | Slight green at the very start of the intro movie | **fixed?** | Bridge waits up to 200 ms for the first ffmpeg frame; intro clean in screenshots. |
| 6 | Green garbage on equipment (helmets) | **fixed** | `NHL_VK_RB_MAX_PER_DEST` was a lifetime cap; readback `fast` instead. |
| 7 | Transition wipes broken (blocky black/white) | **fixed?** | All movie players shared one ffmpeg stream (arena TV + wipe stole frames, wipe fell back to the miscompiled guest decoder). Now one stream per player ring. Windows uses the same bridge. |
| 8 | Black players in team select | **likely by design** | The preview is `feprop_jersey.rx2`: a mannequin whose body mesh has no texture; identical on fsi/ROV/rtv. Confirm against 360 footage. |
| 9 | Odd sound effects when scrolling menus | **open** | Also in the original. Audio Worker spins ~75 % of a core; check XMA decode. |
| 10 | Goal horn not heard | **open** | Not verified whether the game should play one; no failed audio file opens in the logs. |
| 11 | Multi-second pauses in menus and when leaving a match | **open** | Related: intermittent boot stall on the cloud screen (~1 in 4 bench runs); bench now gdb-dumps on a stall. |
| 12 | Steam overlay stutters / unusable | **fixed?** | Works in SteamWorld. Launcher now opens an X11 window (Xwayland) instead of Wayland — not yet confirmed. |
| 13 | Xbox/Guide button captured by the game | **fixed?** | `NHL_NO_GUIDE=1` + `NHL_DIRECT_PRESENT=1` (no enhancements overlay). |
| 14 | FPS counter | **done** | MangoHud top right (Shift_R+F12). |
| 15 | Same improvements for the Windows build | **open** | rtv fix (shared SDK code; D3D12 uses ROV because of the same black-3D bug), `NHL_SWAP_INTERVAL=0`, readback, `clear_memory_page_state=false`. Needs a Windows test. |
| 16 | Launch without Wolf UI | **done** | Moonlight tiles "Steam" and "NHL Legacy" (direct), icons from the real logos. |
| 17 | Low-latency launch option | **done** | Steam shortcut "NHL Legacy (Linux, low latency)". |
| 18 | 4K stream costs performance | **done** | Erik streams below 4K now. |

Environment notes: Frigate on the host and any active Moonlight session share the iGPU — stop them for clean
measurements. `/dev/shm` guest-memory leak fixed (was 15 GB).
