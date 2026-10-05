/*
 * Driver for the arduino unit of the makak (zenith) camera at Ondrejov:
 * external shutter/cover in front of the lens, lens heating and one
 * temperature/humidity sensor.
 *
 * The unit is a descendant of the D50 WF-camera unit (d50-wfunit), but
 * speaks a different reply format and controls heating instead of fans.
 * Classic RTS2 at makak ran it from an uncommitted rewrite of
 * src/sensord/d50-wfunit.cpp (morpheus:~/rts2, 2023-09-22); this is that
 * code made into a driver of its own.
 *
 * The unit tends to drop off now and then; classic makak worked around
 * that with a daily reboot of the whole computer from cron. This driver
 * instead resets the USB hub port the unit hangs on (needs root, as the
 * daemons run) when the unit stops answering or the shutter does not
 * follow the command, waits for the port to come back and reopens it.
 *
 * Copyright (C) 2011 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2014 Martin Jelinek
 * Copyright (C) 2019 Jan Strobl
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/*
Commands (single character, no terminator):
i : info
o : open shutter
c : close shutter
0 : heating off
1 : heating on
2 : heating automatic

whatever is sent, the unit answers with a status line:
H: 50.59 T: 28.60 Closed 2
H: nan T: nan Open 0        (temperature/humidity sensor not working)

that is humidity, temperature, shutter state (Closed/Open) and heating
state (0 manual-off, 1 manual-on, 2 auto-off, 3 auto-on). After a reset
(which opening the port causes) the unit first prints a help line:
i = info, o = open, c = close, 0/1/2(auto) = heating
*/

#include "sensord.h"
#include "connserial.h"
#include "utilsfunc.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libgen.h>
#include <unistd.h>

namespace rts2sensord
{

/**
 * Makak camera shutter / heating / weather unit.
 *
 * @author Petr Kubanek <petr@kubanek.net>, Martin Jelinek <mates@iaa.es>, Jan Strobl
 */
class MakakShutter:public Sensor
{
	public:
		MakakShutter (int argc, char **argv);
		virtual ~MakakShutter ();
		virtual int scriptEnds ();
		virtual int idle ();
		virtual int commandAuthorized (rts2core::Connection *conn);

	protected:
		virtual int processOption (int opt);
		virtual int initHardware ();
		virtual int info ();

		virtual int setValue (rts2core::Value *old_value, rts2core::Value *new_value);

	private:
		const char *device_file;
		rts2core::ConnSerial *unitConn;

		rts2core::ValueSelection *shutter;
		rts2core::ValueSelection *heatingSwitch;
		rts2core::ValueSelection *heatingState;
		rts2core::ValueDoubleStat *temp;
		rts2core::ValueDoubleStat *humi;

		rts2core::ValueInteger *numVal;

		rts2core::ValueBool *autoReset;
		rts2core::ValueInteger *maxFailures;
		rts2core::ValueInteger *shutterTimeout;
		rts2core::ValueInteger *resetCount;
		rts2core::ValueTime *lastReset;
		rts2core::ValueString *usbPortPath;

		// what was asked for (heating: or last seen), re-sent after a
		// reset; -1 = nothing yet
		int wantShutter;
		double wantShutterTime;
		int wantHeating;
		// shutter state from the last reply (the shutter value itself
		// is overwritten with the requested state by the value setting)
		int reportedShutter;

		int failures;

		enum {RECOVERY_NONE, RECOVERY_PORT_OFF, RECOVERY_WAIT_DEVICE, RECOVERY_BOOT} recovery;
		double recoveryTime;
		double nextResetAllowed;
		int resetBackoff;

		int openPort ();
		void findUsbPort ();
		bool writeSysfs (const std::string &path, const char *val);

		int unitCommand (char c);
		int sendCommand (char c);
		void checkUnit (int ret);
		void startRecovery (const char *reason, bool force = false);
		void finishRecovery ();
};

}

using namespace rts2sensord;

MakakShutter::MakakShutter (int argc, char **argv): Sensor (argc, argv)
{
	device_file = "/dev/arduino"; // default value
	unitConn = NULL;

	wantShutter = -1;
	wantShutterTime = 0;
	wantHeating = -1;
	reportedShutter = -1;
	failures = 0;
	recovery = RECOVERY_NONE;
	recoveryTime = 0;
	nextResetAllowed = 0;
	resetBackoff = 0;

	createValue (numVal, "num_stat", "number of measurements for temp/hum values", false, RTS2_VALUE_WRITABLE);
	numVal->setValueInteger (6);

	createValue (temp, "temp", "temperature at the camera", false);
	createValue (humi, "humidity", "humidity [%] at the camera", false);

	createValue (heatingState, "heating", "State of lens heating", false);
	heatingState->addSelVal ("manual-off");
	heatingState->addSelVal ("manual-on");
	heatingState->addSelVal ("auto-off");
	heatingState->addSelVal ("auto-on");

	createValue (heatingSwitch, "htSwitch", "heating switch", false, RTS2_VALUE_WRITABLE);
	heatingSwitch->addSelVal ("OFF");
	heatingSwitch->addSelVal ("ON");
	heatingSwitch->addSelVal ("AUTO");
	heatingSwitch->setValueInteger (0);

	createValue (shutter, "shutter", "shutter/cover state", false, RTS2_VALUE_WRITABLE);
	shutter->addSelVal ("CLOSED");
	shutter->addSelVal ("OPEN");

	createValue (autoReset, "auto_reset", "reset the USB port of the unit when it stops working", false, RTS2_VALUE_WRITABLE);
	autoReset->setValueBool (true);

	createValue (maxFailures, "max_failures", "failed queries in a row which trigger the reset", false, RTS2_VALUE_WRITABLE);
	maxFailures->setValueInteger (3);

	createValue (shutterTimeout, "shutter_timeout", "[s] time for the shutter to reach the requested state before the reset", false, RTS2_VALUE_WRITABLE);
	shutterTimeout->setValueInteger (30);

	createValue (resetCount, "resets", "number of unit resets since driver start", false);
	resetCount->setValueInteger (0);

	createValue (lastReset, "last_reset", "time of the last unit reset", false);

	createValue (usbPortPath, "usb_port", "sysfs USB hub port the unit is connected to", false);

	addOption ('f', NULL, 1, "serial port with the module (may be /dev/ttyUSBn, defaults to /dev/arduino)");

	setIdleInfoInterval (10);
}

MakakShutter::~MakakShutter ()
{
	delete unitConn;
}

int MakakShutter::processOption (int opt)
{
	switch (opt)
	{
		case 'f':
			device_file = optarg;
			break;
		default:
			return Sensor::processOption (opt);
	}
	return 0;
}

int MakakShutter::openPort ()
{
	delete unitConn;
	unitConn = new rts2core::ConnSerial (device_file, this, rts2core::BS9600, rts2core::C8, rts2core::NONE, 50);
	if (unitConn->init ())
	{
		delete unitConn;
		unitConn = NULL;
		return -1;
	}
	unitConn->flushPortIO ();
	unitConn->setDebug (getDebug ());
	return 0;
}

/**
 * Finds the USB hub port of the serial converter behind device_file:
 * /sys/class/tty/ttyUSB0/device leads into the USB interface of the
 * converter, the USB device above it has a "port" link to its hub port
 * (/sys/.../3-3.4:1.0/3-3.4-port4), whose "disable" attribute drops and
 * re-enumerates whatever is plugged in. The hub port stays there when the
 * device falls off the bus, so it is looked up while the unit works.
 */
void MakakShutter::findUsbPort ()
{
	char tty[PATH_MAX];
	char dev[PATH_MAX];
	char hubPort[PATH_MAX];

	if (realpath (device_file, tty) == NULL)
		return;

	std::string sysTty = std::string ("/sys/class/tty/") + basename (tty) + "/device";
	if (realpath (sysTty.c_str (), dev) == NULL)
		return;

	std::string dir = dev;
	while (dir.length () > strlen ("/sys/devices"))
	{
		if (access ((dir + "/idVendor").c_str (), F_OK) == 0 && realpath ((dir + "/port").c_str (), hubPort) != NULL)
		{
			if (usbPortPath->getValue () == NULL || strcmp (usbPortPath->getValue (), hubPort))
			{
				usbPortPath->setValueCharArr (hubPort);
				sendValueAll (usbPortPath);
				logStream (MESSAGE_INFO) << device_file << " is on USB port " << hubPort << sendLog;
			}
			return;
		}
		dir = dir.substr (0, dir.rfind ('/'));
	}
	logStream (MESSAGE_WARNING) << "cannot find USB hub port of " << device_file << ", reset will only reopen the port" << sendLog;
}

bool MakakShutter::writeSysfs (const std::string &path, const char *val)
{
	int fd = open (path.c_str (), O_WRONLY);
	if (fd < 0)
	{
		logStream (MESSAGE_ERROR) << "cannot open " << path << ": " << strerror (errno) << sendLog;
		return false;
	}
	ssize_t ret = write (fd, val, strlen (val));
	int err = errno;
	close (fd);
	if (ret < 0)
	{
		logStream (MESSAGE_ERROR) << "cannot write " << val << " to " << path << ": " << strerror (err) << sendLog;
		return false;
	}
	return true;
}

int MakakShutter::initHardware ()
{
	if (device_file == NULL)
	{
		logStream (MESSAGE_ERROR) << "you must specify device file (TTY port)" << sendLog;
		return -1;
	}

	int ret = openPort ();
	if (ret)
		return ret;

	findUsbPort ();

	// the arduino resets when the port is opened and does not answer
	// for a while - the classic driver did not query it here either,
	// the first status comes with the first info () call
	return 0;
}

int MakakShutter::info ()
{
	if (recovery == RECOVERY_NONE)
		checkUnit (unitCommand ('i'));
	return Sensor::info ();
}

int MakakShutter::idle ()
{
	double now = getNow ();
	switch (recovery)
	{
		case RECOVERY_NONE:
			break;
		case RECOVERY_PORT_OFF:
			if (now >= recoveryTime)
			{
				writeSysfs (std::string (usbPortPath->getValue ()) + "/disable", "0");
				recovery = RECOVERY_WAIT_DEVICE;
				recoveryTime = now + 30;
			}
			break;
		case RECOVERY_WAIT_DEVICE:
			if (access (device_file, F_OK) == 0 && openPort () == 0)
			{
				// opening the port resets the arduino, it needs a
				// moment to boot and prints its help line
				recovery = RECOVERY_BOOT;
				recoveryTime = now + 3;
			}
			else if (now >= recoveryTime)
			{
				logStream (MESSAGE_ERROR) << device_file << " did not come back after the reset" << sendLog;
				recovery = RECOVERY_NONE;
			}
			break;
		case RECOVERY_BOOT:
			if (now >= recoveryTime)
				finishRecovery ();
			break;
	}
	return Sensor::idle ();
}

int MakakShutter::commandAuthorized (rts2core::Connection *conn)
{
	if (conn->isCommand ("reset"))
	{
		if (!conn->paramEnd ())
			return -2;
		startRecovery ("reset requested", true);
		return 0;
	}
	return Sensor::commandAuthorized (conn);
}

int MakakShutter::scriptEnds ()
{
	// close the shutter whenever a script ends, as the classic driver did
	changeValue (shutter, 0);
	return Sensor::scriptEnds ();
}

int MakakShutter::setValue (rts2core::Value *old_value, rts2core::Value *new_value)
{
	if (old_value == shutter)
	{
		int v = new_value->getValueInteger ();
		if (v != 0 && v != 1)
			return -2;
		if (recovery != RECOVERY_NONE)
		{
			// closing is safe to do late, opening is not - an exposure
			// could start before the shutter opens
			if (v == 0)
			{
				wantShutter = 0;
				wantShutterTime = getNow ();
				logStream (MESSAGE_WARNING) << "unit is being reset, shutter will be closed when it is back" << sendLog;
			}
			else
			{
				logStream (MESSAGE_ERROR) << "unit is being reset, cannot open shutter" << sendLog;
			}
			return -2;
		}
		// set before sending: the reply is checked against it, and the
		// previous request may be minutes old (setting the shutter to the
		// state it already has does not get here), which would look like
		// a shutter stuck past shutter_timeout
		wantShutter = v;
		wantShutterTime = getNow ();
		return sendCommand (v ? 'o' : 'c') ? -2 : 0;
	}

	if (old_value == heatingSwitch)
	{
		int v = new_value->getValueInteger ();
		if (v < 0 || v > 2)
			return -2;
		wantHeating = v;
		if (recovery != RECOVERY_NONE)
			return 0;
		return sendCommand ('0' + v) ? -2 : 0;
	}

	return Sensor::setValue (old_value, new_value);
}

int MakakShutter::sendCommand (char c)
{
	int ret = unitCommand (c);
	checkUnit (ret);
	return ret;
}

int MakakShutter::unitCommand (char c)
{
	char buf[200];
	char shutState[8];
	float temp1, hum1;
	int heater;
	int ret = 0;

	if (unitConn == NULL)
		return -1;

	unitConn->flushPortIO ();
	if (unitConn->writePort (&c, 1) < 0)
		return -1;

	// one extra line in case the reply is preceded by the help line
	// the unit prints after a reset
	for (int i = 0; i < 2; i++)
	{
		int len = unitConn->readPort (buf, sizeof (buf) - 1, '\n');
		if (len < 0)
			return -1;
		// readPort does not terminate the buffer on success
		buf[len] = '\0';

		// base note: the classic makak code did not check the sscanf
		// result, so a garbled reply put an uninitialized heater value
		// and shutter character into the driver state; it also read
		// the reply past its end (see the termination above).
		ret = sscanf (buf, "H: %f T: %f %7s %d", &hum1, &temp1, shutState, &heater);
		if (ret == 4 || strncmp (buf, "i = info", 8))
			break;
	}
	if (ret != 4)
	{
		logStream (MESSAGE_ERROR) << "cannot parse reply from unit, reply was: '" << buf << "', return " << ret << sendLog;
		return -1;
	}

	switch (shutState[0])
	{
		case 'C':
			reportedShutter = 0;
			break;
		case 'O':
			reportedShutter = 1;
			break;
		default:
			logStream (MESSAGE_ERROR) << "unexpected shutter state in response from unit: '" << buf << "'" << sendLog;
			return -1;
	}
	shutter->setValueInteger (reportedShutter);

	if (heater < 0 || heater > 3)
	{
		logStream (MESSAGE_ERROR) << "unexpected heating state in response from unit: '" << buf << "'" << sendLog;
		return -1;
	}
	heatingState->setValueInteger (heater);
	// keep the switch showing the mode the unit is actually in (it
	// survives a driver restart): manual-off, manual-on, auto
	heatingSwitch->setValueInteger (heater < 2 ? heater : 2);
	// the arduino forgets the heating mode when it resets, restore the
	// mode it was in even if it was not set by this driver run
	if (wantHeating < 0 && recovery == RECOVERY_NONE)
		wantHeating = heatingSwitch->getValueInteger ();

	// the sensor answers nan when it does not work; do not let that into
	// the statistics. Check the I2C sensor credibility as well.
	if (!std::isnan (temp1) && !std::isnan (hum1) && hum1 <= 110.0)
	{
		temp->addValue (temp1, numVal->getValueInteger ());
		temp->calculate ();
		humi->addValue (hum1, numVal->getValueInteger ());
		humi->calculate ();
	}

	return 0;
}

void MakakShutter::checkUnit (int ret)
{
	if (ret)
	{
		failures++;
		if (failures >= maxFailures->getValueInteger ())
			startRecovery ("unit does not answer");
		return;
	}
	failures = 0;

	if (wantShutter >= 0 && reportedShutter != wantShutter)
	{
		if (getNow () - wantShutterTime > shutterTimeout->getValueInteger ())
			startRecovery ("shutter does not reach the requested state");
		return;
	}

	// all fine
	resetBackoff = 0;
	if (getState () & DEVICE_ERROR_HW)
		clearHWError ();
}

void MakakShutter::startRecovery (const char *reason, bool force)
{
	if (recovery != RECOVERY_NONE)
		return;
	if (!force)
	{
		if (!autoReset->getValueBool ())
		{
			raiseHWError ();
			return;
		}
		if (getNow () < nextResetAllowed)
			return;
	}

	logStream (MESSAGE_ERROR) << reason << ", resetting " << device_file << sendLog;
	raiseHWError ();

	resetCount->inc ();
	sendValueAll (resetCount);
	lastReset->setNow ();
	sendValueAll (lastReset);

	delete unitConn;
	unitConn = NULL;
	failures = 0;

	if (usbPortPath->getValue () != NULL && *usbPortPath->getValue () && writeSysfs (std::string (usbPortPath->getValue ()) + "/disable", "1"))
	{
		recovery = RECOVERY_PORT_OFF;
		recoveryTime = getNow () + 2;
	}
	else
	{
		// at least reopen the port, which resets the arduino
		recovery = RECOVERY_WAIT_DEVICE;
		recoveryTime = getNow () + 30;
	}
}

void MakakShutter::finishRecovery ()
{
	recovery = RECOVERY_NONE;
	findUsbPort ();

	// do not reset in a loop when the reset does not help - wait
	// longer after every reset that was not followed by a good reply
	nextResetAllowed = getNow () + resetBackoff;
	resetBackoff = resetBackoff ? std::min (resetBackoff * 2, 3600) : 60;

	int ret = 0;
	if (wantHeating >= 0)
		ret = unitCommand ('0' + wantHeating);
	if (ret == 0 && wantShutter >= 0)
	{
		ret = unitCommand (wantShutter ? 'o' : 'c');
		wantShutterTime = getNow ();
	}
	if (ret == 0)
		ret = unitCommand ('i');
	logStream (ret ? MESSAGE_ERROR : MESSAGE_INFO) << "unit reset " << (ret ? "did not help" : "done") << sendLog;
	checkUnit (ret);
	infoAll ();
}

int main (int argc, char **argv)
{
	MakakShutter device (argc, argv);
	return device.run ();
}
