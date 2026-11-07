#!/usr/bin/env python3
"""Exhaustive interleavings of the game's condition variable (sub_82670A78 wait, sub_82670BC0 signal) and its semaphore
wrapper (sub_82671168, sub_82671280, sub_82670F78), driven like the movie handshake: the driver sets state 1, signals and
waits for 2; the per-tick update (sub_8277CB20) does a zero-timeout wait until state is 1, then sets 2 and signals.
The update keeps ticking after it set 2, as in the logs. Sequentially consistent memory, one step per shared access.
Reports any end state with state 2 and the driver still blocked.
usage: guest_cv_model.py [ticks] [none|poll|gate]
  poll: the update reads the state without a wait; gate: every wait passes the gate on entry, as in pthreads-win32."""
import sys

FIX = sys.argv[2] if len(sys.argv) > 2 else "none"

INIT = dict(state=0, blocked=0, unblock=0, gone=0, count=0, pending=0, kernel=0, gate=1, L=None, M=None, ddone=0, udone=0)


def lock(s, name, me, site):
    yield (site + ("lock", name), lambda s: s[name] is None)
    s[name] = me


def unlock(s, name, site):
    yield (site + ("unlock", name), None)
    s[name] = None


def give_back(s, n, site):  # sub_82670F78
    while True:
        yield (site + ("f78.read", n), None)
        c = s["count"]
        if c >= 0:
            break
        new = min(c + n, 0)
        yield (site + ("f78.cas", n, c, new), None)
        if s["count"] == c:
            s["count"] = new
            n = n + c - new
            break
    if n > 0:
        yield (site + ("f78.pending", n), None)
        s["pending"] += n


def sem_wait(s, zero_timeout, site):  # sub_82671168, returns 0 or -2
    yield (site + ("sw.dec",), None)
    s["count"] -= 1
    c = s["count"]
    if c < 0:
        if zero_timeout:
            yield (site + ("sw.kwait0",), None)
            got = s["kernel"] > 0
            if got:
                s["kernel"] -= 1
            else:
                yield from give_back(s, 1, site + ("sw.to",))
                return -2
        else:
            yield (site + ("sw.kwait",), lambda s: s["kernel"] > 0)
            s["kernel"] -= 1
    return 0


def sem_post(s, site):  # sub_82671280, n = 1
    yield (site + ("sp.pend",), None)
    if s["pending"] > 0:
        yield (site + ("sp.count",), None)
        if s["count"] < 0:
            yield (site + ("sp.swap",), None)
            p, s["pending"] = s["pending"], 0
            if p > 0:
                yield from give_back(s, p, site + ("sp.gb",))
    yield (site + ("sp.add",), None)
    old = s["count"]
    s["count"] = old + 1
    if old < 0:
        yield (site + ("sp.release",), None)
        s["kernel"] += 1


def cv_wait(s, me, zero_timeout, site):  # sub_82670A78
    if FIX == "gate":
        yield (site + ("w.gate0",), lambda s: s["gate"] > 0)
        s["gate"] -= 1
        yield (site + ("w.gate1",), None)
        s["blocked"] += 1
        yield (site + ("w.gate2",), None)
        s["gate"] += 1
        yield from cv_wait_blocked(s, me, zero_timeout, site)
        return (yield from cv_wait_rest(s, me, zero_timeout, site))
    yield (site + ("w.inc",), None)
    s["blocked"] += 1
    yield from cv_wait_blocked(s, me, zero_timeout, site)
    return (yield from cv_wait_rest(s, me, zero_timeout, site))


def cv_wait_blocked(s, me, zero_timeout, site):
    yield from unlock(s, "M", site + ("w",))


def cv_wait_rest(s, me, zero_timeout, site):
    r = yield from sem_wait(s, zero_timeout, site + ("w",))
    yield from lock(s, "L", me, site + ("w1", r))
    yield (site + ("w.tu", r), None)
    tu = s["unblock"]
    yield (site + ("w.upd", r, tu), None)
    if tu:
        s["unblock"] = tu - 1
    else:
        s["gone"] += 1
    yield from unlock(s, "L", site + ("w2", r, tu))
    if tu == 1:
        yield (site + ("w.gate", r), None)
        s["gate"] += 1
    yield from lock(s, "M", me, site + ("w3", r))
    return r


def cv_signal(s, me, site):  # sub_82670BC0, not broadcast
    yield from lock(s, "L", me, site + ("s",))
    yield (site + ("s.tu",), None)
    tu = s["unblock"]
    post = False
    if tu:
        yield (site + ("s.b1", tu), None)
        if s["blocked"]:
            s["unblock"] = tu + 1
            yield (site + ("s.dec1",), None)
            s["blocked"] -= 1
            post = True
    else:
        yield (site + ("s.b0",), None)
        b = s["blocked"]
        yield (site + ("s.g", b), None)
        if b > s["gone"]:
            yield (site + ("s.gate",), lambda s: s["gate"] > 0)
            s["gate"] -= 1
            yield (site + ("s.gone",), None)
            g = s["gone"]
            if g:
                yield (site + ("s.sub", g), None)
                s["blocked"] -= g
                s["gone"] = 0
            s["unblock"] = 1
            yield (site + ("s.dec0",), None)
            s["blocked"] -= 1
            post = True
    yield from unlock(s, "L", site + ("s2", post))
    if post:
        yield from sem_post(s, site + ("s3",))


def driver(s):
    yield from lock(s, "M", "D", ("d0",))
    yield (("d.set",), None)
    s["state"] = 1
    yield from cv_signal(s, "D", ("d1",))
    while True:
        yield (("d.check",), None)
        if s["state"] == 2:
            break
        yield from cv_wait(s, "D", False, ("d2",))
    yield from unlock(s, "M", ("d3",))
    s["ddone"] = 1


def update(s, ticks):
    done = False
    for t in range(ticks):
        tk = ("u", t, done)
        ok = False
        if FIX == "poll":
            yield (tk + ("poll",), None)
            ok = s["state"] == 1
        else:
            yield from lock(s, "M", "U", tk + ("0",))
            while True:
                yield (tk + ("check",), None)
                if s["state"] == 1:
                    ok = True
                    break
                r = yield from cv_wait(s, "U", True, tk + ("w",))
                yield (tk + ("check2", r), None)
                if s["state"] == 1:
                    ok = True
                    break
                if r != 0:
                    break
            yield from unlock(s, "M", tk + ("1", ok))
        if ok:
            yield from lock(s, "M", "U", tk + ("2",))
            yield (tk + ("set",), None)
            s["state"] = 2
            yield from cv_signal(s, "U", tk + ("3",))
            yield from unlock(s, "M", tk + ("4",))
            done = True
    s["udone"] = 1


class Run:
    """Replays a schedule, since generators can't be copied."""

    def __init__(self, ticks, sched=()):
        self.s = dict(INIT)
        self.g = [driver(self.s), update(self.s, ticks)]
        self.at = [None, None]
        for i in (0, 1):
            self.adv(i)
        for t in sched:
            self.adv(t)

    def adv(self, i):
        try:
            self.at[i] = next(self.g[i])
        except StopIteration:
            self.at[i] = ("done", None)

    def enabled(self):
        return [i for i in (0, 1) if self.at[i][0] != "done" and (self.at[i][1] is None or self.at[i][1](self.s))]

    def key(self):
        return (tuple(sorted(self.s.items(), key=lambda kv: kv[0])), self.at[0][0], self.at[1][0])


def explore(ticks):
    seen, stack, bad, ends, n = set(), [()], [], {}, 0
    while stack:
        sched = stack.pop()
        r = Run(ticks, sched)
        k = repr(r.key())
        if k in seen:
            continue
        seen.add(k)
        n += 1
        en = r.enabled()
        if not en:
            e = "ok" if r.s["ddone"] else ("LOST" if r.s["state"] == 2 else "no-update-in-time")
            ends[e] = ends.get(e, 0) + 1
            if e == "LOST" and len(bad) < 3:
                bad.append((sched, dict(r.s), r.at[0][0], r.at[1][0]))
        for t in en:
            stack.append(sched + (t,))
    return n, ends, bad


if __name__ == "__main__":
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    n, ends, bad = explore(ticks)
    print(f"ticks={ticks} states={n} ends={ends}")
    for sched, s, a, b in bad:
        print("LOST", "".join("DU"[t] for t in sched), s, a, b)
