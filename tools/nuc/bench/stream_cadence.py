#!/usr/bin/env python3
"""stream_cadence.py: what Wolf/Sunshine send to a Moonlight client, timed on the host.

  sudo python3 stream_cadence.py -i eth0 -o log.txt [--seconds N]      live (AF_PACKET, needs root)
  python3 stream_cadence.py --pcap cap.pcap                            offline (tcpdump -i eth0 -w cap.pcap udp)
  python3 stream_cadence.py --self-test                                synthetic pcap, no root, no network

Per second: video frames, video packets, kB/s, longest frame gap, audio packets, longest audio gap, RTP seq gaps.
Summary: frame interval histogram (p50/p95/p99/max), frames with a gap > 25 ms (with clock times), audio gaps > 30 ms.

Why the old version saw 0-3 frames/s (12:03-12:14 on 2026-11-07): it counted a frame as a new RTP timestamp. Sunshine and
Wolf leave the RTP timestamp of video packets at 0 (Moonlight ignores it), so every packet looked like the same frame.
Audio worked because it was counted per packet.

Video packet layout (Moonlight protocol, moonlight-common-c Video.h / Sunshine stream.cpp; from memory, not re-fetched,
which is why the detector checks itself): RTP header 12 bytes (byte0 0x80|ext, seq big-endian at +2, timestamp 0), 4 reserved
bytes, then NV_VIDEO_PACKET: streamPacketIndex u32 LE (+16), frameIndex u32 LE (+20), flags u8 (+24; 1 = picture data,
2 = end of frame, 4 = start of frame), then multiFec bytes and fecInfo. All data and FEC parity packets of a frame carry the
same frameIndex. Frames are grouped by frameIndex when that field looks sane (--group auto); otherwise (encrypted video: the
frame index is then inside the AES-GCM payload, or a layout I got wrong) by burst: a new frame starts after an
inter-packet gap > --burst-gap ms (default 3). Burst grouping underestimates frames when two frames are sent back to back
and can split a frame whose packets are paced, so frame-index mode is the one to trust; the summary says which was used.

Ports: not assumed. Wolf logs "RTP ping server on ports 48100 and 48200" (the ports it binds, video 48100, audio 48200,
Sunshine default 47998/48000), the control channel is 47999 (ENet, not RTP). Audio = UDP source port --audio-port; video =
any other RTP-looking UDP packet (version 2, >300 bytes) from this host to the client address learned from audio, or from
--video-port. The ports and sizes seen are printed in the summary. Payload is read only for the video frame index.
"""
import argparse
import collections
import socket
import struct
import sys
import time

ETH_P_ALL = 3


class Analyzer:
    def __init__(self, out, audio_port=48200, video_port=48100, group='auto', burst_gap=3.0):
        self.out, self.audio_port, self.video_port = out, audio_port, video_port
        self.group, self.burst_gap = group, burst_gap
        self.client = None
        self.ports = collections.Counter()      # (sport, 'v'/'a') -> packets
        self.sizes = collections.Counter()      # video packet payload size buckets
        self.cur = None
        self.last_pkt = {'v': None, 'a': None}
        self.lastseq = {'v': None, 'a': None}
        self.sec = self._new_sec()
        self.v_gaps, self.a_gaps = [], []       # (t, ms)
        self.v_frames = 0
        self.v_bytes = self.v_pkts = self.a_pkts = 0
        self.last_fidx = None
        self.last_frame_t = None
        self.probe = []                         # first video frameIndex values, for the sanity check
        self.mode = None                        # 'frameindex' or 'burst' once decided
        self.t0 = self.t1 = None

    @staticmethod
    def _new_sec():
        return {'frames': 0, 'vpk': 0, 'vbytes': 0, 'vgap': 0.0, 'apk': 0, 'agap': 0.0, 'lv': 0, 'la': 0}

    def _flush(self, sec_id):
        s = self.sec
        self.out.write(f"{time.strftime('%H:%M:%S', time.localtime(sec_id))} frames={s['frames']} vpkts={s['vpk']} "
                       f"kB/s={s['vbytes'] / 1000:.0f} max_frame_gap={s['vgap']:.1f}ms audio_pkts={s['apk']} "
                       f"max_audio_gap={s['agap']:.1f}ms seq_gaps_v={s['lv']} seq_gaps_a={s['la']}\n")
        self.sec = self._new_sec()

    def _decide_mode(self):
        if self.group in ('frameindex', 'burst'):
            return self.group
        p = self.probe
        if len(p) < 60:
            return None
        distinct = len(set(p))
        steps_ok = sum(1 for x, y in zip(p, p[1:]) if 0 <= (y - x) & 0xFFFFFFFF <= 4)
        # sane: several packets per frame (few distinct values), counter only steps by 0..4
        if 2 <= distinct <= 0.6 * len(p) and steps_ok >= 0.97 * (len(p) - 1):
            return 'frameindex'
        return 'burst'

    def packet(self, t, dst_ip, sport, udp_payload, wire_len):
        if len(udp_payload) < 12 or udp_payload[0] >> 6 != 2:
            return
        if sport == self.audio_port:
            k = 'a'
            self.client = dst_ip
        else:
            if sport != self.video_port and not (self.client is not None and dst_ip == self.client and len(udp_payload) > 300):
                return
            if self.client is None:
                self.client = dst_ip
            k = 'v'
        self.ports[(sport, k)] += 1
        seq = struct.unpack_from('>H', udp_payload, 2)[0]
        if self.t0 is None:
            self.t0 = t
        self.t1 = t
        if self.cur is None:
            self.cur = int(t)
        while int(t) > self.cur:
            self._flush(self.cur)
            self.cur += 1
        s = self.sec
        if self.lastseq[k] is not None and seq != (self.lastseq[k] + 1) & 0xFFFF:
            s['l' + k] += 1
        self.lastseq[k] = seq
        prev = self.last_pkt[k]
        self.last_pkt[k] = t
        if k == 'a':
            s['apk'] += 1
            self.a_pkts += 1
            if prev is not None:
                g = (t - prev) * 1000
                s['agap'] = max(s['agap'], g)
                self.a_gaps.append((t, g))
            return
        s['vpk'] += 1
        s['vbytes'] += wire_len
        self.v_pkts += 1
        self.v_bytes += wire_len
        self.sizes[min(len(udp_payload) // 100 * 100, 1400)] += 1
        fidx = struct.unpack_from('<I', udp_payload, 20)[0] if len(udp_payload) >= 28 else None
        if self.mode is None:
            if fidx is not None:
                self.probe.append(fidx)
            self.mode = self._decide_mode()
        if self.mode == 'frameindex':
            new_frame = fidx != self.last_fidx
        else:                                    # burst, or still deciding (provisional)
            new_frame = prev is None or (t - prev) * 1000 > self.burst_gap
        self.last_fidx = fidx
        if new_frame:
            self.v_frames += 1
            s['frames'] += 1
            if self.last_frame_t is not None:
                g = (t - self.last_frame_t) * 1000
                s['vgap'] = max(s['vgap'], g)
                self.v_gaps.append((t, g))
            self.last_frame_t = t

    def finish(self):
        if self.cur is not None:
            self._flush(self.cur)
        w = self.out.write
        mode = self.mode or ('burst (too few packets to decide)' if self.v_pkts else 'none')
        dur = (self.t1 - self.t0) if self.t0 is not None and self.t1 > self.t0 else 0.0
        w(f"streams seen (source port/kind: packets): { {f'{p}/{k}': n for (p, k), n in sorted(self.ports.items())} }\n")
        w(f"video packet payload sizes (100 B buckets): {dict(sorted(self.sizes.items()))}\n")
        g = sorted(x for _, x in self.v_gaps)
        a = sorted(x for _, x in self.a_gaps)

        def pct(x, p):
            return x[min(len(x) - 1, int(len(x) * p))] if x else 0.0
        w(f"SUMMARY video: grouping={mode}, frames={self.v_frames}, packets={self.v_pkts}, "
          f"{self.v_frames / dur if dur else 0:.1f} frames/s, {self.v_pkts / dur if dur else 0:.0f} pkt/s, "
          f"{self.v_bytes * 8 / dur / 1e6 if dur else 0:.1f} Mbit/s (wire)\n")
        w(f"  frame interval ms: p50 {pct(g, .5):.1f} p95 {pct(g, .95):.1f} p99 {pct(g, .99):.1f} max {(g[-1] if g else 0):.1f}; "
          f">25 ms: {sum(x > 25 for x in g)}, >40 ms: {sum(x > 40 for x in g)}, >100 ms: {sum(x > 100 for x in g)}\n")
        if g:
            edges = [8, 12, 15, 18, 20, 25, 33, 40, 50, 100, 1e9]
            h, lo = [], 0
            for e in edges:
                h.append(f"{lo}-{'inf' if e > 1e8 else e}: {sum(lo <= x < e for x in g)}")
                lo = e
            w("  histogram ms  " + "  ".join(h) + "\n")
        slow = [(t, x) for t, x in self.v_gaps if x > 25]
        for t, x in slow[:40]:
            w(f"  frame gap {x:.1f} ms at {time.strftime('%H:%M:%S', time.localtime(t))}.{int(t % 1 * 1000):03d}\n")
        if len(slow) > 40:
            w(f"  ... {len(slow) - 40} more\n")
        w(f"SUMMARY audio: packets={self.a_pkts}, gap p50 {pct(a, .5):.1f} p99 {pct(a, .99):.1f} max {(a[-1] if a else 0):.1f} ms, "
          f">30 ms: {sum(x > 30 for x in a)}\n")
        aslow = [(t, x) for t, x in self.a_gaps if x > 30]
        for t, x in aslow[:40]:
            w(f"  AUDIO GAP {x:.1f} ms at {time.strftime('%H:%M:%S', time.localtime(t))}.{int(t % 1 * 1000):03d}\n")
        if len(aslow) > 40:
            w(f"  ... {len(aslow) - 40} more\n")
        if self.v_pkts == 0:
            w("NO VIDEO seen: check the audio flow was detected (client address) or pass --video-port; "
              "Wolf video may use another source port.\n")


def parse_eth(frame):
    """Ethernet(+optional VLAN) / IPv4 / UDP -> (dst_ip, sport, payload) or None."""
    if len(frame) < 14 + 20 + 8:
        return None
    et, off = frame[12:14], 14
    if et == b'\x81\x00':
        et, off = frame[16:18], 18
    if et != b'\x08\x00':
        return None
    ihl = (frame[off] & 15) * 4
    if frame[off + 9] != 17:
        return None
    sport = struct.unpack_from('>H', frame, off + ihl)[0]
    return frame[off + 16:off + 20], sport, frame[off + ihl + 8:]


def read_pcap(path):
    """Classic pcap (as written by tcpdump -w), microsecond or nanosecond, Ethernet link type."""
    with open(path, 'rb') as f:
        magic = f.read(24)[:4]
        end = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>', b'\x4d\x3c\xb2\xa1': '<', b'\xa1\xb2\x3c\x4d': '>'}.get(magic)
        if end is None:
            raise SystemExit('not a classic pcap file (pcapng: use tcpdump -w)')
        nano = magic in (b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d')
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            sec, frac, caplen, wlen = struct.unpack(end + 'IIII', h)
            yield sec + frac / (1e9 if nano else 1e6), f.read(caplen), wlen


def run_pcap(path, an):
    for t, frame, wlen in read_pcap(path):
        r = parse_eth(frame)
        if r:
            an.packet(t, r[0], r[1], r[2], wlen)
    an.finish()


def run_live(a, an):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(ETH_P_ALL))
    s.bind((a.iface, 0))
    t_start = None
    try:
        while True:
            s.settimeout(1.0)
            try:
                pkt, addr = s.recvfrom(65535)
            except socket.timeout:
                if a.seconds and t_start and time.time() - t_start > a.seconds:
                    break
                continue
            if addr[2] != 4:  # only packets leaving this host
                continue
            now = time.time()
            t_start = t_start or now
            r = parse_eth(pkt)
            if r:
                an.packet(now, r[0], r[1], r[2], len(pkt))
            if a.seconds and now - t_start > a.seconds:
                break
    except KeyboardInterrupt:
        pass
    an.finish()


# ---------------------------------------------------------------- self-test
def _frame(sport, dst, payload):
    udp = struct.pack('>HHHH', sport, 50000, 8 + len(payload), 0) + payload
    ip = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, bytes([10, 0, 0, 2]), dst) + udp
    return b'\x00' * 12 + b'\x08\x00' + ip


def _video_pkt(seq, fidx, flags, size=1400):
    rtp = struct.pack('>BBHII', 0x90, 0, seq & 0xFFFF, 0, 0)          # timestamp 0 like Sunshine
    nv = struct.pack('<IIBBBBI', seq << 8, fidx, flags, 0, 0, 0, 0)
    return rtp + b'\x00' * 4 + nv + b'\x00' * (size - 12 - 4 - 16)


def _audio_pkt(seq):
    return struct.pack('>BBHII', 0x80, 97, seq & 0xFFFF, seq * 240, 0) + b'\x00' * 100


def synth_pcap(path, encrypted=False):
    """12 s at 60 fps, 8 packets per frame (7 data + 1 FEC) in a 2 ms burst, audio every 5 ms.
    Injected: 70 ms frame gap at t=4 s, 33 ms gap at t=8 s... (>25 ms both), 60 ms audio dropout at t=6 s.
    encrypted: the frameIndex bytes are random noise (as inside an AES payload)."""
    import random
    rnd = random.Random(1)
    dst = bytes([10, 0, 0, 9])
    ev = []
    t, fidx, vseq = 0.0, 0, 0
    next_stall = {4.0: 0.054, 8.0: 0.017}
    while t < 12.0:
        for i in range(8):
            p = bytearray(_video_pkt(vseq, fidx, 1 | (4 if i == 0 else 0) | (2 if i == 7 else 0)))
            if encrypted:
                p[20:24] = rnd.randbytes(4)
            ev.append((t + i * 0.0003, _frame(48100, dst, bytes(p))))
            vseq += 1
        fidx += 1
        t += 1 / 60
        for k in list(next_stall):
            if t >= k:
                t += next_stall.pop(k)
    t, aseq = 0.0, 0
    while t < 12.0:
        if not (6.0 <= t < 6.06):
            ev.append((t, _frame(48200, dst, _audio_pkt(aseq))))
        aseq += 1
        t += 0.005
    ev.sort(key=lambda e: e[0])
    base = 1_700_000_000.0
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for tt, fr in ev:
            ts = base + tt
            f.write(struct.pack('<IIII', int(ts), int((ts % 1) * 1e6), len(fr), len(fr)) + fr)


def self_test():
    import io
    import os
    import tempfile
    ok = True

    def check(name, cond, info=''):
        nonlocal ok
        print(('PASS ' if cond else 'FAIL ') + name + (' ' + info if info else ''))
        ok &= bool(cond)
    for enc in (False, True):
        path = os.path.join(tempfile.mkdtemp(), 'syn.pcap')
        synth_pcap(path, encrypted=enc)
        buf = io.StringIO()
        an = Analyzer(buf)
        run_pcap(path, an)
        txt = buf.getvalue()
        if not enc:
            print(txt)
        gaps = [g for _, g in an.v_gaps]
        n = 'burst(encrypted)' if enc else 'frameindex'
        check(f'[{n}] grouping mode', an.mode == ('burst' if enc else 'frameindex'), f'got {an.mode}')
        check(f'[{n}] frame count ~ 718', abs(an.v_frames - 718) <= 3, f'got {an.v_frames}')
        check(f'[{n}] exactly 2 frame gaps > 25 ms', sum(g > 25 for g in gaps) == 2, f'got {sum(g > 25 for g in gaps)}')
        check(f'[{n}] max frame gap ~ 70 ms', 65 < max(gaps) < 75, f'got {max(gaps):.1f}')
        check(f'[{n}] one audio gap > 30 ms flagged', sum(g > 30 for _, g in an.a_gaps) == 1 and 'AUDIO GAP' in txt)
    print('SELF-TEST ' + ('OK' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-i', '--iface', default='eth0')
    ap.add_argument('-o', '--out', default='-')
    ap.add_argument('--pcap', help='analyse a tcpdump -w capture instead of sniffing')
    ap.add_argument('--video-port', type=int, default=48100)
    ap.add_argument('--audio-port', type=int, default=48200)
    ap.add_argument('--group', choices=['auto', 'frameindex', 'burst'], default='auto')
    ap.add_argument('--burst-gap', type=float, default=3.0, help='ms gap that starts a new frame in burst grouping')
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        sys.exit(self_test())
    out = sys.stdout if a.out == '-' else open(a.out, 'a', buffering=1)
    an = Analyzer(out, a.audio_port, a.video_port, a.group, a.burst_gap)
    if a.pcap:
        run_pcap(a.pcap, an)
    else:
        run_live(a, an)


if __name__ == '__main__':
    main()
