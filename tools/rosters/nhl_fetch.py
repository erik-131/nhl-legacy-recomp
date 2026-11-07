#!/usr/bin/env python3
"""Fetch NHL club rosters and season stats from the public (unofficial, undocumented) NHL web API into a cache directory.

  nhl_fetch.py [--cache DIR] [--refresh] [--max-age HOURS] [--teams ANA,BOS,...] [--delay SECONDS]

For the user to run on their own machine; nothing fetched is shipped in the repo (see docs/rosters-pipeline.md for the
terms-of-use situation). Default cache: ~/nhl-work/rosters-out/nhl-cache (outside git).

Per club three JSON files: roster_<T>.json (v1/roster/<T>/current), stats_cur_<T>.json (v1/club-stats/<T>/now) and
stats_prev_<T>.json (previous regular season, v1/club-stats/<T>/<prev>/2). manifest.json records the URL, the time and the
size of each file. Idempotent: a file already in the cache is not fetched again unless --refresh (or older than --max-age).
At most 2 requests per second (default delay 0.6 s), User-Agent names this tool.
"""
import json
import os
import sys
import time
import urllib.request
from datetime import datetime, timezone

BASE = 'https://api-web.nhle.com/v1'
UA = 'nhl-legacy-recomp-rosters/0.1 (personal offline modding tool; cached, <=2 req/s)'
DEFAULT_CACHE = os.path.expanduser('~/nhl-work/rosters-out/nhl-cache')
ALL_TEAMS = ['ANA', 'BOS', 'BUF', 'CAR', 'CBJ', 'CGY', 'CHI', 'COL', 'DAL', 'DET', 'EDM', 'FLA', 'LAK', 'MIN', 'MTL', 'NJD',
             'NSH', 'NYI', 'NYR', 'OTT', 'PHI', 'PIT', 'SEA', 'SJS', 'STL', 'TBL', 'TOR', 'UTA', 'VAN', 'VGK', 'WPG', 'WSH']


def now_iso():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


class Fetcher:
    def __init__(self, cache, delay=0.6, refresh=False, max_age=None, opener=None):
        self.cache, self.delay, self.refresh, self.max_age = cache, delay, refresh, max_age
        self.opener = opener or self._http  # tests inject a fake: opener(url) -> (final_url, bytes)
        self.last = 0.0
        os.makedirs(cache, exist_ok=True)
        self.mpath = os.path.join(cache, 'manifest.json')
        self.manifest = json.load(open(self.mpath)) if os.path.exists(self.mpath) else {}

    @staticmethod
    def _http(url):
        req = urllib.request.Request(url, headers={'User-Agent': UA})
        with urllib.request.urlopen(req, timeout=30) as r:  # follows the /current redirect
            return r.geturl(), r.read()

    def fresh(self, fname):
        p = os.path.join(self.cache, fname)
        if not os.path.exists(p) or self.refresh:
            return False
        if self.max_age is not None and time.time() - os.path.getmtime(p) > self.max_age * 3600:
            return False
        return True

    def get(self, fname, url):
        """Fetch url into fname unless cached. Returns (parsed json, fetched_now)."""
        p = os.path.join(self.cache, fname)
        if self.fresh(fname):
            return json.load(open(p)), False
        wait = self.delay - (time.time() - self.last)
        if wait > 0:
            time.sleep(wait)
        final, body = self.opener(url)
        self.last = time.time()
        data = json.loads(body)  # fail before writing anything on bad JSON
        tmp = p + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(body)
        os.replace(tmp, p)
        self.manifest[fname] = {'url': url, 'final_url': final, 'fetched': now_iso(), 'bytes': len(body)}
        self.save()
        return data, True

    def save(self):
        tmp = self.mpath + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.manifest, f, indent=1, sort_keys=True)
        os.replace(tmp, self.mpath)


def prev_season(season):
    """20262027 -> 20252026"""
    a = int(str(season)[:4])
    return f'{a - 1}{a}'


def fetch_all(cache, teams=None, delay=0.6, refresh=False, max_age=None, opener=None, log=print):
    f = Fetcher(cache, delay, refresh, max_age, opener)
    n = 0
    for t in teams or ALL_TEAMS:
        _, new = f.get(f'roster_{t}.json', f'{BASE}/roster/{t}/current')
        n += new
        cur, new = f.get(f'stats_cur_{t}.json', f'{BASE}/club-stats/{t}/now')
        n += new
        prev = prev_season(cur.get('season', ''))
        _, new = f.get(f'stats_prev_{t}.json', f'{BASE}/club-stats/{t}/{prev}/2')
        n += new
        log(f'{t}: season {cur.get("season")} (+{prev})')
    log(f'{n} requests made, manifest {f.mpath}')
    return f


def main(argv):
    a = list(argv)

    def opt(name, default=None, conv=str):
        if name in a:
            i = a.index(name)
            v = conv(a[i + 1])
            del a[i:i + 2]
            return v
        return default
    cache = opt('--cache', DEFAULT_CACHE)
    delay = opt('--delay', 0.6, float)
    max_age = opt('--max-age', None, float)
    teams = opt('--teams')
    refresh = '--refresh' in a
    if refresh:
        a.remove('--refresh')
    if a:
        print(__doc__)
        return 2
    fetch_all(cache, teams.split(',') if teams else None, max(delay, 0.5), refresh, max_age)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
