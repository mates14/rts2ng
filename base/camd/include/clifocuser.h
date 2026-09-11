/*
 * Client for focuser attached to the camera.
 * Copyright (C) 2005-2007 Petr Kubanek <petr@kubanek.net>
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

// base note (2026-09-11): ported alongside cliwheel.h. This is the camera-side
// focuser client - distinct from rts2image::DevClientFocusImage, which writes
// a focuser's values into a FITS header and was ported with the image layer.
// Without this class Camera::setFocuser()/offsetForFilter() post
// EVENT_FOCUSER_SET/EVENT_FOCUSER_FILTEROFFSET into the void, so a camera's
// per-filter focus offsets never reach the focuser at all - which is how the
// classic "different filter means refocus" arrangement is meant to work.

#pragma once

#include "devclient.h"
#include "camd.h"

namespace rts2camd
{

/**
 * Client for a focuser a camera drives.
 *
 * Turns the camera's EVENT_FOCUSER_* events into value changes on the
 * focuser's connection - FOC_TAR for an absolute set, FOC_FILTEROFF for a
 * filter offset, FOC_TOFF for a temporary one.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class ClientFocusCamera:public rts2core::DevClientFocus
{
	public:
		ClientFocusCamera (rts2core::Connection * in_connection);
		virtual ~ ClientFocusCamera (void);
		virtual void postEvent (rts2core::Event * event);

	protected:
		virtual void focusingEnd ();
		virtual void focusingFailed (int status);

	private:
		void *activeConn;
};

}
