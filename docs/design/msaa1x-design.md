# nhl_force_host_msaa_1x — 1-sample host render targets (performance mode)

Patch: `msaa1x.patch` (+52/−12, 3 files), against SDK HEAD 1053b71, `git apply --check` OK. **Uncompiled.**
Cvar `nhl_force_host_msaa_1x` (bool, default false, read once at RT-cache init; host-RT path only).

## Idea
Keep `RenderTargetKey::msaa_samples` (guest) for everything EDRAM: keys, tile pitch/height, ownership,
transfer/dump address math, resolve. Only the *host image* gets 1 sample. A guest-2x RT already has the
pixel grid as its host extent (2x = 1x2 samples per pixel, 4x = 2x2), so extents stay unchanged.

## Touch points (all in the patch)
| Site | Change |
|---|---|
| `render_target_cache.cpp` Initialize (host-RT path) | read cvar; require D24S8 support, force `depth_float24_convert_in_pixel_shader_` so the host depth store / host-depth transfer sources (compiled per guest sample count) are never needed |
| `GetHostRenderTargetsRenderPass` | attachment samples = 1 |
| `VulkanRenderTarget` image creation | `samples = 1` |
| `GetColorOwnershipTransferVulkanFormat` | format support checked for 1 sample |
| transfer shader (`GetTransferShader`) | source/host-depth image declared non-MS and fetched with LOD (no sample operand); EDRAM math still from `source_msaa_samples`; destination: no `SampleId`, sample id = const 0 (host sample 0) |
| transfer pipelines (`GetTransferPipelines`) | `dest_sample_count = 1` (no per-sample pipelines / masks) |
| dump shader (`GetDumpPipeline`) | split `source_is_guest_multisampled` (pixel/sample addressing) from host `source_is_multisampled` (image type + fetch) — every guest sample reads the pixel |
| `pipeline_cache.cpp` multisample state | `rasterizationSamples = 1` (before the 2x-as-4x branch) |

Not touched (verified): pixel shader translator (`kSysFlag_MsaaSamples` is FSI-only; centroid interpolation
is a no-op at 1x), texture cache (always 1x), resolve compute (reads the EDRAM buffer), resolve clears
(`vkCmdClearAttachments`), our step-1/clear-through code (guest-side math only).

## Behaviour / risks
- **Visual:** no anti-aliasing (jaggies on boards/players/lines). k01 resolves average two identical samples.
  Alpha-to-mask emulation degrades to a dithered alpha test at 1 sample.
- Each host pixel holds one guest sample (dest uses host sample 0 → guest sample per `GetD3D10SampleIndexForGuest2xMSAA`);
  transfers between guest-2x and guest-4x aliases (NHL's 4x clear quads) stay consistent but lossy by design.
- 64bpp destination fed from a 32bpp 4x source reads the same pixel twice (edge case; NHL's 3D targets are 32bpp).
- `REXGPU_INFO`/`WARN` and `xenos::MsaaSamples::k1X` in a ternary inside `switch(...)`: should compile, not checked.
- Shader/pipeline caches: the cvar is fixed for the process, so keys need no extra bit; a persistent pipeline
  cache built with the mode off is fine (different render-pass sample count → different pipelines).

## Expected GPU saving (reasoning, not measured)
Profile now: draw 10.4 ms, resolve 2.4, rt 1.6, tex 1.4 (16.5 ms/frame). Pixel shaders run per pixel already,
so 2x→1x mainly removes per-sample depth/stencil test, colour write/blend and MSAA compression traffic:
typically 10–25 % of raster-bound draw time on Intel Xe → **~1–2.5 ms**. Dump/transfer shaders read half the
samples and host RT memory halves → resolve/rt **~0.5–1 ms**. Total **~1.5–3.5 ms (≈10–20 %)**, at the cost of AA.
Measure with `REX_GPU_PROF` A/B (≥2 runs each) and screenshots of edges before deciding.

## How to verify
1. Apply, build dev, run with `REX_NHL_FORCE_HOST_MSAA_1X=true` (+ the play config), confirm the log line
   "host render targets forced to 1 sample" and no Vulkan validation errors.
2. Screenshots menus/gameplay/replay/close-up: image structure identical except AA; no black 3D, no garbage
   at tile seams (would indicate address-math mistakes).
3. A/B GPU profile and presented-fps histogram vs. the current play config.
