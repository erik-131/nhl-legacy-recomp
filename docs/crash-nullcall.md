# The "null indirect call, lr=8278AF8C" crash (root cause and fix)

Status: fixed in `src/vp6_bridge.cpp` (commit "vp6 bridge: join reader threads in ~BridgeState ..."). Investigated 2026-11-08.

## Summary
It was never a null call in the game. `nullcall_ctx.txt` is written by the SIGABRT tap in `src/diag_hooks.cpp` for **any** abort, and it
prints `lr` and `last_indirect_target` of the dying thread's guest context. Here the abort was a host `std::terminate`
from our own VP6 movie bridge:

* `~BridgeState` ran while its `std::thread reader` was still joinable. `std::thread`'s destructor then calls `std::terminate`
  (backtrace: `terminate -> unexpected -> _M_dispose (BridgeState) -> Vp6BridgePublish -> sub_8277CC98 -> sub_8278AE90 -> ...`).
* `lr=8278AF8C` is only the stale guest `lr`: `sub_8278AE90` set it before calling `sub_8277CC98`, which our bridge hooks
  (`Vp6BridgePublish`); the epilogue of the callee restores that same value. `last_indirect_target=0` is the initial value (no indirect call ever failed).
  The same registers every time (`r3=FBB0FE00 r4=BD6FBFE8 r31=BD502100`, planes filled with `80 80 ..` = neutral chroma) are the movie plane pointers of the publish call.

## Mechanism
`Vp6BridgePublish` calls `ReapIdleLocked` every 64th publish. It erases streams that have not published for 5 s from `by_ring`
and calls `StopMovie` on the **colour** stream only. A colour stream with alpha holds an `alpha_peer` (second `BridgeState`, own ffmpeg
and reader thread); that was never stopped, so when the last `shared_ptr` to the colour stream died, the alpha peer was destroyed with a
joinable reader (a std::thread that ended on EOF stays joinable until joined, too).
It only fires when (a) an alpha-bearing movie (e.g. `environment_25_half.vp6`, wipes `transition_*.vp6`) was bound earlier and has been
idle > 5 s, and (b) the publish counter hits a multiple of 64 at the next movie burst. That is why it appears 8-11 s after a scene change
(period start / the transition wipe `transition_eas.vp6` bursts), why it is timing dependent (counter alignment), and why builds without the
line hooks crashed as well (they just changed the timing/which movies played). Soak runs with mash did not crash because the bound movies
there were still publishing or the counter did not align.

## Evidence
* Extended crash tap (this commit): all GPRs, ctr (=0), guest stack words, memory at r3/r4/r29/r31, and a host backtrace (resolve with `addr2line -f -C -e out/linux-rel/nhllegacy <addr - base>`).
* Job 68 bridge log: `23:41:16 environment_25_half.vp6` last published, `23:43:05.007 bind ring=BD502298 transition_eas.vp6 (+alpha)`,
  crash `23:43:05.09`, 25 ms after the fourth ring `BD502100` (= r31) started publishing.
* Baseline (installed code + tap, quick route 165 s, bq jobs 61/62/68/69/70): 5 of 5 aborted with identical registers.

## Fix
* `BridgeState::~BridgeState()` stops and joins anything left (logs `dtor: stopping leftover stream` the first 50 times, so a repeat is visible in `vp6_bridge.txt`).
* `ReapIdleLocked` stops the `alpha_peer` as well and stops a stream whose reader is still joinable.
Harmless: it only joins threads that would otherwise have terminated the process.

## Result
Fixed build (bq jobs 74-83, quick route 165 s): **10 of 10 clean** (full length, clean shutdown, no `nullcall_ctx.txt`), v 5 of 5 aborts before (jobs 61/62/68/69/70). Note: `triage.py` called job 68 'OK' although it aborted at 103 s; look for a fresh `nullcall_ctx.txt` as well.
