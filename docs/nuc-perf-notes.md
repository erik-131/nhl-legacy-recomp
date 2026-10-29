# NUC (Core Ultra 5 225H / Arc iGPU) performance notes — branch `nuc-perf`

Base: Redth/nhl-legacy-recomp-mac (SDK v0.10.0 + `sdk/rexglue-macos-fsi-tiling-fixes.patch`,
which includes the batched end-of-frame readback, 72d560f).

## Baseline (Windows build via Proton in Games on Whales, 2026-10-29)
- Vulkan: Intel(R) Graphics (ARL) 0x7D51, Mesa 25.0.7 ANV, FSI path.
- Gameplay 10–12 fps, 85–100 ms/frame at ~1800 draws/frame (RTX 3070: ~10 ms at the same draw count).

## What the guest actually waits on (verified in generated code)
`sub_827EDAB0` / `sub_827E2BC8` wait until `*(*(device+11024))` reaches a fence value
(`device+11036` = last issued fence). `device+11024` is a 128-byte block allocated in
`nhllegacy_recomp.148.cpp`; the ring-buffer RPTR writeback is pointed at **block+60**
(`VdEnableRingBufferRPtrWriteBack`, `nhllegacy_recomp.24.cpp`), but the poll reads
**block+0**, i.e. the CP SCRATCH register writeback. The SDK writes scratch registers
when the CP thread *processes* the packet (`command_processor.cpp` WriteRegister).

=> The "batch-granular RPTR writeback" hypothesis from docs/macos-perf-where-the-time-goes.md
does not apply to this poll. The guest waits for the **CP thread** to reach the fence
packet; the CP thread in turn blocks on GPU fences. `sdk/rexglue-rptr-incremental.patch`
(`REX_RPTR_INCREMENTAL=1`) is kept for completeness but is expected to be a null result.

## Diagnostics added
- `REX_FENCE_WAIT_STATS=1` (`sdk/rexglue-fence-wait-stats.patch`): once per second logs
  `[nhl-fencewait]` with CP-thread time blocked in `vkWaitForFences`, split into
  frame-slot waits (frames in flight full => GPU-bound) and other waits (readback drains,
  resource reuse). High frame-slot wait share on the NUC => GPU-bound => next step is the
  Simpsons-style host-RT path (drop FSI).

## Linux-native build (cloud)
- clang-20, Ninja, `-march=x86-64-v3`, `REXSDK_DIR=third_party/rexglue-sdk-main`
  (rexglue c94f5eb + the patches above), libsnappy-dev.
- `rexglue codegen nhllegacy_manifest.toml` with the vanilla xex
  (sha256 d818eb15775c1fee34b057b0fc494b211535d07123230ee45bd7d40991c55624) in `game/`: 12 min, 725 files, 384 MB.
