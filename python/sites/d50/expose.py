#!/usr/bin/python3
"""Expose the current target in a sequence of filters, each for a requested SNR,
until the executor wants another target.

    exe expose.py --filters rzgi --snr 30 --fallback-mag 16.3 --acquire 3x10 --dither 0.03

The exposure time of each filter is computed before every cycle from the
current conditions (the COND device) and the target magnitude (--mag, else
the local ATLAS, else lcgen on hog, else --fallback-mag).  There is no
repetition count: the cycles go on while the queuer keeps the target, and
the script ends at an exposure boundary once EXEC has another target as next.
The mount may move meanwhile (centering), that does not end it.

--acquire NxT takes N unfiltered frames of T s first, for the centering.
--dither D offsets the mount by a random (-D..D, -D..D) deg before each
science frame, as T0.WOFFS=(rand(-D,D),rand(-D,D)) does in a script.  A
value change from an exe script does not wait for the device, so an
exposure queued right after it could start before T0 even raises its
exposure block; the script therefore waits until T0 has applied the offset
and no device of --block-devices blocks exposures.
"""

import argparse
import os
import random
from time import sleep, time

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
BOP_EXPOSURE = 0x01000000      # status.h


class Expose(scriptcomm.Rts2Comm):
    """Filter cycles at a requested SNR (or a fixed exposure) on one camera."""

    def __init__(self, filters, snr=None, exptime=None, mag=None, fallback_mag=None,
                 mag_sources=('atlas', 'lcgen'), tmin=1, tmax=300, acquire=None,
                 acquire_filter='N', dither=0.0, block_devices=('T0', 'W0')):
        super().__init__()
        self.filters = filters
        self.acquire, self.acquire_filter = acquire, acquire_filter
        self.dither, self.block_devices = dither, tuple(block_devices)
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

    def blocked(self):
        """True while a device of block_devices blocks exposures."""
        for dev in self.block_devices:
            try:
                if self.getState(dev) & BOP_EXPOSURE:
                    return True
            except scriptcomm.Rts2NotActive:
                raise
            except Exception:
                continue
        return False

    def wait_unblocked(self, timeout=120):
        sleep(0.5)          # let the devices take the changes just sent
        end = time() + timeout
        while self.blocked():
            if time() > end:
                self.log('W', f"{self.name}: exposures still blocked after {timeout}s, exposing anyway")
                return
            sleep(0.5)

    def offset(self):
        """Random dither, waiting until T0 has applied it."""
        dx, dy = (random.uniform(-self.dither, self.dither) for _ in range(2))
        self.setValue('woffs', f'{dx:.6f} {dy:.6f}', 'T0')
        end = time() + 30
        while time() < end:
            try:
                offs = [float(v) for v in (self.getValue('OFFS', 'T0') or '').split()[:2]]
                if len(offs) == 2 and abs(offs[0] - dx) < 1e-5 and abs(offs[1] - dy) < 1e-5:
                    return
            except ValueError:
                pass
            sleep(0.2)
        self.log('W', f"{self.name}: T0 did not report offset {dx:.4f} {dy:.4f} in 30s")

    def take(self, f, t, dither=False):
        """One exposure in filter f for t s; True if an image came."""
        self.setValue('filter', f)
        self.setValue('exposure', t)
        if dither:
            self.offset()
        self.wait_unblocked()
        try:
            image = self.exposure()
        except scriptcomm.Rts2NotActive:
            raise
        except Exception as e:
            self.log('E', f"{self.name}: exposure in {f} failed: {e}")
            sleep(5)
            return False
        self.process(image)
        return True

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
        """Cycles until the executor wants another target; images taken."""
        n = 0
        self.wait_for_tracking()
        if self.acquire:
            count, t = self.acquire
            self.log('I', f"{self.name}: acquisition {count}x{t:g}s in {self.acquire_filter}")
            for _ in range(count):
                if self.next_is_other():
                    return n
                n += self.take(self.acquire_filter, t)
        while True:
            for f, t in self.plan().items():
                if self.next_is_other():
                    self.log('I', f"{self.name}: next target queued, ending after {n} images")
                    return n
                n += self.take(f, t, dither=self.dither > 0)


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
    parser.add_argument("--acquire", help="unfiltered frames before the cycles, NxT, e.g. 3x10")
    parser.add_argument("--acquire-filter", default="N", help="filter of the acquisition frames")
    parser.add_argument("--dither", type=float, default=0.0,
                        help="random offset of each science frame, up to this many deg in RA and Dec")
    parser.add_argument("--block-devices", default="T0,W0",
                        help="devices whose exposure block an exposure waits for")
    args = parser.parse_args()
    if args.acquire:
        try:
            count, t = args.acquire.lower().split('x')
            args.acquire = (int(count), positive_float(t))
        except ValueError:
            parser.error("--acquire takes NxT, e.g. 3x10")
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
                   tmin=args.tmin, tmax=args.tmax, acquire=args.acquire,
                   acquire_filter=args.acquire_filter, dither=args.dither,
                   block_devices=[d for d in args.block_devices.split(',') if d])
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
