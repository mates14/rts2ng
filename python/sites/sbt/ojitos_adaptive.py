#!/usr/bin/python3
"""
Adaptive GRB observing script for SBT dual-camera system.
Extends ojitos.py with 5-level error box adaptive strategy.

IMPORTANT: Error box values are RADIUS (not diameter).

Strategy levels (based on errorbox RADIUS):
1. radius < 0.25° (15'): FLIP/FLAP windowed 1024x1024, fast cadence, 5s exposures → 5s system cadence!
2. radius ≤ 1.5°: Standard ojitos flip/flap full frame, 12s exposures (fits in effective field)
3. 1.5° < radius ≤ 3.125°: 2×2 mosaic, 60s exposures (covers up to 6.25° diameter)
4. 3.125° < radius ≤ 4.8°: 3×3 mosaic, 60s exposures (covers up to 9.5° diameter)
5. Late follow-up (>20min): Full frame, synchronous dual-camera, 120s exposures (manual selection)

FOV: 3.5° diameter (4096x4096 pixels, 3.14 arcsec/pixel)
Effective field radius: 1.5° (with 0.25° margins)
Readout times: Full 12s, 1024x1024 ~2.9s, 512x512 ~1.8s
"""

import time
import sys
import sys
sys.path.insert(0, '/home/mates/src/rts2/python')
from rts2.scriptcomm import Rts2Comm

# Try to import our database query module
try:
    from grb_query import get_grb_info
    HAS_DB_QUERY = True
except ImportError:
    HAS_DB_QUERY = False


class OjitosAdaptive(Rts2Comm):
    """Adaptive GRB observing with dual synchronized cameras"""

    # Constants for SBT telescope (precise measurements)
    PIXEL_SCALE = 3.14           # arcsec/pixel
    CHIP_SIZE = 4096             # pixels

    # Full FOV: 4096 × 3.14 / 3600 = 3.5726°
    FOV_DEGREES = 3.5726         # Full CCD field of view diameter
    FOV_RADIUS = 1.7863          # Full FOV radius (3.5726 / 2)

    # Effective FOV with standard margins
    FOV_EFFECTIVE = 3.5157       # Effective working FOV (leaves 205px margin for 5° errorbox edge case)
    EFFECTIVE_RADIUS = 1.7578    # Effective radius (3.5157 / 2) for mosaic step calculations

    # Note: For 5° errorbox (10° diameter coverage), 3×3 mosaic uses 205 pixel margins:
    # ((3.14 × 4096 × 3) - 10 × 3600) / 3.14 / 4 = 205.76 pixels per edge

    # Strategy thresholds (ERROR BOX RADIUS in degrees)
    THRESHOLD_WINDOW = 0.25     # < 15 arcmin → windowing
    THRESHOLD_STANDARD = 1.5    # ≤ 1.5° radius → standard flip/flap (fits in effective field)
    THRESHOLD_2x2 = 3.125       # ≤ 3.125° radius → 2×2 mosaic (max coverage 6.25° diameter)
    THRESHOLD_MAX = 5.0         # ≤ 5.0° radius → 3×3 mosaic (max coverage ~9.5° diameter with margin squeeze)

    # Late follow-up threshold (in seconds)
    LATE_FOLLOWUP_AGE = 20 * 60  # 20 minutes - use Strategy 5 for old triggers
    ENABLE_LATE_FOLLOWUP = True  # Set to True to enable auto-selection of Strategy 5

    def __init__(self):
        # Initialize parent class first - CRITICAL!
        Rts2Comm.__init__(self)

        # Now initialize our attributes
        self.exptime_windowed = 5   # Fast windowed exposure for strategy 1 (flip/flap)
        self.exptime = 12           # Standard exposure time for strategy 2
        self.exptime_mosaic = 60    # Longer exposure for mosaic strategies 3 & 4
        self.exptime_late = 120     # Long exposure for strategy 5 (late follow-up, >20min)
        #self.exptime_mosaic = 6    # for testing
        self.tick = 2 * self.exptime
        self.tick_windowed = 2 * self.exptime_windowed  # For strategy 1 timing (10s)
        self.parity = False
        self.strategy = None
        self.errorbox = None        # Errorbox RADIUS in degrees
        self.grb_info = None        # Full GRB info from database
        self.trigger_age = None     # Age of trigger in seconds
        self.mosaic_positions = []
        # For mosaic callback
        self.next_offset_ra = None
        self.next_offset_dec = None
        self.return_to_center = False

    def get_grb_params(self):
        """Query database for GRB parameters (errorbox, trigger time, etc.)"""
        try:
            # Get target ID from executor - EXEC.current contains currently executed target ID
            tar_id_str = self.getValue('current', 'EXEC')

            if tar_id_str is None or tar_id_str == 'ERR':
                self.log('W', 'adoji: Could not get current target ID from EXEC')
                return None

            tar_id = int(tar_id_str)

            # Check for invalid target ID (no target to observe)
            if tar_id < 0:
#                self.log('I', f'adoji: Invalid target ID ({tar_id}), no target to observe')
                return -1  # Special value to indicate no target

            self.log('I', f'adoji: Querying GRB info for tar_id={tar_id}')

            if not HAS_DB_QUERY:
                self.log('W', 'adoji: grb_query module not available, using fallback')
                return None

            # Get full GRB info including trigger time
            self.grb_info = get_grb_info(tar_id=tar_id)

            if self.grb_info is None:
                self.log('W', 'adoji: Could not retrieve GRB info from database')
                return None

            errorbox = self.grb_info['grb_errorbox']
            grb_date = self.grb_info['grb_date']

            if errorbox is None:
                self.log('W', 'adoji: No errorbox in database')
                return None

            # Calculate trigger age
            if grb_date is not None:
                import time
                self.trigger_age = time.time() - grb_date
                age_min = self.trigger_age / 60.0
                self.log('I', f'adoji: GRB errorbox radius: {errorbox:.4f}°, trigger age: {age_min:.1f} min')
            else:
                self.trigger_age = None
                self.log('I', f'adoji: GRB errorbox radius: {errorbox:.4f}° (trigger time unknown)')

            return errorbox

        except Exception as e:
            self.log('E', f'adoji: Error querying GRB info: {e}')
            return None

    def determine_strategy(self):
        """
        Determine observing strategy based on error box RADIUS and trigger age.

        Returns:
            int: Strategy number (1-5), or 2 (default) if errorbox unknown
        """
        if self.errorbox is None:
            self.log('W', 'adoji: Unknown errorbox, using default strategy 2')
            return 2

        # Check for late follow-up: old trigger + fits in FOV → long exposures
        if (self.ENABLE_LATE_FOLLOWUP and
            self.trigger_age is not None and
            self.trigger_age > self.LATE_FOLLOWUP_AGE and
            self.errorbox <= self.THRESHOLD_STANDARD):
            age_min = self.trigger_age / 60.0
            self.log('I', f'adoji: Strategy 5 LATE FOLLOW-UP (trigger age {age_min:.1f} min > {self.LATE_FOLLOWUP_AGE/60} min, errorbox {self.errorbox:.3f}° fits in FOV)')
            return 5

        # Standard strategy selection based on errorbox size
        if self.errorbox < self.THRESHOLD_WINDOW:
            self.log('I', f'adoji: Strategy 1 FLIP/FLAP WINDOWED (errorbox radius {self.errorbox:.3f}° < {self.THRESHOLD_WINDOW}°)')
            return 1
        elif self.errorbox <= self.THRESHOLD_STANDARD:
            self.log('I', f'adoji: Strategy 2 STANDARD flip/flap (errorbox radius {self.errorbox:.3f}° ≤ {self.THRESHOLD_STANDARD}°)')
            return 2
        elif self.errorbox <= self.THRESHOLD_2x2:
            self.log('I', f'adoji: Strategy 3 2×2 MOSAIC (errorbox radius {self.errorbox:.3f}° ≤ {self.THRESHOLD_2x2}°)')
            return 3
        elif self.errorbox <= self.THRESHOLD_MAX:
            self.log('I', f'adoji: Strategy 4 3×3 MOSAIC (errorbox radius {self.errorbox:.3f}° ≤ {self.THRESHOLD_MAX}°)')
            return 4
        else:
            self.log('W', f'adoji: Errorbox radius {self.errorbox:.3f}° > {self.THRESHOLD_MAX}° - too large, using strategy 2')
            return 2

    def calculate_mosaic_offsets(self, grid_size):
        """
        Calculate mosaic offset positions to cover error box RADIUS.

        Uses effective FOV (3.5157°) for proper margin distribution.
        For 5° errorbox: 3×3 mosaic with 205 pixel margins on edges.

        Formulas:
          - 2×2: step = 2*R - FOV_EFFECTIVE  (covers diameter 2R up to ~6.25°)
          - 3×3: step = R - EFFECTIVE_RADIUS  (covers diameter 2R up to 10°)

        Where:
          R = errorbox radius
          FOV_EFFECTIVE = 3.5157° (working field with margins)
          EFFECTIVE_RADIUS = 1.7578° (half of effective FOV)

        Args:
            grid_size: 2 for 2x2, 3 for 3x3

        Returns:
            List of (ra_offset, dec_offset) tuples in degrees (RA in coordinate degrees)
        """
        # Get current declination for RA correction
        try:
            tel_coords = self.getValue('TAR', 'T0')
            if tel_coords and tel_coords != 'ERR':
                import math
                coords = tel_coords.split()
                dec_deg = float(coords[1])  # Index 1 is Dec
                cos_dec = math.cos(math.radians(dec_deg))
                # Avoid division by zero near poles
                if cos_dec < 0.1:
                    cos_dec = 0.1
            else:
                cos_dec = 1.0  # Fallback: assume equator
        except:
            cos_dec = 1.0  # Fallback: assume equator

        # Calculate step size based on errorbox RADIUS
        if grid_size == 2:
            # 2×2 mosaic: positions at ±step/2
            # Coverage = step + FOV_EFFECTIVE = 2*R (errorbox diameter)
            # So: step = 2*R - FOV_EFFECTIVE
            step = 2.0 * self.errorbox - self.FOV_EFFECTIVE
            if step < 0:
                step = 0  # Overlapping coverage for small errorboxes
        elif grid_size == 3:
            # 3×3 mosaic: positions at -step, 0, +step
            # Coverage = 2*step + FOV_EFFECTIVE = 2*R (errorbox diameter)
            # So: step = R - EFFECTIVE_RADIUS
            step = self.errorbox - self.EFFECTIVE_RADIUS
            if step < 0:
                step = 0
        else:
            self.log('E', f'adoji: Unsupported grid size: {grid_size}')
            step = 0

        # RA step in coordinate degrees (corrected for declination)
        step_ra = step / cos_dec

        positions = []

        if grid_size == 2:
            # 2×2 grid: positions at ±step/2 along each axis
            half_step_ra = step_ra / 2.0
            half_step_dec = step / 2.0
            for i in [-1, 1]:
                for j in [-1, 1]:
                    ra_offset = i * half_step_ra
                    dec_offset = j * half_step_dec
                    positions.append((ra_offset, dec_offset))

        elif grid_size == 3:
            # 3×3 grid: positions at -step, 0, +step along each axis
            for i in [-1, 0, 1]:
                for j in [-1, 0, 1]:
                    ra_offset = i * step_ra
                    dec_offset = j * step
                    positions.append((ra_offset, dec_offset))

        # Calculate expected coverage
        if grid_size == 2:
            coverage_radius = (step + self.FOV_EFFECTIVE) / 2.0
        elif grid_size == 3:
            coverage_radius = (2.0 * step + self.FOV_EFFECTIVE) / 2.0
        else:
            coverage_radius = 0

        self.log('I', f'adoji: {grid_size}×{grid_size} mosaic: {len(positions)} positions, step={step:.3f}° (sky)')
        self.log('I', f'adoji:   RA step={step_ra:.3f}° (coord), Dec step={step:.3f}° (cos(dec)={cos_dec:.3f})')
        self.log('I', f'adoji:   Errorbox radius: {self.errorbox:.3f}°, Coverage radius: {coverage_radius:.3f}°')

        return positions

    def waitIdle2(self):
        """Wait for telescope to reach target position (from original ojitos.py)"""
        start = time.time()

        while True:
            now = time.time()
            target_distance = self.getValueFloat('target_distance', 'T0')

            if target_distance < 1.0/60:  # within 1 arcmin
                self.log('I', f'adoji: td={target_distance:.3f}')
                time.sleep(1.5)
                break

            if now > start + 120:  # 2 minute timeout
                self.log('W', f'adoji: timeout waiting for telescope')
                break

            time.sleep(0.25)

    def setup_camera_window(self, window_size):
        """
        Set camera to windowed mode for fast readout.

        Args:
            window_size: 512 or 1024 pixels
        """
        camera = self.getRunDevice()

        # Calculate centered window
        center = self.CHIP_SIZE // 2
        x0 = center - window_size // 2
        y0 = center - window_size // 2

        self.log('I', f'adoji: Setting WINDOW {x0} {y0} {window_size} {window_size}')

        try:
            # WINDOW is a VALUE, not a command - use setValue()
            window_str = f'{x0} {y0} {window_size} {window_size}'
            self.setValue('WINDOW', window_str)
            time.sleep(0.5)  # Allow setting to propagate
            return True
        except Exception as e:
            self.log('E', f'adoji: Failed to set window: {e}')
            return False

    def clear_camera_window(self):
        """Clear windowed mode, return to full frame"""
        try:
            # Set WINDOW to -1 to return to full frame
            self.setValue('WINDOW', '-1')
            self.log('I', f'adoji: Cleared WINDOW, using full frame')
            time.sleep(0.5)
        except Exception as e:
            self.log('E', f'adoji: Failed to clear window: {e}')

    def mosaic_callback(self):
        """
        Callback method for setting next mosaic offset during readout.
        Uses instance variables set before each exposure.
        """
        if self.return_to_center:
            self.setValue('WOFFS', '0.0 0.0', 'T0')
            self.log('I', 'adoji: Callback - returning to center')
        elif self.next_offset_ra is not None and self.next_offset_dec is not None:
            offset_str = f'{self.next_offset_ra:.4f} {self.next_offset_dec:.4f}'
            self.setValue('WOFFS', offset_str, 'T0')
            self.log('I', f'adoji: Callback - set next offset ({self.next_offset_ra:.3f},{self.next_offset_dec:.3f})')

    def offset_telescope(self, ra_offset, dec_offset):
        """
        Apply offset to telescope position for mosaic.

        Uses T0.WOFFS with space-separated values: "ra dec" in degrees.

        Args:
            ra_offset: RA offset in degrees
            dec_offset: Dec offset in degrees
        """
        if abs(ra_offset) < 0.001 and abs(dec_offset) < 0.001:
            return  # No offset needed

        self.log('I', f'adoji: Applying telescope offset: RA {ra_offset:.3f}° Dec {dec_offset:.3f}°')

        try:
            # WOFFS is a VALUE: space-separated ra dec in degrees (like WINDOW)
            offset_str = f'{ra_offset:.4f} {dec_offset:.4f}'
            self.setValue('WOFFS', offset_str, 'T0')

            # Wait for telescope to slew to new position
            time.sleep(2)  # Brief pause for offset to be applied
            self.waitIdle2()
        except Exception as e:
            self.log('E', f'adoji: Failed to apply offset: {e}')

    def run_strategy_1_flipflap_windowed(self):
        """Strategy 1: Small errorbox - flip/flap with 1024x1024 windowing for fast cadence"""
        camera = self.getRunDevice()
        serial = self.getValue('CCD_SER')

        # Determine parity (camera role)
        if camera == "C2":
            self.parity = True
            mee = "adoji-s1/o"  # odd parity
        else:
            mee = "adoji-s1/x"  # even parity

        window_size = 1024
        readout_time = 2.9

        self.log('I', f'{mee}: Using {window_size}x{window_size} window (readout ~{readout_time}s)')

        # Setup windowing
        if not self.setup_camera_window(window_size):
            self.log('E', f'{mee}: Window setup failed, falling back to strategy 2')
            self.clear_camera_window()
            return self.run_strategy_2_standard()

        # Setup camera
        self.setValue('SHUTTER', 'LIGHT')
        self.setValue('exposure', self.exptime_windowed)

        #self.setValue('filter', 'clear')
        if camera == "C2":
            self.setValue('filter', 'r')
        else:
            self.setValue('filter', 'r')

        # Wait for telescope
        self.waitIdle2()

        zero = time.time()
        time.sleep(1)

        this_move_end = self.getValueFloat('move_end', 'T0')
        last_move_start = self.getValueFloat('move_started', 'T0')

        # Check if telescope timing values are valid
        if this_move_end is None or last_move_start is None:
            self.log('W', f"{mee} Telescope timing not available, cannot run strategy 1")
            self.clear_camera_window()
            return

        self.log('I', f"{mee} move end: {zero - this_move_end:.1f}s ago")
        self.log('I', f"{mee} move start: {zero - last_move_start:.1f}s ago")

        base = this_move_end
        if self.parity:
            base -= self.exptime_windowed

        firstrun = True
        last_move_start = 0.0

        # Synchronized imaging loop (flip/flap alternating)
        while last_move_start < this_move_end + 1:
            if self.parity and firstrun:
                firstrun = False
            else:
                self.log('I', f"{mee} Exposing {self.exptime_windowed}s (windowed)")
                try:
                    image = self.exposure()
                    self.process(image)
                except Exception as e:
                    self.log('E', f"{mee} Exposure failed: {e}")
                    break

            last_move_start = self.getValueFloat('move_started', 'T0')

            if not last_move_start < this_move_end + 1:
                break

            # Timing synchronization (using windowed tick)
            now = time.time() - base
            next_tick = (int(now/self.tick_windowed) + 1) * self.tick_windowed
            delay = next_tick - now

            self.log('I', f"{mee} Timing: now={now:.2f} next={next_tick:.2f} delay={delay:.2f}")

            if delay < 0:
                delay = 0
            time.sleep(delay)

        # Cleanup
        self.clear_camera_window()

    def run_strategy_5_late_followup(self):
        """Strategy 5: Long exposures for late follow-up (>20min after trigger)

        Full frame, synchronous dual-camera, 120s exposures.
        Maximizes sensitivity for fading GRB, minimizes readout noise.
        """
        camera = self.getRunDevice()
        mee = "adoji-s5"

        self.log('I', f'{mee}: Late follow-up mode - full frame, 120s exposures')

        # Setup - full frame, long exposures
        self.setValue('SHUTTER', 'LIGHT')
        self.setValue('exposure', self.exptime_late)

        #self.setValue('filter', 'clear')
        if camera == "C2":
            self.setValue('filter', 'g')
        else:
            self.setValue('filter', 'i')

        # Wait for telescope
        self.waitIdle2()

        this_move_end = self.getValueFloat('move_end', 'T0')
        last_move_start = 0.0

        # Check if telescope timing values are valid
        if this_move_end is None:
            self.log('W', f'{mee}: Telescope timing not available, cannot run strategy 5')
            return

        # Synchronous imaging loop (both cameras expose simultaneously)
        while last_move_start < this_move_end + 1:
            self.log('I', f'{mee}: Exposing {self.exptime_late}s (full frame, late follow-up)')

            try:
                image = self.exposure()
                self.process(image)
            except Exception as e:
                self.log('E', f'{mee}: Exposure failed: {e}')
                break

            last_move_start = self.getValueFloat('move_started', 'T0')

            if not last_move_start < this_move_end + 1:
                break

            # Wait between exposures (120s exp + ~12s readout = ~132s cycle)
            time.sleep(12)

    def run_strategy_2_standard(self):
        """Strategy 2: Standard ojitos flip/flap synchronized imaging (original code)"""
        camera = self.getRunDevice()
        serial = self.getValue('CCD_SER')

        # Determine parity (camera role)
        if camera == "C2":
            self.parity = True
            mee = "adoji-s2/o"  # odd parity
        else:
            mee = "adoji-s2/x"  # even parity

        # Setup
        self.setValue('SHUTTER', 'LIGHT')
        self.setValue('exposure', self.exptime)
        
        #self.setValue('filter', 'clear')
        if camera == "C2":
            self.setValue('filter', 'r')
        else:
            self.setValue('filter', 'r')

        # Wait for telescope
        self.waitIdle2()

        zero = time.time()
        time.sleep(1)

        this_move_end = self.getValueFloat('move_end', 'T0')
        last_move_start = self.getValueFloat('move_started', 'T0')

        # Check if telescope timing values are valid
        if this_move_end is None or last_move_start is None:
            self.log('W', f"{mee} Telescope timing not available, cannot run strategy 2")
            return

        self.log('I', f"{mee} move end: {zero - this_move_end:.1f}s ago")
        self.log('I', f"{mee} move start: {zero - last_move_start:.1f}s ago")

        base = this_move_end
        if self.parity:
            base -= self.exptime

        firstrun = True
        last_move_start = 0.0

        # Synchronized imaging loop
        while last_move_start < this_move_end + 1:
            if self.parity and firstrun:
                firstrun = False
            else:
                self.log('I', f"{mee} Exposing {self.exptime}s")
                try:
                    image = self.exposure()
                    self.process(image)
                except Exception as e:
                    self.log('E', f"{mee} Exposure failed: {e}")
                    break

            last_move_start = self.getValueFloat('move_started', 'T0')

            if not last_move_start < this_move_end + 1:
                break

            # Timing synchronization
            now = time.time() - base
            next_tick = (int(now/self.tick) + 1) * self.tick
            delay = next_tick - now

            self.log('I', f"{mee} Timing: now={now:.2f} next={next_tick:.2f} delay={delay:.2f}")

            if delay < 0:
                delay = 0
            time.sleep(delay)

    def run_mosaic_strategy(self, grid_size, strategy_num):
        """
        Generic mosaic strategy for both 2×2 and 3×3 patterns.

        Args:
            grid_size: 2 for 2×2, 3 for 3×3
            strategy_num: 3 or 4 (for logging)
        """
        camera = self.getRunDevice()

        # Determine which camera controls the telescope (master/follower pattern)
        # Only C1 controls telescope to avoid conflicting WOFFS commands
        is_master = (camera == "C1")
        mee = f"adoji-s{strategy_num}/M" if is_master else f"adoji-s{strategy_num}/F"

        # Setup with longer exposures for mosaics
        self.setValue('SHUTTER', 'LIGHT')
        self.setValue('exposure', self.exptime_mosaic)
        
        # self.setValue('filter', 'clear')
        if camera == "C2":
            self.setValue('filter', 'g')
        else:
            self.setValue('filter', 'i')

        self.log('I', f'{mee}: Using {self.exptime_mosaic}s exposures')

        # Calculate mosaic positions
        self.mosaic_positions = self.calculate_mosaic_offsets(grid_size)

        self.log('I', f'{mee}: Starting {grid_size}×{grid_size} mosaic, {len(self.mosaic_positions)} positions')

        # Wait for telescope initial slew
        self.waitIdle2()

        # Set first offset - ONLY if master camera
        if is_master:
            first_ra, first_dec = self.mosaic_positions[0]
            offset_str = f'{first_ra:.4f} {first_dec:.4f}'
            self.setValue('WOFFS', offset_str, 'T0')
            self.log('I', f'{mee}: Set initial offset ({first_ra:.3f},{first_dec:.3f})')
        else:
            self.log('I', f'{mee}: Follower mode - waiting for master')

        for idx, (ra_off, dec_off) in enumerate(self.mosaic_positions):
            self.log('I', f'{mee}: Mosaic position {idx+1}/{len(self.mosaic_positions)}, offset=({ra_off:.3f},{dec_off:.3f})')

            # Prepare next offset for callback - ONLY for master camera
            if is_master:
                if idx + 1 < len(self.mosaic_positions):
                    # Set next position in instance variables
                    self.next_offset_ra, self.next_offset_dec = self.mosaic_positions[idx + 1]
                    self.return_to_center = False
                    self.log('I', f'{mee}: Prepared next offset ({self.next_offset_ra:.3f},{self.next_offset_dec:.3f}) for callback')
                else:
                    # Last position: prepare to return to center
                    self.next_offset_ra = None
                    self.next_offset_dec = None
                    self.return_to_center = True
                    self.log('I', f'{mee}: Prepared return to center for callback')

            # Take exposure (with callback only if master)
            try:
                if is_master:
                    self.log('I', f'{mee}: Starting exposure with callback')
                    image = self.exposure(before_readout_callback=self.mosaic_callback)
                else:
                    self.log('I', f'{mee}: Starting exposure (follower)')
                    image = self.exposure()
                self.log('I', f'{mee}: Processing image')
                self.process(image)
                self.log('I', f'{mee}: Position {idx+1} complete')
            except Exception as e:
                self.log('E', f'{mee}: Exposure failed at position {idx+1}: {e}')
                continue

    def run(self):
        """Main execution: query GRB params, select strategy, execute"""
        camera = self.getRunDevice()
        serial = self.getValue('CCD_SER')

#        self.log('I', f'adoji: *** ADAPTIVE OJITOS START sn {serial} ***')

        # Query GRB parameters from database (errorbox, trigger time, etc.)
        self.errorbox = self.get_grb_params()

        # Check for invalid target (tar_id=-1 or similar)
        if self.errorbox == -1:
#            self.log('I', 'adoji: No valid target, exiting gracefully')
            time.sleep(1)
            return

        # Determine strategy (considers errorbox size and trigger age)
        self.strategy = self.determine_strategy()

        # Execute appropriate strategy
        try:
            if self.strategy == 1:
                self.run_strategy_1_flipflap_windowed()  # Flip/flap with windowing
            elif self.strategy == 2:
                self.run_strategy_2_standard()  # Standard flip/flap full frame
            elif self.strategy == 3:
                self.run_mosaic_strategy(grid_size=2, strategy_num=3)  # 2×2 mosaic
            elif self.strategy == 4:
                self.run_mosaic_strategy(grid_size=3, strategy_num=4)  # 3×3 mosaic
            elif self.strategy == 5:
                self.run_strategy_5_late_followup()  # Long exposures for late follow-up (>20min)
            else:
                self.log('E', f'adoji: Unknown strategy {self.strategy}, using default')
                self.run_strategy_2_standard()

        except Exception as e:
            self.log('E', f'adoji: Strategy {self.strategy} failed: {e}')
            self.log('W', 'adoji: Falling back to strategy 2')
            self.run_strategy_2_standard()


if __name__ == '__main__':
    """Main loop - called by RTS2 executor"""
    while True:
        try:
            ojitos_adaptive = OjitosAdaptive()
            ojitos_adaptive.run()
        except KeyboardInterrupt:
            print("Interrupted by user")
            sys.exit(0)
        except Exception as e:
            print(f"Fatal error: {e}", file=sys.stderr)
            sys.exit(1)
