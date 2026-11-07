# NUC benchmark helpers (used on top of tools/nuc/nhl_bench.sh)

Work files (frequency logs, stall dumps, screenshots) go to `$NHL_WORK` (default `~/nhl-work`, not /tmp: it does not survive a reboot).
Run inside the CT as the `claude` user (sudo for the bench). Start a Steam-free lobby first:
`WOLF_NOSTEAM=1 tools/nuc/ct105/wolf_session.sh start`. Experiments: `NHL_DIR=nhl-linux-dev`.

- `abrun.sh <tag> "<env>"` - one bench run + GPU clock sampling; prints presented fps median/p5, draws, average GPU MHz
  and the present-interval histogram (≤17/20/25/34/>34 ms). If the EA logo movie hasn't started at 110 s it gdb-dumps
  all threads (boot-stall watchdog). Pass `DUR=300` with the latency route. Pins the main guest thread and the GPU
  command processor to the P-cores like `nhl_play.sh` (`PIN=0` to leave placement to the scheduler).
- In-process A/B: put `NHL_AB_TOGGLE="<cvar>=<a>,<b>[;...]@<seconds>"` in the env (runtime cvars only, e.g.
  `nhl_upload_prepass=false,true@20`) and use `DUR=420`: the game alternates A/B inside one match and `abrun.sh`
  prints per-phase results via `abphase.py <log> [skip_s]` (same scene, heat and matchup for both settings).
  A boot stall now kills the run at 110 s and retries once (`RETRY=0` off).
- `lat2.py <log>` - reaction time of scripted START/A presses in gameplay ([nhl-lat], needs `NHL_LAT_PROBE=1` and the
  route in `latroute.txt` appended to the env: `$(cat tools/nuc/bench/latroute.txt)`).
- `pres.py <log>` - guest vs presented fps and present gaps for one log.
- `bootloop-dev.sh <outdir> <N>` - N boots of the dev install, 75 s each; reports OK/STUCK (EA logo movie started?),
  dumps threads on a stall.

Interleave A/B runs (A,B,A,B…), ≥2 per config; run-to-run variance is ±10–30 % (scene/matchup, GPU clock).
Discard runs whose GPU clock differs a lot from their neighbours (SLPC downclocking, host load).

- **Default route: `ROUTE=quick DUR=165`** on the ice ~40 s after the first log line, ~120 s of gameplay. Compared to the long route in 3 + 3 runs: same medians and gaps within noise (2026-11-06). It boots through quick start, so use the long route (`DUR=285`) for anything about the boot.

## stream_cadence.py

Times the Moonlight/Wolf RTP flows as they leave the host. Live (`sudo python3 stream_cadence.py -i eth0 -o log`) or offline
(`tcpdump -i eth0 -w cap.pcap udp` then `--pcap cap.pcap`); `--self-test` runs a synthetic pcap (no root, no network).
Output: per-second frames/packets/kB/s/max gaps, then a summary with frame-interval p50/p95/p99/max, a histogram, frames
with a gap > 25 ms and audio gaps > 30 ms (with clock times). Video frames are grouped by the NV_VIDEO_PACKET frameIndex
(Sunshine/Wolf leave the RTP timestamp at 0; the old timestamp-based count gave 0-3 frames/s); if that field does not look
sane (encrypted video) it falls back to burst grouping (`--burst-gap`, default 3 ms). Not yet run against a real stream.
