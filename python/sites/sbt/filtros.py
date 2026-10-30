#!/usr/bin/python3
"""Perform synchronized observations with two cameras so that the
cameras exchange filters synchronously"""

from time import time, sleep
from math import ceil
import argparse
import os

from rts2 import scriptcomm
# the calculator from this checkout, which the script itself comes from; the
# installed rts2 package may carry an older snrexp
_snrexp = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', '..', 'rts2', 'snrexp.py')
if os.path.exists(_snrexp):
    import importlib.util
    _spec = importlib.util.spec_from_file_location('snrexp', _snrexp)
    snrexp = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(snrexp)
else:
    from rts2 import snrexp

class Filtros(scriptcomm.Rts2Comm):
    """Perform synchronized observations with two cameras with synchronized filter exchange.

    This class handles the coordination of multiple cameras, each with their own
    filter wheel, ensuring synchronized exposures and filter changes.

    Attributes:
        exptime (float): Exposure time in seconds (the longest one with --snr)
        tick (float): Total cycle time (exposure + overhead)
        snr (float): Requested SNR; None for a fixed exposure time
        exptimes (dict): 'camera/filter' -> exposure time, with --snr
        camera (str): Camera identifier (C1, C2, or C3)
        narrow (bool): Flag for narrow-band mode
        name (str): Identifier string for logging
    """

    # Class-level constants
    OVERHEAD_TIME = 15  # Overhead time in seconds
    FILTER_MAPS = {
        'wide': {
            'C1': ['i', 'r'],    # C1 alternates between i and r
            'C2': ['r', 'g'],    # C2 alternates between r and g
            'C3': ['B']          # C3 stays on B
        },
        'narrow': {
            'C1': ['halpha', 'sii'], # Narrow band filters for C1
            'C2': ['oiii', 'B'], # Narrow band filters for C2
            'C3': ['I']          # C3 stays on I (but typically in narrow mode it would be guiding)
        }
    }

    def __init__(self, exptime=120, narrow=False, snr=None, mag=None, fallback_mag=None,
                 mag_sources=('atlas', 'lcgen'), tmin=5, tmax=300):
        """Initialize the Filtros instance.

        Args:
            exptime (float, optional): Exposure time in seconds. Defaults to 120.
            narrow (bool, optional): Whether to use narrow-band filters. Defaults to False.
            snr (float, optional): Requested SNR; the exposure time of each filter
                is then computed from the current conditions (COND) instead.
            mag (str, optional): With snr: target magnitude, '17.5' or 'r=17.5,g=18.1';
                otherwise taken from mag_sources (local ATLAS, lcgen on hog).
            fallback_mag (float, optional): Magnitude when no source knows the target.
            mag_sources (tuple, optional): Where to look the magnitude up, in order.
            tmin, tmax (float, optional): Limits of the computed exposure times.

        Raises:
            ValueError: If exptime is not positive
            RuntimeError: If camera identification fails
        """
        # Initialize parent class first
        super().__init__()

        # Validate exposure time
        if exptime <= 0:
            raise ValueError("Exposure time must be positive")

        # Initialize basic attributes
        self.exptime = exptime
        self.tick = self.OVERHEAD_TIME + self.exptime
        self.narrow = narrow
        self.snr = snr
        # with snr, the exposure of filters without any magnitude
        self.fallback_t = exptime if snr else None
        self.mag = mag
        self.fallback_mag = fallback_mag
        self.mag_sources = tuple(mag_sources)
        self.tmin, self.tmax = tmin, tmax
        self.exptimes = None

        # Get and validate camera
        try:
            self.camera = self.getRunDevice()
            if self.camera not in self.FILTER_MAPS['wide']:
                raise RuntimeError(f"Unknown camera identifier: {self.camera}")
        except Exception as e:
            raise RuntimeError(f"Failed to identify camera: {str(e)}")

        # Set name based on mode and camera
        self.name = self._generate_name()

        # Our target, to notice when the executor wants another one
        self.target = self.getValueInteger('current', 'EXEC')
        self.stop = False

        # Log initialization
        if self.snr:
            self.log('I', f"{self.name}: Initialized with snr={self.snr} mag={self.mag}, "
                         f"narrow={self.narrow}, camera={self.camera}")
        else:
            self.log('I', f"{self.name}: Initialized with exptime={self.exptime}s, "
                         f"narrow={self.narrow}, camera={self.camera}")

    def _generate_name(self):
        """Generate the identifier name for this instance.

        Returns:
            str: Formatted name string
        """
        mode = 'n' if self.narrow else 'w'
        return f"Filtros/{mode}/{self.camera}"

    def next_is_other(self):
        """Check whether the executor has another target queued as next.

        Returns:
            bool: True if EXEC next is set to a target other than ours
        """
        if self.target is None:
            return False
        next_t = self.getValueInteger('next', 'EXEC')
        return next_t is not None and next_t >= 0 and next_t != self.target

    def get_current_filters(self):
        """Get the filter set for current mode and camera.

        Returns:
            list: List of available filters for this camera in current mode
        """
        mode = 'narrow' if self.narrow else 'wide'
        return self.FILTER_MAPS[mode][self.camera]

    def set_filter(self, pulse):
        """Set filter based on pulse count and current mode.

        Args:
            pulse (int): Current pulse count

        Returns:
            int: 0 for success, -1 for error

        Raises:
            IOError: If filter cannot be set
        """
        filters = self.get_current_filters()

        if len(filters) == 1:
            # Single filter mode (e.g., C3)
            filter_value = filters[0]
        else:
            # Alternating filters mode
            filter_value = filters[pulse % len(filters)]

        try:
            self.setValue('filter', filter_value)
            if self.exptimes:
                t = self.exptimes[f'{self.camera}/{snrexp.long_filter(filter_value)}']
                self.setValue('exposure', t)
                self.log('I', f"{self.name}: Set filter to {filter_value}, exposure {t:g}s")
            else:
                self.log('I', f"{self.name}: Set filter to {filter_value}")
            return 0
        except scriptcomm.Rts2NotActive:
            raise
        except Exception as e:
            self.log('E', f"{self.name}: Failed to set filter: {str(e)}")
            raise IOError(f"Failed to set filter to {filter_value}: {str(e)}")

    def switch_mode(self, narrow=None):
        """Switch between wide and narrow band modes.

        Args:
            narrow (bool, optional): If provided, set to this mode.
                                   If None, toggle current mode.
        """
        if narrow is None:
            self.narrow = not self.narrow
        else:
            self.narrow = narrow

        self.name = self._generate_name()
        self.log('I', f"{self.name}: Switched to {'narrow' if self.narrow else 'wide'} band mode")

    def target_magnitudes(self, filters):
        """Target magnitude per filter (long names): --mag, ATLAS, lcgen, --fallback-mag.

        Returns:
            dict: filter -> (magnitude, origin); filters without one are left out
        """
        ra, dec = snrexp.target_position(self)
        log = lambda level, text: self.log(level, f"{self.name}: {text}")
        mags = snrexp.target_mags(filters, ra, dec, snrexp.parse_mag(self.mag), self.mag_sources,
                                  self.fallback_mag, log)
        for f in filters:
            if f not in mags:
                log('W', f"no magnitude for {f}, exposing {min(self.fallback_t or self.tmax, self.tmax):g}s")
        return mags

    def plan_exposures(self):
        """Exposure time per filter for the requested SNR, shared with the other cameras.

        The cameras exchange filters on a common tick, so the scripts of one
        observation must agree on it: the first one computes the times of all
        the synchronised cameras (all but C3) and the others read its plan.
        C3 follows that tick with times of its own, capped to fit in it.
        """
        mode = 'narrow' if self.narrow else 'wide'
        groups = {c: [snrexp.long_filter(f) for f in fl]
                  for c, fl in self.FILTER_MAPS[mode].items() if c != 'C3'}
        own = [snrexp.long_filter(f) for f in self.get_current_filters()]
        reader = lambda: snrexp.CondReader('SBT', lambda name: self.getValue(name, 'COND'))

        def compute():
            mags = self.target_magnitudes(sorted({f for fl in groups.values() for f in fl}))
            return snrexp.make_plan(groups, mags, self.snr, snrexp.target_airmass(self), reader(),
                                    self.tmin, self.tmax, self.OVERHEAD_TIME, self.fallback_t,
                                    snrexp.twilight(self))

        obsid = self.getValueInteger('obsid', 'EXEC')
        key = f'{obsid}-{mode}' if obsid is not None and obsid >= 0 else f'pid{os.getpid()}'
        plan = snrexp.shared_plan(key, compute)
        self.tick = plan['tick']
        self.exptimes = dict(plan['exptime'])
        if self.camera not in groups:
            mine = snrexp.make_plan({self.camera: own}, self.target_magnitudes(own), self.snr,
                                    snrexp.target_airmass(self), reader(), self.tmin,
                                    max(min(self.tmax, self.tick - self.OVERHEAD_TIME), self.tmin),
                                    fallback_t=self.fallback_t, sun=snrexp.twilight(self))
            self.exptimes.update(mine['exptime'])
            plan['info'].update(mine['info'])
        self.exptime = max(self.exptimes[f'{self.camera}/{f}'] for f in own)
        self.log('I', f"{self.name}: plan {key} ({'shared' if plan['shared'] else 'computed'}): "
                      f"SNR {plan['snr']:g} X={plan['airmass']:.2f} tick {self.tick:g}s")
        for k, i in sorted(plan['info'].items()):
            if k.split('/')[0] == self.camera or not plan['shared']:
                self.log('I', f"{self.name}:   {snrexp.describe(k, i)}")

    def run(self, num_requested):
        """Execute a sequence of timed exposures with synchronized filter changes.

        Performs exposures at regular intervals, changing filters between exposures
        according to the camera's configuration. Continues until either the requested
        number of exposures is achieved or the mount moves.

        Args:
            num_requested (int): Number of exposures to take

        Returns:
            int: Number of successful exposures completed

        Raises:
            ValueError: If telescope parameters cannot be retrieved
            IOError: If camera or mount communication fails
        """
        def wait_for_telescope_ready():
            """Wait for telescope to finish slewing and be on target.

            Returns:
                float: Base time for synchronization

            Raises:
                TimeoutError: If telescope doesn't reach tracking state within timeout
            """
            # Wait for telescope to reach tracking state (timeout 300 seconds)
            tel_name = 'T0'

            # fail fast and with a clear reason rather than silently
            # burning the full 300s timeout below waiting for a tracking
            # state that a parked mount will never reach on its own
            if self.isParked(tel_name):
                raise TimeoutError(f"{self.name}: telescope is parked, not waiting for tracking")

            self.log('I', f"{self.name}: Waiting for telescope to reach tracking state...")
            ret = self.waitMask(tel_name, scriptcomm.TEL_TRACKING, 300)
            self.log('I', f"{self.name}: Telescope tracking, state=0x{ret:03x}")

            # Optional: add small delay for any final settling
            sleep(2)

            # Use current time as base since telescope is already ready
            base_time = ceil(time())
            self.log('I', f"{self.name}: Telescope ready. Base time: {base_time}")
            return base_time

        def prepare_camera():
            """Configure camera for the observation sequence.

            Raises:
                IOError: If camera configuration fails
            """
            try:
                self.setValue('SHUTTER', 'LIGHT')
                # in case we are exposing, do not wait
                # dangerous!
                # self.sendCommand('stopexpo');
                if self.snr:
                    # before the wait for tracking, so that the other
                    # cameras' scripts find the plan ready and start on the
                    # same pulse
                    self.plan_exposures()
                else:
                    self.setValue('exposure', self.exptime)
                self.set_filter(0)  # Set initial filter (and its exposure)
            except scriptcomm.Rts2NotActive:
                raise
            except Exception as e:
                self.log('E', f"{self.name}: Failed to prepare camera: {str(e)}")
                raise IOError(f"Camera preparation failed: {str(e)}")

        def calculate_next_exposure_delay(base_time):
            """Calculate delay until next exposure should start.

            Args:
                base_time (float): Reference time for synchronization

            Returns:
                tuple: (delay_seconds, pulse_number)
            """
            current_time = time() - base_time
            pulse = ceil(current_time / self.tick)
            next_start = pulse * self.tick
            delay = max(next_start - current_time, 0)

            return delay, pulse

        def check_mount_movement(base_time):
            """Check if mount has moved since observation started.

            Args:
                base_time (float): Reference time for synchronization

            Returns:
                bool: True if mount has moved, False otherwise
            """
            try:
                move_end = self.getValueFloat('move_end', 'T0')
                last_move = self.getValueFloat('move_started', 'T0')
                return last_move > move_end + 1
            except scriptcomm.Rts2NotActive:
                raise
            except Exception as e:
                self.log('W', f"{self.name}: Error checking mount movement: {str(e)}")
                return False  # Continue if we can't check movement

        # Main execution
        try:
            num_completed = 0
            prepare_camera()
            base_time = wait_for_telescope_ready()

            while num_completed < num_requested:
                # Calculate timing for next exposure
                delay, pulse = calculate_next_exposure_delay(base_time)
                self.log('I', f"{self.name}: Waiting {delay:.1f}s until next exposure")

                # Wait and take exposure
                sleep(delay)

                # Executor wants another target - end here, at an exposure boundary
                if self.next_is_other():
                    self.log('I', f"{self.name}: next target queued, ending sequence")
                    self.stop = True
                    break

                self.log('I', f"{self.name}: Starting exposure {num_completed + 1}/{num_requested:g}")

                try:
                    image = self.exposure()
                    self.log('I', f"{self.name}: Exposure completed: {image}")
                    self.process(image)
                    num_completed += 1
                except scriptcomm.Rts2NotActive:
                    raise
                except Exception as e:
                    self.log('E', f"{self.name}: Exposure failed: {str(e)}")
                    continue

                # Prepare for next exposure
                self.set_filter(pulse + 1)

                # Check if mount has moved
                if check_mount_movement(base_time):
                    self.log('I', f"{self.name}: Mount movement detected, ending sequence")
                    break

            return num_completed

        except TimeoutError as e:
            self.log('E', f"{self.name}: {str(e)}")
            return 0
        except scriptcomm.Rts2NotActive:
            raise
        except Exception as e:
            self.log('E', f"{self.name}: Run failed: {str(e)}")
            raise

def parse_arguments():
    """Parse and validate command line arguments.

    Returns:
        argparse.Namespace: Parsed command line arguments

    Raises:
        ArgumentTypeError: If exposure time is not positive
    """
    def positive_float(value):
        """Validate that exposure time is positive."""
        fvalue = float(value)
        if fvalue <= 0:
            raise argparse.ArgumentTypeError(f"{value} must be positive")
        return fvalue

    parser = argparse.ArgumentParser(
        description="Perform synchronized observations with filter exchange",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    parser.add_argument(
        "-e", "--exptime",
        type=positive_float,
        default=None,
        help="exposure time [s] (120); with --snr, the one of filters without any magnitude (300, at most --tmax)"
    )

    parser.add_argument(
        "-s", "--snr",
        type=positive_float,
        help="exposure time of each filter for this SNR, from the current conditions (COND)"
    )

    parser.add_argument(
        "--magerr",
        type=positive_float,
        help="as --snr, for this magnitude error"
    )

    parser.add_argument(
        "-m", "--mag",
        help="with --snr: target magnitude, '17.5' or 'r=17.5,g=18.1' (else --mag-source)"
    )

    parser.add_argument(
        "--mag-source",
        default="atlas,lcgen",
        help="with --snr and no --mag: where to look the target magnitude up, in order"
    )

    parser.add_argument(
        "-f", "--fallback-mag",
        type=float,
        help="with --snr: magnitude when no source knows the target"
    )

    parser.add_argument(
        "--tmin",
        type=positive_float,
        default=5.0,
        help="with --snr: shortest exposure"
    )

    parser.add_argument(
        "--tmax",
        type=positive_float,
        default=None,
        help="with --snr: longest exposure, the SNR may then not be reached "
             "(120 s, with -u 300 s)"
    )

    parser.add_argument(
        "-n", "--number",
        type=int,
        default=None,
        help="number of exposures to obtain (default: until EXEC has another target as next)"
    )

    parser.add_argument(
        "-u", "--narrow",
        action="store_true",
        help="take halpha/oiii/sii/B instead of gri"
    )

    args = parser.parse_args()
    if args.magerr:
        args.snr = snrexp.MAGERR_K / args.magerr
    # with --snr, -e is the exposure of filters without any magnitude
    if not args.snr and not args.exptime:
        args.exptime = 120.0
    return args

if __name__ == '__main__':
    try:
        # Parse command line arguments
        args = parse_arguments()

        # Initialize Filtros with parsed arguments
        filtros = Filtros(exptime=args.exptime or (300.0 if args.snr else 120.0),
                          narrow=args.narrow, snr=args.snr,
                          mag=args.mag, fallback_mag=args.fallback_mag,
                          mag_sources=[s for s in args.mag_source.split(',') if s],
                          tmin=args.tmin, tmax=args.tmax or (300.0 if args.narrow else 120.0))

        # without -n: until the executor wants another target (next_is_other)
        total = num = args.number if args.number else float('inf')
        if args.snr:
            filtros.log('I', f"{filtros.name}: starts snr:{args.snr:g} mag:{args.mag or args.mag_source} num:{num}")
        else:
            filtros.log('I', f"{filtros.name}: starts exp:{filtros.exptime} num:{num}")

        while num > 0 and not filtros.stop:
            done = filtros.run(num)
            num -= done
            if done == 0:
                # nothing taken (parked, failing camera): do not spin, and
                # do not hold the target once another one is queued
                if filtros.next_is_other():
                    break
                sleep(10)

        filtros.log('I', f"{filtros.name}: ended with {total - num:g} images done")

    except scriptcomm.Rts2NotActive:
        # executor ended our script (target change, kill) - just go
        exit(0)

    except argparse.ArgumentTypeError as e:
        print(f"Invalid argument: {str(e)}")
        exit(1)
    except ValueError as e:
        print(f"Value error: {str(e)}")
        exit(1)
    except Exception as e:
        print(f"Unexpected error occurred: {str(e)}")
        raise
