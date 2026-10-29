# Moving the NHL/Wolf setup from CT 105 (privileged) to an unprivileged LXC

Everything in git (branch `nuc-perf`) is safe. What only exists on CT 105 and must be carried over:

| What | Path on CT 105 | Notes |
|---|---|---|
| Windows game install (xex, `game/`, ~10 GB) | `/etc/wolf/profile-data/user/WolfSteam/nhl` | user's own copy, never in git |
| Linux build + runtime libs | `/etc/wolf/profile-data/user/WolfSteam/nhl-linux` | `nhllegacy`, `librexruntime.so`, `librexgpu-xenos.so`, `game -> ../nhl/game`, `logs/`; rebuildable, but copy to keep the exact tested build |
| Steam shortcut "nhllegacy.exe" | inside `/etc/wolf/profile-data/user/WolfSteam` (Steam userdata) | comes along if the whole Wolf profile dir is copied |
| Bench harness + results | `/root/nhlbench/` | `nhl_bench.sh` (v8+), `*.txt` results, `shots_*`, gate script |
| SSH key for automation | `/root/.ssh/authorized_keys` | line with key `nhl_nuc`, `from="192.168.0.85"` (Windows PC) |

## Checklist
1. Copy `/etc/wolf` (whole Wolf config + profile data) and `/root/nhlbench`. For unprivileged CTs the
   files must be owned by the shifted ids (container uid 1000 = host uid 101000). Easiest: `vzdump` the old
   CT and restore with `--unprivileged 1` (pct remaps ownership), or `rsync -aH --numeric-ids` then
   `chown -R 1000:1000` *inside* the new CT for `profile-data/user/WolfSteam`.
2. Keep IP `192.168.0.22` if possible — the PC bats (`nhl_cmd.bat`, `nhl_upload.bat`, `nhl_get.bat`,
   `nhl_backup.bat`) and the build-CT gate use it. Otherwise tell Claude the new IP.
3. Re-add the `authorized_keys` line (same key, same `from=`).
4. New host key: on the PC run `ssh-keygen -R 192.168.0.22` once (the bats use `BatchMode=yes` and will
   fail on a changed key).
5. Wolf in unprivileged LXC needs: `features: nesting=1,keyctl=1`; `/dev/dri/*` passed in with the
   render/video gids mapped (`dev0: /dev/dri/renderD128,gid=<render gid in CT>`); `/dev/uinput` and
   `/dev/uhid` for virtual input (vpad route in the bench depends on Wolf's virtual gamepad).
6. RAM: keep 16 GB (8 GB thrashed). Swap 8 GB.
7. Verify: start the Steam app in Wolf once via Moonlight, disconnect the client (ctrl+alt+shift+q) so the
   session stays, then run a baseline bench, e.g. `nhl_bench.sh base-fsi ""` — expect ~10 fps fsi gameplay
   with Frigate on; `NHL_VK_RT_PATH=rtv NHL_VK_READBACK_MODE=some NHL_SWAP_INTERVAL=0` ~44 fps.

## Open investigation at the time of the move (2026-10-29 17:45)
- rtv renders 3D black: per-frame resolve log (`REX_RESOLVE_LOG_AT=230`) shows rtv frames have 13 resolves vs
  24 in fsi and the 3D-pass depth resolve is nearly empty → the 3D draws do not rasterize in rtv.
  `REX_NATIVE_2X_MSAA=false` and `REX_NATIVE_STENCIL_VALUE_OUTPUT=false` did not change it.
  Next: diff the `draws x N rp=… surf=… dctl=…` runs between `runs-fsi.txt` and `runs-rtv.txt`.
- CT 105 hung twice; memory "used" grew ~0.6–1.7 GB per bench run. Check `mem_after_rtv.txt`.

## Update 17:50 — CT 105 is converted in place
Erik converts CT 105 itself (backup → restore as unprivileged), so data stays where it is; only ownership is
remapped by pct. Re-check device passthrough (step 5), keys (step 3/4) and run `tools/nuc/ct105/setup.sh` —
Claude Code will run inside CT 105 from now on (see `docs/nuc-handoff.md`).
