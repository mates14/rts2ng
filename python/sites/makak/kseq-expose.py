#!/usr/bin/env python3
#
# makak (zenith camera) observing loop: 10 s exposures all night while the
# lascaux roof is open, every 11th frame a dark (shutter closed) starting
# with the 4th, so the first three frames of the night clean the chip of
# the light it got during the day. Each image is renamed into the archive
# and handed to an external processing script.
#
# Python rewrite of ~/bin/kseq-expose.sh at makak.
#
# Run like this:
#   rts2-scriptexec -c C0 -s 'exe /usr/share/rts2/scripts/kseq-expose.py'
#
# The lascaux credentials are read from ~/.netrc of the user running it
# (root under makak-acquire.service), e.g.
#   machine lascaux.asu.cas.cz login observer password ...
#
# (C) 2026 Martin Jelinek
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

import base64
import json
import netrc
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request

from rts2 import scriptcomm

IMG_PROCESS = '/home/mates/bin/img_process_x.sh'
IMG_PROCESS_USER = 'mates'
EXPOSURE_TIME = 10
ARCHIVE_PATTERN = '%b/%Y/%N/%f'

SHUTTER = 'SHUTTER'

DOME_URL = 'https://lascaux.asu.cas.cz/images'
DOME_DEVICE = 'DOME'
DOME_VALUE = 'south_roof_closed'

# rts2-state -c numbers
STATE_NIGHT = 3

# frames in a cycle and the one taken as a dark
CYCLE = 11
DARK_FRAME = 3


class KSeqExpose(scriptcomm.Rts2Comm):
	def __init__(self):
		scriptcomm.Rts2Comm.__init__(self, log_device=False)
		self.processes = []

	def getExecDevice(self):
		# rts2-scriptexec is not a device, do not ask it with exec_device
		# (see base/known-bugs.md); setValue/getValue only need to know
		# that no device name is our own
		return None

	def shutter(self, state):
		"""Open (1) or close (0) the shutter."""
		self.setValue('shutter', state, SHUTTER)
		# also to the camera, it goes to the FITS header
		self.setValue('slitposx', state)
		# the command is sent without waiting for the reply, give the
		# shutter time to move
		time.sleep(1)

	def systemState(self):
		"""Day/evening/dusk/night/dawn/morning as computed by rts2-state, None if it fails."""
		try:
			r = subprocess.run(['rts2-state', '-c'], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
			return int(r.stdout.strip())
		except (OSError, ValueError, subprocess.TimeoutExpired) as ex:
			self.log('E', 'cannot get system state from rts2-state: {0}'.format(ex))
			return None

	def roofClosed(self):
		"""Value of DOME.south_roof_closed at lascaux, None if it cannot be read."""
		host = urllib.parse.urlparse(DOME_URL).hostname
		try:
			auth = netrc.netrc().authenticators(host)
			req = urllib.request.Request('{0}/api/get?{1}'.format(DOME_URL, urllib.parse.urlencode({'d': DOME_DEVICE})))
			if auth:
				req.add_header('Authorization', 'Basic ' + base64.b64encode('{0}:{1}'.format(auth[0], auth[2]).encode()).decode())
			with urllib.request.urlopen(req, timeout=10) as f:
				return int(json.load(f)[DOME_VALUE])
		except (OSError, ValueError, KeyError, TypeError, netrc.NetrcParseError) as ex:
			self.log('E', 'cannot get {0}.{1} from {2}: {3}'.format(DOME_DEVICE, DOME_VALUE, DOME_URL, ex))
			return None

	def expose(self):
		"""Take one exposure, archive it and start its processing."""
		self.setValue('exposure', EXPOSURE_TIME)
		image = self.exposure()
		if image is None:
			self.log('W', 'exposure ended without an image')
			return
		renamed = self.rename(image, ARCHIVE_PATTERN)
		self.log('I', 'running {0} on {1}'.format(IMG_PROCESS, renamed))
		cmd = [IMG_PROCESS, renamed]
		if os.geteuid() == 0:
			cmd = ['su', IMG_PROCESS_USER] + cmd
		# stdout is the connection to scriptexec, keep the processing off it
		try:
			self.processes.append(subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=sys.stderr))
		except OSError as ex:
			self.log('E', 'cannot run {0}: {1}'.format(IMG_PROCESS, ex))

	def reap(self):
		self.processes = [p for p in self.processes if p.poll() is None]

	def run(self):
		old_state = None
		old_roof = None
		frame = 0

		while True:
			self.reap()

			state = self.systemState()
			if state != old_state:
				self.log('I', 'Detected state change from {0} to {1}'.format(old_state, state))
				old_state = state

			roof = self.roofClosed()
			if roof != old_roof:
				self.log('I', 'Detected dome change from {0} to {1}'.format(old_roof, roof))
				old_roof = roof

			if roof is None or roof >= 1:
				# roof closed or unknown: wait for it
				time.sleep(10)
			elif state == STATE_NIGHT:
				try:
					self.shutter(0 if frame == DARK_FRAME else 1)
					self.expose()
				except scriptcomm.Rts2Exception as ex:
					self.log('E', 'exposure failed: {0}'.format(ex))
					time.sleep(10)
				frame = (frame + 1) % CYCLE
			else:
				self.setValue('shutter', 0, SHUTTER)
				self.setValue('slitposx', 0)
				frame = 0
				time.sleep(60)


if __name__ == '__main__':
	KSeqExpose().run()
