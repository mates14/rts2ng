/*
 * Driver for D50 dome in Ondrejov, using unit, made by Martin ("Ford") Nekola.
 * Copyright (C) 2022 Jan Strobl
 * Copyright (C) 2007-2008 Petr Kubanek <petr@kubanek.net>
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

#include "domeford.h"

// base note: classic had 30 s; the roof takes 26-27 s each way (D50 logs,
// 2026-10-12..16), too close: an opening that ends on the timeout is
// "interrupted" and left short of the switch
#define TIME_TO_OPEN_DOME              40
// this is to surely cross the EMI-caused false signal of endswitch being connected
#define MINIMAL_TIME_TO_MOVE              2

// domeMotorState selection values
#define DOME_MOTOR_STATE_OFF		0
#define DOME_MOTOR_STATE_OPENING	1
#define DOME_MOTOR_STATE_CLOSING	2

#define OPT_MOUNT_DEVICE		OPT_LOCAL + 150
#define OPT_MOUNT_INDOME_VARIABLE	OPT_LOCAL + 151

typedef enum
{
	OTEVIRANI,
	ZAVIRANI,
	VYSTUP_3,
	VYSTUP_4,
	VYSTUP_5,
	VYSTUP_6,
	VYSTUP_7,
	UNDEFINED_B_128,
	UNDEFINED_A_1,
	KONCAK_OTEVRENI,
	KONCAK_ZAVRENI,
	VSTUP_3,
	VSTUP_4,
	UNDEFINED_A_32,
	UNDEFINED_A_64,
	UNDEFINED_A_128
} vystupy;


using namespace rts2dome;

namespace rts2dome
{

/**
 * Class for D50 dome control.
 *
 * @author Jan Strobl
 * @author Petr Kubanek <petr@kubanek.net>
 */
class D50:public Ford
{
	private:
		// base note: classic shared one pair between opening and closing, and
		// endOpen ()/endClose () returned before clearing it on a failed read. An
		// expired leftover made every isClosed () return -2 without reading the
		// switch, and Dome's domeCloseStart () took that for "already closed".
		time_t timeToOpenMax, timeToOpenMin;
		time_t timeToCloseMax, timeToCloseMin;

		/**
		 * Switches both motors off and checks they are off.
		 *
		 * @return 0 when both are off, -1 otherwise (logged as critical).
		 */
		int stopMotors ();

		/**
		 * Reads the board: 1 when only the given motor runs, 0 when not, -1 on error.
		 */
		int onlyMotor (int motor);

		rts2core::ValueInteger *sw_state;
		rts2core::ValueSelection *domeMotorState, *domeMotorStateOpening, *domeMotorStateClosing, *domeSwitchOpened, *domeSwitchClosed;

		const char *mountDevice;
		const char *mountInsideDomeVariable;

		rts2core::ValueBool *ignoreMountState;

		/**
		* Read data from (external) mount device and find out if the dome can be safely closed.
		*
		* @return zero (0) if the dome is parked or inside "closed-dome" safe area, -1 if an error occured, 1 when parking in progress, 2 otherwise
		*/
		int getMountState ();

	protected:
		virtual int init ();
		virtual int info ();
		virtual int setValue (rts2core::Value * old_value, rts2core::Value * new_value);
		virtual int willConnect (rts2core::NetworkAddress * in_addr);
		virtual int processOption (int in_opt);

		virtual int startOpen ();
		virtual long isOpened ();
		virtual int endOpen ();
		virtual int startClose ();
		virtual long isClosed ();
		virtual int endClose ();
		virtual int stop ();

	public:
		D50 (int argc, char **argv);
		virtual ~D50 ();
};

}

D50::D50 (int argc, char **argv):Ford (argc, argv)
{
	timeToOpenMax = timeToOpenMin = 0;
	timeToCloseMax = timeToCloseMin = 0;

	createValue (sw_state, "sw_state", "state of dome switches", false, RTS2_DT_HEX);
	createValue (domeMotorState, "motor_state", "dome-motor state", false);
	domeMotorState->addSelVal ("off");
	domeMotorState->addSelVal ("opening (moving)");
	domeMotorState->addSelVal ("closing (moving)");
	createValue (domeMotorStateOpening, "motor_state_opening", "motor opening output state", false, RTS2_DT_ONOFF);
	domeMotorStateOpening->addSelVal ("---");
	domeMotorStateOpening->addSelVal ("OPENING");
	createValue (domeMotorStateClosing, "motor_state_closing", "motor closing output state", false, RTS2_DT_ONOFF);
	domeMotorStateClosing->addSelVal ("---");
	domeMotorStateClosing->addSelVal ("CLOSING");
	createValue (domeSwitchOpened, "roof_opened", "switch on south roof, opened state", false, RTS2_DT_ONOFF);
	domeSwitchOpened->addSelVal ("---");
	domeSwitchOpened->addSelVal ("OPENED");
	createValue (domeSwitchClosed, "roof_closed", "switch on south roof, closed state", false, RTS2_DT_ONOFF);
	domeSwitchClosed->addSelVal ("---");
	domeSwitchClosed->addSelVal ("CLOSED");

	addOption (OPT_MOUNT_DEVICE, "mount-device", 1, "name of mount device [T0]");
	addOption (OPT_MOUNT_INDOME_VARIABLE, "mount-indome-variable", 1, "variable name of the mount device showing the mount is inside iof the dome [inside_dome_limit]");
	mountDevice = "T0";
	mountInsideDomeVariable = "inside_dome_limit";

	createValue (ignoreMountState, "ignore_mount_state", "ignore state of mount (teld) when closing the dome (DANGEROUS!)", false, RTS2_VALUE_WRITABLE);
	ignoreMountState->setValueBool (false);

	setIdleInfoInterval(5);
}

D50::~D50 ()
{
}

int D50::stopMotors ()
{
	for (int i = 0; i < 3; i++)
	{
		if (switchOffPins (OTEVIRANI, ZAVIRANI) == 0 && zjisti_stav_portu () == 0
			&& !getPortState (OTEVIRANI) && !getPortState (ZAVIRANI))
			return 0;
		sleep (1);
	}
	logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "cannot switch the roof motors off" << sendLog;
	return -1;
}

int D50::onlyMotor (int motor)
{
	if (zjisti_stav_portu ())
		return -1;
	int other = (motor == OTEVIRANI) ? ZAVIRANI : OTEVIRANI;
	return getPortState (motor) && !getPortState (other);
}

// base note: classic did not read back its motor writes (they have no reply). A
// lost VYP of one direction followed by ZAP of the other drove both outputs. The
// running motor is now checked to be off before the other one is started, and
// the started one to be the only one on.
int D50::startOpen ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "startOpen: zjisti_stav_portu () error" << sendLog;
		return ret;	// error
	}

	if (getPortState (OTEVIRANI) && !getPortState (ZAVIRANI))
	{
		// opening already in progress
		if (timeToOpenMax == 0)
		{
			time (&timeToOpenMax);
			timeToOpenMin = timeToOpenMax;
			timeToOpenMax += TIME_TO_OPEN_DOME;
		}
		return 0;
	}

	if (getPortState (ZAVIRANI))
	{
		// the dome is just closing, but this is a decent dome, we can simply stop, wait a bit and then start the opening as requested
		if (stopMotors ())
		{
			logStream (MESSAGE_ERROR) << "startOpen: cannot stop the closing motor" << sendLog;
			return -1;
		}
		sleep (2);
	}

	// TODO: Pridat check na reseni zaparkovani dalekohledu atd.

	if (ZAP (OTEVIRANI) || onlyMotor (OTEVIRANI) != 1)
	{
		logStream (MESSAGE_ERROR) << "startOpen: cannot switch on the opening motor" << sendLog;
		stopMotors ();
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "opening the dome" << sendLog;

	timeToCloseMax = timeToCloseMin = 0;
	time (&timeToOpenMax);
	timeToOpenMin = timeToOpenMax + MINIMAL_TIME_TO_MOVE;
	timeToOpenMax += TIME_TO_OPEN_DOME;

	return 0;
}

long D50::isOpened ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret == 0 && getPortState (KONCAK_OTEVRENI) && !getPortState (ZAVIRANI) && timeToOpenMin < time (NULL))
		return -2;	// dome is open!
	if (timeToOpenMax != 0 && timeToOpenMax < time (NULL))
	{
		logStream (MESSAGE_ERROR) << "A timeout reached during the dome opening!" << sendLog;
		return -1;
	}
	// base note: classic aborted the opening on one failed read; the motor keeps
	// going, the timeout ends it
	if (ret)
		logStream (MESSAGE_ERROR) << "isOpened: zjisti_stav_portu () error" << sendLog;
	return USEC_SEC;
}

int D50::endOpen ()
{
	int ret;
	timeToOpenMax = timeToOpenMin = 0;
	if (stopMotors ())
		logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "endOpen: cannot switch off the opening motor!" << sendLog;
	ret = zjisti_stav_portu ();	//kdyz se to vynecha, neposle to posledni prikaz nebo znak
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "endOpen: zjisti_stav_portu () error" << sendLog;
		return ret;	// error
	}

	setTimeout (2 * USEC_SEC);
	if (!getPortState (KONCAK_OTEVRENI))
		return -1;	// dome is not open properly :-(

	return 0;
}

int D50::startClose ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "startClose: zjisti_stav_portu () error" << sendLog;
		return ret;	// error
	}

	bool closing = getPortState (ZAVIRANI) && !getPortState (OTEVIRANI);

	if (!ignoreMountState->getValueBool ())
	{
		int mountState = getMountState ();
		logStream (MESSAGE_DEBUG) << "startClose: mountState: " << mountState << sendLog;
		// base note: in bad weather this runs at every idle, also while the roof
		// closes. Classic then returned 1 with the motor running: Dome went to
		// "waiting" and nothing watched the motor any more. A mount found moving
		// or outside stops the roof now; a mount that stopped answering does not
		// (it was inside when the close started and refuses moves while it runs).
		if (closing && (mountState == 1 || mountState == 2))
		{
			logStream (MESSAGE_WARNING | MESSAGE_REPORTIT) << "mount not in the safe position, stopping the closing roof" << sendLog;
			timeToCloseMax = timeToCloseMin = 0;
			if (stopMotors ())
				return -1;
			closing = false;
		}
		switch (mountState)
		{
			case -1:
				if (closing)
				{
					logStream (MESSAGE_WARNING) << "startClose: mount is not responding, letting the running close finish" << sendLog;
					break;
				}
				logStream (MESSAGE_ERROR) << "startClose: Cannot close dome, mount is not responding (use 'ignore_mout_state' switch to bypass, but BE SURE the mount is really parked before!)." << sendLog;
				return -1;
				break;
			case 1:
				logStream (MESSAGE_DEBUG) << "startClose: Mount is moving or parking, waiting for it to stop" << sendLog;
				return 1;
				break;
			case 2:
				logStream (MESSAGE_DEBUG) << "startClose: Mount is not within limits at the moment, sending 'park' command." << sendLog;
				rts2core::Connection *connMount = NULL;
				connMount = getOpenConnection (mountDevice);
				if (connMount == NULL)
				{
					logStream (MESSAGE_ERROR) << "startClose: device connection error!" << sendLog;
					return -1;
				}
				connMount->queCommand (new rts2core::Command (this, COMMAND_TELD_PARK));
				return 1;
				break;
		}
	}

	if (closing)
	{
		// closing already in progress
		if (timeToCloseMax == 0)
		{
			time (&timeToCloseMax);
			timeToCloseMin = timeToCloseMax;
			timeToCloseMax += TIME_TO_OPEN_DOME;
		}
		return 0;
	}

	if (getPortState (OTEVIRANI) || getPortState (ZAVIRANI))
	{
		// the dome is just opening, but this is a decent dome, we can simply stop, wait a bit and then start the closing as requested
		if (stopMotors ())
		{
			logStream (MESSAGE_ERROR) << "startClose: cannot stop the opening motor" << sendLog;
			return -1;
		}
		sleep (2);
	}

	if (ZAP (ZAVIRANI) || onlyMotor (ZAVIRANI) != 1)
	{
		logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "startClose: cannot switch on the closing motor" << sendLog;
		stopMotors ();
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "closing the dome" << sendLog;

	timeToOpenMax = timeToOpenMin = 0;
	time (&timeToCloseMax);
	timeToCloseMin = timeToCloseMax + MINIMAL_TIME_TO_MOVE;
	timeToCloseMax += TIME_TO_OPEN_DOME;

	return 0;
}

long D50::isClosed ()
{
	int ret;

	// base note: also called by domeCloseStart () as "is it closed already?", in
	// bad weather at every idle, so -2 needs the switch and no opening motor (the
	// closed switch is still made in the first moments of an opening)
	ret = zjisti_stav_portu ();
	if (ret == 0 && getPortState (KONCAK_ZAVRENI) && !getPortState (OTEVIRANI) && timeToCloseMin < time (NULL))
		return -2;	// dome is closed

	// base note: classic returned -2 (closed) on the timeout
	if (timeToCloseMax != 0 && timeToCloseMax < time (NULL))
	{
		logStream (MESSAGE_ERROR) << "A timeout reached during the dome closing." << sendLog;
		return -1;
	}

	// a failed read keeps the motor going, the timeout ends it
	if (ret)
		logStream (MESSAGE_ERROR) << "isClosed: zjisti_stav_portu () error" << sendLog;

	return USEC_SEC;
}

int D50::endClose ()
{
	int ret;
	timeToCloseMax = timeToCloseMin = 0;
	if (stopMotors ())
		logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "endClose: cannot switch off the closing motor!" << sendLog;
	ret = zjisti_stav_portu ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "endClose: zjisti_stav_portu () error" << sendLog;
		return ret;	// error
	}

	if (!getPortState (KONCAK_ZAVRENI))
		return -1;	// dome is not closed properly :-(

	return 0;
}

// base note: classic acted on the last read state (switching off only the motor
// it believed on) and failed when none was
int D50::stop ()
{
	timeToOpenMax = timeToOpenMin = 0;
	timeToCloseMax = timeToCloseMin = 0;
	return stopMotors ();
}

int D50::getMountState ()
{
	rts2core::Connection *connMount = NULL;
	rts2core::Value *tmpValue = NULL;

	connMount = getOpenConnection (mountDevice);
	if (connMount == NULL)
	{
		logStream (MESSAGE_ERROR) << "getMountState: device connection error!" << sendLog;
		return -1;
	}
	if (connMount->getConnState () != CONN_AUTH_OK && connMount->getConnState () != CONN_CONNECTED)
	{
		logStream (MESSAGE_ERROR) << "getMountState: device connection not ready!" << sendLog;
		return -1;
	}

	tmpValue = connMount->getValue ("infotime");
	if (tmpValue == NULL)
	{
		logStream (MESSAGE_ERROR) << "getMountState: infotime is NULL..." << sendLog;
		return -1;
	}
	if (getNow () - tmpValue->getValueDouble () > 10.0)
	{
		logStream (MESSAGE_ERROR) << "getMountState: infotime too old: " << getNow () - tmpValue->getValueDouble () << "s" << sendLog;
		return -1;
	}

	tmpValue = connMount->getValue (mountInsideDomeVariable);
	if (tmpValue == NULL)
	{
		logStream (MESSAGE_ERROR) << "getMountState: wrong mountInsideDomeVariable variable name?" << sendLog;
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "startClose: getState () & TEL_MASK_MOVING: " << std::hex << (connMount->getState () & TEL_MASK_MOVING) << sendLog;
	// base note: classic trusted inside_dome_limit before looking at the state. The value
	// reaches us with the mount's info, the state at once - a mount that has just started
	// to leave the dome can still show the "inside" of its last park. Wait for it to stop.
	if ((connMount->getState () & TEL_MASK_MOVING) == TEL_PARKING)
	{
		// telescope is just parking...
		return 1;
	}
	if ((connMount->getState () & TEL_MASK_MOVING) == TEL_MOVING)
	{
		logStream (MESSAGE_DEBUG) << "getMountState: mount is moving, waiting for it to stop before trusting " << mountInsideDomeVariable << sendLog;
		return 1;
	}
	if (tmpValue->getValueInteger () == 1)
	{
		return 0;	// telescope is in the safe zone, we can close!
	}
	return 2; // telescope in obstructing the safe dome-close, it also is not parking
}

int D50::init ()
{
	int ret = Ford::init ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "init: Ford::init () error" << sendLog;
		return ret;
	}

	// get state
	ret = zjisti_stav_portu ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "init: zjisti_stav_portu () error" << sendLog;
		return -1;
	}

	// base note: classic left a motor found running here without a timeout (and
	// quit when both were on). With systemd restarting a hung daemon, a restart
	// mid-move is real: stop it, Dome starts the move again under supervision.
	if (getPortState (OTEVIRANI) || getPortState (ZAVIRANI))
	{
		logStream (MESSAGE_WARNING | MESSAGE_REPORTIT) << "a roof motor was running at start (B 0x" << std::hex << (int) getPortB () << "), stopping it" << sendLog;
		if (stopMotors ())
			return -1;
	}

	// base note: classic tested isOn (), which is -1 (true) on a failed read
	if (!getPortState (KONCAK_OTEVRENI) && getPortState (KONCAK_ZAVRENI))
		maskState (DOME_DOME_MASK, DOME_CLOSED, "dome is closed");
	else if (getPortState (KONCAK_OTEVRENI) && !getPortState (KONCAK_ZAVRENI))
		maskState (DOME_DOME_MASK, DOME_OPENED, "dome is open");
	else
		logStream (MESSAGE_ERROR) << "dome is neither open nor closed...?!?!" << sendLog;

	return 0;
}

int D50::setValue (rts2core::Value * old_value, rts2core::Value * new_value)
{
	// nothing here at the moment...
	return Ford::setValue (old_value, new_value);
}

int D50::info ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret)
	{
		logStream (MESSAGE_ERROR) << "info: zjisti_stav_portu () error" << sendLog;
		return -1;
	}
	sw_state->setValueInteger ((getPortState (KONCAK_OTEVRENI))
		| (getPortState (KONCAK_ZAVRENI) << 1)
		| (getPortState (OTEVIRANI) << 2)
		| (getPortState (ZAVIRANI) << 3));

	if (getPortState (OTEVIRANI))
		domeMotorState->setValueInteger (DOME_MOTOR_STATE_OPENING);
	else if (getPortState (ZAVIRANI))
		domeMotorState->setValueInteger (DOME_MOTOR_STATE_CLOSING);
	else
		domeMotorState->setValueInteger (DOME_MOTOR_STATE_OFF);

	domeMotorStateOpening->setValueInteger (getPortState (OTEVIRANI));
	domeMotorStateClosing->setValueInteger (getPortState (ZAVIRANI));
	domeSwitchOpened->setValueInteger (getPortState (KONCAK_OTEVRENI));
	domeSwitchClosed->setValueInteger (getPortState (KONCAK_ZAVRENI));

	// to be independent on teld info period, we ask for it...
	if (!ignoreMountState->getValueBool ())
	{
		rts2core::Connection *connMount = NULL;

		connMount = getOpenConnection (mountDevice);
		if (connMount == NULL)
		{
			logStream (MESSAGE_ERROR) << "info (): mount connection error!" << sendLog;
		}
		else
			connMount->queCommand (new rts2core::Command (this, COMMAND_INFO));
	}

	return Ford::info ();
}

int D50::willConnect (rts2core::NetworkAddress * in_addr)
{
	if (in_addr->getType () == DEVICE_TYPE_MOUNT && in_addr->isAddress (mountDevice)) {
		logStream (MESSAGE_DEBUG) << "D50::willConnect to DEVICE_TYPE_MOUNT: "<< mountDevice << sendLog;
		return 1;
	}
	return Ford::willConnect (in_addr);
}

int D50::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_MOUNT_DEVICE:
			mountDevice = optarg;
			break;
		case OPT_MOUNT_INDOME_VARIABLE:
			mountInsideDomeVariable = optarg;
			break;
		default:
			return Ford::processOption (in_opt);
	}
	return 0;
}

int main (int argc, char **argv)
{
	D50 device (argc, argv);
	return device.run ();
}
