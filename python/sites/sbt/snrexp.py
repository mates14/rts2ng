"""Exposure times for a requested SNR on SBT, from the current conditions.

The noise model is the photometric pipeline's own (it reproduces its
MAGERR_AUTO to ~1 %, phdb 2026-05..10, ~/expcalc/sbt on the laptop):

    sigma_F^2 = F/GAIN + AREA * (floor + s t)        [ADU^2]
    AREA = pi (FWHM^2 + 1.5^2),  F = t 10^(-0.4 (mag - zp1))

so SNR(t) is a quadratic in t with a closed-form solution.  The conditions
(zp1 = zeropoint for 1 s, s = sky variance rate, FWHM) come from the COND
device, which publishes them per camera/filter from the newest pipeline
ECSVs.  A filter without a fresh frame gets its zeropoint from the freshest
transparency and its sky from another filter's, shifted by a typical sky
colour.  The magnitude can be given or taken from lcgen on hog.

Pure python (math, json, urllib) so that it runs in any RTS2 exe script.
"""

import fcntl
import json
import math
import os
import time
import urllib.request
from base64 import b64encode

PSF_AP2 = 1.5**2            # pipeline aperture: APERTURE^2 = FWHM^2 + 1.5^2
MAGERR_K = 2.5 / math.log(10)

# gain [e-/ADU, header GAIN, what the pipeline's errors use], floor = BGSIGMA^2
# at zero sky [ADU^2], bgk = d(BGSIGMA^2)/d(sky ADU) [ADU], pix ["/px];
# per filter zp1 at airmass 1 (clear night), extinction k, sky colour
# (SB - SB of C2/Sloan_r) - the same numbers as the COND configuration.
CAMERAS = {
    'C1': dict(gain=1.6, floor=76.0, bgk=0.700, pix=3.12, filters={
        'Sloan_r': (18.38, 0.16, -0.16), 'Sloan_i': (17.53, 0.12, -0.17),
        'N': (19.75, 0.34, 0.12), 'halpha': (15.54, 0.10, 1.04), 'sii': (15.66, 0.10, 1.45)}),
    'C2': dict(gain=1.6, floor=52.0, bgk=0.695, pix=3.12, filters={
        'Sloan_g': (18.60, 0.28, 0.29), 'Sloan_r': (18.36, 0.18, 0.0),
        'N': (19.68, 0.20, 0.25), 'oiii': (15.42, 0.25, 0.79),
        'B': (17.18, 0.30, 1.0)}),     # C2/B: one night (2026-10-26), k and colour guessed
    'C3': dict(gain=1.5, floor=42.4, bgk=0.667, pix=3.83, filters={
        'B': (14.95, 0.37, 1.44), 'I': (14.82, 0.06, 0.18), 'N': (17.22, 0.13, 0.91)}),
}
FILTER_LONG = {'g': 'Sloan_g', 'r': 'Sloan_r', 'i': 'Sloan_i', 'z': 'Sloan_z'}
FILTER_SHORT = {v: k for k, v in FILTER_LONG.items()}

# lcgen filters to try, in order, when the requested one has no data (no colour terms)
LC_FALLBACK = {
    'Sloan_g': ['Sloan_g', 'V', 'B', 'Sloan_r', 'R'],
    'Sloan_r': ['Sloan_r', 'R', 'V', 'N', 'Sloan_i', 'Sloan_g'],
    'Sloan_i': ['Sloan_i', 'I', 'Sloan_r', 'R', 'N'],
    'Sloan_z': ['Sloan_z', 'Sloan_i', 'I'],
    'B': ['B', 'Sloan_g', 'V'], 'I': ['I', 'Sloan_i', 'R'], 'N': ['N', 'Sloan_r', 'R', 'V'],
    'halpha': ['Sloan_r', 'R', 'N'], 'sii': ['Sloan_r', 'R', 'N'], 'oiii': ['Sloan_g', 'V', 'N'],
}

TIME_GRID = (1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 450, 600, 900)
DEFAULT_SKY = 19.5          # mag/"2, dark sky at Ondrejov (r)
DEFAULT_FWHM = 1.8          # px


def long_filter(f):
    return FILTER_LONG.get(f, f)


def cond_prefix(camera, filt):
    """COND value prefix: C1/Sloan_r -> C1_r"""
    return f'{camera}_{FILTER_SHORT.get(filt, filt)}'


def snr_of(mag, t, zp1, s, fwhm, cam):
    F = t * 10**(-0.4 * (mag - zp1))
    area = math.pi * (fwhm**2 + PSF_AP2)
    return F / math.sqrt(F / cam['gain'] + area * (cam['floor'] + s * t))


def exptime_for(mag, snr, zp1, s, fwhm, cam):
    """Closed-form exposure time [s] that reaches snr for a star of mag."""
    f = 10**(-0.4 * (mag - zp1))
    area = math.pi * (fwhm**2 + PSF_AP2)
    B = snr**2 * (f / cam['gain'] + area * s)
    return (B + math.sqrt(B**2 + 4 * f**2 * snr**2 * area * cam['floor'])) / (2 * f**2)


def round_up(t, tmin, tmax):
    """Next value of TIME_GRID at or above t, within [tmin, tmax]."""
    t = min(max(t, tmin), tmax)
    for g in TIME_GRID:
        if g >= t - 1e-6 and g <= tmax:
            return float(g)
    return float(tmax)


class Conditions:
    """Conditions for one camera/filter at the target, and where they came from."""

    def __init__(self, camera, filt, zp1, s, fwhm, source):
        self.camera, self.filter = camera, filt
        self.zp1, self.s, self.fwhm, self.source = zp1, s, fwhm, source
        self.cam = CAMERAS[camera]

    def exptime(self, mag, snr):
        return exptime_for(mag, snr, self.zp1, self.s, self.fwhm, self.cam)

    def snr(self, mag, t):
        return snr_of(mag, t, self.zp1, self.s, self.fwhm, self.cam)

    def __str__(self):
        return f'zp1={self.zp1:.2f} s={self.s:.2f} fwhm={self.fwhm:.2f} ({self.source})'


def _num(x):
    try:
        v = float(str(x).split()[0])
        return None if math.isnan(v) else v
    except (TypeError, ValueError, IndexError):
        return None


class CondReader:
    """COND values through an Rts2Comm-like getter, get(value_name) -> str or None."""

    def __init__(self, get, max_age=1800.0):
        self.get, self.max_age = get, max_age
        self.cache = {}

    def value(self, name):
        if name not in self.cache:
            try:
                self.cache[name] = _num(self.get(name))
            except Exception:
                self.cache[name] = None
        return self.cache[name]

    def fresh(self, camera, filt):
        """dict(zp1, bgv, fwhm, X, sky, age) if COND has a fresh frame of camera/filter."""
        p = cond_prefix(camera, filt)
        age = self.value(f'{p}_age')
        if age is None or age > self.max_age:
            return None
        d = {f: self.value(f'{p}_{f}') for f in ('zp1', 'bgv', 'fwhm', 'X', 'sky')}
        if d['zp1'] is None:
            return None
        d['age'] = age
        return d

    def conditions(self, camera, filt, X):
        """Conditions of camera/filter at airmass X, with fallbacks."""
        cam = CAMERAS[camera]
        zpref, k, skycol = cam['filters'].get(filt, (math.nan, 0.15, 0.0))
        own = self.fresh(camera, filt)
        if own and own['bgv'] is not None:
            fwhm = own['fwhm'] or DEFAULT_FWHM
            Xf = own['X'] if own['X'] else X
            return Conditions(camera, filt, own['zp1'] - k * (X - Xf), own['bgv'], fwhm,
                              f'own frame {own["age"]:.0f}s old')
        # zeropoint: reference at this airmass + the freshest transparency
        tr = self.value('transp')
        src = []
        if tr is None or math.isnan(zpref):
            tr = 0.0
            src.append('clear-sky zp')
        else:
            src.append('transp')
        zp1 = zpref - k * (X - 1) + tr
        # sky and FWHM: from another fresh filter of SBT (same pixel scale for C1/C2)
        sky, fwhm = None, None
        for c2 in (camera, 'C1', 'C2', 'C3'):
            for f2, (_, _, col2) in CAMERAS[c2]['filters'].items():
                o = self.fresh(c2, f2)
                if not o:
                    continue
                if sky is None and o['sky'] is not None:
                    sky = o['sky'] + (skycol - col2)
                    src.append(f'sky from {c2}/{f2}')
                if fwhm is None and o['fwhm'] and c2 != 'C3':
                    fwhm = o['fwhm'] * CAMERAS[c2]['pix'] / cam['pix']
        if sky is None:
            sky = DEFAULT_SKY + skycol
            src.append('default sky')
        s = cam['bgk'] * cam['pix']**2 * 10**(-0.4 * (sky - zp1))
        return Conditions(camera, filt, zp1, s, fwhm or DEFAULT_FWHM, ', '.join(src))


# ---------------------------------------------------------------------------
# magnitudes from lcgen
# ---------------------------------------------------------------------------
LCGEN_URL = 'https://hog.asu.cas.cz/lcgen/api/lc'
LCGEN_AUTH = ('/etc/rts2/lcgen.auth', os.path.expanduser('~/.lcgen.auth'))


def lcgen_latest(ra, dec, radius=5.0, timeout=15.0, auth_files=LCGEN_AUTH):
    """{filter: (mag, jd, n)} - median of the newest night per filter."""
    url = f'{LCGEN_URL}?ra={ra:.6f}&dec={dec:.6f}&r={radius}'
    req = urllib.request.Request(url, headers={'User-Agent': 'rts2-filtros'})
    for f in auth_files:
        try:
            with open(f) as fh:
                cred = fh.read().strip()
            req.add_header('Authorization', 'Basic ' + b64encode(cred.encode()).decode())
            break
        except OSError:
            continue
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    names = d.get('filters', [])
    per = {}
    for jd, fi, mag in zip(d.get('jd', []), d.get('filter', []), d.get('mag', [])):
        name = names[fi] if isinstance(fi, int) and fi < len(names) else str(fi)
        per.setdefault(name, []).append((jd, mag))
    out = {}
    for name, pts in per.items():
        last = max(j for j, _ in pts)
        night = sorted(m for j, m in pts if last - j < 0.5)
        out[name] = (night[len(night) // 2], last, len(night))
    return out


def magnitude_for(filt, mags):
    """(mag, lcgen filter used) for filt from lcgen_latest() output, or (None, None)."""
    for cand in LC_FALLBACK.get(filt, [filt]):
        if cand in mags:
            return mags[cand][0], cand
    return None, None


# ---------------------------------------------------------------------------
# a plan shared by the scripts of one observation
# ---------------------------------------------------------------------------
PLAN_DIR = '/tmp'


def shared_plan(key, compute, ttl=900.0):
    """The plan for key: computed by the first caller, read by the others.

    All the cameras of a synchronised observation must use the same tick, so
    one script computes the exposure times of all of them and the others
    take its result rather than their own (COND may change in between).
    """
    path = os.path.join(PLAN_DIR, f'filtros-plan-{key}.json')
    with open(os.path.join(PLAN_DIR, 'filtros-plan.lock'), 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            with open(path) as f:
                plan = json.load(f)
            if time.time() - plan.get('created', 0) < ttl:
                plan['shared'] = True
                return plan
        except (OSError, ValueError):
            pass
        plan = compute()
        plan['created'] = time.time()
        tmp = path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(plan, f, indent=1)
        os.replace(tmp, path)
        plan['shared'] = False
        return plan


def make_plan(groups, mags, snr, X, reader, tmin, tmax, overhead):
    """Exposure time of every camera/filter in groups ({camera: [filters]}) and the common tick.

    mags: {filter: mag}.  Each time is rounded up to TIME_GRID and limited to
    [tmin, tmax]; the tick is the longest one plus the overhead.
    """
    exp, info = {}, {}
    for camera, filters in groups.items():
        for filt in filters:
            mag = mags.get(filt)
            c = reader.conditions(camera, filt, X)
            if mag is None:
                t, need = tmax, math.nan
            else:
                need = c.exptime(mag, snr)
                t = round_up(need, tmin, tmax)
            exp[f'{camera}/{filt}'] = t
            info[f'{camera}/{filt}'] = dict(
                mag=mag, need=need, t=t, snr=(c.snr(mag, t) if mag is not None else None), cond=str(c))
    tick = max(exp.values()) + overhead if exp else tmax + overhead
    return dict(exptime=exp, tick=tick, info=info, snr=snr, airmass=X)
