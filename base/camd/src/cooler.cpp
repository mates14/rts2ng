/*
 * Shared CCD temperature regulation for cooled cameras.
 * Copyright (C) 2026 Martin Jelinek
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

#include <cmath>

#include "cooler.h"
#include "camd.h"
#include "utilsfunc.h"

using namespace rts2camd;

Cooler::Cooler (Camera *_camera, CoolerHardware *_hw, float _offTemp)
{
	camera = _camera;
	hw = _hw;
	offTemp = _offTemp;
	power = nullptr;
	stableSince = NAN;

	camera->createTempSet ();

	camera->createValue (stable, "temp_stable", "CCD temperature has settled at CCD_SET", false);
	stable->setValueBool (false);
	camera->markEssential (stable);

	camera->createValue (tolerance, "temp_tol", "[C] how close to CCD_SET counts as settled", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	tolerance->setValueFloat (1.0);

	// base note: classic's gxccd and fli defaulted to 1 C/min, andor to
	// 100; the shared default is 100, practically an immediate on and off
	camera->createValue (tempRamp, "TERAMP", "[C/min] temperature ramping", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	tempRamp->setValueFloat (100.0);

	camera->createValue (tempTarget, "TETAR", "[C] current target temperature", false);
}

void Cooler::createPower ()
{
	if (power == nullptr)
		camera->createValue (power, "TEMPPWR", "[%] utilization of cooling power", true);
}

void Cooler::setPower (float percent)
{
	if (power)
		power->setValueFloat (percent);
}

int Cooler::setTarget (float temp)
{
	camera->tempSet->setValueDouble (temp);
	camera->sendValueAll (camera->tempSet);
	if (camera->coolingOnOff->getValueBool ())
		startRamp ();
	return 0;
}

int Cooler::switchCooling (bool on)
{
	if (on)
	{
		if (std::isnan (camera->tempSet->getValueFloat ()))
		{
			if (camera->nightCoolTemp == nullptr || std::isnan (camera->nightCoolTemp->getValueFloat ()))
			{
				logStream (MESSAGE_ERROR) << "cooling on: neither CCD_SET nor nightcool is set" << sendLog;
				return -1;
			}
			camera->tempSet->setValueDouble (camera->nightCoolTemp->getValueFloat ());
			camera->sendValueAll (camera->tempSet);
		}
		if (hw->coolerPower (true))
			return -1;
		// a ramp from an off cooler starts at the CCD's temperature, not at
		// the off setpoint it was left at
		if (!camera->coolingOnOff->getValueBool ())
			tempTarget->setValueFloat (NAN);
	}

	camera->coolingOnOff->setValueBool (on);
	camera->sendValueAll (camera->coolingOnOff);
	startRamp ();
	return 0;
}

void Cooler::startRamp ()
{
	camera->deleteTimers (EVENT_COOLER_RAMP);
	camera->addTimer (1, new rts2core::Event (EVENT_COOLER_RAMP));
	checkStable ();
}

void Cooler::rampStep (rts2core::Event *event)
{
	if (hw->coolerBusy ())
	{
		camera->addTimer (5, event);
		return;
	}

	bool on = camera->coolingOnOff->getValueBool ();
	float goal = on ? camera->tempSet->getValueFloat () : offTemp;
	if (std::isnan (goal))
	{
		delete event;
		return;
	}

	float cur = tempTarget->getValueFloat ();
	if (std::isnan (cur))
	{
		cur = camera->tempCCD ? camera->tempCCD->getValueFloat () : NAN;
		if (std::isnan (cur))
		{
			// no temperature yet: give info () time to read one
			camera->addTimer (10, event);
			return;
		}
	}

	float step = tempRamp->getValueFloat () * RAMP_INTERVAL / 60.0;
	float next;
	if (std::isnan (step) || step <= 0 || fabs (goal - cur) <= step)
		next = goal;
	else
		next = cur + (goal > cur ? step : -step);

	// the setpoint is sent at every step, also when it did not change,
	// so a camera that lost it (a reinit) gets it again
	if (hw->coolerSetPoint (next))
		logStream (MESSAGE_ERROR) << "cannot set CCD setpoint to " << next << sendLog;
	tempTarget->setValueFloat (next);
	camera->sendValueAll (tempTarget);

	if (next != goal)
	{
		camera->addTimer (RAMP_INTERVAL, event);
	}
	else
	{
		if (!on && hw->coolerPower (false))
			logStream (MESSAGE_ERROR) << "cannot switch the cooler off" << sendLog;
		delete event;
	}
	checkStable ();
}

void Cooler::checkStable ()
{
	float set = camera->tempSet->getValueFloat ();
	float ccd = camera->tempCCD ? camera->tempCCD->getValueFloat () : NAN;
	bool within = camera->coolingOnOff->getValueBool ()
		&& tempTarget->getValueFloat () == set
		&& fabs (ccd - set) <= tolerance->getValueFloat ();

	if (!within)
		stableSince = NAN;
	else if (std::isnan (stableSince))
		stableSince = getNow ();

	bool s = within && getNow () - stableSince >= STABLE_SEC;
	if (s != stable->getValueBool ())
	{
		stable->setValueBool (s);
		camera->sendValueAll (stable);
	}
}
