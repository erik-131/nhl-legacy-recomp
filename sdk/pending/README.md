# Pending SDK patches

Uncommitted working-tree changes in `third_party/rexglue-sdk-main` (base: SDK `nhl-nuc` HEAD `1af2842`) as of 2026-11-08 (exported from a working tree).
Both are default off (cvars `false`). They were not committed in the SDK repo because the SDK tree is shared by all
worktrees and was still being edited when this was exported, and the final state was not rebuilt here. Apply with
`git apply sdk/pending/<name>.patch` in the SDK directory (reverse-checked against the working tree).

- `nhl_ps_pcf_gather.patch`: cvar `nhl_ps_pcf_gather` (default false), 16 nearest shadow taps as four OpImageGather. Picture-identical, +2 % alone, about 0 with implicit LOD.
- `nhl_force_host_msaa_1x_4x.patch`: cvar `nhl_force_host_msaa_1x_4x` (default false), 1-sample host targets for 4x guest targets only. Supersedes the untracked `sdk/rexglue-host-msaa-1x-4x.patch` of the main tree.

The replay100 changes (vertex texture-fetch specialisation etc.) are already committed in the SDK (`1af2842`) and are in `series-nhl-nuc`.
