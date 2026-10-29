"""Exposure times for a requested SNR, from the current observing conditions.

The noise model is the photometric pipeline's own (it reproduces its
MAGERR_AUTO to ~1 % on D50 and SBT, phdb 2026-05..10):

    sigma_F^2 = F/GAIN + AREA * (floor + s t)        [ADU^2]
    AREA = pi (FWHM^2 + 1.5^2),  F = t 10^(-0.4 (mag - zp1))

so SNR(t) is a quadratic in t with a closed-form solution.  The conditions
(zp1 = zeropoint for 1 s, s = sky variance rate, FWHM) come from the COND
device, which publishes them per camera/filter from the newest pipeline
ECSVs.  A filter without a fresh frame gets its zeropoint from the freshest
transparency and its sky from another filter's, shifted by a typical sky
colour.

Each time is also capped so that the image does not saturate: the sky at
the end of the exposure stays below SKY_MAX_FRAC of the saturation level,
and the target's peak pixel plus sky below PEAK_MAX_FRAC of it.  In a
rising twilight the sky is extrapolated from the frame it was measured on
to the end of the planned exposure (~1 mag per degree of solar altitude).

The target magnitude comes from the first of: the local ATLAS RefCat2
(`atlas` command, g r i z), lcgen on hog (the latest photometry), a
fallback given by the caller.

Pure python (math, json, subprocess, urllib) so that it runs in any RTS2
exe script.
"""

import fcntl
import json
import math
import os
import subprocess
import time
import urllib.request
from base64 import b64encode

PSF_AP2 = 1.5**2            # pipeline aperture: APERTURE^2 = FWHM^2 + 1.5^2
MAGERR_K = 2.5 / math.log(10)
SKY_MAX_FRAC = 0.5          # sky at most this fraction of the saturation level
PEAK_MAX_FRAC = 0.8         # target peak + sky at most this fraction
TWILIGHT_SLOPE = 0.42       # d log10(sky) / d(solar altitude) [1/deg], sky fit 2026-10
SITE_LAT = 49.91            # Ondrejov, deg

# Per telescope and camera: gain [e-/ADU, header GAIN, what the pipeline's
# errors use], floor = BGSIGMA^2 at zero sky [ADU^2], bgk = d(BGSIGMA^2)/d(sky
# ADU) [ADU], pix ["/px], sat = saturation [ADU above bias; 16-bit ADCs, single
# frames in phdb stay below ~50000]; per filter (zp1 at airmass 1 on a clear night,
# extinction k, sky colour = SB - SB of SBT C2/Sloan_r).  The same numbers
# as the COND configurations.
TELESCOPES = {
    'SBT': {
        'C1': dict(gain=1.6, floor=76.0, bgk=0.700, pix=3.12, sat=50000, filters={
            'Sloan_r': (18.38, 0.16, -0.16), 'Sloan_i': (17.53, 0.12, -0.17),
            'N': (19.75, 0.34, 0.12), 'halpha': (15.54, 0.10, 1.04), 'sii': (15.66, 0.10, 1.45)}),
        'C2': dict(gain=1.6, floor=52.0, bgk=0.695, pix=3.12, sat=50000, filters={
            'Sloan_g': (18.60, 0.28, 0.29), 'Sloan_r': (18.36, 0.18, 0.0),
            'N': (19.68, 0.20, 0.25), 'oiii': (15.42, 0.25, 0.79),
            'B': (17.18, 0.30, 1.0)}),     # C2/B: one night (2026-10-26), k and colour guessed
        'C3': dict(gain=1.5, floor=42.4, bgk=0.667, pix=3.83, sat=50000, filters={
            'B': (14.95, 0.37, 1.44), 'I': (14.82, 0.06, 0.18), 'N': (17.22, 0.13, 0.91)}),
    },
    'D50': {
        'C0': dict(gain=0.81, floor=80.8, bgk=1.354, pix=1.188, sat=50000, filters={
            'Sloan_g': (22.82, 0.29, 1.14), 'Sloan_r': (22.77, 0.20, 0.74),
            'Sloan_i': (22.15, 0.13, 0.84), 'Sloan_z': (21.11, 0.16, 0.20),
            'N': (23.88, 0.26, 0.41)}),
    },
}
FILTER_LONG = {'g': 'Sloan_g', 'r': 'Sloan_r', 'i': 'Sloan_i', 'z': 'Sloan_z'}
FILTER_SHORT = {v: k for k, v in FILTER_LONG.items()}

# magnitudes to try, in order, for a filter (catalogue and lcgen names; no colour terms)
MAG_FALLBACK = {
    'Sloan_g': ['Sloan_g', 'V', 'B', 'Sloan_r', 'R'],
    'Sloan_r': ['Sloan_r', 'R', 'V', 'N', 'Sloan_i', 'Sloan_g'],
    'Sloan_i': ['Sloan_i', 'I', 'Sloan_r', 'R', 'N'],
    'Sloan_z': ['Sloan_z', 'Sloan_i', 'I'],
    'B': ['B', 'Sloan_g', 'V'], 'I': ['I', 'Sloan_i', 'R'], 'N': ['N', 'Sloan_r', 'R', 'V'],
    'halpha': ['Sloan_r', 'R', 'N'], 'sii': ['Sloan_r', 'R', 'N'], 'oiii': ['Sloan_g', 'V', 'N'],
}

TIME_GRID = (1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 450, 600, 900)
DEFAULT_SKY = 19.5          # mag/"2, dark sky at Ondrejov (r)
DEFAULT_SEEING = 5.0        # arcsec


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
        if t - 1e-6 <= g <= tmax:
            return float(g)
    return float(tmax)


def round_down(t, tmin=TIME_GRID[0]):
    """Largest value of TIME_GRID at or below t (at least TIME_GRID[0])."""
    best = TIME_GRID[0]
    for g in TIME_GRID:
        if g <= t + 1e-6:
            best = g
    return float(best)


def twilight(comm, lat=SITE_LAT):
    """(sun altitude [deg], its rate [deg/s]) from centrald, or (None, 0).

    dh/dt = 15 deg/h cos(lat) sin(A), A the solar azimuth from the north
    towards the east; RTS2 azimuths are south-based (A = sun_az + 180).
    """
    try:
        alt = comm.getValueFloat('sun_alt', 'centrald')
        az = comm.getValueFloat('sun_az', 'centrald')
    except Exception:
        return None, 0.0
    if alt is None or az is None:
        return None, 0.0
    rate = 15.0 / 3600 * math.cos(math.radians(lat)) * math.sin(math.radians(az + 180))
    return alt, rate


class Conditions:
    """Conditions for one camera/filter at the target, and where they came from."""

    def __init__(self, cam, zp1, s, fwhm, source, age=0.0):
        self.cam, self.zp1, self.s, self.fwhm, self.source = cam, zp1, s, fwhm, source
        self.age = age          # seconds since the frame the sky was measured on
        self.grow = 0.0         # sky growth [dex/s], set by set_twilight()

    def set_twilight(self, sun_alt, sun_rate):
        """Sky brightening with a rising Sun between -20 deg and the horizon."""
        if sun_alt is not None and -20 < sun_alt < 5 and sun_rate > 0:
            self.grow = TWILIGHT_SLOPE * sun_rate

    def sky_rate(self, t):
        """Sky [ADU/s/px] at the end of an exposure of t s starting now."""
        return self.s / self.cam['bgk'] * 10**(self.grow * (self.age + t))

    def max_time(self, mag=None):
        """(longest exposure that does not saturate, 'sky' or 'star' - whichever limits it)"""
        sat = self.cam.get('sat', 50000)
        sigma = self.fwhm / 2.3548
        peak = math.erf(0.5 / (math.sqrt(2) * sigma))**2      # flux fraction in the central pixel
        f = 10**(-0.4 * (mag - self.zp1)) if mag is not None else 0.0
        limits = [('sky', lambda t: self.sky_rate(t) * t - SKY_MAX_FRAC * sat)]
        if f > 0:
            limits.append(('star', lambda t: (f * peak + self.sky_rate(t)) * t - PEAK_MAX_FRAC * sat))
        best = (math.inf, None)
        for name, excess in limits:
            if excess(TIME_GRID[-1]) <= 0:
                continue
            lo, hi = 0.0, float(TIME_GRID[-1])
            for _ in range(60):
                mid = (lo + hi) / 2
                lo, hi = (mid, hi) if excess(mid) <= 0 else (lo, mid)
            if lo < best[0]:
                best = (lo, name)
        return best

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
    """COND values of one telescope through a getter, get(value_name) -> str or None."""

    def __init__(self, telescope, get, max_age=1800.0):
        self.cams = TELESCOPES[telescope]
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
        cam = self.cams[camera]
        zpref, k, skycol = cam['filters'].get(filt, (math.nan, 0.15, 0.0))
        own = self.fresh(camera, filt)
        if own and own['bgv'] is not None:
            fwhm = own['fwhm'] or DEFAULT_SEEING / cam['pix']
            Xf = own['X'] if own['X'] else X
            return Conditions(cam, own['zp1'] - k * (X - Xf), own['bgv'], fwhm,
                              f'own frame {own["age"]:.0f}s old', own['age'])
        # zeropoint: reference at this airmass + the freshest transparency
        tr = self.value('transp')
        src = []
        if tr is None or math.isnan(zpref):
            tr = 0.0
            src.append('clear-sky zp')
        else:
            src.append('transp')
        zp1 = zpref - k * (X - 1) + tr
        # sky and seeing: from another fresh filter of this telescope
        sky, seeing, age = None, None, 0.0
        for c2 in [camera] + [c for c in self.cams if c != camera]:
            for f2, (_, _, col2) in self.cams[c2]['filters'].items():
                o = self.fresh(c2, f2)
                if not o:
                    continue
                if sky is None and o['sky'] is not None:
                    sky = o['sky'] + (skycol - col2)
                    age = o['age']
                    src.append(f'sky from {c2}/{f2}')
                if seeing is None and o['fwhm']:
                    seeing = o['fwhm'] * self.cams[c2]['pix']
        if sky is None:
            sky = DEFAULT_SKY + skycol
            src.append('default sky')
        s = cam['bgk'] * cam['pix']**2 * 10**(-0.4 * (sky - zp1))
        return Conditions(cam, zp1, s, (seeing or DEFAULT_SEEING) / cam['pix'], ', '.join(src), age)


# ---------------------------------------------------------------------------
# target magnitudes
# ---------------------------------------------------------------------------
ATLAS_DIR = '/home/mates/cat/atlas'
ATLAS_SPLITS = ('00_m_16', '16_m_17', '17_m_18', '18_m_19', '19_m_20')
LCGEN_URL = 'https://hog.asu.cas.cz/lcgen/api/lc'
LCGEN_AUTH = ('/etc/rts2/lcgen.auth', os.path.expanduser('~/.lcgen.auth'))


def atlas_mags(ra, dec, radius=3.0, atlas_dir=ATLAS_DIR, timeout=20.0):
    """{Sloan_g/r/i/z: mag} of the ATLAS RefCat2 star nearest to ra, dec within radius ["]."""
    half = 2 * radius / 3600
    best = None
    for split in ATLAS_SPLITS:
        d = os.path.join(atlas_dir, split)
        if not os.path.isdir(d):
            continue
        r = subprocess.run(['atlas', f'{ra}', f'{dec}', '-rect', f'{half},{half}', '-dir', d,
                            '-mlim', '21', '-ecsv'], capture_output=True, text=True, timeout=timeout)
        lines = [l.split() for l in r.stdout.splitlines() if l and not l.startswith('#')]
        if len(lines) < 2:
            continue
        cols = lines[0]
        for row in lines[1:]:
            rec = dict(zip(cols, row))
            dra = (float(rec['radeg']) - ra) * math.cos(math.radians(dec))
            dist = 3600 * math.hypot(dra, float(rec['decdeg']) - dec)
            if dist <= radius and (best is None or dist < best[0]):
                best = (dist, {f: float(rec[f]) for f in ('Sloan_g', 'Sloan_r', 'Sloan_i', 'Sloan_z')
                               if f in rec and float(rec[f]) < 90})
    return best[1] if best else {}


def lcgen_mags(ra, dec, radius=5.0, timeout=15.0, auth_files=LCGEN_AUTH):
    """{filter: mag} - median of the newest night per filter in lcgen."""
    url = f'{LCGEN_URL}?ra={ra:.6f}&dec={dec:.6f}&r={radius}'
    req = urllib.request.Request(url, headers={'User-Agent': 'rts2-snrexp'})
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
        out[name] = night[len(night) // 2]
    return out


def parse_mag(spec):
    """'17.5' -> {'*': 17.5}; 'r=17.5,g=18.1' -> {'Sloan_r': 17.5, 'Sloan_g': 18.1}; None -> {}"""
    if spec is None or str(spec).strip() == '':
        return {}
    spec = str(spec).strip()
    if '=' not in spec:
        return {'*': float(spec)}
    return {long_filter(k.strip()): float(v) for k, v in (kv.split('=') for kv in spec.split(','))}


def target_mags(filters, ra=None, dec=None, mag=None, sources=('atlas', 'lcgen'),
                fallback=None, log=None):
    """{filter: (mag, origin)} for each of filters that can get one.

    mag (parse_mag output) wins; then each source in turn, each filter taking
    the first name of MAG_FALLBACK that the source has; then fallback.
    """
    log = log or (lambda level, text: None)
    out = {}
    given = dict(mag or {})
    for f in filters:
        if '*' in given:
            out[f] = (given['*'], 'given')
        else:
            for cand in MAG_FALLBACK.get(f, [f]):
                if cand in given:
                    out[f] = (given[cand], f'given {cand}')
                    break
    for src in sources:
        missing = [f for f in filters if f not in out]
        if not missing or ra is None:
            break
        try:
            have = atlas_mags(ra, dec) if src == 'atlas' else lcgen_mags(ra, dec)
        except Exception as e:
            log('W', f'{src} failed: {e}')
            continue
        log('I', f'{src}: ' + (', '.join(f'{k}={v:.2f}' for k, v in sorted(have.items())) or 'nothing'))
        for f in missing:
            for cand in MAG_FALLBACK.get(f, [f]):
                if cand in have:
                    out[f] = (have[cand], f'{src} {cand}')
                    break
    if fallback is not None:
        for f in filters:
            out.setdefault(f, (fallback, 'fallback'))
    return out


def target_position(comm):
    """(ra, dec) of the current target from T0.ORI, or (None, None)."""
    try:
        ori = (comm.getValue('ORI', 'T0') or '').split()
        return float(ori[0]), float(ori[1])
    except (ValueError, IndexError, TypeError):
        return None, None


def target_airmass(comm, default=1.5):
    try:
        X = comm.getValueFloat('AIRMASS', 'T0')
    except Exception:
        X = None
    return X if X and 1 <= X < 10 else default


# ---------------------------------------------------------------------------
# planning
# ---------------------------------------------------------------------------
PLAN_DIR = '/tmp'


def make_plan(groups, mags, snr, X, reader, tmin, tmax, overhead=0.0, fallback_t=None,
              sun=(None, 0.0)):
    """Exposure time of every camera/filter in groups ({camera: [filters]}) and their tick.

    mags: {filter: (mag, origin)}.  Each time is rounded up to TIME_GRID and
    limited to [tmin, tmax]; a filter without a magnitude gets fallback_t
    (or tmax).  Then it is cut (rounded down) to what does not saturate the
    sky or the target, with the sky extrapolated in a rising twilight; sun is
    twilight()'s output.  The tick is the longest one plus overhead.
    """
    exp, info = {}, {}
    for camera, filters in groups.items():
        for filt in filters:
            mag, origin = mags.get(filt, (None, None))
            c = reader.conditions(camera, filt, X)
            c.set_twilight(*sun)
            if mag is None:
                t, need = min(fallback_t or tmax, tmax), None
            else:
                need = c.exptime(mag, snr)
                t = round_up(need, tmin, tmax)
            tsat, limit = c.max_time(mag)
            cap = None
            if t > tsat:
                t, cap = round_down(tsat), limit
            exp[f'{camera}/{filt}'] = t
            info[f'{camera}/{filt}'] = dict(mag=mag, origin=origin, need=need, t=t, cap=cap,
                                           tsat=(None if math.isinf(tsat) else tsat),
                                           snr=(c.snr(mag, t) if mag is not None else None),
                                           cond=str(c))
    tick = (max(exp.values()) if exp else tmax) + overhead
    return dict(exptime=exp, tick=tick, info=info, snr=snr, airmass=X)


def describe(key, i):
    """One log line for a plan entry."""
    mag = 'no magnitude' if i['mag'] is None else f"{i['origin']} {i['mag']:.2f}"
    need = '' if i['need'] is None else f" (needs {i['need']:.0f}s)"
    snr = '' if i['snr'] is None else f" SNR {i['snr']:.0f}"
    cap = f" CUT to {i['t']:g}s: {i['cap']} would saturate at {i['tsat']:.0f}s" if i.get('cap') else ''
    return f"{key}: {mag}: {i['t']:g}s{need}{snr}{cap}, {i['cond']}"


def shared_plan(key, compute, ttl=900.0):
    """The plan for key: computed by the first caller, read by the others.

    All the cameras of a synchronised observation must use the same tick, so
    one script computes the exposure times of all of them and the others
    take its result rather than their own (COND may change in between).
    """
    path = os.path.join(PLAN_DIR, f'snrexp-plan-{key}.json')
    with open(os.path.join(PLAN_DIR, 'snrexp-plan.lock'), 'a') as lock:
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

