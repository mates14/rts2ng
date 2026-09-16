"""Star-image metrics and focus-curve fitting for RTS2 focus scripts.

Two halves, both deliberately small:

  measurement   hfd() and fwhm() of a star in a cutout. No profile fitting,
                no starting guess, no iteration: a bad frame costs a NaN
                rather than an exception, and both are computed in 2D around
                the star's actual peak rather than an assumed centre.

  focus curve   FocusCurve fits HFD(x) = sqrt(a^2 + b^2 (x-x0)^2) to every
                measurement taken so far and reports the vertex x0 with an
                uncertainty. The vertex of a constrained curve is far better
                determined than the lowest single sample, which is what a
                naive argmin returns - and argmin is biased towards whichever
                point a favourable noise excursion happened to hit.

Why HFD and not FWHM for focusing: away from focus a star becomes a broad
disc or, with a central obstruction, a donut, and the half-maximum crossing
becomes ill-defined or bimodal. Half-flux diameter stays monotonic and very
nearly linear in defocus on both sides, so the curve is well behaved over the
whole range a focus run covers. FWHM remains the right thing to *report* for
seeing once focused.
"""

import numpy as np

__all__ = ['hfd', 'fwhm', 'peak_of', 'centroid_of', 'FocusCurve', 'BrightStars']


def peak_of(box, smooth=True):
    """(row, col) of the star in a cutout.

    A 3x3 box-smoothed maximum by default, so a single hot pixel or cosmic
    ray does not win against a real star.
    """
    data = np.asarray(box, dtype=float)
    if smooth and min(data.shape) >= 3:
        k = np.ones((3, 3))
        acc = np.zeros_like(data)
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                acc += np.roll(np.roll(data, dr, axis=0), dc, axis=1)
        acc[0, :] = acc[-1, :] = acc[:, 0] = acc[:, -1] = -np.inf
        if np.isfinite(acc).any():
            return np.unravel_index(np.argmax(acc), acc.shape)
    return np.unravel_index(np.argmax(data), data.shape)


def centroid_of(data, start, radius, sky):
    """Flux-weighted centre of the star, iterated from `start`.

    HFD must be measured about the centre of the light, not the brightest
    pixel: a defocused star on an obstructed telescope is a donut whose peak
    sits on the ring, several pixels off centre. Measuring radii from there
    understates the diameter and can make HFD turn over just when the run is
    furthest from focus.
    """
    data = np.asarray(data, dtype=float)
    rows = np.arange(data.shape[0])[:, None]
    cols = np.arange(data.shape[1])[None, :]
    cr, cc = float(start[0]), float(start[1])
    for _ in range(3):
        rr = np.sqrt((rows - cr) ** 2 + (cols - cc) ** 2)
        w = np.where(rr <= radius, data - sky, 0.0)
        w = np.where(w > 0, w, 0.0)
        tot = w.sum()
        if not np.isfinite(tot) or tot <= 0:
            return (cr, cc)
        nr = float((w * rows).sum() / tot)
        nc = float((w * cols).sum() / tot)
        if abs(nr - cr) < 0.05 and abs(nc - cc) < 0.05:
            cr, cc = nr, nc
            break
        cr, cc = nr, nc
    return (cr, cc)


def _background(data, peak, inner):
    """Median of the cutout outside a box of half-width `inner` around the peak."""
    mask = np.ones(data.shape, dtype=bool)
    inner = int(inner)                       # radius may arrive as a float
    pr, pc = int(peak[0]), int(peak[1])
    r0, r1 = max(0, pr - inner), min(data.shape[0], pr + inner + 1)
    c0, c1 = max(0, pc - inner), min(data.shape[1], pc + inner + 1)
    mask[r0:r1, c0:c1] = False
    if mask.sum() < 16:
        return float(np.median(data))
    return float(np.median(data[mask]))


def hfd(box, radius=15, peak=None):
    """Half-flux diameter of the star in `box`, in pixels.

    The diameter of the circle containing half the star's flux, measured by
    accumulating background-subtracted flux in growing annuli around the
    peak and interpolating where the running sum crosses half the total.

    Robust where FWHM is not: it needs no half-maximum crossing, so it stays
    meaningful (and monotonic in defocus) for a defocused disc or a donut.
    Returns NaN when there is no positive flux to measure.
    """
    data = np.asarray(box, dtype=float)
    if data.ndim != 2 or min(data.shape) < 5:
        return float('nan')
    if peak is None:
        peak = peak_of(data)

    sky = _background(data, peak, max(3, radius // 2))
    # about the centre of light, not the peak - see centroid_of()
    cr, cc = centroid_of(data, peak, radius, sky)
    rows = np.arange(data.shape[0])[:, None] - cr
    cols = np.arange(data.shape[1])[None, :] - cc
    rr = np.sqrt(rows * rows + cols * cols)

    flux = data - sky
    inside = rr <= radius
    flux = np.where(inside & (flux > 0), flux, 0.0)
    total = flux.sum()
    if not np.isfinite(total) or total <= 0:
        return float('nan')

    # running flux against radius, then interpolate the half-flux crossing
    order = np.argsort(rr[inside])
    r_sorted = rr[inside][order]
    cum = np.cumsum(flux[inside][order])
    half = total / 2.0
    i = int(np.searchsorted(cum, half))
    if i <= 0:
        return float(2.0 * r_sorted[0])
    if i >= len(cum):
        return float(2.0 * r_sorted[-1])
    c0, c1 = cum[i - 1], cum[i]
    r0, r1 = r_sorted[i - 1], r_sorted[i]
    frac = 0.0 if c1 == c0 else (half - c0) / (c1 - c0)
    return float(2.0 * (r0 + frac * (r1 - r0)))


def fwhm(box, size=15, peak=None):
    """FWHM of the star in `box`, in pixels, from the area above half maximum.

    Area rather than a fit: no starting guess to get wrong, no iteration to
    fail to converge. Measured on a `size` box around the actual peak, so a
    second star elsewhere in the cutout cannot inflate it. Diagnostic of
    seeing once focused; use hfd() to drive focusing.
    """
    data = np.asarray(box, dtype=float)
    if data.ndim != 2 or min(data.shape) < 5:
        return float('nan')
    if peak is None:
        peak = peak_of(data)

    half = int(size) // 2
    pr, pc = int(peak[0]), int(peak[1])
    r0, r1 = max(0, pr - half), min(data.shape[0], pr + half + 1)
    c0, c1 = max(0, pc - half), min(data.shape[1], pc + half + 1)
    sub = data[r0:r1, c0:c1]
    sky = _background(data, peak, half)
    amplitude = float(np.max(sub)) - sky
    if not np.isfinite(amplitude) or amplitude <= 0:
        return float('nan')
    above = np.count_nonzero(sub > sky + amplitude / 2.0)
    return float(2.0 * np.sqrt(above / np.pi))


def saturated(box, full_well, fraction=0.9):
    """True if the star is close enough to full well to distort its profile.

    A flat-topped star biases HFD and FWHM alike, so a focus run must notice
    and shorten the exposure rather than trust the number.
    """
    data = np.asarray(box, dtype=float)
    return bool(np.nanmax(data) >= fraction * full_well)


class FocusCurve:
    """Accumulates (focuser position, HFD) and fits the defocus hyperbola.

    HFD(x) = sqrt(a^2 + b^2 (x - x0)^2)

      a   half-flux diameter at best focus (seeing plus optics)
      b   how fast it opens up per focuser unit
      x0  best focus

    Linear far from focus, rounded at the minimum - the shape defocus
    actually has. A parabola is wrong on the wings, a pure V is wrong at the
    bottom; this is right across the range a focus run covers, and three
    parameters fit comfortably from a handful of points.

    Fitted with Nelder-Mead, which needs no derivatives and does not mind a
    NaN measurement being dropped. Every point constrains the vertex, so the
    result is much better than the best single sample.
    """

    def __init__(self):
        self.positions = []
        self.widths = []

    def add(self, position, width):
        """Record one measurement; NaN widths are ignored."""
        if width is None or not np.isfinite(width):
            return False
        self.positions.append(float(position))
        self.widths.append(float(width))
        return True

    def __len__(self):
        return len(self.positions)

    @staticmethod
    def _model(params, x):
        a, b, x0 = params
        return np.sqrt(a * a + b * b * (x - x0) ** 2)

    def _residual(self, params):
        x = np.asarray(self.positions)
        y = np.asarray(self.widths)
        return float(np.sum((self._model(params, x) - y) ** 2))

    def fit(self):
        """Fit the curve. Returns (x0, sigma_x0, a, b) or None if under-determined.

        sigma_x0 comes from how far x0 can move before the summed square
        residual grows by its per-point variance - a plain, assumption-light
        error bar that widens honestly when the branches are not yet pinned.
        """
        if len(self) < 4:
            return None
        x = np.asarray(self.positions)
        y = np.asarray(self.widths)
        if x.max() - x.min() <= 0:
            return None

        try:
            from scipy.optimize import minimize
        except ImportError:
            return None

        # start from the lowest sample and the span of what we have
        i = int(np.argmin(y))
        a0 = max(float(y[i]), 1e-3)
        span = max(float(x.max() - x.min()), 1.0)
        b0 = max((float(y.max()) - a0) / span, 1e-6)
        best = None
        for x0_guess in (x[i], x.mean(), x[i] + span / 4, x[i] - span / 4):
            r = minimize(self._residual, [a0, b0, x0_guess], method='Nelder-Mead',
                         options={'xatol': 1e-3, 'fatol': 1e-6, 'maxiter': 2000})
            if best is None or r.fun < best.fun:
                best = r
        if best is None or not np.isfinite(best.fun):
            return None

        a, b, x0 = best.x
        a, b = abs(a), abs(b)
        if b <= 0:
            return None

        dof = max(len(self) - 3, 1)
        var = best.fun / dof                      # per-point residual variance
        step = max(span / 200.0, 1e-3)
        sigma = float('inf')
        for k in range(1, 401):                   # walk x0 out until chi2 rises by var
            trial = self._residual([a, b, x0 + k * step])
            if trial - best.fun >= var:
                sigma = k * step
                break
        return float(x0), float(sigma), float(a), float(b)

    def next_position(self, current, coarse_step):
        """Where to measure next.

        With too few points, step out to open the branches - a run that
        starts near focus has no leverage on b and must earn it. Once the
        curve is fitted, sample the side that is currently weaker, about one
        'a/b' out from the vertex, which is where the branch starts to bite.
        """
        if len(self) < 4:
            direction = 1 if len(self) % 2 else -1
            return current + direction * coarse_step * (1 + len(self) // 2)

        got = self.fit()
        if got is None:
            return current + coarse_step

        x0, sigma, a, b = got
        lever = max(a / b, coarse_step)           # defocus where HFD ~ sqrt(2)*a
        x = np.asarray(self.positions)
        below = int(np.sum(x < x0))
        above = int(np.sum(x > x0))
        side = 1 if above <= below else -1
        return x0 + side * lever * 1.5


class BrightStars:
    """Bright stars from a local FITS catalogue, for picking a focus target.

    Reads the pre-filtered Gaia bright-star table (the 'makak' catalogue used
    by pyrt): columns radeg, decdeg, G. Small enough to read per call - 200k
    rows load in about 0.1 s - so there is no cache to go stale.
    """

    DEFAULT_PATH = '/home/mates/catalogs/gaia_bright_stars.fits'

    def __init__(self, path=None):
        self.path = path or self.DEFAULT_PATH
        self._table = None

    def _load(self):
        if self._table is None:
            from astropy.table import Table
            self._table = Table.read(self.path)
        return self._table

    def near_zenith(self, latitude, lst_deg, mag_range=(5.0, 8.5),
                    ha_range=(5.0, 40.0), max_zenith_distance=30.0):
        """Best focus star near the zenith and PAST the meridian.

        Past the meridian on purpose: a star still rising would cross the
        meridian during the run, and on a German mount that means a flip in
        the middle of a focus sweep. `ha_range` is degrees of hour angle west
        of the meridian - positive, already setting, with enough margin to
        finish.

        Near the zenith for the thinnest air and the roundest image. Among
        the candidates, the one closest to the zenith wins; brightness only
        has to be inside the range, since any star there gives ample signal.
        """
        cat = self._load()
        ra_c = np.asarray(cat['radeg'], dtype=float)
        dec_c = np.asarray(cat['decdeg'], dtype=float)
        mag = np.asarray(cat['G'], dtype=float)

        ha = (lst_deg - ra_c + 180.0) % 360.0 - 180.0     # -180..180, + is west
        zd = np.sqrt(((ha) * np.cos(np.radians(dec_c))) ** 2 + (dec_c - latitude) ** 2)

        ok = ((mag >= mag_range[0]) & (mag <= mag_range[1])
              & (ha >= ha_range[0]) & (ha <= ha_range[1])
              & (zd <= max_zenith_distance))
        if not ok.any():
            return None

        i = int(np.argmin(np.where(ok, zd, np.inf)))
        return {'ra': float(ra_c[i]), 'dec': float(dec_c[i]), 'mag': float(mag[i]),
                'ha': float(ha[i]), 'zenith_distance': float(zd[i]),
                'distance': float(zd[i])}

    def nearest(self, ra, dec, mag_range=(5.0, 8.5), max_distance=5.0):
        """Closest catalogue star to (ra, dec) in degrees within the magnitude range.

        Returns a dict with ra, dec, mag and distance, or None. Chooses the
        nearest suitable star rather than the brightest available: a short
        slew keeps pointing, and any star in range gives ample signal for a
        sub-second exposure.
        """
        cat = self._load()
        ra_c = np.asarray(cat['radeg'], dtype=float)
        dec_c = np.asarray(cat['decdeg'], dtype=float)
        mag = np.asarray(cat['G'], dtype=float)

        ok = (mag >= mag_range[0]) & (mag <= mag_range[1])
        if not ok.any():
            return None

        d2r = np.pi / 180.0
        cosd = np.cos(dec * d2r)
        dra = (ra_c - ra + 180.0) % 360.0 - 180.0
        dist = np.sqrt((dra * cosd) ** 2 + (dec_c - dec) ** 2)
        dist = np.where(ok, dist, np.inf)

        i = int(np.argmin(dist))
        if not np.isfinite(dist[i]) or dist[i] > max_distance:
            return None
        return {'ra': float(ra_c[i]), 'dec': float(dec_c[i]),
                'mag': float(mag[i]), 'distance': float(dist[i])}
