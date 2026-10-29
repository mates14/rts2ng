#!/usr/bin/python3
"""Expose the current target in a sequence of filters, each for a requested SNR,
until the executor wants another target.

    exe expose.py --filters rzgi --snr 30 --fallback-mag 16.3

The exposure time of each filter is computed before every cycle from the
current conditions (the COND device) and the target magnitude (--mag, else
the local ATLAS, else lcgen on hog, else --fallback-mag).  There is no
repetition count: the cycles go on while the queuer keeps the target, and
the script ends at an exposure boundary once EXEC has another target as next.
"""

import argparse
import os
from time import sleep

from rts2 import scriptcomm
try:
    from rts2 import snrexp
except ImportError:
    # the installed rts2 package predates snrexp: take it from this checkout
    import importlib.util
    _spec = importlib.util.spec_from_file_location('snrexp', os.path.join(
        os.path.dirname(os.path.realpath(__file__)), '..', '..', 'rts2', 'snrexp.py'))
    snrexp = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(snrexp)

TELESCOPE = 'D50'


class Expose(scriptcomm.Rts2Comm):
    """Filter cycles at a requested SNR (or a fixed exposure) on one camera."""

    def __init__(self, filters, snr=None, exptime=None, mag=None, fallback_mag=None,
                 mag_sources=('atlas', 'lcgen'), tmin=1, tmax=300):
        super().__init__()
        self.filters = filters
        self.snr, self.exptime = snr, exptime
        self.mag, self.fallback_mag, self.mag_sources = mag, fallback_mag, tuple(mag_sources)
        self.tmin, self.tmax = tmin, tmax
        self.camera = self.getRunDevice()
        if self.snr and self.camera not in snrexp.TELESCOPES[TELESCOPE]:
            raise RuntimeError(f"no calibration for camera {self.camera}")
        self.name = f"Expose/{self.camera}"
        # our target, to notice when the executor wants another one
        self.target = self.getValueInteger('current', 'EXEC')
        self.mags = None

    def next_is_other(self):
        """True if EXEC has a target other than ours queued as next."""
        if self.target is None:
            return False
        next_t = self.getValueInteger('next', 'EXEC')
        return next_t is not None and next_t >= 0 and next_t != self.target

    def mount_moved(self):
        try:
            return self.getValueFloat('move_started', 'T0') > self.getValueFloat('move_end', 'T0') + 1
        except scriptcomm.Rts2NotActive:
            raise
        except Exception:
            return False

    def wait_for_tracking(self):
        if self.isParked('T0'):
            raise TimeoutError("telescope is parked, not waiting for tracking")
        self.waitMask('T0', scriptcomm.TEL_TRACKING, 300)

    def plan(self):
        """{filter name: exposure time} for the next cycle."""
        if not self.snr:
            return {f: self.exptime for f in self.filters}
        long = [snrexp.long_filter(f) for f in self.filters]
        log = lambda level, text: self.log(level, f"{self.name}: {text}")
        if self.mags is None:
            # the target does not change: look its magnitudes up once
            ra, dec = snrexp.target_position(self)
            self.mags = snrexp.target_mags(long, ra, dec, snrexp.parse_mag(self.mag),
                                           self.mag_sources, self.fallback_mag, log)
            for f in long:
                if f not in self.mags:
                    log('W', f"no magnitude for {f}, exposing {self.tmax:g}s")
        reader = snrexp.CondReader(TELESCOPE, lambda name: self.getValue(name, 'COND'))
        plan = snrexp.make_plan({self.camera: long}, self.mags, self.snr,
                                snrexp.target_airmass(self), reader, self.tmin, self.tmax)
        for k, i in plan['info'].items():
            log('I', snrexp.describe(k, i))
        return {f: plan['exptime'][f'{self.camera}/{lf}'] for f, lf in zip(self.filters, long)}

    def run(self):
        """Cycles until the executor wants another target or the mount moves; images taken."""
        n = 0
        self.wait_for_tracking()
        while True:
            for f, t in self.plan().items():
                if self.next_is_other():
                    self.log('I', f"{self.name}: next target queued, ending after {n} images")
                    return n
                self.setValue('filter', f)
                self.setValue('exposure', t)
                try:
                    image = self.exposure()
                except scriptcomm.Rts2NotActive:
                    raise
                except Exception as e:
                    self.log('E', f"{self.name}: exposure in {f} failed: {e}")
                    sleep(5)
                    continue
                self.process(image)
                n += 1
                if self.mount_moved():
                    self.log('I', f"{self.name}: mount moved, ending after {n} images")
                    return n


def parse_arguments():
    def positive_float(value):
        fvalue = float(value)
        if fvalue <= 0:
            raise argparse.ArgumentTypeError(f"{value} must be positive")
        return fvalue

    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--filters", required=True,
                        help="filters in order, 'rzgi' or 'r,z,g,i'")
    parser.add_argument("-s", "--snr", type=positive_float, help="SNR to reach in each filter")
    parser.add_argument("--magerr", type=positive_float, help="magnitude error to reach (instead of --snr)")
    parser.add_argument("-e", "--exptime", type=positive_float, help="fixed exposure time instead of --snr")
    parser.add_argument("-m", "--mag", help="target magnitude, '16.3' or 'r=16.3,g=16.9' (default: --mag-source)")
    parser.add_argument("--mag-source", default="atlas,lcgen", help="where to look the magnitude up, in order")
    parser.add_argument("-f", "--fallback-mag", type=float, help="magnitude when no source knows the target")
    parser.add_argument("--tmin", type=positive_float, default=1.0, help="shortest exposure")
    parser.add_argument("--tmax", type=positive_float, default=300.0, help="longest exposure")
    args = parser.parse_args()
    if args.magerr:
        args.snr = snrexp.MAGERR_K / args.magerr
    if bool(args.snr) == bool(args.exptime):
        parser.error("give one of --snr/--magerr or -e")
    args.filters = args.filters.split(',') if ',' in args.filters else list(args.filters)
    return args


if __name__ == '__main__':
    args = parse_arguments()
    try:
        e = Expose(args.filters, snr=args.snr, exptime=args.exptime, mag=args.mag,
                   fallback_mag=args.fallback_mag,
                   mag_sources=[s for s in args.mag_source.split(',') if s],
                   tmin=args.tmin, tmax=args.tmax)
        e.log('I', f"{e.name}: starts filters {''.join(args.filters)} "
                   + (f"snr {args.snr:g}" if args.snr else f"exp {args.exptime:g}s"))
        n = e.run()
        e.log('I', f"{e.name}: ended with {n} images")
    except scriptcomm.Rts2NotActive:
        # executor ended our script (target change, kill) - just go
        exit(0)
    except TimeoutError as ex:
        print(f"expose.py: {ex}")
        exit(1)
