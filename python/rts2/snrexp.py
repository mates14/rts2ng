"""Exposure times for a requested SNR, from the current observing conditions.

The noise model is the photometric pipeline's own (it reproduces its
MAGERR_AUTO to ~1 % on D50 and SBT, phdb 2026-05..10):

    sigma_F^2 = F/GAIN + AREA * (floor + s t)        [ADU^2]
    AREA = pi (FWHM^2 + 1.5^2),  F = t 10^(-0.4 (mag - zp1))

so SNR(t) is a quadratic in t with a closed-form solution.  The conditions
(zp1 = zeropoint for 1 s, s = sky variance rate, FWHM) come from the COND
device, which publishes them per camera/filter from the newest pipeline
ECSVs.  A filter without a fresh frame gets its zeropoint from the freshest
transparency.  The sky at the target is a sky model (Sun, Moon, light
pollution, airglow) plus the departure from it of the freshest measured sky:
the filter's own frame, another filter of the telescope, or the Makak sky
monitor; filters without a model take another filter's sky, shifted by a
typical sky colour.

Each time is also capped so that the image does not saturate: the sky at
the end of the exposure stays below SKY_MAX_FRAC of the saturation level,
and the target's peak pixel plus sky below PEAK_MAX_FRAC of it.  A camera
that can accumulate (D50's Andor: ACQMODE=2, ACCNUM sub-exposures summed
on the chip with ~no readout time, saved as 32 bits) instead splits such an
exposure into N sub-exposures that each stay below the limits, N being the
smallest that reaches the SNR with N read-noise floors.  In a
rising twilight the sky is extrapolated from the frame it was measured on
to the end of the planned exposure (~1 mag per degree of solar altitude).

The target magnitude comes from lcgen on hog (our own photometry: median of
the nightly medians of the newest few nights, so a regularly observed variable
target follows its level) or the local ATLAS RefCat2 (`atlas` command, g r i z),
the filter's own band from either before a substitute band; else a fallback
given by the caller.

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
        'C0': dict(gain=0.81, floor=80.8, bgk=1.354, pix=1.188, sat=50000, accumulate=True, filters={
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
SUB_GRID = (0.1, 0.2, 0.3, 0.5) + TIME_GRID     # sub-exposures of an accumulation
MAX_ACC = 1000
DEFAULT_SKY = 19.5          # mag/"2, dark sky at Ondrejov (r)
DEFAULT_SEEING = 5.0        # arcsec
SITE_LON = 14.7836          # Ondrejov, deg east

# Sky model per camera/filter (and Makak), clear-equivalent SB [mag/"2], fitted
# on phdb 2026-05..10 (Makak on its noise sky, floor from the chip temperature):
# flux relative to SB=20 = dark + light pollution + moon (Krisciunas & Schaefer
# 1991) + twilight, see sky_model_sb().  Alone it is off by a per-night constant
# (0.1-0.25 mag), so it is used differentially: the sky at the target is the
# model there plus the departure from the model of a measured sky: the own
# filter's frame, else the first fresh one of SKY_DONORS.
SKY_PARAMS = ('dark', 'lp', 'lp_Xpow', 'lp_dir', 'lp_az', 'moon', 'mie', 'twi', 'twi_slope', 'twi_dir')
SKY_MODEL = {
    'C1/Sloan_r': (0.09349, -1.619, 3.277, 2.422, 295.2, 1.565, 1.499, 5.707, 0.444, 1.298),
    'C1/Sloan_i': (0.1891, -2.256, 3.136, 3, 287.1, 1.616, 1.677, 6.177, 0.4483, 0.7365),
    'C2/Sloan_g': (-0.557, -0.6395, 2.247, 0.4627, 294.8, 1.653, 1.058, 5.819, 0.4209, 0.5078),
    'C2/Sloan_r': (-0.06952, -0.6918, 2.732, 0.6983, 294.5, 1.543, 1.458, 5.121, 0.4044, 1.708),
    'C0/Sloan_g': (-0.4649, -1.927, 4.057, 1.82, 297.6, 1.483, 0.4165, 3.75, 0.3004, 3),
    'C0/Sloan_r': (-0.2277, -1.052, 3.577, 0.831, 328.7, 1.407, 0.8296, 3.637, 0.3119, 3),
    'C0/Sloan_i': (-0.07552, -1.954, 4.007, 1.56, 303.1, 1.333, 1.203, 5.175, 0.4292, 3),
    'C0/Sloan_z': (-5, 0.3327, 1.318, 0.06496, 314, 1.24, 1.045, 5.866, 0.4709, 3),
    'MK/N': (-4.9, -0.3031, 5, 3, 269.1, 1.491, 0.8988, 4.997, 0.3882, 0.8189),
}
# Whose departure a filter without a fresh frame takes, best first (backtest on
# phdb 2026-05..10, mad of the predicted sky): the same band on the other camera
# (0.10-0.12), the same colour family (g/r or i/z, 0.16-0.28), Makak (g/r 0.16-0.25,
# i 0.26-0.30).  Across the families the sky does not transfer (D50 r from z 0.49,
# worse than the model alone at 0.27-0.41), so those are left out.
SKY_DONORS = {
    'C1/Sloan_r': ('C2/Sloan_r', 'C1/Sloan_i', 'MK/N', 'C2/Sloan_g'),
    'C1/Sloan_i': ('C1/Sloan_r', 'C2/Sloan_r', 'MK/N', 'C2/Sloan_g'),
    'C2/Sloan_g': ('C2/Sloan_r', 'MK/N', 'C1/Sloan_r'),
    'C2/Sloan_r': ('C1/Sloan_r', 'C1/Sloan_i', 'C2/Sloan_g', 'MK/N'),
    'C0/Sloan_g': ('C0/Sloan_r', 'MK/N'),
    'C0/Sloan_r': ('C0/Sloan_g', 'MK/N'),
    'C0/Sloan_i': ('MK/N', 'C0/Sloan_r', 'C0/Sloan_z'),
    'C0/Sloan_z': ('C0/Sloan_i',),
}
# Makak (MK/N in COND, a fixed camera): clear-equivalent SB = MK_ZREF + sky - zp1
MK_ZREF, MK_K, MK_MAX_AGE = 16.17, 0.20, 900.0

# Extinction k_f = k_r + EXT_BETA[f] (~/extinkce's model; beta from its pair
# method refitted on D50 2026-08..10, after the realuminization of 2026-07-14).
# EXT_Z0 = zeropoint above the atmosphere, D50 from per-night fits of zp
# against airmass on the clear nights (Z0_r 22.79, 0.06 night to night; k_r
# median 0.18); Makak (fixed airmass MK_X) calibrated on D50's k_r.  SBT has
# none of its own yet: its clear-sky zp1 at X=1 plus the typical k.
EXT_KR_TYP = 0.184
EXT_BETA = {'Sloan_g': 0.099, 'Sloan_r': 0.0, 'Sloan_i': -0.046, 'Sloan_z': -0.046}
EXT_Z0 = {'C0/Sloan_g': 22.94, 'C0/Sloan_r': 22.79, 'C0/Sloan_i': 22.165, 'C0/Sloan_z': 21.097, 'MK/N': 16.34}
MK_X = 1.096


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


def round_down(t, grid=TIME_GRID):
    """Largest value of grid at or below t (at least grid[0])."""
    best = grid[0]
    for g in grid:
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


def sun_moon(t):
    """Geocentric (ra, dec) [deg] of the Sun and the Moon at unix time t, low precision
    (Sun ~0.01 deg, Moon ~0.3 deg)."""
    d = t / 86400.0 - 10957.5                   # days since J2000.0
    r = math.radians
    eps = r(23.439 - 4e-7 * d)
    g = r(357.529 + 0.98560028 * d)
    sun = (r(280.459 + 0.98564736 * d + 1.915 * math.sin(g) + 0.020 * math.sin(2 * g)), 0.0)
    M, F, D = r(134.963 + 13.064993 * d), r(93.272 + 13.229350 * d), r(297.850 + 12.190749 * d)
    moon = (r(218.316 + 13.176396 * d + 6.289 * math.sin(M) + 1.274 * math.sin(2 * D - M)
              + 0.658 * math.sin(2 * D) + 0.214 * math.sin(2 * M) - 0.186 * math.sin(g) - 0.114 * math.sin(2 * F)),
            r(5.128 * math.sin(F) + 0.280 * math.sin(M + F) + 0.278 * math.sin(M - F) + 0.173 * math.sin(2 * D - F)))
    out = []
    for lon, lat in (sun, moon):
        ra = math.atan2(math.sin(lon) * math.cos(eps) - math.tan(lat) * math.sin(eps), math.cos(lon))
        dec = math.asin(math.sin(lat) * math.cos(eps) + math.cos(lat) * math.sin(eps) * math.sin(lon))
        out.append((math.degrees(ra) % 360, math.degrees(dec)))
    return out


def altaz(ra, dec, t, lat=SITE_LAT, lon=SITE_LON):
    """(alt, az north-based) [deg] of ra, dec [deg] at unix time t."""
    lst = 280.46061837 + 360.98564736629 * (t / 86400.0 - 10957.5) + lon
    h, d, p = math.radians(lst - ra), math.radians(dec), math.radians(lat)
    alt = math.asin(math.sin(p) * math.sin(d) + math.cos(p) * math.cos(d) * math.cos(h))
    az = math.atan2(math.sin(h), math.cos(h) * math.sin(p) - math.tan(d) * math.cos(p))
    return math.degrees(alt), (math.degrees(az) + 180) % 360


def _sep(alt1, az1, alt2, az2):
    a1, a2, da = math.radians(alt1), math.radians(alt2), math.radians(az1 - az2)
    c = math.sin(a1) * math.sin(a2) + math.cos(a1) * math.cos(a2) * math.cos(da)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def sky_geometry(alt, az, t):
    """Sun/Moon geometry for a pointing (alt, az north-based) at unix time t."""
    (sra, sdec), (mra, mdec) = sun_moon(t)
    salt, saz = altaz(sra, sdec, t)
    malt, maz = altaz(mra, mdec, t)
    malt -= 0.95 * math.cos(math.radians(malt))         # lunar parallax
    elong = _sep(sdec, sra, mdec, mra)                  # same formula on the equatorial sphere
    return dict(alt=alt, az=az, sunalt=salt, sunsep=_sep(alt, az, salt, saz),
                moonalt=malt, moonsep=_sep(alt, az, malt, maz), phase=180 - elong)


def sky_model_sb(key, d, k):
    """Model clear-equivalent sky [mag/"2] of key ('C1/Sloan_r', 'MK/N') for geometry d, extinction k."""
    p = dict(zip(SKY_PARAMS, SKY_MODEL[key]))
    xvr = lambda alt: 1 / math.sqrt(1 - 0.96 * math.sin(math.radians(90 - alt))**2)
    X = xvr(d['alt'])
    flux = 10**p['dark'] * X * 10**(-0.4 * k * (X - 1))
    flux += 10**p['lp'] * X**p['lp_Xpow'] * math.exp(p['lp_dir'] * math.cos(math.radians(d['az'] - p['lp_az'])))
    if d['moonalt'] > 0:
        a, rho = abs(d['phase']), d['moonsep']
        frho = 1.06 + math.cos(math.radians(rho))**2 + 10**p['mie'] * 10**(-rho / 40)
        flux += (10**p['moon'] * 10**(-0.4 * (0.026 * a + 4e-9 * a**4)) * frho
                 * 10**(-0.4 * k * xvr(d['moonalt'])) * (1 - 10**(-0.4 * k * X)))
    flux += 10**max(-30.0, min(30.0, p['twi'] + p['twi_slope'] * d['sunalt'])) \
        * math.exp(p['twi_dir'] * math.cos(math.radians(d['sunsep'])))
    return 20 - 2.5 * math.log10(flux)


def extinction(key, zp1, X):
    """k_r [mag/airmass] from one frame of key ('C0/Sloan_r', 'MK/N') with zp1 at airmass X, or None."""
    cam, _, f = key.partition('/')
    if key == 'MK/N':
        return (EXT_Z0[key] - zp1) / MK_X
    if f not in EXT_BETA or not X:
        return None
    z0 = EXT_Z0.get(key)
    if z0 is None:
        tel = next((t for t in TELESCOPES.values() if cam in t and f in t[cam]['filters']), None)
        if tel is None:
            return None
        z0 = tel[cam]['filters'][f][0] + EXT_KR_TYP + EXT_BETA[f]
    return (z0 - zp1) / X - EXT_BETA[f]


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

    def snr(self, mag, t, n=1):
        """SNR of n accumulated sub-exposures of t s (n read-noise floors)."""
        if n == 1:
            return snr_of(mag, t, self.zp1, self.s, self.fwhm, self.cam)
        F = n * t * 10**(-0.4 * (mag - self.zp1))
        area = math.pi * (self.fwhm**2 + PSF_AP2)
        return F / math.sqrt(F / self.cam['gain'] + area * n * (self.cam['floor'] + self.s * t))

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

    def __init__(self, telescope, get, max_age=1800.0, radec=(None, None), now=None):
        self.cams = TELESCOPES[telescope]
        self.get, self.max_age = get, max_age
        self.radec = radec          # target (ra, dec) [deg], for the sky model
        self.now = now or time.time()
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
        d = {f: self.value(f'{p}_{f}') for f in ('zp1', 'bgv', 'fwhm', 'X', 'sky', 'alt', 'az')}
        if d['zp1'] is None:
            return None
        d['age'] = age
        return d

    def departure(self, camera, filt):
        """(measured minus model clear-equivalent sky [mag], source): the own filter's
        fresh frame, else the first fresh one of its SKY_DONORS (MK/N = Makak)."""
        for key in (f'{camera}/{filt}',) + SKY_DONORS.get(f'{camera}/{filt}', ('MK/N',)):
            c, _, f = key.partition('/')
            o = self.fresh(c, f)
            if not o or None in (o['sky'], o['alt'], o['az']):
                continue
            d = sky_geometry(o['alt'], o['az'], self.now - o['age'])
            if key == 'MK/N':
                if o['age'] <= MK_MAX_AGE:
                    return MK_ZREF + o['sky'] - o['zp1'] - sky_model_sb(key, d, MK_K), f'Makak {o["age"]:.0f}s old'
                continue
            if c not in self.cams or o['X'] is None:
                continue                    # another telescope's camera: not used
            zpref, k, _ = self.cams[c]['filters'][f]
            sbc = o['sky'] - (o['zp1'] - (zpref - k * (o['X'] - 1)))
            return sbc - sky_model_sb(key, d, k), f'{key} {o["age"]:.0f}s old'
        return 0.0, 'no measured sky'

    def model_sky(self, camera, filt, X):
        """(sky variance rate s, source) at the target from the sky model, or None."""
        ra, dec = self.radec
        if ra is None or f'{camera}/{filt}' not in SKY_MODEL:
            return None
        cam = self.cams[camera]
        zpref, k, _ = cam['filters'][filt]
        alt, az = altaz(ra, dec, self.now)
        if alt < 5:
            return None
        dep, src = self.departure(camera, filt)
        sbc = sky_model_sb(f'{camera}/{filt}', sky_geometry(alt, az, self.now), k) + dep
        s = cam['bgk'] * cam['pix']**2 * 10**(-0.4 * (sbc - (zpref - k * (X - 1))))
        return s, f'sky model {sbc - dep:.2f}{dep:+.2f} from {src}'

    def conditions(self, camera, filt, X):
        """Conditions of camera/filter at airmass X, with fallbacks."""
        cam = self.cams[camera]
        zpref, k, skycol = cam['filters'].get(filt, (math.nan, 0.15, 0.0))
        own = self.fresh(camera, filt)
        model = self.model_sky(camera, filt, X)
        if model:
            # the sky as now at the target, so no twilight extrapolation from an older frame
            if own:
                fwhm = own['fwhm'] or DEFAULT_SEEING / cam['pix']
                Xf = own['X'] if own['X'] else X
                return Conditions(cam, own['zp1'] - k * (X - Xf), model[0], fwhm,
                                  f'own frame {own["age"]:.0f}s old, {model[1]}')
            tr = self.value('transp')
            zp1 = zpref - k * (X - 1) + (tr or 0.0)
            seeing = next((o['fwhm'] * self.cams[c]['pix'] for c in self.cams for f in self.cams[c]['filters']
                           for o in [self.fresh(c, f)] if o and o['fwhm']), DEFAULT_SEEING)
            return Conditions(cam, zp1, model[0], seeing / cam['pix'],
                              f'{"transp" if tr is not None else "clear-sky zp"}, {model[1]}')
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
# site conditions (COND publishes them for the monitor pages)
# ---------------------------------------------------------------------------
SITE_SKY = {'D50': {'r': 'C0/Sloan_r', 'i': 'C0/Sloan_i'}, 'SBT': {'r': 'C2/Sloan_r', 'i': 'C1/Sloan_i'}}
SITE_EXT_ORDER = ('C0', 'MK', 'C1', 'C2')  # D50 is calibrated, Makak on its scale, SBT approximate
SITE_EXT_FRESH = 900.0                     # seconds a frame counts for k_r
SITE_SUN_MAX = -6.0                        # no sky above this solar altitude


def telescope_of(cameras):
    """The TELESCOPES entry that has these cameras, or None."""
    return next((t for t, cams in TELESCOPES.items() if set(cameras) & set(cams)), None)


def site_conditions(telescope, sources, now=None):
    """Best-effort sky at the zenith (r, i) [mag/"2] and extinction k_r [mag/airmass].

    sources: COND's frames {'C0/Sloan_r': {zp1, sky, X, alt, az, tend, ...}}, local
    and remote.  The sky is the model at the zenith plus the departure of the best
    measured sky (CondReader.departure: own filter, its donors, Makak); k_r comes
    from the freshest frame of D50, else Makak, else SBT.  Returns {'sky_r': (value,
    source), 'sky_i': ..., 'k_r': (value, source)}, the source a camera/filter, 'MK/N'
    or 'model'; a quantity it cannot give is left out.
    """
    now = now or time.time()
    vals = {}
    for key, s in sources.items():
        cam, _, filt = key.partition('/')
        p = cond_prefix(cam, filt)
        for f in ('zp1', 'sky', 'bgv', 'fwhm', 'X', 'alt', 'az'):
            v = s.get(f)
            vals[f'{p}_{f}'] = None if v is None or (isinstance(v, float) and math.isnan(v)) else v
        vals[f'{p}_age'] = now - s['tend']
    out = {}
    zenith = sky_geometry(90.0, 0.0, now)
    if telescope in SITE_SKY and zenith['sunalt'] < SITE_SUN_MAX:
        reader = CondReader(telescope, lambda n: None if vals.get(n) is None else str(vals[n]), now=now)
        for band, key in SITE_SKY[telescope].items():
            cam, _, filt = key.partition('/')
            dep, src = reader.departure(cam, filt)
            src = 'MK/N' if src.startswith('Makak') else ('model' if src.startswith('no ') else src.split()[0])
            k = TELESCOPES[telescope][cam]['filters'][filt][1]
            out[f'sky_{band}'] = (sky_model_sb(key, zenith, k) + dep, src)
    for cam in SITE_EXT_ORDER:
        cand = [(s['tend'], key) for key, s in sources.items() if key.startswith(cam + '/')
                and now - s['tend'] < SITE_EXT_FRESH and vals.get(f'{cond_prefix(*key.split("/", 1))}_zp1') is not None]
        if not cand:
            continue
        key = max(cand)[1]
        k = extinction(key, sources[key]['zp1'], sources[key].get('X'))
        if k is not None and math.isfinite(k):
            out['k_r'] = (k, key)
            break
    return out


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


LCGEN_NIGHTS = 5            # robust average over the newest nights with data ...
LCGEN_SPAN = 60.0           # ... within this many days of the newest one
LCGEN_MAX_DMAG = 0.2


def lcgen_fetch(ra, dec, radius=5.0, timeout=15.0, auth_files=LCGEN_AUTH):
    """lcgen's light curve around ra, dec: ({filter name: [(jd, mag, dmag), ...]}, note).

    note is '' or why the newest data may be missing: without credentials
    (user:password in one of auth_files) lcgen serves only the public part,
    older than its embargo."""
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
    jds, mags = d.get('jd', []), d.get('mag', [])
    dmags = d.get('dmag') or [0.0] * len(jds)
    per = {}
    for jd, fi, mag, dm in zip(jds, d.get('filter', []), mags, dmags):
        name = names[fi] if isinstance(fi, int) and fi < len(names) else str(fi)
        per.setdefault(name, []).append((jd, mag, dm))
    note = ''
    if not d.get('user'):
        pu = d.get('public_until')
        note = 'anonymous, no ' + ' or '.join(auth_files) + (
            f': nothing newer than {time.strftime("%Y-%m-%d", time.gmtime((pu - 2440587.5) * 86400))}'
            if pu else '')
    return per, note


def robust_mag(pts, nights=LCGEN_NIGHTS, span=LCGEN_SPAN, max_dmag=LCGEN_MAX_DMAG, now_jd=None):
    """(mag, n_nights, age_days) of one filter's photometry [(jd, mag, dmag)], or None.

    Each night (changing at local noon) gives the median of its good points; the
    result is the median of the newest `nights` nightly values within `span` days
    of the newest one - a variable target follows its recent level, while a
    single bad night or frame cannot pull it.
    """
    good = [(j, m) for j, m, dm in pts if m == m and (dm is None or dm <= max_dmag)]
    if not good:
        return None
    per = {}
    for j, m in good:
        per.setdefault(math.floor(j + SITE_LON / 360), []).append(m)
    keys = sorted(per, reverse=True)
    keys = [k for k in keys if keys[0] - k <= span][:nights]
    vals = sorted(sorted(per[k])[len(per[k]) // 2] for k in keys)
    n = len(vals)
    med = vals[n // 2] if n % 2 else 0.5 * (vals[n // 2 - 1] + vals[n // 2])
    last = max(j for j, _ in good)
    now_jd = now_jd or time.time() / 86400 + 2440587.5
    return med, n, now_jd - last


def lcgen_mags(ra, dec, **kw):
    """{filter: mag} - robust average of the newest nights per filter in lcgen."""
    return {k: v[0] for k, v in lcgen_stats(ra, dec, **kw)[0].items()}


def lcgen_stats(ra, dec, **kw):
    """({filter: (mag, n_nights, age_days)}, note) from lcgen."""
    per, note = lcgen_fetch(ra, dec, **kw)
    out = {}
    for name, pts in per.items():
        r = robust_mag(pts)
        if r:
            out[name] = r
    return out, note


def parse_mag(spec):
    """'17.5' -> {'*': 17.5}; 'r=17.5,g=18.1' -> {'Sloan_r': 17.5, 'Sloan_g': 18.1}; None -> {}"""
    if spec is None or str(spec).strip() == '':
        return {}
    spec = str(spec).strip()
    if '=' not in spec:
        return {'*': float(spec)}
    return {long_filter(k.strip()): float(v) for k, v in (kv.split('=') for kv in spec.split(','))}


def target_mags(filters, ra=None, dec=None, mag=None, sources=('lcgen', 'atlas'),
                fallback=None, log=None):
    """{filter: (mag, origin)} for each of filters that can get one.

    mag (parse_mag output) wins; then the sources: for each filter the best name
    of MAG_FALLBACK that any source has (the filter itself before a substitute
    band), sources in order breaking the tie - so ATLAS's own i beats lcgen's r
    for i, while lcgen's r beats ATLAS's r; then fallback.
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
    missing = [f for f in filters if f not in out]
    have = []
    for src in sources if missing and ra is not None else ():
        try:
            if src == 'lcgen':
                st, note = lcgen_stats(ra, dec)
                if note:
                    log('W', f'lcgen: {note}')
                got = {k: v[0] for k, v in st.items()}
                desc = ', '.join(f'{k}={v[0]:.2f} ({v[1]}n, {v[2]:.0f}d)' for k, v in sorted(st.items()))
            else:
                got = atlas_mags(ra, dec)
                desc = ', '.join(f'{k}={v:.2f}' for k, v in sorted(got.items()))
        except Exception as e:
            log('W', f'{src} failed: {e}')
            continue
        log('I', f'{src}: ' + (desc or 'nothing'))
        have.append((src, got))
    for f in missing:
        best = None
        for si, (src, got) in enumerate(have):
            for rank, cand in enumerate(MAG_FALLBACK.get(f, [f])):
                if cand in got:
                    if best is None or (rank, si) < best[0]:
                        best = ((rank, si), (got[cand], f'{src} {cand}'))
                    break
        if best:
            out[f] = best[1]
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


def accumulation(c, mag, snr, t, tsat, tmax):
    """(sub-exposure, n) for an accumulation whose sub-exposures stay below tsat.

    Of the sub-exposure lengths that reach the SNR within tmax in total, the
    one with the shortest total (then the fewest readouts); if none does,
    the highest SNR within tmax.  Without a magnitude (mag None), n
    sub-exposures adding up to about t.
    """
    best = None
    for sub in (g for g in SUB_GRID if g <= tsat):
        nmax = max(1, min(MAX_ACC, int(tmax / sub + 1e-9)))
        if mag is None:
            n = min(nmax, max(1, math.ceil(t / sub - 1e-9)))
            key = (abs(n * sub - t), n)
        else:
            n = 1
            while n < nmax and c.snr(mag, sub, n) < snr:
                n += 1
            reached = c.snr(mag, sub, n) >= snr
            key = (0, n * sub, n) if reached else (1, -c.snr(mag, sub, n), n)
        if best is None or key < best[0]:
            best = (key, sub, n)
    if best is None:
        return SUB_GRID[0], 1
    return best[1], best[2]


def make_plan(groups, mags, snr, X, reader, tmin, tmax, overhead=0.0, fallback_t=None,
              sun=(None, 0.0)):
    """Exposure time of every camera/filter in groups ({camera: [filters]}) and their tick.

    mags: {filter: (mag, origin)}.  Each time is rounded up to TIME_GRID and
    limited to [tmin, tmax]; a filter without a magnitude gets fallback_t
    (or tmax).  Then it is cut (rounded down) to what does not saturate the
    sky or the target, with the sky extrapolated in a rising twilight; sun is
    twilight()'s output.  The tick is the longest one plus overhead.
    """
    exp, acc, info = {}, {}, {}
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
            cap, n = None, 1
            if t > tsat:
                cap = limit
                if c.cam.get('accumulate'):
                    t, n = accumulation(c, mag, snr, t, tsat, tmax)
                else:
                    t = round_down(tsat)
            key = f'{camera}/{filt}'
            exp[key] = t
            acc[key] = n
            info[key] = dict(mag=mag, origin=origin, need=need, t=t, n=n, cap=cap,
                             tsat=(None if math.isinf(tsat) else tsat),
                             snr=(c.snr(mag, t, n) if mag is not None else None),
                             cond=str(c))
    tick = (max(exp[k] * acc[k] for k in exp) if exp else tmax) + overhead
    return dict(exptime=exp, acc=acc, tick=tick, info=info, snr=snr, airmass=X)


def describe(key, i):
    """One log line for a plan entry."""
    mag = 'no magnitude' if i['mag'] is None else f"{i['origin']} {i['mag']:.2f}"
    need = '' if i['need'] is None else f" (needs {i['need']:.0f}s)"
    snr = '' if i['snr'] is None else f" SNR {i['snr']:.0f}"
    if not i.get('cap'):
        cap = ''
    elif i.get('n', 1) > 1:
        cap = f" ACCUMULATED: {i['cap']} would saturate at {i['tsat']:.1f}s"
    else:
        cap = f" CUT: {i['cap']} would saturate at {i['tsat']:.1f}s"
    t = f"{i['n']}x{i['t']:g}s" if i.get('n', 1) > 1 else f"{i['t']:g}s"
    return f"{key}: {mag}: {t}{need}{snr}{cap}, {i['cond']}"


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

