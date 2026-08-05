#!/usr/bin/python3
"""Perform synchronized observations with two cameras so that the
cameras exchange filters synchronously"""

from time import time, sleep
from math import ceil
import argparse

from rts2 import scriptcomm

class Filtros(scriptcomm.Rts2Comm):
    """Perform synchronized observations with two cameras with synchronized filter exchange.

    This class handles the coordination of multiple cameras, each with their own
    filter wheel, ensuring synchronized exposures and filter changes.

    Attributes:
        exptime (float): Exposure time in seconds
        tick (float): Total cycle time (exposure + overhead)
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

    def __init__(self, exptime=120, narrow=False):
        """Initialize the Filtros instance.

        Args:
            exptime (float, optional): Exposure time in seconds. Defaults to 120.
            narrow (bool, optional): Whether to use narrow-band filters. Defaults to False.

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

        # Get and validate camera
        try:
            self.camera = self.getRunDevice()
            if self.camera not in self.FILTER_MAPS['wide']:
                raise RuntimeError(f"Unknown camera identifier: {self.camera}")
        except Exception as e:
            raise RuntimeError(f"Failed to identify camera: {str(e)}")

        # Set name based on mode and camera
        self.name = self._generate_name()

        # Log initialization
        self.log('I', f"{self.name}: Initialized with exptime={self.exptime}s, "
                     f"narrow={self.narrow}, camera={self.camera}")

    def _generate_name(self):
        """Generate the identifier name for this instance.

        Returns:
            str: Formatted name string
        """
        mode = 'n' if self.narrow else 'w'
        return f"Filtros/{mode}/{self.camera}"

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
            self.log('I', f"{self.name}: Set filter to {filter_value}")
            return 0
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
                self.setValue('exposure', self.exptime)
                self.set_filter(0)  # Set initial filter
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
                self.log('I', f"{self.name}: Starting exposure {num_completed + 1}/{num_requested}")

                try:
                    image = self.exposure()
                    self.log('I', f"{self.name}: Exposure completed: {image}")
                    self.process(image)
                    num_completed += 1
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
        default=120.0,
        help="exposure time of single exposures in seconds"
    )

    parser.add_argument(
        "-n", "--number",
        type=int,
        default=10,
        help="number of exposures to obtain"
    )

    parser.add_argument(
        "-u", "--narrow",
        action="store_true",
        help="take halpha/oiii/sii/B instead of gri"
    )

    return parser.parse_args()

if __name__ == '__main__':
    try:
        # Parse command line arguments
        args = parse_arguments()

        # Initialize Filtros with parsed arguments
        filtros = Filtros(exptime=args.exptime, narrow=args.narrow)

        num = args.number
        filtros.log('I', f"{filtros.name}: starts exp:{filtros.exptime} num:{num}")

        while num > 0:
            num -= filtros.run(num)

        filtros.log('I', f"{filtros.name}: ended with {num} images done")

    except argparse.ArgumentTypeError as e:
        print(f"Invalid argument: {str(e)}")
        exit(1)
    except ValueError as e:
        print(f"Value error: {str(e)}")
        exit(1)
    except Exception as e:
        print(f"Unexpected error occurred: {str(e)}")
        raise
