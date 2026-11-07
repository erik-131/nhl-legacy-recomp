# Art mods: replacing textures (.rx2)

Status 2026-11-07: end to end proven on Linux (a replaced centre-ice logo shows in a quick-route match, see
"In-game test"). Never commit game files or textures extracted from the game; commit only
original art you made yourself.

## Pipeline

```
your PNG (original art) -> tools/art/rx2tool.py replace DONOR.rx2 SLOT art.png --out NEW.rx2
                        -> cache/guest/rendering/<dir>/<same file name>.rx2   (or mods/<mod>/rendering/...)
```

1. Pick the file to change (table below). The donor is the game's own file of that name from
   `game/_compiled/rendering/...` on your install (read only; do not edit the install).
2. `rx2tool.py info DONOR.rx2` shows the slots (size, format, mip count). Your image is resized to the
   slot size; a DDS must match size and format exactly and carry at least as many mips.
3. `rx2tool.py replace` keeps the header, name table and mip layout of the donor and swaps only the
   pixel buffer. The result has the donor's exact size, so the loader sees a valid file.
4. Put the result at the same relative path: `cache/guest/rendering/...` (proven override layer,
   same as the database override) or `mods/<mod>/rendering/...` ([mod-layers.md](mod-layers.md)).
   The file name must stay the same; the game looks assets up by name (artid in the name).

Other commands: `info`, `dump` (DDS + PNG per slot, to look at a donor), `roundtrip` (untile and
retile, byte compare; a quick check that the tool understands a file). Tests:
`python3 tools/art/test_rx2tool.py` (synthetic file, no game data).

## Lookup by artid

`artid` is column `artid` of table `exhibitionteams` (read with `tools/rosters/tdb.py export DB
exhibitionteams`). For the 30 stock NHL slots artid equals the slot number (ANA 0, CBJ 8, ARZ 22).
Formats measured with `rx2tool.py info` on the stock files (slot 0 unless noted):

| What | File (under `rendering/`) | Slot size, format, mips | Which team |
|---|---|---|---|
| centre-ice logo | `icesurface/centerlogo_<artid>_cm.rx2` | 1024x1024 DXT3, 11 | home team |
| puck | `puck/puck_<artid>_cm.rx2` | 256x512 DXT1, 10 | home team (per artid) |
| boards banner | `banner/banner_<artid>_cm.rx2` (+ `_<n>_dm/_tm`, `_nm`, `_sm` variants) | 512x512 DXT3, 10 | home team |
| logo on jersey, helmet, chest | `logo/texlib_<artid>_<style>_<var>.rx2` | several slots (the earlier read: 256x128, 128x64, 128x128 DXT5) | per team |
| jersey / pants / socks | `jersey/texlib_<home0/away1>_<artid>_<n>.rx2`, `pant/...`, `sock/sock_<h/a>_<artid>_<n>_cm.rx2`, `jersey/name_*` | not measured here | per team, home and away |
| arena spotlight, tunnel, jumbotron | `spotlight/texlib_<n>`, `tunnel/texlib_<n>`, `jumbotron/texlib_<n>` | not measured | the table's `spotlight`, `tunnel`, arena ids (equal artid for stock) |
| menu logos | `fe/ion/artassets/teamlogos*/<t|s|d|c|w|r><artid>.big` | ARGB8888, XPR2 + Apt, NOT rx2 | not handled by this tool |

A new team needs the whole set under its artid (see the list for slot 22 in the worklog); a missing
kit file gives untextured jerseys.

## Verified and not verified

Verified on real files (sweep of 180 files, 447 textures: logo, puck, banner, icesurface,
spotlight, jersey, pant, sock, tunnel, jumbotron): untile then retile is byte-identical on 443;
the other 4 differ in 16 padding bytes of degenerate 4x4 DXT1 slots (not pixel data). `replace` output
re-reads with `roundtrip` OK, the dumped DDS decodes with Pillow, and the file size is unchanged.
Formats covered: DXT1/3/5, DXN, ARGB8888 for the round trip; `replace` supports DXT1/3/5 only.

Not verified: the in-game look of anything but the centre logo; ARGB8888 and DXN
replacement; slots with more than one texture beyond the round trip; how the game maps
the logo texlib slots to the helmet, chest and shoulder; banner and puck UV layouts (the first
tests use obvious stripes, not final art).

## Encoding quality

The built-in encoder is a plain range fit (min/max colour, nearest index) with a box-filter mip
chain. It is lossy: gradients band, thin lines shift colour, and DXT3 keeps only 4 bit alpha.
It is for proving the pipeline and quick tests. For final art make the DDS with a real tool
(AMD Compressonator, NVIDIA Texture Tools `nvcompress`, `texconv`; use the slot's format, a full
mip chain, linear, no flip) and pass the DDS to `replace`; the tool then copies the blocks as given.

## In-game test (2026-11-07)

Scratch cache root `nhl-arttest/cache` (`REX_CACHE_ROOT=/home/retro/nhl-arttest/cache`), dev install,
`ROUTE=quick`, `NHL_LOG_ASSET_OPENS=1`. The quick route's match is ANA (away) at SJ (home), so the
home artid is 25, not 8 or 0.

- Run 1 had files only for artids 0 and 8 (centre logo, puck, banner). The log showed the guest asks
  `cache:\rendering\icesurface\centerlogo_25_cm.rx2` first and, on a miss (0xc000000f), reads the loose
  tree: so the cache layer is consulted before `_compiled`, by name, and the unused artids are never
  asked. (Loose tree opens are logged as `[asset-open]`; cache hits are not.)
- Run 2 added `centerlogo_25_cm.rx2` and `puck_25_cm.rx2` made with `rx2tool replace` from an original
  PNG (disc in team-ish colour, "LOCAL TEST", "ID 25", 1024x1024 DXT3). No miss line for them any more,
  and the faceoff screenshots (44 to 60 s) show the new logo at centre ice with correct alpha and
  mips (it stays clean from the broadcast camera to the close-up); the red line draws over it.
  The text runs along the rink's long axis, so draw the art with that orientation in mind.
- Puck: the file was served (no miss) but no screenshot is close enough to judge the puck texture.
  Banner: `banner_<artid>_cm` was not opened in this match (not asked, so untested).

## Files carrying Arizona's artid (22), slot 22 in `exhibitionteams`

Names only (from `game/_compiled`), so a replacement team in that slot needs these under its artid,
or a free artid with the full set. Per-team assets, not data: `rx2` unless noted.

- `rendering/icesurface/`: `centerlogo_22_cm`, `icesurface_22_bm`, `ice_cline_22_cm`, `ice_hotspot_1_22_cm`
- `rendering/puck/puck_22_cm`; `rendering/banner/`: `banner_22_cm`, `year_22_cm`
- `rendering/logo/texlib_22_0_{0,1,3,4}`, `texlib_22_1_{0,1,2,3,4}` (9 files)
- `rendering/jersey/`: `texlib_{0,1}_22_{0,1,3,4}` (8), `name_{0,1}_22_{0,1,3,4}_cm` (8)
- `rendering/pant/texlib_{0,1}_22_{0,1,3,4}` (8); `rendering/sock/sock_{0,1}_22_{0,1,3,4}_cm` (8)
- `rendering/czlogo/logoct_22_*` (12: dm, `0_{nm,sm,tm}`, `1_{nm,sm}`, `2_{nm,sm}`, `4_{nm,sm}`, `5_{nm,sm}`)
- goalie gear: `goaliehelmet_{0,1,2,6}_22_cm` (4), `goaliepad_*_22_*` (8), `goaliestick_*_22_*` (2),
  `blocker/*_22_*` (16), `trapper/*_22_*` (16), `coach/coach_1_22_cm`
- crowd and arena (these use the arena, spotlight, tunnel or crowd id, equal to 22 for stock): `crowd/team_22_{0,1}_cm`,
  `crowd/prop_team_{0,1}_22_cm`, `crowddata/crowd_22.{cwd,dat}`, `spotlight/texlib_22`, `tunnel/texlib_22`,
  `jumbotron/texlib_22`, `enviro/helmet_22`, `stadium/stadium_22_container_0`, `effects/powerring*_22_*` (14),
  `particle/fx_22_*.lnb` (3) and `stadium_22.rvi`, `movies/environment_22_half.vp6`
- front end (`fe/ion/artassets/`): `teamlogos/t22.big`, `teamlogossmall/s22`, `teamlogoswide/w22`,
  `teamlogosdynasty/d22`, `teamlogoscalendar/c22`, `teamlogosreflection/r22`, `twittericonsteam/t22`,
  `strategies/s22`, `jerseys/jersey_{0,1}_22_{0,1,3,4}` (8), `playermenubackgrounds/22_{0,1}`

Minimum for an on-ice look change of the team: centre logo, puck, logo texlibs, jersey/pant/sock sets
(home and away), name textures. The rest (goalie gear, crowd, arena, FE logos) is secondary. Which of
the arena items follow the team's artid or its arena id was not tested.
