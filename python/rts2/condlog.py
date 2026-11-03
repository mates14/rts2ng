#!/usr/bin/env python3
"""condlog - the night's observing conditions for the monitor pages, from COND snapshots.

Polls the cond.json snapshots of this telescope's COND, the other
telescope's and Makak's, logs every new frame to <out>/log/YYYYMMDD.jsonl
(night of UT - 12 h) and keeps <out>/conditions.json with the last --hours
of best-effort

    sky_r, sky_i   sky at the zenith [mag/"2]: the sky model there plus the
                   departure from the model of the best measured sky
                   (snrexp.CondReader.departure: own filter, its donors, Makak)
    k_r            extinction in r [mag/airmass] from the freshest frame of
                   D50, else Makak, else SBT (snrexp.extinction)

each as points [unix time, value, source], the source being a camera/filter,
'MK/N' (Makak) or 'model' (no measured sky), and the dew warning of each
telescope's COND ({on, src, written}, from its snapshot).  The monitor pages
draw them under the cloud sensor and next to "Rain?".

    condlog.py --telescope D50 --out ~/public_html/cond
"""

import argparse
import glob
import json
import math
import os
import sys
import tempfile
import time
import urllib.request
from datetime import datetime, timezone

# snrexp from this checkout (the installed rts2 package may carry an older one)
_snrexp = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'snrexp.py')
if os.path.exists(_snrexp):
    import importlib.util
    _spec = importlib.util.spec_from_file_location('snrexp', _snrexp)
    snrexp = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(snrexp)
else:
    from rts2 import snrexp

SNAPSHOTS = {
    'D50': ['~/public_html/cond/cond.json', 'https://lascaux.asu.cas.cz/m/cond/cond.json',
            'https://lascaux.asu.cas.cz/m/makak/cond.json'],
    'SBT': ['~/public_html/cond/cond.json', 'https://d50.asu.cas.cz/m/cond/cond.json',
            'https://lascaux.asu.cas.cz/m/makak/cond.json'],
}
SKY_REF = {'D50': {'sky_r': 'C0/Sloan_r', 'sky_i': 'C0/Sloan_i'},
           'SBT': {'sky_r': 'C2/Sloan_r', 'sky_i': 'C1/Sloan_i'}}
EXT_ORDER = ('C0', 'MK', 'C1', 'C2')       # D50 is calibrated, Makak on its scale, SBT approximate
FIELDS = ('zp1', 'sky', 'bgv', 'fwhm', 'X', 'alt', 'az', 'tr')
FRESH = 900.0                              # seconds a frame counts for k_r
SUN_MAX = -6.0                             # no sky above this solar altitude


def night_of(t):
    return datetime.fromtimestamp(t - 43200, tz=timezone.utc).strftime('%Y%m%d')


def fetch(src):
    """A COND snapshot: {host, written, sources, dew (if it checks for dew)}."""
    try:
        if src.startswith('http'):
            with urllib.request.urlopen(src, timeout=10) as r:
                return json.loads(r.read().decode())
        with open(os.path.expanduser(src)) as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        print(f'condlog: {src}: {e}', file=sys.stderr)
        return {}


def telescope_of(sources):
    """Whose snapshot it is, from its cameras."""
    cams = {k.partition('/')[0] for k in sources}
    return 'D50' if 'C0' in cams else ('SBT' if cams & {'C1', 'C2', 'C3'} else ('Makak' if 'MK' in cams else None))


def write_json(path, data):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', dir=d, delete=False, suffix='.tmp') as f:
        json.dump(data, f, separators=(',', ':'))
    os.chmod(f.name, 0o644)
    os.replace(f.name, path)


class CondLog:
    def __init__(self, telescope, snapshots, out, hours):
        self.telescope, self.snapshots, self.out, self.hours = telescope, snapshots, out, hours
        self.last = {}                     # source -> tend of the last logged frame
        self.series = {'sky_r': [], 'sky_i': [], 'k_r': []}
        self.kr_used = 0.0
        self.dew = None
        self.load()

    def load(self):
        """The series back from the logs of the last nights."""
        since = time.time() - self.hours * 3600
        for path in sorted(glob.glob(os.path.join(self.out, 'log', '*.jsonl')))[-4:]:
            with open(path) as f:
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    if 'p' in r and r['t'] >= since:
                        for k, v in r['p'].items():
                            self.series[k].append([r['t'], v[0], v[1]])
                    elif 'f' in r:
                        self.last[r['f']] = max(self.last.get(r['f'], 0), r['t'])
        self.kr_used = max((p[0] for p in self.series['k_r']), default=0.0)

    def log(self, rec):
        path = os.path.join(self.out, 'log', night_of(rec['t']) + '.jsonl')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'a') as f:
            f.write(json.dumps(rec, separators=(',', ':')) + '\n')

    def points(self, sources, now):
        """Best-effort sky_r, sky_i and k_r now: {name: (t, value, source)}."""
        vals = {}
        for key, s in sources.items():
            cam, _, filt = key.partition('/')
            p = snrexp.cond_prefix(cam, filt)
            for f in FIELDS:
                vals[f'{p}_{f}'] = s.get(f)
            vals[f'{p}_age'] = now - s['tend']
        reader = snrexp.CondReader(self.telescope, lambda n: None if vals.get(n) is None else str(vals[n]), now=now)
        out = {}
        zenith = snrexp.sky_geometry(90.0, 0.0, now)
        if zenith['sunalt'] < SUN_MAX:
            for name, key in SKY_REF[self.telescope].items():
                cam, _, filt = key.partition('/')
                dep, src = reader.departure(cam, filt)
                src = 'MK/N' if src.startswith('Makak') else ('model' if src.startswith('no ') else src.split()[0])
                k = snrexp.TELESCOPES[self.telescope][cam]['filters'][filt][1]
                out[name] = (now, round(snrexp.sky_model_sb(key, zenith, k) + dep, 3), src)
        for cam in EXT_ORDER:
            cand = [(s['tend'], key) for key, s in sources.items() if key.startswith(cam + '/')
                    and now - s['tend'] < FRESH and s.get('zp1') is not None]
            if not cand:
                continue
            t, key = max(cand)
            k = snrexp.extinction(key, sources[key]['zp1'], sources[key].get('X'))
            if k is not None and math.isfinite(k):
                if t > self.kr_used:
                    out['k_r'] = (t, round(k, 3), key)
                    self.kr_used = t
                break
        return out

    def step(self):
        now = time.time()
        sources, dew = {}, {}
        for src in self.snapshots:
            snap = fetch(src)
            tel = telescope_of(snap.get('sources', {}))
            if tel and 'dew' in snap:
                dew[tel] = dict(snap['dew'], written=round(snap.get('written', 0), 1))
            for key, s in snap.get('sources', {}).items():
                if s.get('tend') and (key not in sources or s['tend'] > sources[key]['tend']):
                    sources[key] = s
        new = False
        for key, s in sorted(sources.items(), key=lambda x: x[1]['tend']):
            if s['tend'] > self.last.get(key, 0):
                self.log(dict(t=round(s['tend'], 1), f=key, **{f: s.get(f) for f in FIELDS}))
                self.last[key] = s['tend']
                new = True
        if new:
            pts = self.points(sources, now)
            if pts:
                self.log(dict(t=round(now, 1), p={k: [v[1], v[2]] for k, v in pts.items()}))
                for k, (t, v, src) in pts.items():
                    self.series[k].append([round(t, 1), v, src])
            since = now - self.hours * 3600
            for k in self.series:
                self.series[k] = [p for p in self.series[k] if p[0] >= since]
        if new or dew != self.dew:
            self.dew = dew
            write_json(os.path.join(self.out, 'conditions.json'),
                       dict(telescope=self.telescope, generated=round(now, 1), hours=self.hours,
                            last_frame=round(max(self.last.values(), default=0), 1), dew=dew,
                            series=self.series))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--telescope', required=True, choices=sorted(SKY_REF))
    ap.add_argument('--snapshot', action='append', help='cond.json path or URL (repeatable; default per telescope)')
    ap.add_argument('--out', default='~/public_html/cond', help='where conditions.json and log/ go')
    ap.add_argument('--hours', type=float, default=72.0, help='hours kept in conditions.json (72)')
    ap.add_argument('--interval', type=float, default=30.0, help='poll interval [s] (30)')
    ap.add_argument('--once', action='store_true', help='one poll, then exit')
    a = ap.parse_args()
    c = CondLog(a.telescope, a.snapshot or SNAPSHOTS[a.telescope], os.path.expanduser(a.out), a.hours)
    while True:
        try:
            c.step()
        except Exception as e:          # keep logging through a bad snapshot or a full disk
            print(f'condlog: {type(e).__name__}: {e}', file=sys.stderr)
        if a.once:
            break
        time.sleep(a.interval)


if __name__ == '__main__':
    main()
