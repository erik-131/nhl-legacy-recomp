# macOS: where gameplay frame time actually goes

Measured 2026-09-09 on M4 / MoltenVK, scripted gameplay route, ~48 ms frames
(~19-20 fps) at ~1820 draws/frame.

| path | cost per frame | share |
|------|----------------|-------|
| `IssueDraw` (1.85–2.34 us/draw x ~1820) | ~3.4 ms | ~7% |
| `IssueSwap` (EndSubmission + readback flush + present) | 0.45–0.73 ms | ~1.3% |
| `IssueCopy` (~7 resolves x 4.6–8.9 us) | ~0.04 ms | ~0.1% |
| **total graphics command submission** | **~4 ms** | **~8%** |
| unaccounted (recompiled guest CPU) | ~44 ms | ~92% |

## This contradicts docs/cpu-perf-optimization-plan.md

That plan states the game is "draw-submission / single-thread CPU bound
(~1600 draws/frame, ~10–18 us/draw)". That was measured on the **Windows**
build. On macOS/arm64 the same path costs **1.85–2.34 us/draw**, five to eight
times less, and the whole graphics submission layer is ~8% of frame time.

**Consequence: the six-item plan cannot deliver meaningful macOS fps.** Even
eliminating 100% of graphics submission would move 48 ms to 44 ms - 19 fps to
about 20.7. Items #1 and #2 are implemented (see below) and are individually
sound, but their ceiling here is a fraction of a percent. Items #3 (sampler
caching) and #6 (multithreaded command recording) target the same ~8% and should
NOT be prioritised on macOS on the strength of that plan.

## What was implemented anyway, and what it is worth

Both are correct, both remove real work, both are kept - they are simply not
where macOS frame time is.

- **#1 descriptor set reuse** (3d4a110). 51.7% of texture descriptor-set
  allocations and `vkUpdateDescriptorSets` eliminated (94.0% vertex, 29.3%
  pixel). Timed: the descriptor path went 255 -> 199 ns/draw, about **0.16 ms on
  a 52 ms frame**.
- **#2 pipeline state description short-circuit** (68138c9). 69.0% of draws skip
  `GetCurrentStateDescription`, against a ~70% ceiling.

## CORRECTION (2026-09-23): the guest is mostly WAITING, not computing

The section above concluded "the port is guest-CPU bound" because the command
processor accounts for only ~8% of the frame. That was an inference - the other
92% was simply *not the command processor*. Two guest-compute optimizations and a
profile then showed it is largely the guest waiting:

| lever on the recompiled guest | gameplay fps | verdict |
|---|---|---|
| baseline (`-mcpu=apple-m1` default) | 20.4-20.9 | - |
| `-mcpu=apple-m4` (`NHLLEGACY_HOST_CPU`) | 20.9 | **null** |
| PGO (`NHLLEGACY_PGO=use`), interleaved 3 v 3 | 21.30 vs 21.66 baseline | **null** - ranges overlap |

PGO did optimize the code (the binary is 15% smaller) and the frame rate did not
move. That is what happens when the hot path is waiting.

**The profile says what it waits on.** The single hottest guest function is
`sub_827E2BC8`, in the title's Xbox 360 D3D runtime. It lowers thread priority
(`cctpl`), burns cycles with `db16cyc` delay hints, restores priority (`cctpm`),
then polls a value the GPU writes back - `*(device+11024)` against a target at
`device+11036`, reached from `sub_827EDAB0`. It is a spin-wait on GPU progress.

- 23.18 billion entries over a 403 s capture, and the "value changed" branch
  fired ~7.7 million times: the poll finds nothing 99.97% of the time.
- **85% of the spinning happens in gameplay**, ~11.5 M delay-loop iterations per
  gameplay frame vs ~2.2 M per menu frame (menus-only capture dumped at frame
  4900, full capture at 10000).
- The recompiler emits NOTHING for `db16cyc`/`cctpl`/`cctpm`
  (`src/codegen/builders/system.cpp`, "no effect in recompiled code" - reasoning
  written for x86). On Xenon those hand the core to the sibling hardware thread;
  here the thread spins flat out on a real core.

**What is not yet explained.** The GPU itself finishes quickly - at swap the
command processor waits only ~0.5 ms for it - and the command processor wakes
promptly on a write-pointer update (event-signalled, not polled). So the fence is
slow to arrive for some other reason. Leading hypothesis, UNTESTED: the ring-buffer
read-pointer writeback the guest polls is only stored after `ExecutePrimaryBuffer`
finishes an entire pending batch (`CommandProcessor::WorkerThreadMain`), not as
work progresses, so the guest waits on batch granularity.

**So the lever is fence latency, not codegen.** Further PGO / -mcpu / LTO work on
the guest is unlikely to pay while this dominates. Menus sit at exactly 30.0 fps,
a hard cap; gameplay at ~21 is set by how long the guest waits per frame.

## How to measure (do not use fps)

Live runs diverge and never render the same content, so frame-to-frame or
run-to-run comparison is meaningless - measured 11 vs 30 samples over one
nominal frame range. The deterministic replay bench the plan prescribes is
unavailable on macOS: SDK 0.10.x removed `rex/graphics/trace_protocol.h`, so
`tools/replay/src/xtr_player_unsupported.cpp` is linked instead of the real
player, and the SDK ships no trace writer at all (the `trace_gpu_*` cvars are
inert shims in `src/sdk_cvar_compat.cpp`).

For guest-side changes use `tools/macos/gameplay_bench.sh <build dir> <tag>`:
wall time over guest frames 6500-8000 (on-ice play, below the 30 fps cap). It
refuses to report unless the window averaged >1000 draws/frame - an earlier
"baseline" ran with its input script missing (it lived in /tmp, which was
cleared), idled on a menu at a flat 30.00 fps, and looked perfectly clean.
Identical runs still spread ~7%, so interleave builds and require the ranges not
to overlap.

Use instead: `NHL_DRAW_TIMING=1` (per-draw, per-resolve and per-swap wall time)
and `REX_CPU_TIMING=1` (nanoseconds in the descriptor path). Per-draw
nanoseconds inside a named code path is content-normalised and comparable across
runs. `REX_DESCRIPTOR_STATS=1`, `REX_PIPELINE_STATS=1` and `REX_SHORTCUT_STATS=1`
report hit rates.
