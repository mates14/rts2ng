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

#pragma once

#include <string>
#include <vector>

namespace rts2teld
{

// arcsec of HA in one worm turn (1/480 of the wheel)
const double PEC_HA_PER_TURN = 2700.0;
// a factor further from 1 than this is refused: the 2026-11-14 model needs
// up to 2.3 % (h7, h8 are steep); the guide pulses use 10 %
const double PEC_MAX_CHANGE = 0.05;

class PecModel
{
	public:
		/**
		 * Read the model: one line per harmonic, "east|west <h> <c> <s>",
		 * c and s in arcsec of HA; # starts a comment.
		 *
		 * @return true on success; false with err set (the model stays empty)
		 */
		bool load (const std::string &path, std::string &err);

		bool empty () const { return east.empty () && west.empty (); }

		/** the sky error [arcsec HA] at phi [rad], west weight w (0 east .. 1 west) */
		double error (double phi, double w) const;

		/**
		 * Step frequency factor at phi [rad] and west weight w; NAN when it
		 * would change the frequency by more than PEC_MAX_CHANGE.
		 */
		double factor (double phi, double w) const;

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

		static double value (const std::vector<Term> &t, double phi);
		static double slope (const std::vector<Term> &t, double phi);
};

}
