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

#pragma once

#include "event.h"
#include "value.h"

#define EVENT_COOLER_RAMP     RTS2_LOCAL_EVENT + 677

namespace rts2camd
{

class Camera;

/**
 * What the driver of a cooled camera implements for Cooler. Only the
 * hardware calls; the values, the ramp, night cooling and the stability
 * flag are Cooler's.
 *
 * base note: not in classic. Classic had each driver write its own ramp
 * (gxccd, fli and andor had three diverging copies); see Cooler.
 */
class CoolerHardware
{
	public:
		virtual ~CoolerHardware () {}

		/**
		 * Send a regulation setpoint to the camera.
		 *
		 * @return 0 on success, -1 on error (logged by the driver)
		 */
		virtual int coolerSetPoint (float temp) = 0;

		/**
		 * Switch the cooler itself on or off, for cameras that have such a
		 * switch. Off is called once the ramp has reached the off
		 * temperature. Cameras without one keep the default: they stay
		 * regulated at the off temperature.
		 *
		 * @return 0 on success, -1 on error
		 */
		virtual int coolerPower (bool on) { return 0; }

		/**
		 * True while the camera must not be sent anything; the ramp step
		 * is then retried a few seconds later.
		 */
		virtual bool coolerBusy () { return false; }
};

/**
 * CCD temperature regulation, owned by Camera (Camera::createCooler ()).
 *
 * Values: COOLING, CCD_SET, nightcool, autocooling (Camera's, created by
 * Camera::createTempSet ()), TERAMP (ramp speed), TETAR (the setpoint the
 * camera has now), temp_tol and temp_stable, and TEMPPWR when the driver
 * calls createPower ().
 *
 * Cooling on ramps TETAR from the current CCD temperature to CCD_SET
 * (nightcool when CCD_SET is not set), TERAMP degrees per minute in steps
 * every RAMP_INTERVAL seconds. Cooling off ramps it to the driver's off
 * temperature and then calls CoolerHardware::coolerPower (false). CCD_SET
 * is left as it is, so cooling on again goes back to it. The default
 * TERAMP of 100 C/min makes both practically immediate; a site that wants
 * gentle ramps sets it (it is autosaved).
 *
 * temp_stable is true once cooling is on, the ramp has reached CCD_SET and
 * CCD_TEMP has stayed within temp_tol of it for STABLE_SEC seconds.
 */
class Cooler
{
	public:
		/**
		 * @param _camera   owning camera; creates the values on it
		 * @param _hw       the driver's hardware calls
		 * @param _offTemp  [C] the setpoint cooling off ramps to
		 */
		Cooler (Camera *_camera, CoolerHardware *_hw, float _offTemp);

		/**
		 * For drivers that learn only in initHardware () whether the camera
		 * has a cooler (gxccd). An unavailable cooler refuses COOLING and
		 * CCD_SET changes.
		 */
		void setAvailable (bool a) { available = a; }
		bool isAvailable () { return available; }

		/**
		 * Create TEMPPWR, for cameras that report the cooler's power. Call
		 * it before the device starts serving (constructor or
		 * initHardware ()).
		 */
		void createPower ();

		/**
		 * Report the cooler's power utilization, in percent (0-100; an
		 * SDK giving a fraction is scaled by the driver).
		 */
		void setPower (float percent);

		/**
		 * New CCD_SET. Ramps to it when cooling is on, otherwise only
		 * stores it.
		 */
		int setTarget (float temp);

		/**
		 * COOLING on or off.
		 */
		int switchCooling (bool on);

		/**
		 * One ramp step; the camera's EVENT_COOLER_RAMP. Reuses or deletes
		 * the event.
		 */
		void rampStep (rts2core::Event *event);

		/**
		 * The setpoint the camera has now (TETAR), NAN before the first
		 * ramp step. For a driver restoring it after a reinitialization.
		 */
		float getSetPoint () { return tempTarget->getValueFloat (); }

		/**
		 * Update temp_stable from CCD_TEMP; Camera::info () calls it.
		 */
		void checkStable ();

		static const int RAMP_INTERVAL = 60;
		static const int STABLE_SEC = 120;

	private:
		Camera *camera;
		CoolerHardware *hw;
		float offTemp;
		bool available;

		rts2core::ValueFloat *tempRamp;
		rts2core::ValueFloat *tempTarget;
		rts2core::ValueFloat *tolerance;
		rts2core::ValueBool *stable;
		rts2core::ValueFloat *power;

		// time CCD_TEMP came within tolerance, NAN when it is not
		double stableSince;

		void startRamp ();
};

}
