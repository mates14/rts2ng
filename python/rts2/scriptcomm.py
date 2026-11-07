#!/usr/bin/env python3
#
# RTS2 script communication - Python 3 version
# (C) 2009,2010 Petr Kubanek <petr@kubanek.net>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

import os
import sys
import time
import re

# constants for device types
DEVICE_TELESCOPE   = "TELESCOPE"
DEVICE_CCD         = "CCD"
DEVICE_DOME        = "DOME"
DEVICE_WEATHER     = "WEATHER"
DEVICE_PHOT        = "PHOT"
DEVICE_PLAN        = "PLAN"
DEVICE_FOCUS       = "FOCUS"
DEVICE_CUPOLA      = "CUPOLA"
DEVICE_FW          = "FW"
DEVICE_SENSOR      = "SENSOR"
DEVICE_EXECUTOR    = "EXEC"
DEVICE_SELECTOR    = "SELECTOR"
DEVICE_IMGP        = "IMGP"

# constants for device states
CAM_EXPOSING       = 0x001
# telescope moving-state sub-mask (bits 0-2, see status.h TEL_MASK_MOVING) -
# a device's raw state (getState()) always has exactly one of these three
# set within that mask; TEL_TRACKING (bit 5) is independent of it and can
# be set at the same time as TEL_OBSERVING
TEL_MASK_MOVING    = 0x007
TEL_OBSERVING      = 0x000
TEL_MOVING         = 0x001
TEL_PARKED         = 0x002
TEL_TRACKING       = 0x020
FOC_FOCUSING       = 0x001
DOME_OPENING       = 0x002
DOME_CLOSING       = 0x008

# constants for datatypes
DT_RA              = "DT_RA"
DT_DEC             = "DT_DEC"
DT_DEGREES         = "DT_DEGREES"
DT_DEG_DIST        = "DT_DEG_DIST"
DT_PERCENTS        = "DT_PERCENTS"
DT_ROTANG          = "DT_ROTANG"
DT_HEX             = "DT_HEX"
DT_BYTESIZE        = "DT_BYTESIZE"
DT_KMG             = "DT_KMG"
DT_INTERVAL        = "DT_INTERVAL"
DT_ONOFF           = "DT_ONOFF"

def quote_if_string(value):
	"""Return quoted string if value is string, or just the value itself"""
	if isinstance(value, str):
		return '"' + value + '"'
	else:
		return value

class Rts2Exception(Exception):
	"""Thrown on exceptions on communicated over stdin/stdout connection."""
	def __init__(self, message):
		Exception.__init__(self, message)

class Rts2NotActive(Exception):
	def __init__(self):
		Exception.__init__(self, 'script is not active')

# set once the pipes to RTS2 are closed: log () then has no one to tell
_gone = False

def _connection_gone():
	"""The camera daemon closed our pipes: RTS2 ended the script, which is what
	Rts2NotActive says. Point stdout at /dev/null first, so the handlers'
	last log () and the interpreter's flush at exit write nowhere instead of
	raising again (classic left a BrokenPipeError traceback, 20-80 a night at
	SBT, and an empty read returned '' to the caller). [JS 2026-11-07]"""
	global _gone
	_gone = True
	try:
		devnull = os.open(os.devnull, os.O_WRONLY)
		os.dup2(devnull, sys.stdout.fileno())
		os.close(devnull)
	except OSError:
		pass
	raise Rts2NotActive()

def _flush():
	try:
		sys.stdout.flush()
	except BrokenPipeError:
		_connection_gone()

class Rts2Comm:
	"""Class for communicating with RTS2 in exe command."""
	def __init__(self, log_device = True):
		self.exception_re = re.compile(r'([!&]) (\S.*)')
		self.__run_device = None
		self.__exec_device = None
		self.__log_device = log_device

	def sendCommand(self, command, device = None):
		"""Send command to device."""
		if device is None:
			print('command {0}'.format(command))
		else:
			print('C {0} {1}'.format(device, command))
		_flush()

	def readline(self):
		"""Reads single line from standard input. Checks for exceptions."""
		ex = None
		while True:
			a = sys.stdin.readline()
			if a == '':
				_connection_gone()	# end of file, not an empty line
			a = a.rstrip('\n')
			# handle exceptions
			m = self.exception_re.match(a)
			if m:
				if m.group(1) == '!':
					self.log('W', 'exception from device: {0}'.format(m.group(2)))
					ex = Rts2Exception(m.group(2))
				elif m.group(1) == '&':
					raise Rts2NotActive()
			elif ex:
				raise ex
			else:
				return a

	def getValue(self, value, device = None):
		"""Returns given value."""
		if device is None:
			print('? {0}'.format(value))
		elif (device == self.getExecDevice()):
			return self.getOwnValue(value)
		else:
			print('G {0} {1}'.format(device, value))
		_flush()

		value = self.readline()
		if value == 'ERR':
			return None
		else:
			return value

	def getOwnValue(self, value):
		print('get_own {0}'.format(value))
		_flush()

		value = self.readline()
		if value == 'ERR':
			return None
		else:
			return value

	def getLoopCount(self):
		print('loopcount')
		_flush()
		return int(self.readline())

	def getRunDevice(self):
		if self.__run_device is None:
			print('run_device')
			_flush()
			self.__run_device = self.readline()
		return self.__run_device

	def getExecDevice(self):
		if self.__exec_device is None:
			print('exec_device')
			_flush()
			self.__exec_device = self.readline()
		return self.__exec_device

	def getValueFloat(self, value, device = None):
		"""Return value as float number."""
		value = self.getValue(value, device)
		if value is None:
			return None
		return float(value.split(" ")[0])

	def getValueInteger(self, value, device = None):
		val = self.getValue(value, device)
		if val is None:
			return None
		if val[0:2] == "0x":
			vals = val.split()[0:2]
			if len(vals[0]) > 2:
				return int(vals[0], base=16)
			else:
				return int(vals[1], base=16)
		else:
			return int(val.split(" ")[0])

	def incrementValue(self, name, new_value, device = None):
		if (device is None):
			print("value {0} += {1}".format(name, quote_if_string(new_value)))
		else:
			print("V {0} {1} += {2}".format(device, name, quote_if_string(new_value)))
		_flush()

	def incrementValueType(self, device, name, new_value):
		print("VT {0} {1} += {2}".format(device, name, quote_if_string(new_value)))
		_flush()

	def setValue(self, name, new_value, device = None):
		if (device is None):
			print("value {0} = {1}".format(name, quote_if_string(new_value)))
		elif (device == self.getExecDevice()):
			self.setOwnValue(name, new_value)
		else:
			print("V {0} {1} = {2}".format(device, name, quote_if_string(new_value)))
		_flush()

	def setOwnValue(self, name, new_value):
		print('set_own {0} {1}'.format(name, new_value))
		_flush()

	def setValueByType(self, device, name, new_value):
		"""Set value for all devices of given type. Please use DEVICE_xx constants to specify device type."""
		print("VT {0} {1} = {2}".format(device, name, quote_if_string(new_value)))
		_flush()

	def getDeviceByType(self, device):
		"""Returns name of the first device with given type."""
		print("device_by_type {0}".format(device))
		_flush()
		return self.readline()

	def targetDisable(self):
		"""Disable target for autonomous selection."""
		print("target_disable")
		_flush()

	def targetTempDisable(self, ti):
		"""Temporary disable target for given number of seconds."""
		print("target_tempdisable {0}".format(ti))
		_flush()

	def loopDisable(self):
		"""Temporarily disable looping in executor."""
		print("loop_disable")
		_flush()

	def endScript(self):
		"""Ask controlling process to end the current script."""
		print("end_script")
		_flush()

	def getState(self, device):
		"""Retrieve device state"""
		print('S {0}'.format(device))
		_flush()
		return int(self.readline())

	def isMoving(self, device):
		"""True if the telescope is currently slewing (not parked, not just tracking/observing in place).

		A non-blocking, current-instant answer via getState() - unlike move_started/move_end, which
		answer "did a move happen since I last checked", not "is it moving right now"."""
		return (self.getState(device) & TEL_MASK_MOVING) == TEL_MOVING

	def isParked(self, device):
		"""True if the telescope is currently parked."""
		return (self.getState(device) & TEL_MASK_MOVING) == TEL_PARKED

	def isTracking(self, device):
		"""True if the telescope is currently tracking."""
		return (self.getState(device) & TEL_TRACKING) == TEL_TRACKING

	def waitIdle(self, device, timeout):
		"""Wait for idle state (with timeout)"""
		print('waitidle {0} {1}'.format(device, timeout))
		_flush()
		return int(self.readline())

	def waitMask(self, device, mask, timeout):
		"""Wait for given state (when mask & state = mask)"""
		print('waitmask {0} {1} {2}'.format(device, mask, timeout))
		_flush()
		return int(self.readline())

	def exposure(self, before_readout_callback = None, fileexpand = None, overwrite = False, noblock = False):
		"""Start new exposure. Allow user to specify fallback function to call after end of exposure and expand pattern for new file.
		Unless fileexpand parameter is provided, user is responsible to specify image treatment.
		noblock=True is a guider's exposure: no block (offset, correction, telescope move) holds it,
		and it does not hold telescope moves. The script must itself drop frames taken during a move."""
		cmd = "exposure_noblock" if noblock else "exposure"
		if fileexpand:
			if overwrite:
				print("{0}_overwrite {1}".format(cmd, fileexpand))
			else:
				print("{0}_wfn {1}".format(cmd, fileexpand))
		else:
			print(cmd)
		_flush()
		a = self.readline()
		if a == 'exposure_failed':
			raise Rts2Exception("exposure failed")
		if a == 'exposure_end_noimage':
			return None
		if a != "exposure_end":
			self.log('E', "invalid return from exposure - expected exposure_end, received " + a)
		if not (before_readout_callback is None):
			before_readout_callback()
		a = self.readline()
		image, fn = a.split()
		return fn

	def mountMoving(self, since = None, device = 'T0'):
		"""True if the mount moves now - or, with since (a time.time ()), if it moved at any
		time after it. A guider taking noblock exposures drops the frames taken during a move:
		mountMoving(since = time before the exposure) after the exposure."""
		ended = self.getValueFloat('move_end', device)
		# move_end is the estimated end while a move runs, the actual end after it
		if ended is not None and ended == ended and ended > (time.time() if since is None else since):
			return True
		if since is None:
			return False
		started = self.getValueFloat('move_started', device)
		return started is not None and started == started and started >= since

	def waitMountStill(self, device = 'T0', poll = 0.2, timeout = 600):
		"""Wait until the mount does not move (at most timeout seconds)."""
		end = time.time() + timeout
		while self.mountMoving(device = device) and time.time() < end:
			time.sleep(poll)

	def endShift(self, fileexpand = None, overwrite = False):
		"""Ends shiftstore sequence.
		Unless fileexpand parameter is provided, user is responsible to specify image treatment."""
		if fileexpand:
			if overwrite:
				print("exposure_overwrite {0}".format(fileexpand))
			else:
				print("exposure_wfn {0}".format(fileexpand))
		else:
			print("endshift")
		_flush()
		a = self.readline()
		if a == 'exposure_failed':
			raise Rts2Exception("exposure failed")
		if a == 'exposure_end_noimage':
			return None
		if a != "exposure_end":
			self.log('E', "invalid return from exposure - expected exposure_end, received " + a)
		if not (before_readout_callback is None):
			before_readout_callback()
		a = self.readline()
		image, fn = a.split()
		return fn

	def progressUpdate(self, expected_end, start=time.time()):
		print("progress {0} {1}".format(start, expected_end))
		_flush()

	def radec(self, ra, dec):
		print("radec {0} {1}".format(ra, dec))
		_flush()

	def newObs(self, ra, dec):
		print("newobs {0} {1}".format(ra, dec))
		_flush()

	def hadec(self, ha, dec):
		print("hadec {0} {1}".format(ha, dec))
		_flush()

	def newObsHaDec(self, ha, dec):
		print("newhadec {0} {1}".format(ha, dec))
		_flush()

	def altaz(self, alt, az):
		print("altaz {0} {1}".format(alt, az))
		_flush()

	def newObsAltAz(self, alt, az):
		print("newaltaz {0} {1}".format(alt, az))
		_flush()

	def __imageAction(self, action, imagename):
		print(action, imagename)
		_flush()
		return self.readline()

	def rename(self, imagename, pattern):
		print("rename", imagename, pattern)
		_flush()
		return self.readline()

	def move(self, imagename, pattern):
		"""Move image to new path, delete it from the database."""
		print("move", imagename, pattern)
		_flush()
		return self.readline()

	def toFlat(self, imagename):
		return self.__imageAction("flat", imagename)

	def toDark(self, imagename):
		return self.__imageAction("dark", imagename)

	def toArchive(self, imagename):
		"""Move image at path to archive. Return new image path."""
		return self.__imageAction("archive", imagename)

	def toTrash(self, imagename):
		"""Move image at path to trash. Return new image path."""
		return self.__imageAction("trash", imagename)

	def delete(self, imagename):
		"""Delete image from disk."""
		print("delete", imagename)
		_flush()

	def process(self, imagename):
		"""Put image to image processor queue."""
		print("process", imagename)
		_flush()

	def valueCreate(self, name, value=None, device=None, desc='created from script', type="string", writable=False, fits=False, temporary=False, rts2_type=None):
		"""Add to device new value of given type with given flags"""
		chunks = ['value_create', type, name, '"' + desc.replace('"', '\\"') + '"']

		if value is not None:
			if type == 'string':
				chunks.append('"' + str(value).replace('"', '\\"') + '"')
			else:
				chunks.append(str(value))

		if writable:
			chunks.append('writable')

		if fits:
			chunks.append('fits')

		if temporary:
			chunks.append('temporary')

		if rts2_type:
			chunks.append(rts2_type)

		if device is not None and device != '.':
			chunks.insert(0, 'C ' + device)

		print(" ".join(chunks))

		_flush()

	def valueDelete(self, name, device=None):
		if device is not None and device != '.':
			print("C", device, "value_delete", name)
		else:
			print("value_delete", name)

		_flush()

	def doubleValue(self, name, desc, value, rts2_type=None):
		"""Add to device double value."""
		print("double", name, '"{0}"'.format(desc), value, rts2_type if rts2_type else '')
		_flush()

	def doubleVariable(self, name, desc, value):
		"""Add to device double writable variable."""
		print("double_w", name, '"{0}"'.format(desc), value)
		_flush()

	def timeValue(self, name, desc, value, rts2_type=None):
		"""Add to device time value."""
		print("timevalue", name, '"{0}"'.format(desc), value, rts2_type if rts2_type else '')
		_flush()

	def timeVariable(self, name, desc, value):
		"""Add to device time writable variable."""
		print("timevalue_w", name, '"{0}"'.format(desc), value)
		_flush()

	def integerValue(self, name, desc, value):
		"""Add to device integer value."""
		print("integer", name, '"{0}"'.format(desc), value)
		_flush()

	def integerVariable(self, name, desc, value):
		"""Add to device integer writable variable."""
		print("integer_w", name, '"{0}"'.format(desc), value)
		_flush()

	def stringValue(self, name, desc, value):
		"""Add to device string value."""
		print("string", name, '"{0}"'.format(desc), value)
		_flush()

	def stringVariable(self, name, desc, value):
		"""Add to device string writable variable."""
		print("string_w", name, '"{0}"'.format(desc), value)
		_flush()

	def boolValue(self, name, desc, value):
		"""Add to device boolean value."""
		print("bool", name, '"{0}"'.format(desc), value)
		_flush()

	def boolVariable(self, name, desc, value):
		"""Add to device boolean writable variable."""
		print("bool_w", name, '"{0}"'.format(desc), value)
		_flush()

	def onoffValue(self, name, desc, value):
		"""Add to device boolean value with on/off display type."""
		print("onoff", name, '"{0}"'.format(desc), value)
		_flush()

	def onoffVariable(self, name, desc, value):
		"""Add to device boolean writable variable with on/off display type."""
		print("onoff_w", name, '"{0}"'.format(desc), value)
		_flush()

	def raDecValue(self, name, desc, ra, dec):
		"""Add RA/DEC value pair"""
		print("radec", name, '"{0}"'.format(desc), ra, dec)
		_flush()

	def doubleArrayValue(self, name, desc, values):
		print("double_array", name, '"{0}"'.format(desc), ' '.join(map(str, values)))
		_flush()

	def doubleArrayVariable(self, name, desc, values):
		print("double_array_w", name, '"{0}"'.format(desc), ' '.join(map(str, values)))
		_flush()

	def valueAdd(self, name, values, device=None):
		"""Add new double to existing double array or statistical double value."""
		if device and device != '.':
			print("C", device, "value_add", name, ' '.join(map(str, values)))
		else:
			print("value_add", name, ' '.join(map(str, values)))
		_flush()

	def doubleArrayAdd(self, name, values, device=None):
		"""Add new double to existing double array value."""
		if device and device != '.':
			print("C", device, "double_array_add", name, ' '.join(map(str, values)))
		else:
			print("double_array_add", name, ' '.join(map(str, values)))
		_flush()

	def statAdd(self, name, desc, maxsize, values, device=None):
		"""Add new double to existing statistical double value."""
		if device and device != '.':
			print("C", device, "stat_add", name, maxsize, ' '.join(map(str, values)))
		else:
			print("C", device, "stat_add", name, maxsize, ' '.join(map(str, values)))
		_flush()

	def log(self, level, *text):
		if _gone:
			return
		if self.__log_device:
			text = (self.getRunDevice(),) + text
		print("log", level, *text)
		_flush()

	def isEvening(self):
		"""Returns true if is evening - sun is on West"""
		sun_az = self.getValueFloat('sun_az', 'centrald')
		return sun_az < 180.0

	def tempentry(self, entry):
		print('tempentry', entry)
		_flush()

	def endTarget(self):
		print('end_target')
		_flush()

	def stopTarget(self):
		print('stop_target')
		_flush()

	def waitTargetMove(self):
		"""Returns after move was commanded"""
		print('wait_target_move')
		_flush()
		return self.readline()

	def queueClear(self, queue, selector=None):
		"""Clear selector queue"""
		if selector is None:
			selector = self.getDeviceByType(DEVICE_SELECTOR)
		return self.sendCommand('clear {0}'.format(queue), selector)

	def queueAppend(self, queue, target_id, selector=None):
		if selector is None:
			selector = self.getDeviceByType(DEVICE_SELECTOR)
		return self.sendCommand('queue {0} {1}'.format(queue, target_id), selector)

	def queueInsert(self, queue, target_id, index=0, selector=None):
		if selector is None:
			selector = self.getDeviceByType(DEVICE_SELECTOR)
		return self.sendCommand('insert {0} {1} {2}'.format(queue, index, target_id), selector)

	def requeue(self, queue, delay):
		print('requeue {0} {1}'.format(queue, delay))
		_flush()
		return self.readline()


# Compatibility alias for old code
Rts2Comm3 = Rts2Comm
