#!/usr/bin/env python3
"""Fast autofocus for RTS2.

Slew to a bright catalogue star, window the camera on it, and walk the
focuser to the vertex of the defocus curve. Seconds, not minutes: the speed
comes from the bright star and the small window, which together allow
sub-second exposures, and from a measurement that is worth trusting after one
frame instead of needing to be averaged.

  metric        half-flux diameter about the star's centre of light, not
                FWHM: away from focus a star is a disc or (with a central
                obstruction) a donut, where the half-maximum crossing is
                ill-defined but HFD stays monotonic. See rts2.focus.

  convergence   every measurement feeds a fit of HFD(x) = sqrt(a^2+b^2(x-x0)^2)
                and the answer is the fitted vertex, never the lowest sample.
                Sampling is adaptive - out on the weak branch while the curve
                is poorly constrained, near the vertex once it is - and the
                run stops on the uncertainty of x0, not on a step size or a
                point budget.

Usage as an RTS2 script:   exe focus.py
Options are read from the command line when run by hand, e.g.
   focus.py --camera C1 --no-slew --max-points 12
"""

import argparse
import sys
import time

import numpy as np
from astropy.io import fits

from rts2 import scriptcomm
from rts2.focus import hfd, fwhm, peak_of, saturated, FocusCurve, BrightStars


class FocusScript(scriptcomm.Rts2Comm):
    def __init__(self, args):
        scriptcomm.Rts2Comm.__init__(self)
        self.args = args
        self.telescope = args.telescope
        self.camera = args.camera
        self.focuser = None
        self.saved = {}

    # ---- helpers -------------------------------------------------------

    def _frame(self, exptime):
        """One exposure; returns (data, filename) with the frame already deleted.

        delete(), not toTrash()/toArchive(): the images row is inserted when
        the camera client saves the frame, and only delete() removes it too.
        A focus run takes a dozen frames that nobody will ever look at.
        """
        self.setValue('exposure', exptime)
        image = self.exposure()
        with fits.open(image, memmap=False) as hdul:
            data = np.array(hdul[0].data, dtype=float)
        self.delete(image)
        return data

    def _measure(self, exptime):
        """HFD of the windowed star, or NaN."""
        data = self._frame(exptime)
        if self.args.full_well and saturated(data, self.args.full_well):
            self.log('W', 'focus: star saturated - shorten the exposure')
            return float('nan'), data
        return hfd(data, radius=self.args.hfd_radius), data

    def _set_focus(self, offset):
        self.setValue('FOC_TOFF', offset, self.focuser)
        self.waitIdle(self.focuser, self.args.focuser_timeout)

    # ---- steps ---------------------------------------------------------

    def _lst(self):
        """Local sidereal time in degrees, from the telescope or the clock."""
        try:
            return float(self.getValueFloat('LST', self.telescope))
        except Exception:
            pass
        lon = self.args.longitude
        if lon is None:
            lon = float(self.getValueFloat('LONGITUD', self.telescope))
        jd = time.time() / 86400.0 + 2440587.5
        return (280.46061837 + 360.98564736629 * (jd - 2451545.0) + lon) % 360.0

    def pick_star(self):
        """Slew to a bright focus star: near the zenith and past the meridian.

        Past the meridian on purpose - a star still east of it would cross
        during the run, and on a German mount that is a flip in the middle of
        a focus sweep. Near the zenith for the thinnest air.

        This matters for target 3 in particular: it is an ordinary fixed
        target at RA 0, Dec 0 - 'o' is not in createTarget()'s switch, so it
        falls through to ConstTarget - which is wherever the celestial equator
        happens to be, possibly below the horizon. The script picks its own
        star rather than trusting the target's coordinates.
        """
        if self.args.no_slew:
            self.log('I', 'focus: --no-slew, using whatever is in the field')
            return None

        cat = BrightStars(self.args.catalog)
        star = None
        try:
            if self.args.near_pointing:
                tel = self.getValue('TEL', self.telescope).split()
                star = cat.nearest(float(tel[0]), float(tel[1]),
                                   mag_range=(self.args.mag_min, self.args.mag_max),
                                   max_distance=self.args.max_slew)
            else:
                lat = self.args.latitude
                if lat is None:
                    lat = float(self.getValueFloat('LATITUDE', self.telescope))
                star = cat.near_zenith(lat, self._lst(),
                                       mag_range=(self.args.mag_min, self.args.mag_max),
                                       ha_range=(self.args.ha_min, self.args.ha_max),
                                       max_zenith_distance=self.args.max_zenith_distance)
        except Exception as e:
            self.log('W', 'focus: catalogue unavailable (%s) - staying put' % e)
            return None

        if star is None:
            self.log('W', 'focus: no suitable focus star found - staying put')
            return None

        self.log('I', 'focus: slew START'); self.log('I',
                 'focus: slewing to G=%.1f  HA %+.1f deg  %.1f deg from zenith  (%.4f %+.4f)'
                 % (star['mag'], star.get('ha', float('nan')),
                    star.get('zenith_distance', star['distance']), star['ra'], star['dec']))
        self.radec(star['ra'], star['dec'])
        self._wait_on_target(star['ra'], star['dec'])
        return star

    def _wait_on_target(self, ra, dec, tolerance=0.2):
        """Wait until the telescope reports it is on the requested position.

        NOT waitIdle(): a telescope that has arrived is tracking, not idle, so
        waiting for idle can block until the timeout - or for ever if the
        state never matches. Polling the reported position asks the question
        we actually care about and cannot outlive its own deadline.
        """
        deadline = time.time() + self.args.slew_timeout
        while time.time() < deadline:
            try:
                tel = self.getValue('TEL', self.telescope).split()
                cra, cdec = float(tel[0]), float(tel[1])
            except Exception:
                time.sleep(2)
                continue
            dra = ((cra - ra + 180.0) % 360.0 - 180.0) * np.cos(np.radians(dec))
            if np.sqrt(dra ** 2 + (cdec - dec) ** 2) <= tolerance:
                self.log('I', 'focus: on target')
                return True
            time.sleep(2)
        self.log('W', 'focus: telescope did not reach the target in %.0f s - going on anyway'
                 % self.args.slew_timeout)
        return False

    def acquire(self):
        """Find the brightest star in a binned full frame and window on it."""
        self.saved['WINDOW'] = self.getValue('WINDOW')
        try:
            self.saved['binning'] = self.getValue('binning')
        except Exception:
            self.saved['binning'] = None

        if self.args.acq_binning is not None:
            # binning is a selection on most cameras; an out-of-range code is
            # silently ignored rather than allowed to wedge the run
            self.log('I', 'focus: acquisition binning %s' % self.args.acq_binning)
            self.setValue('binning', self.args.acq_binning)
        self.setValue('WINDOW', '-1 -1 -1 -1')

        self.log('I', 'focus: acquisition exposure %.2f s' % self.args.acq_exposure)
        data = self._frame(self.args.acq_exposure)
        peak = peak_of(data)
        binf = self.args.bin_factor
        y, x = int(peak[0]) * binf, int(peak[1]) * binf
        self.log('I', 'focus: brightest star at %d %d (unbinned)' % (x, y))

        if self.saved['binning'] is not None:
            self.setValue('binning', self.saved['binning'])
        half = self.args.box // 2
        self.setValue('WINDOW', '%d %d %d %d' % (x - half, y - half,
                                                 self.args.box, self.args.box))
        return x, y

    def tune_exposure(self):
        """Shortest exposure that still gives a solid star, from a quick probe."""
        exptime = self.args.exposure
        if not self.args.full_well:
            return exptime
        for _ in range(4):
            data = self._frame(exptime)
            peak = float(np.nanmax(data)) - float(np.nanmedian(data))
            target = self.args.target_level * self.args.full_well
            if peak <= 0:
                return exptime
            if peak > 0.9 * self.args.full_well:
                exptime = max(exptime * 0.4, self.args.min_exposure)
                continue
            scale = target / peak
            new = min(max(exptime * scale, self.args.min_exposure), self.args.max_exposure)
            if abs(new - exptime) / max(exptime, 1e-6) < 0.25:
                return new
            exptime = new
        return exptime

    def run_focus(self, exptime):
        """Adaptive walk to the vertex. Returns (x0, sigma) or None."""
        curve = FocusCurve()
        offset = 0.0
        started = time.time()

        for n in range(self.args.max_points):
            self._set_focus(offset)
            width, data = self._measure(exptime)
            ok = curve.add(offset, width)
            got = curve.fit() if len(curve) >= 4 else None

            if got:
                x0, sigma, a, b = got
                self.log('I', 'focus: %2d  toff %+7.1f  hfd %5.2f   x0 %+7.1f +- %.1f  (a %.2f)'
                         % (n + 1, offset, width, x0, sigma, a))
                if sigma <= self.args.tolerance and len(curve) >= self.args.min_points:
                    self.log('I', 'focus: converged, sigma %.1f <= %.1f after %d points, %.1f s'
                             % (sigma, self.args.tolerance, len(curve), time.time() - started))
                    return x0, sigma
            else:
                self.log('I', 'focus: %2d  toff %+7.1f  hfd %s'
                         % (n + 1, offset, '%5.2f' % width if ok else '  nan'))

            offset = curve.next_position(offset, self.args.coarse_step)
            offset = float(np.clip(offset, -self.args.max_offset, self.args.max_offset))

        got = curve.fit()
        if got:
            self.log('W', 'focus: point budget spent, taking the fit anyway (sigma %.1f)' % got[1])
            return got[0], got[1]
        self.log('E', 'focus: could not fit a focus curve')
        return None

    def run(self):
        self.focuser = self.getValue('FOCUSER')
        self.log('I', 'focus: **** FOCUS RUN **** camera %s focuser %s'
                 % (self.camera or 'default', self.focuser))
        try:
            self.pick_star()
            self.acquire()
            exptime = self.tune_exposure()
            self.log('I', 'focus: exposure %.3f s' % exptime)

            got = self.run_focus(exptime)
            if got is None:
                return 1
            x0, sigma = got

            self._set_focus(x0)
            width, data = self._measure(exptime)
            seeing = fwhm(data)
            self.log('I', 'focus: at best focus toff %+.1f: hfd %.2f  fwhm %.2f px'
                     % (x0, width, seeing))

            if self.args.commit:
                base = self.getValueFloat('FOC_POS', self.focuser)
                self.setValue('FOC_POS', base + x0, self.focuser)
                self.setValue('FOC_TOFF', 0, self.focuser)
                self.log('I', 'focus: FOC_POS set to %.1f' % (base + x0))
            else:
                self.log('I', 'focus: --no-commit, leaving FOC_TOFF at %+.1f' % x0)
            return 0
        except scriptcomm.Rts2NotActive:
            self.log('I', 'focus: deactivated by RTS2, ending')
            return 0
        finally:
            if self.saved.get('WINDOW') is not None:
                self.setValue('WINDOW', self.saved['WINDOW'])


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--telescope', default='T0')
    p.add_argument('--camera', default=None, help='camera name for logs; the script exposes on its own device')
    p.add_argument('--catalog', default=None, help='bright star FITS catalogue (default: rts2.focus.BrightStars)')
    p.add_argument('--no-slew', action='store_true', help='focus on whatever is in the field now')
    p.add_argument('--mag-min', type=float, default=5.0)
    p.add_argument('--mag-max', type=float, default=8.5)
    p.add_argument('--max-slew', type=float, default=5.0, help='[deg] furthest star in --near-pointing mode')
    p.add_argument('--near-pointing', action='store_true',
                   help='pick a star near the current pointing instead of near the zenith')
    p.add_argument('--ha-min', type=float, default=5.0, help='[deg] least hour angle past the meridian')
    p.add_argument('--ha-max', type=float, default=40.0, help='[deg] most hour angle past the meridian')
    p.add_argument('--max-zenith-distance', type=float, default=30.0, help='[deg]')
    p.add_argument('--latitude', type=float, default=None, help='[deg] default: LATITUDE from the telescope')
    p.add_argument('--longitude', type=float, default=None, help='[deg] only if LST is unreadable')
    p.add_argument('--slew-timeout', type=float, default=120.0)

    p.add_argument('--box', type=int, default=64, help='[px] windowed readout size')
    p.add_argument('--hfd-radius', type=float, default=15.0, help='[px] aperture for HFD')
    p.add_argument('--acq-exposure', type=float, default=1.0)
    p.add_argument('--acq-binning', default=None, help='binning value for acquisition (site specific)')
    p.add_argument('--bin-factor', type=int, default=1, help='pixels per binned pixel during acquisition')

    p.add_argument('--exposure', type=float, default=0.5)
    p.add_argument('--min-exposure', type=float, default=0.05)
    p.add_argument('--max-exposure', type=float, default=5.0)
    p.add_argument('--full-well', type=float, default=None, help='ADU; enables saturation control')
    p.add_argument('--target-level', type=float, default=0.35, help='fraction of full well to aim at')

    p.add_argument('--coarse-step', type=float, default=60.0, help='focuser units for the first probes')
    p.add_argument('--max-offset', type=float, default=600.0, help='never move FOC_TOFF beyond this')
    p.add_argument('--tolerance', type=float, default=8.0, help='stop when sigma(x0) is below this')
    p.add_argument('--min-points', type=int, default=6)
    p.add_argument('--max-points', type=int, default=15)
    p.add_argument('--focuser-timeout', type=float, default=30.0)
    p.add_argument('--no-commit', dest='commit', action='store_false',
                   help='measure and report, but do not move FOC_POS')
    return p.parse_args(argv)


if __name__ == '__main__':
    sys.exit(FocusScript(parse_args(sys.argv[1:])).run())
