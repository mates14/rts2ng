/*
 * Client for filter wheel attached to the camera.
 * Copyright (C) 2005-2008,2012 Petr Kubanek <petr@kubanek.net>
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

// base note (2026-09-11): ported. This was deferred during the original camd
// port on the reasoning that a camera without --wheeldev never needs it. That
// was wrong in one respect that only showed up in real data: a camera *with*
// --wheeldev still creates FILTA/FILTB (a ValueSelection with write_to_fits),
// so without this class the header records a filter index that never moves
// off zero - a FITS file that confidently names the wrong filter. See
// base/STATUS.md.
//
// The event codes and struct filterStart live in camd.h rather than here,
// because Camera uses them unconditionally - see the note there.

#pragma once

#include "devclient.h"
#include "camd.h"

namespace rts2camd
{

class FilterVal;

/**
 * Client for a filter wheel a camera drives.
 *
 * Translates the camera's EVENT_FILTER_* events into commands on the wheel's
 * connection, and reports the wheel's movement back to the camera.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class ClientFilterCamera:public rts2core::DevClientFilter
{
	public:
		ClientFilterCamera (rts2core::Connection * conn, FilterVal *fv);
		virtual ~ ClientFilterCamera (void);
		virtual void filterMoveFailed (int status);
		virtual void postEvent (rts2core::Event * event);
		virtual void valueChanged (rts2core::Value * value);
	protected:
		virtual void filterMoveEnd ();
	private:
		FilterVal *filterVal;
};

}
