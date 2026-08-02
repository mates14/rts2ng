/*
 * Generic class for focusing.
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

// base note: classic tree has this at src/focusc/focusclient.h. EVENT_
// INTEGRATE_START/EVENT_INTEGRATE_STOP/EVENT_XWIN_SOCK dropped - grepping
// the classic tree shows they're only ever used by xfitsimage.cpp/
// xfocusc.cpp (the X11 GUI, deliberately out of scope here - see the
// separate rts2x/rts2-viewer plan for that tool), never by
// focusclient.cpp/focusc.cpp itself. EVENT_EXP_CHECK is kept - it drives
// this CLI tool's own live exposure/readout progress indicator.
//
// EVENT_CAMERA_CHOICE is new (no classic equivalent) - see the "no -d
// given" fix in focusclient.cpp for what it does.

#pragma once

#include "client.h"
#include "devclifoc.h"

#include <string>
#include <vector>

#define EVENT_EXP_CHECK RTS2_LOCAL_EVENT + 305
#define EVENT_CAMERA_CHOICE RTS2_LOCAL_EVENT + 306

class FocusCameraClient;

class FocusClient:public rts2core::Client
{
	public:
		FocusClient (int argc, char **argv);
		virtual ~ FocusClient (void);

		virtual rts2core::DevClient *createOtherType (rts2core::Connection * conn, int other_device_type);
		virtual int init ();
		virtual int run ();

		virtual void postEvent (rts2core::Event *event);

		float defaultExpousure () { return defExposure; }
		const char *getExePath () { return focExe; }
		int getAutoSave () { return autoSave; }
		int getFocusingQuery () { return query; }
		int getAutoDark () { return autoDark; }

		bool printChanges () { return printStateChanges; };

	protected:
		int autoSave;

		virtual int processOption (int in_opt);
		std::vector < char *>cameraNames;

		char *focExe;

		virtual FocusCameraClient *createFocCamera (rts2core::Connection * conn);

	private:
		// Cameras this process itself has told to expose (either named via
		// -d, or picked by the no-camera-given auto-selection below) - the
		// only ones stopOwnedExposures() is allowed to send a stopexpo to
		// on shutdown. Never touch a camera we're only passively watching.
		std::vector < std::string > armedCameras;

		bool isNamedCamera (const char *camName);
		void armCamera (FocusCameraClient * cam);
		void stopOwnedExposures ();
		void warnIfSettingsAmbiguous ();

		float defExposure;
		int defCenter;
		int defBin;

		// to take darks images, set that to true
		bool darks;

		int xOffset;
		int yOffset;

		int imageHeight;
		int imageWidth;

		// How many times -e/-b/-X/-Y/-W/-H were each given on the command
		// line - not what they were last set to. -e/-b/... are single
		// scalars applied identically to every -d camera (see
		// warnIfSettingsAmbiguous()); a flag given more than once while
		// more than one camera is named is a strong sign the user expected
		// per-camera settings, which this tool does not support.
		int optExposureCount;
		int optBinCount;
		int optXCount;
		int optYCount;
		int optWidthCount;
		int optHeightCount;

		int autoDark;
		int query;
		double tarRa;
		double tarDec;

		char *photometerFile;
		float photometerTime;
		int photometerFilterChange;

		std::vector < int >skipFilters;

		char *configFile;
		int bop;
		bool printStateChanges;
};

class FocusCameraClient:public rts2image::DevClientCameraFoc
{
	public:
		FocusCameraClient (rts2core::Connection * in_connection, FocusClient * in_master);
		virtual ~ FocusCameraClient (void);

		virtual void postEvent (rts2core::Event *event);

		virtual void stateChanged (rts2core::ServerState * state);
		virtual rts2image::Image *createImage (const struct timeval *expStart);
		void center (int centerWidth, int centerHeight);

		virtual rts2image::imageProceRes processImage (rts2image::Image * image);

		/**
		 * Set condition mask describing when command cannot be send.
		 *
		 * @param bop Something from BOP_EXPOSURE,.. values
		 */
		void setBop (int _bop) { bop = _bop; }

		int singleSave;
	protected:
		int autoSave;

		virtual void exposureStarted (bool expectImage);

	private:
		FocusClient * master;
		int bop;
};
