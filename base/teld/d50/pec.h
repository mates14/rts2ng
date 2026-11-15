/*
 * D50 periodic error correction: the worm model and the step frequency factor.
 * Copyright (C) 2026 Jan Strobl
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

// base note: wholly new, stage 3 of the D50 periodic error correction
// [JS 2026-11-14] (base/HISTORY.md, "D50 PEC"). The sky error the mount
// makes without guiding, in arcsec of HA, is a function of the worm angle
// phi (from the worm encoder, phi = 2 pi S / 2^23), one shape for each side
// of the meridian:
//	p (phi) = sum over h of c_h cos (h phi) + s_h sin (h phi)
// The guider's offset moves by +1 x the commanded extra motion (measured
// on 2026-11-13: +0.96), so cancelling p needs the step frequency
//	f = f_default * (1 - 2 pi / 2700 * dp/dphi)
// (2700" of HA in one worm turn). This holds whichever way the encoder
// counts. Between the two sides the shapes are blended linearly in HA.
// Optional motor terms [JS 2026-11-15]: the gearbox between the motor and
// the worm adds an error locked to the motor angle theta (the RA servo's
// counts mod 65536, one motor turn = 1/4.8 worm turn), the same on both
// sides (HISTORY, "the first night with PEC and the servo position"):
//	m (theta) = sum over k of c_k cos (k theta) + s_k sin (k theta)
// The servo counts down while tracking, as the worm encoder does, so a
// motor term enters the factor as a worm term does, 4.8 times steeper:
//	f = f_default * (1 - 2 pi / 2700 * (dp/dphi + 4.8 dm/dtheta))

#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace rts2teld
{

// arcsec of HA in one worm turn (1/480 of the wheel)
const double PEC_HA_PER_TURN = 2700.0;
// motor turns in one worm turn (RA_TICKS = 2304 x 65536, 2304 = 480 x 4.8),
// and servo counts in one motor turn
const double PEC_MOTOR_PER_TURN = 4.8;
const int PEC_MOTOR_COUNTS = 65536;
// a factor further from 1 than this is refused: the 2026-11-14 model needs
// up to 2.3 % (h7, h8 are steep); the guide pulses use 10 %
const double PEC_MAX_CHANGE = 0.05;

class PecModel
{
	public:
		/**
		 * Read the model: one line per harmonic, "east|west <h> <c> <s>",
		 * and optionally "motor <k> <c> <s>", c and s in arcsec of HA;
		 * # starts a comment.
		 *
		 * @return true on success; false with err set (the model stays empty)
		 */
		bool load (const std::string &path, std::string &err);

		bool empty () const { return east.empty () && west.empty (); }
		bool hasMotor () const { return !motor.empty (); }

		/**
		 * The sky error [arcsec HA] at phi [rad], west weight w (0 east .. 1
		 * west), and the motor angle theta [rad]; theta NAN leaves the motor
		 * terms out.
		 */
		double error (double phi, double w, double theta = NAN) const;

		/**
		 * Step frequency factor at phi [rad], west weight w and motor angle
		 * theta [rad] (NAN: worm terms only); NAN when it would change the
		 * frequency by more than PEC_MAX_CHANGE.
		 */
		double factor (double phi, double w, double theta = NAN) const;

		/** the motor angle [rad] of a servo position [counts] */
		static double motorAngle (double counts);

		/** west weight at HA [h]: 0 up to haEast, 1 from haWest, linear between */
		static double westWeight (double ha, double haEast, double haWest);

	private:
		struct Term
		{
			int h;
			double c;
			double s;
		};
		std::vector<Term> east;
		std::vector<Term> west;
		std::vector<Term> motor;

		static double value (const std::vector<Term> &t, double phi);
		static double slope (const std::vector<Term> &t, double phi);
};

}
