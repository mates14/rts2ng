/*
 * Copyright (C) Petr Kubanek <petr@kubanek.net>
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

// base note: classic tree has this at include/rts2fits/devclifoc.h -
// flattened to kernel/include/devclifoc.h, same convention as
// fitsfile.h/channel.h/image.h/cameraimage.h/devcliimg.h.

#pragma once

#include "devcliimg.h"
#include "connfork.h"

#include <fstream>
#include <vector>

#define EVENT_START_FOCUSING  RTS2_LOCAL_EVENT + 500
#define EVENT_FOCUSING_END  RTS2_LOCAL_EVENT + 501
#define EVENT_CHANGE_FOCUS  RTS2_LOCAL_EVENT + 502

namespace rts2image
{

class ConnFocus;

class DevClientCameraFoc:public DevClientCameraImage
{
	public:
		DevClientCameraFoc (rts2core::Connection * in_connection, const char *in_exe);
		virtual ~ DevClientCameraFoc (void);
		virtual void postEvent (rts2core::Event * event);
		virtual imageProceRes processImage (Image * image);
		// will cause camera to change focus by given steps BEFORE exposition
		// when change == INT_MAX, focusing don't converge
		virtual void focusChange (rts2core::Connection * focus);

	protected:
		char *exe;

		ConnFocus *focConn;

	private:
		int isFocusing;

};

class DevClientFocusFoc:public DevClientFocusImage
{
	public:
		DevClientFocusFoc (rts2core::Connection * in_connection);
		virtual void postEvent (rts2core::Event * event);

	protected:
		virtual void focusingEnd ();
};

class ConnFocus:public rts2core::ConnFork
{
	public:
		ConnFocus (rts2core::Block * in_master, Image * in_image, const char *in_exe, int in_endEvent);
		virtual ~ ConnFocus (void);
		virtual int newProcess ();
		virtual void processLine ();
		int getChange () { return change; }
		void setChange (int new_change) { change = new_change; }
		const char *getCameraName () { return image->getCameraName (); }
		Image *getImage () { return image; }
		void nullCamera () { image = NULL; }

	protected:
		virtual void initFailed ();
		virtual void beforeFork ();

	private:
		char *img_path;
		Image *image;
		int change;
		int endEvent;
};

class DevClientPhotFoc:public rts2core::DevClientPhot
{
	public:
		DevClientPhotFoc (rts2core::Connection * in_conn, char *in_photometerFile, float in_photometerTime, int in_photometerFilterChange, std::vector < int >in_skipFilters);
		virtual ~ DevClientPhotFoc (void);

	protected:
		virtual void addCount (int count, float exp, bool is_ov);

	private:
		std::ofstream os;
		char *photometerFile;
		float photometerTime;
		int photometerFilterChange;
		int countCount;
		std::vector < int >skipFilters;
		int newFilter;
};

}
