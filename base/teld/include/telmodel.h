/*
 * Telescope T-Point pointing model.
 * Copyright (C) 2006-2015 Petr Kubanek <petr@kubanek.net>
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

// base note: this abstract interface is ported in full (it's small and
// pure-virtual, no .cpp needed), so Telescope::model (a TelModel*) type-
// checks correctly. The concrete implementations - GPointModel and
// TPointModel (lib/rts2tel/gpointmodel.cpp + tpointmodel.cpp +
// tpointmodelterm.cpp, ~1090 lines) - are NOT ported: they're only ever
// constructed behind the --rts2-model/--t-point-model command-line options
// (see teld.cpp's init()/signaledHUP()), so `model` stays nullptr unless a
// driver explicitly loads a pointing-model file, which dummy never does.
// Same deferral reasoning as SEP in camd and SimbadTarget in the monitor.

/**
 * @file
 * Basic TPoint routines.
 *
 * @defgroup RTS2TPoint TPoint interface
 */

#include "logstream.h"

#include <libnova/libnova.h>
#include <iostream>

namespace rts2telmodel
{

/**
 * Telescope pointing model abstract class.
 * Child class should implement abstract model, and can be loaded
 * from teld.cpp source file for different pointing model corrections.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 *
 * @ingroup RTS2TelModel
 */
class TelModel
{
	public:
		TelModel (double in_latitude)
		{
			tel_latitude = in_latitude;
			tel_latitude_r = ln_deg_to_rad (in_latitude);
		}

		virtual ~ TelModel (void)
		{
		}

		virtual int load (const char *modelFile) = 0;
		/**
		 * Apply model to coordinates. Pos.ra is hour angle, not RA.
		 */
		virtual int apply (struct ln_equ_posn *pos) = 0;
		virtual int applyVerbose (struct ln_equ_posn *pos) = 0;

		virtual int reverse (struct ln_equ_posn *pos, struct ln_hrz_posn *hrz) = 0;
		virtual int reverseVerbose (struct ln_equ_posn *pos, struct ln_hrz_posn *hrz) = 0;

		virtual void getErrAltAz (struct ln_hrz_posn *hrz, struct ln_equ_posn *equ, struct ln_hrz_posn *err) { logStream (MESSAGE_ERROR) << "unexpected getErrAltAz call" << sendLog; };

                virtual double getRMS () { return -1; }

		virtual std::istream & load (std::istream & is) = 0;
		virtual std::ostream & print (std::ostream & os, char frmt = 'r') = 0;

		double getLatitudeRadians () { return tel_latitude_r; }

	protected:
		double tel_latitude;
		double tel_latitude_r;
};

}
