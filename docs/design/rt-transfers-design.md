# Host-RT cost reduction for NHL (rtv path) — design

Sources read: Simpsons SDK `tools/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp`
(`native_rt_size_by_use` 3063–3460, `GetDrawOverwrittenRenderTargets` 8074–8366, transfer skip/defer in
Update 2466–2575), `pipeline/render_target/cache.cpp` (create-rows hooks 680, 1164, 1185, 1400),
`util/draw_extent_estimator.cpp` (`GetAutoIndexedVertexPositions`); ours: `third_party/rexglue-sdk-main`
same files; the ownership log `scratchpad/own-rtv.txt` (one logged rtv frame).

## Key finding: size-by-use is not where NHL's "rt" GPU time is

The "rt" category (3.6–3.8 ms/frame, 13–17 %) is the ownership-transfer draws issued by
`PerformTransfersAndResolveClears` from `VulkanRenderTargetCache::Update`. Their cost scales with the
**tiles transferred**, not with image size. In the logged frame:

| transfer direction (key MSAA, 0=1x 1=2x 2=4x) | tiles |
|---|---|
| into 4x targets (x→2) | **13 312** |
| out of 4x targets (2→x) | **10 816** |
| 2x/1x ↔ same MSAA (depth format/pitch aliasing) | ~2 500 |
| resolve-clear | 2 880 |

≈ 27 k tiles × 5 KB (80×16 samples × 4 B) ≈ 140 MB read + written per frame through transfer shaders —
consistent with ~3–4 ms on the Arc iGPU. **~90 % of it is the 2x/1x ↔ 4x round trip around the XDK clear
quads** (`surf=05020140`: 4x MSAA, pitch 320, `dctl=8777`, `cmask=FFFF`, and the depth-only `rp=6`
quad): before each clear the 2x/1x owner's data is transferred into the 4x alias (then completely
overwritten by the clear), and after it the cleared 4x data is transferred back.

`native_rt_size_by_use` (1280×720 instead of 1280×2048 images) saves VRAM (~2.8× per target; on a
16 GB shared-memory iGPU this is not a constraint) and possibly some layout-transition/CCS-resolve
bandwidth, but it does not reduce transferred tiles. Expected GPU gain: small, unmeasured
(≤ 0.5 ms guess). It is also the riskiest piece (every path that assumes full-period height:
dump rectangles, transfer source sampling, resolve EDRAM wrapping, framebuffer extents). → Do it last,
only if steps 1–2 leave a gap.

## Plan (all new behaviour behind cvars, default off, A/B-able)

### Step 0 — measure (≈40 LOC, no risk)
`[nhl-xfer]` once per second in `PerformTransfersAndResolveClears`: transfer rectangles, tiles, split by
(source MSAA → dest MSAA, depth/color), plus how many were issued for a draw that
`GetDrawOverwrittenRenderTargets` (step 1) proves to overwrite the destination. Confirms the table above
on more than one frame and gives the expected saving before any behaviour change.

### Step 1 — skip transfers into areas the draw overwrites (`nhl_rt_skip_overwritten_transfers`)
Port from Simpsons, generic and validated there:
- `DrawExtentEstimator::GetAutoIndexedVertexPositions` (~60 LOC; our estimator already has the CPU shader
  interpreter used by `EstimateVertexMaxY`).
- `VulkanRenderTargetCache::GetDrawOverwrittenRenderTargets` (~290 LOC). Proof requires: depth test
  always/off; stencil always + REPLACE with full write mask (NHL's `dctl=8777` decodes to z+stencil,
  zfunc ALWAYS, stencilfunc ALWAYS, zpass REPLACE — qualifies if `stencilwritemask==0xFF`); opaque blend
  and full color mask per RT; rectangle list of 3 auto-indexed vertices; no cull/poly mode; full AA mask;
  PS without kill/depth write; no alpha test/alpha-to-mask. Coverage from the vertex buffer (XDK clear
  VS hash — Simpsons' `0x0A6D1DD7767FDF27`; NHL's XDK is newer, so log its clear VS hash and rely on the
  CPU-VS path if it differs), window offset applied, clamped to the scissor.
- In `VulkanRenderTargetCache::Update` (ours: line ~1543, the single
  `PerformTransfersAndResolveClears(..., last_update_transfers())` call): split each RT's transfers into
  dropped (fully inside the overwrite rectangle) and cut (performed around it via the existing
  `resolve_clear_rectangle` cutout parameter — our `PerformTransfersAndResolveClears` already has the same
  signature as Simpsons'). ~70 LOC.
- `nhl_rt_debug_poison_overwrites` (~25 LOC): fill each proven area with garbage before the draw — the
  final image must not change. Mandatory for validation.
Expected: removes the transfers *into* the 4x clear aliases (≈ 13.3 k of 27 k tiles) → roughly
**1.5–2 ms/frame** off "rt". Skip Simpsons' deferral (`native_rt_defer_overwritten_transfers`): NHL's
clear quads are followed by normal 2x draws, so holding transfers back gains nothing here.

### Step 2 — NHL-specific "clear-through" for aliased clear quads (`nhl_rt_clear_through_aliases`)
The transfers *out of* the 4x aliases (≈ 10.8 k tiles) only carry the clear value back. When step 1
proves a clear draw overwrites whole tiles of an EDRAM range currently owned by a 1x/2x RT with the same
base, pitch in tiles and format family, do not bind the 4x RT at all: clear the owner's corresponding
tile-aligned rectangle with the same value (reuse the resolve-clear machinery:
`PerformTransfersAndResolveClears(..., render_target_resolve_clear_values, rectangle)`), and leave
ownership unchanged. Conditions: coverage aligned to tile rows/columns (`exact_edges` + tile alignment),
constant output (clear VS + constant PS, value read from the PS constant / vertex buffer as Simpsons'
clear-draws-as-clears does), color format and depth format identical across the alias. Also turns the
clear into a real attachment clear (Intel fast-clear), which is what `native_rt_clear_draws_as_clears`
buys Simpsons (~0.45 ms at 2x on AMD).
Expected: removes most of the remaining round trip → together with step 1 **~3 ms/frame** of the
3.6–3.8 ms "rt", plus fewer render-pass breaks. ~150–200 LOC, NHL-specific, medium risk (must keep
exact ownership semantics when coverage is partial → fall back to step 1 behaviour).

### Step 3 — `native_rt_size_by_use` (only if needed)
Port (~250 LOC): `VulkanRenderTarget::Image` + `ReplaceImage`, `CreateRenderTargetImage(key, tile_rows)`
split out of our `CreateRenderTarget` (ours: render_target_cache.cpp:2002), `EnsureRenderTargetTileRows`
(copy old rows into the grown image, retire old image/framebuffers by submission),
`render_target_create_tile_rows_` set around `GetOrCreateRenderTarget` in `RenderTargetCache::Update`
(ours: cache.cpp:643) and in `PrepareHostRenderTargetsResolveClear`, `EnsureRenderTargetTileRows` at the
top of `ChangeOwnership` for the host-RT path, framebuffer extent clamped to the attachments' rows
(ours: render_target_cache.cpp:2476), and every transfer/dump/resolve source-extent computation bounded
by `tile_rows()`. NHL risks: predicated-tiling halves (window offset −640) reuse the same rows — fine;
depth at tile 736 with 90 rows ends at 1456 < 2048, no wrap — fine; any pass that draws past the first
estimate (unclipped draws are now CPU-estimated) forces a grow + full copy mid-frame (log it; Simpsons
has `native_rt_debug_start_tile_rows` to force growth for testing — port it).

## A/B order for the main session
1. Step 0 build: confirm transfer tiles per frame over several frames (and the clear VS hash).
2. Step 1 with poison on (screenshots identical to off), then poison off: `REX_GPU_PROF` "rt" ms and
   presented fps, interleaved A/B ≥ 3 runs each (run-to-run variance is ±10–30 %).
3. Step 2 likewise. 4. Step 3 only if "rt"/"resolve" still dominate after 1–2.

No draft patch was produced (nothing compiled); LOC figures are from the Simpsons sources.
