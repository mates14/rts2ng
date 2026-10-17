/*
 * Driver for Ford boards.
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

#define CAS_NA_OTEVRENI              20
// how often a roof that should be closed is checked against the closed switch
#define ROOF_CHECK_INTERVAL          10
// how often a roof found open is tried again (it may be under manual control)
#define ROOF_RECLOSE_INTERVAL        120

typedef enum
{
	ZASUVKA_1, ZASUVKA_2, ZASUVKA_3, ZASUVKA_4, ZASUVKA_5, ZASUVKA_6, MOTOR,
	SMER, SVETLO, KONCAK_ZAVRENI_JIH, KONCAK_OTEVRENI_JIH,
	KONCAK_OTEVRENI_SEVER, KONCAK_ZAVRENI_SEVER
} vystupy;

#define NUM_ZAS   6

#define OFF   0
#define STANDBY   1
#define OBSERVING 2

// zasuvka c.1 kamera (NF), c.2 kamera (WF), c.5 montaz. c.6 topeni /Torman 18.2.2013
int zasuvky_index[NUM_ZAS] =
{ ZASUVKA_1, ZASUVKA_2, ZASUVKA_3, ZASUVKA_4, ZASUVKA_5, ZASUVKA_6 };

enum stavy
{ ZAS_VYP, ZAS_ZAP };

// prvni index - cislo stavu (0 - off, 1 - standby, 2 - observing), druhy je stav zasuvky cislo j)
enum stavy zasuvky_stavy[3][NUM_ZAS] =
{
	// off
	{ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_VYP},
	// standby
	{ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_VYP},
	// observnig
	{ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP, ZAS_ZAP}
};

using namespace rts2dome;


namespace rts2dome
{

/**
 * Class for BART dome control.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class Bart:public Ford
{
	private:
		int handle_zasuvky (int zas);
		int stopMotor ();
		bool motorRunning (bool closing);

		// base note: classic shared one domeTimeout between opening and closing, so
		// isOpened () called on a roof closing for more than 20 s (an open command)
		// timed out and reported it opened
		time_t openTimeout;
		time_t closeTimeout;
		bool switchFaultReported;

		// a roof found open while it should be closed was already reported;
		// cleared when the closed switch makes again
		bool roofFoundOpen;
		double lastRoofCheck;
		double nextQuietClose;

		rts2core::ValueInteger *sw_state;
		rts2core::ValueBool *socket1, *socket2, *socket3, *socket4, *socket5, *socket6;
		rts2core::ValueBool *domeMotorPower;
		rts2core::ValueSelection *domeMotorDirection, *domeSouthSwitchOpened, *domeSouthSwitchClosed;
		rts2core::ValueBool *domeNorthSwitchOpened, *domeNorthSwitchClosed;

	protected:
		virtual int init ();


		virtual int info ();
		virtual int idle ();

		virtual int setValue (rts2core::Value * old_value, rts2core::Value * new_value);

		virtual int startOpen ();
		virtual long isOpened ();
		virtual int endOpen ();
		virtual int startClose ();
		virtual long isClosed ();
		virtual int endClose ();

		virtual int observing ();
		virtual int standby ();
		virtual int off ();

	public:
		Bart (int argc, char **argv);
		virtual ~Bart ();
};

}

Bart::Bart (int argc, char **argv):Ford (argc, argv)
{
	openTimeout = 0;
	closeTimeout = 0;
	switchFaultReported = false;
	roofFoundOpen = false;
	lastRoofCheck = 0;
	nextQuietClose = 0;

	createValue (sw_state, "sw_state", "state of dome switches", false, RTS2_DT_HEX);
	createValue (domeMotorPower, "motor_power", "power to dome-motor", false, RTS2_DT_ONOFF);
	createValue (domeMotorDirection, "motor_direction", "dome-motor direction", false, RTS2_DT_ONOFF);
	domeMotorDirection->addSelVal ("opening (off)");
	domeMotorDirection->addSelVal ("closing (on)");
	createValue (domeSouthSwitchOpened, "south_roof_opened", "switch on south roof, opened state", false, RTS2_DT_ONOFF);
	domeSouthSwitchOpened->addSelVal ("---");
	domeSouthSwitchOpened->addSelVal ("OPENED");
	createValue (domeSouthSwitchClosed, "south_roof_closed", "switch on south roof, closed state", false, RTS2_DT_ONOFF);
	domeSouthSwitchClosed->addSelVal ("---");
	domeSouthSwitchClosed->addSelVal ("CLOSED");
	// base note: the north switches are wired (BART logs show their bits) but their
	// meaning is not confirmed, so they are only shown, not used
	createValue (domeNorthSwitchOpened, "north_roof_opened", "switch on north roof, opened state (not used)", false, RTS2_DT_ONOFF);
	createValue (domeNorthSwitchClosed, "north_roof_closed", "switch on north roof, closed state (not used)", false, RTS2_DT_ONOFF);
	createValue (socket1, "socket1", "wall socket #1 (camera C1)", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
	createValue (socket2, "socket2", "wall socket #2 (camera C2)", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
	createValue (socket3, "socket3", "wall socket #3 (camera C3)", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
	createValue (socket4, "socket4", "wall socket #4", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
	createValue (socket5, "socket5", "wall socket #5 (mount T0)", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
	createValue (socket6, "socket6", "wall socket #6 (anti-dew heating of NF & WF)", false, RTS2_DT_ONOFF | RTS2_VALUE_WRITABLE);
}

Bart::~Bart ()
{
}

// Switches the motor off and checks that it is off. Every end of a move and
// every failed start goes through here.
int Bart::stopMotor ()
{
	for (int i = 0; i < 3; i++)
	{
		if (VYP (MOTOR) == 0 && isOn (MOTOR) == 1)
			return 0;
		sleep (1);
	}
	logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "cannot switch the roof motor off" << sendLog;
	return -1;
}

bool Bart::motorRunning (bool closing)
{
	if (zjisti_stav_portu ())
		return false;
	return getPortState (MOTOR) && getPortState (SMER) == closing;
}

// base note: classic ignored the result of every pin write and of isOn () (-1 on
// a failed read counted as true). A direction write lost to one bad read let the
// next ZAP (MOTOR) drive a closing roof open. Each step is now checked, and the
// motor is stopped again unless it runs in the intended direction.
int Bart::startOpen ()
{
	int notOpened = isOn (KONCAK_OTEVRENI_JIH);
	if (notOpened < 0)
		return -1;
	if (!notOpened)
		return endOpen ();
	if (stopMotor ())
		return -1;
	sleep (1);
	if (VYP (SMER) || isOn (SMER) != 1)
	{
		logStream (MESSAGE_ERROR) << "cannot set the roof motor direction to opening" << sendLog;
		return -1;
	}
	sleep (1);
	if (ZAP (MOTOR) || !motorRunning (false))
	{
		stopMotor ();
		logStream (MESSAGE_ERROR) << "roof motor did not start in the opening direction, stopped" << sendLog;
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "oteviram strechu" << sendLog;

	closeTimeout = 0;
	time (&openTimeout);
	openTimeout += CAS_NA_OTEVRENI;

	return 0;
}

long Bart::isOpened ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret == 0 && getPortState (KONCAK_OTEVRENI_JIH))
		return -2;
	// the BART open switch rarely makes (2026-10 logs): most openings end here
	if (openTimeout != 0 && openTimeout < time (NULL))
	{
		logStream (MESSAGE_ERROR) << "Timeout reached during dome opening, dome is regarded as opened" << sendLog;
		return -2;
	}
	// base note: classic returned the failed read (-1), and Dome then stopped the
	// roof half open and marked it opened. One bad reply is not worth that; a
	// failed read keeps the move going, and the timeout ends it.
	return USEC_SEC;
}

int Bart::endOpen ()
{
	int ret = stopMotor ();
	zjisti_stav_portu ();		 //kdyz se to vynecha, neposle to posledni prikaz nebo znak
	setTimeout (USEC_SEC);
	openTimeout = 0;
	return ret;
}

int Bart::startClose ()
{
	int notClosed;
	int motor;
	int smer;
	notClosed = isOn (KONCAK_ZAVRENI_JIH);
	if (notClosed < 0)
		return -1;
	if (!notClosed)
		return endClose ();
	motor = isOn (MOTOR);
	smer = isOn (SMER);
	if (motor == -1 || smer == -1)
	{
		// errror
		return -1;
	}
	if (!motor)
	{
		// closing in progress
		if (!smer)
		{
			// base note: a close found running (at startup) had no timeout
			if (closeTimeout == 0)
			{
				time (&closeTimeout);
				closeTimeout += CAS_NA_OTEVRENI;
			}
			return 0;
		}
		// let it go to open state and then close it..
		// (kept as classic: the roof may have to finish opening before it can
		// reverse; to be tested on site. Dome retries the close every 30 s.)
		return -1;
	}
	if (ZAP (SMER) || isOn (SMER) != 0)
	{
		logStream (MESSAGE_ERROR) << "cannot set the roof motor direction to closing" << sendLog;
		return -1;
	}
	sleep (1);
	if (ZAP (MOTOR) || !motorRunning (true))
	{
		stopMotor ();
		logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "roof motor did not start in the closing direction, stopped" << sendLog;
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "zaviram strechu" << sendLog;

	openTimeout = 0;
	time (&closeTimeout);
	closeTimeout += CAS_NA_OTEVRENI;

	return 0;
}

long Bart::isClosed ()
{
	int ret;

	ret = zjisti_stav_portu ();
	if (ret == 0 && getPortState (KONCAK_ZAVRENI_JIH))
	{
		roofFoundOpen = false;
		return -2;
	}

	// base note: classic returned -2 here, which Dome takes for "closed": a roof
	// that never reached the switch was logged "dome closed" and, through the
	// domeCloseStart () probe, the motor was left running (BART, 2026-09-15). It
	// is an error now, so Dome stops the motor, retries and reports it.
	if (closeTimeout != 0 && closeTimeout < time (NULL))
	{
		logStream (closingQuietly () ? MESSAGE_INFO : MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "roof did not reach the closed switch within " << CAS_NA_OTEVRENI << " s" << sendLog;
		return -1;
	}

	// a failed read while closing keeps the motor going; the timeout ends it
	return USEC_SEC;
}

int Bart::endClose ()
{
	closeTimeout = 0;
	// base note: classic switched the motor off only when it read it as running,
	// so a failed or wrong read left it on
	int ret = stopMotor ();
	sleep (1);
	if (VYP (SMER))
		logStream (MESSAGE_ERROR) << "cannot reset the roof motor direction" << sendLog;
	zjisti_stav_portu ();
	return ret;
}

int Bart::init ()
{
	int ret = Ford::init ();
	if (ret)
		return ret;


	// get state
	ret = zjisti_stav_portu ();
	if (ret)
		return -1;

	bool opened = getPortState (KONCAK_OTEVRENI_JIH);
	bool closed = getPortState (KONCAK_ZAVRENI_JIH);
	if (closed && !opened)
		maskState (DOME_DOME_MASK, DOME_CLOSED, "dome is closed");
	else if (opened && !closed)
		maskState (DOME_DOME_MASK, DOME_OPENED, "dome is opened");

	return 0;
}

int Bart::handle_zasuvky (int zas)
{
	int i;
	for (i = 0; i < NUM_ZAS; i++)
	{
		int zasuvka_num = zasuvky_index[i];
		if (zasuvky_stavy[zas][i] == ZAS_VYP)
		{
			if (ZAP (zasuvka_num))
				logStream (MESSAGE_ERROR) << "cannot switch off socket " << (i + 1) << sendLog;
		}
		else
		{
			if (VYP (zasuvka_num))
				logStream (MESSAGE_ERROR) << "cannot switch on socket " << (i + 1) << sendLog;
		}
		sleep (1);				 // doplnil Ford
	}
	return 0;
}

int Bart::setValue (rts2core::Value * old_value, rts2core::Value * new_value)
{
	if (old_value == socket1)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_1) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_1) == 0 ? 0 : -2;
		}
	}
	if (old_value == socket2)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_2) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_2) == 0 ? 0 : -2;
		}
	}
	if (old_value == socket3)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_3) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_3) == 0 ? 0 : -2;
		}
	}
	if (old_value == socket4)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_4) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_4) == 0 ? 0 : -2;
		}
	}
	if (old_value == socket5)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_5) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_5) == 0 ? 0 : -2;
		}
	}
	if (old_value == socket6)
	{
		if (((rts2core::ValueBool* )new_value)->getValueBool () == true)
		{
			return VYP(ZASUVKA_6) == 0 ? 0 : -2;
		}
		else
		{
			return ZAP(ZASUVKA_6) == 0 ? 0 : -2;
		}
	}
	return Ford::setValue (old_value, new_value);
}

int Bart::info ()
{
	int ret;
	ret = zjisti_stav_portu ();
	if (ret)
		return -1;
	sw_state->setValueInteger ((!getPortState (KONCAK_OTEVRENI_JIH))
		| (!getPortState (SMER) << 1)
		| (!getPortState (KONCAK_ZAVRENI_JIH) << 2)
		| (!getPortState (MOTOR) << 3));
	domeMotorPower->setValueBool (getPortState (MOTOR));
	domeMotorDirection->setValueInteger (getPortState (SMER));
	domeSouthSwitchOpened->setValueInteger (getPortState (KONCAK_OTEVRENI_JIH));
	domeSouthSwitchClosed->setValueInteger (getPortState (KONCAK_ZAVRENI_JIH));
	domeNorthSwitchOpened->setValueBool (getPortState (KONCAK_OTEVRENI_SEVER));
	domeNorthSwitchClosed->setValueBool (getPortState (KONCAK_ZAVRENI_SEVER));
	if (getPortState (KONCAK_OTEVRENI_JIH) && getPortState (KONCAK_ZAVRENI_JIH))
	{
		if (!switchFaultReported)
			logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "both south roof switches (opened and closed) are active, check the wiring" << sendLog;
		switchFaultReported = true;
	}
	else
	{
		switchFaultReported = false;
	}
	socket1->setValueBool (!getPortState (ZASUVKA_1));
	socket2->setValueBool (!getPortState (ZASUVKA_2));
	socket3->setValueBool (!getPortState (ZASUVKA_3));
	socket4->setValueBool (!getPortState (ZASUVKA_4));
	socket5->setValueBool (!getPortState (ZASUVKA_5));
	socket6->setValueBool (!getPortState (ZASUVKA_6));
	return Ford::info ();
}

// base note: classic read the end switches only on a state change or in bad
// weather, so a roof opened by hand and then handed back to PC control stayed
// open while the weather was good. The box does not report its manual/PC
// selector, so a roof found open cannot be told from a visitor working it by
// hand: it is reported once as a warning and then closed quietly every
// ROOF_RECLOSE_INTERVAL s (in manual the relays are bypassed and the hardware
// end switches stop the motor), so it closes soon after PC control is back.
// In bad weather Dome closes it as always (retries, EMERGENCY).
int Bart::idle ()
{
	// a failing ordinary close is left to Dome: a quiet one would cancel its
	// retries and the EMERGENCY at their end
	if ((getState () & DOME_DOME_MASK) == DOME_CLOSED && autoCloseEnabled () && !closeFailing ()
		&& getNow () > lastRoofCheck + ROOF_CHECK_INTERVAL && isGoodWeather ())
	{
		lastRoofCheck = getNow ();
		if (zjisti_stav_portu () == 0)
		{
			if (getPortState (KONCAK_ZAVRENI_JIH))
			{
				roofFoundOpen = false;
			}
			else if (!getPortState (MOTOR) && getNow () > nextQuietClose)
			{
				if (!roofFoundOpen)
					logStream (MESSAGE_WARNING | MESSAGE_REPORTIT) << "roof found open while it should be closed (by hand or wind?), closing it, then every " << ROOF_RECLOSE_INTERVAL << " s" << sendLog;
				roofFoundOpen = true;
				nextQuietClose = getNow () + ROOF_RECLOSE_INTERVAL;
				domeCloseStart (true);
			}
		}
	}
	return Ford::idle ();
}

int Bart::off ()
{
	Ford::off ();
	return handle_zasuvky (OFF);
}

int Bart::standby ()
{
	Ford::standby ();
	return handle_zasuvky (STANDBY);
}

int Bart::observing ()
{
	handle_zasuvky (OBSERVING);
	return Ford::observing ();
}

int main (int argc, char **argv)
{
	Bart device (argc, argv);
	return device.run ();
}
