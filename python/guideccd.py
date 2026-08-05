#!/usr/bin/env python3

# Guiding script for camera C3
# (C) 2010 Martin Jelinek & Petr Kubanek

import sys
import subprocess
import math
from time import sleep
import time

from rts2 import scriptcomm

class GuideScript(scriptcomm.Rts2Comm):
    """Pulse guiding script for RTS2."""

    def __init__(self):
        self.detect4g = "/etc/rts2/detect4g"
        self.telname = "T0"

        # Camera C3 chip properties
        self.chip_w = 1536
        self.chip_h = 1024
        self.offset_w = 0
        self.offset_h = 0

        # Guiding box size (depends on typical FWHM of the guiding camera)
        self.small_w = 64
        self.small_h = 64

        # Initial acquisition window: maximum area minus small guiding square
        # Could be made smaller for wide-field cameras to reduce field rotation effects
        self.big_w = self.chip_w - self.small_w
        self.big_h = self.chip_h - self.small_h

        # Center the initial acquisition area
        self.big_x = self.offset_w + (self.chip_w - self.big_w) // 2
        self.big_y = self.offset_h + (self.chip_h - self.big_h) // 2

        # Sensitivity threshold (offsets smaller than this are ignored)
        self.x_sensitivity = 0.01  # pixels
        self.y_sensitivity = 0.01  # pixels

        # Aggressiveness factors (how much of detected offset to apply, to dampen oscillation)
        self.ra_aggresivity = 0.7
        self.dec_aggresivity = 0.7

        # Exposure time settings
        self.exp_granularity = 0.01  # Step in exposure time adjustment
        self.exp_mintime = 0.05      # Minimum exposure time allowed
        self.exp_time = 2            # Initial exposure time (seconds)
        self.exp_snr = 20            # Target signal-to-noise ratio

        scriptcomm.Rts2Comm.__init__(self)

    def runProgrammeGetArray(self, command):
        """Run external program and return output as array."""
        sb = subprocess.Popen(command, stdout=subprocess.PIPE)
        sb.wait()
        return sb.stdout.readline().split()

    def newExposure(self, f, df):
        """Calculate new exposure time for next iteration based on SNR.
        Currently disabled - to be implemented for adaptive exposure control."""
        snr = f / df
        # TODO: Implement adaptive exposure time adjustment

    def doGuiding(self, x, y):
        """Execute pulse guiding loop to keep star at position X,Y."""

        # Set small guiding window centered on target position
        winfmt = "%d %d %d %d" % (x - self.small_w // 2, y - self.small_h // 2,
                                   self.small_w, self.small_h)
        self.setValue('WINDOW', winfmt)

        # Get mount timing information
        this_move_end = self.getValueFloat('move_end', 'T0')
        last_move_start = self.getValueFloat('move_started', 'T0')
        self.log('I', 'guide: move_end=%.3f move_started=%.3f' % (this_move_end, last_move_start))

        # RA movement direction depends on meridian flip state
        if self.getValueFloat('MNT_FLIP', 'T0') == 1:
            ra_direction_coeff = 1
        else:
            ra_direction_coeff = -1

        failed_attempts = 0

        # Guide loop continues until:
        # 1. Mount starts a new move (last_move_start >= this_move_end)
        # 2. Main cameras (C1, C2) stop exposing for more than 20 seconds
        while last_move_start < this_move_end:

            # Check if main cameras are still active
            now = time.time()
            exposure_end_C1 = self.getValueFloat('exposure_end', 'C1')
            exposure_end_C2 = self.getValueFloat('exposure_end', 'C2')
            if (now - exposure_end_C1) > 20 and (now - exposure_end_C2) > 20:
                self.log('I', 'guide: C1 inactive for %.1fs, C2 for %.1fs - ending' %
                         (now - exposure_end_C1, now - exposure_end_C2))
                sys.exit(0)

            # Take guide exposure
            self.setValue('exposure', self.exp_time)
            image = self.exposure()

            # Detect guide star position using sextractor
            values = self.runProgrammeGetArray([self.detect4g, image])
            self.toTrash(image)

            # Handle detection failures (cosmic rays, clouds, etc.)
            if len(values) < 4:
                self.log('I', 'guide: detection failed, retrying')
                last_move_start = self.getValueFloat('move_started', 'T0')
                failed_attempts += 1
                if failed_attempts > 5:
                    self.log('W', 'guide: too many failed detections, restarting')
                    break  # Restart from beginning with new guide star
                continue

            failed_attempts = 0

            # Calculate pixel offset from center
            change_x = float(values[0]) - self.small_w // 2
            change_y = float(values[1]) - self.small_h // 2

            # Convert pixel offsets to pulse guide time units
            # Calibration factors: 33 for RA, 50 for DEC (empirically determined for C3)
            county = ra_direction_coeff * change_x * 33
            county = max(min(county, 255), -255)  # Clamp to [-255, 255]

            countydec = change_y * 50
            countydec = max(min(countydec, 255), -255)  # Clamp to [-255, 255]

            self.log('I', 'guide: offset x=%.2f y=%.2f snr=%.2f pulse_ra=%.0f pulse_dec=%.0f' %
                     (change_x, change_y, float(values[3]), county, countydec))

            # Send pulse guide corrections to mount
            self.setValue('pulse_guide_ra', ' %d' % (county), 'T0')
            sleep(0.25)
            self.setValue('pulse_guide_dec', ' %d' % (countydec), 'T0')
            sleep(0.25)

            # Check if mount has started a new move
            last_move_start = self.getValueFloat('move_started', 'T0')

    def run(self):
        """Initialize and start guiding sequence."""

        self.log('I', 'guide: **** GUIDING STARTED ****')

        # Wait for mount to settle after slew
        sleep(5)

        # Configure camera for initial acquisition
        self.setValue('exposure', self.exp_time)
        winfmt = "%d %d %d %d" % (self.big_x, self.big_y, self.big_w, self.big_h)
        self.setValue('WINDOW', winfmt)
        self.setValue('SHUTTER', 0)
        self.setValue('FILTER', 'I')

        # Take acquisition image to find guide star
        image = self.exposure()
        values = self.runProgrammeGetArray([self.detect4g, image])

        self.log('I', 'guide: guide star found at %.1f %.1f' %
                 (float(values[0]), float(values[1])))
        self.log('I', 'guide: acquisition window offset %d %d' %
                 (self.big_x, self.big_y))

        # Calculate position for small guiding window
        x = int(float(values[0])) + (self.chip_w - self.big_w) // 2
        y = int(self.big_h - float(values[1])) + (self.chip_h - self.big_h) // 2

        self.log('I', 'guide: guiding window at %d %d' % (x, y))

        self.toTrash(image)

        # Start pulse guiding loop
        self.doGuiding(x, y)


# Main loop: restart guiding if it exits
while True:
    a = GuideScript()
    a.run()
