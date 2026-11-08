#!/usr/bin/env python3
"""memdump.py <delay_s> <guest_addr_hex> <len_hex> <out> : wait for nhllegacy, then copy guest memory (host base 0x100000000). Needs sudo."""
import sys, subprocess, time
d, a, n, o = float(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
for _ in range(120):
    r = subprocess.run(['pgrep', '-x', 'nhllegacy'], capture_output=True, text=True).stdout.split()
    if r: break
    time.sleep(1)
else: sys.exit(1)
time.sleep(d)
f = open('/proc/%s/mem' % r[0], 'rb', 0); f.seek(0x100000000 + a)
open(o, 'wb').write(f.read(n))
