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

#include "focusclient.h"
#include "configuration.h"
#include "utilsfunc.h"

// base note: classic used autotools RTS2_HAVE_CURSES_H/RTS2_HAVE_NCURSES_
// CURSES_H detection to pick between <curses.h>/<ncurses/curses.h>; base
// targets modern Linux only (see STATUS.md conventions), so this goes
// straight to the plain system headers, same as rts2-mon's ncurses use.
#include <curses.h>
#include <term.h>

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <csignal>
#include <cstring>

#define OPT_CHANGE_FILTER   OPT_LOCAL + 50
#define OPT_SKIP_FILTER     OPT_LOCAL + 51
#define OPT_PHOTOMETER_TIME OPT_LOCAL + 52
#define OPT_NOSYNC          OPT_LOCAL + 53
#define OPT_DARK            OPT_LOCAL + 54
#define OPT_IGNORE_BLOCK    OPT_LOCAL + 55

#define CHECK_TIMER         0.1

// How long to wait, after login, for centrald to report every currently
// connected camera before deciding what "-d not given" means. Only used
// when cameraNames is empty - see the EVENT_CAMERA_CHOICE handler below.
#define CAMERA_CHOICE_DELAY 1.0

// How long to give a stopexpo command (see stopOwnedExposures()) to
// actually reach the device and be acknowledged before giving up and
// letting the process exit anyway.
#define STOP_EXPOSURE_GRACE 2.0

FocusCameraClient::FocusCameraClient (rts2core::Connection * in_connection, FocusClient * in_master):rts2image::DevClientCameraFoc (in_connection, in_master->getExePath ())
{
	master = in_master;

	bop = BOP_EXPOSURE;

	autoSave = master->getAutoSave ();
	singleSave = 0;
}

FocusCameraClient::~FocusCameraClient (void)
{
}

void FocusCameraClient::exposureStarted (bool expectImage)
{
	if (exe == NULL)
	{
		queCommand (new rts2core::CommandExposure (getMaster (), this, bop >= 0 ? bop : 0));
	}
	rts2image::DevClientCameraFoc::exposureStarted (expectImage);
}

void FocusCameraClient::postEvent (rts2core::Event *event)
{
	switch (event->getType ())
	{
		case EVENT_EXP_CHECK:
			if (getConnection ()->getState () & (CAM_EXPOSING | CAM_READING))
			{
				double fr = getConnection ()->getProgress (getNow ());
				std::cout << ((getConnection ()->getState () & CAM_EXPOSING) ? "EXPOSING " : "READING ")  << ProgressIndicator (fr, COLS - 20) << std::fixed << std::setprecision (1) << std::setw (5) << fr << "% \r";
				std::cout.flush ();
			}
			break;
	}
	rts2image::DevClientCameraFoc::postEvent (event);
}

void FocusCameraClient::stateChanged (rts2core::ServerState * state)
{
	if (master->printChanges ())
		std::cout << "State changed (" << getName () << "): "
			<< " value:" << getConnection()->getStateString ()
			<< " (" << state->getValue () << ")"
			<< std::endl;
	rts2image::DevClientCameraFoc::stateChanged (state);
}

rts2image::Image *FocusCameraClient::createImage (const struct timeval *expStart)
{
	rts2image::Image *image;

	if (autoSave || singleSave)
	{
		image = rts2image::DevClientCameraFoc::createImage (expStart);
		image->keepImage ();

		if (singleSave)
		{
			std::cout << "Next image will be saved to disk" << std::endl;
			singleSave = 0;
		}

		return image;
	}
	if (exe)
	{
		std::ostringstream _os;
		_os << "!/tmp/" << connection->getName () << "_" << getpid () << ".fits";
		image = new rts2image::Image (expStart);
		image->openFile (_os.str ().c_str ());
		image->keepImage ();
		return image;
	}
	// memory-only image
	image = new rts2image::Image (expStart);
	return image;
}

void FocusCameraClient::center (int centerWidth, int centerHeight)
{
	connection->queCommand (new rts2core::CommandCenter (this, centerWidth, centerHeight));
}

rts2image::imageProceRes FocusCameraClient::processImage (rts2image::Image * image)
{
	rts2image::imageProceRes res = DevClientCameraFoc::processImage (image);

	if (image->getFileName ())
		// A bit hackish way to determine whether the image has been just saved to disk.
		std::cout << image->getFileName () << std::endl;

	return res;
}

FocusClient::FocusClient (int in_argc, char **in_argv):rts2core::Client (in_argc, in_argv, "focusclient")
{
	defExposure = NAN;
	defCenter = 0;
	defBin = -1;

	xOffset = -1;
	yOffset = -1;
	imageWidth = -1;
	imageHeight = -1;

	optExposureCount = 0;
	optBinCount = 0;
	optXCount = 0;
	optYCount = 0;
	optWidthCount = 0;
	optHeightCount = 0;

	autoSave = 0;

	darks = false;

	focExe = NULL;

	printStateChanges = false;

	query = 0;
	tarRa = -999.0;
	tarDec = -999.0;

	photometerFile = NULL;
	photometerTime = 1;
	photometerFilterChange = 0;
	configFile = NULL;

	bop = BOP_EXPOSURE;

	addOption (OPT_CONFIG, "config", 1, "configuration file");

	addOption ('d', NULL, 1, "camera device name(s) (multiple for multiple cameras)");
	addOption ('e', NULL, 1, "exposure (defaults to 10 sec)");
	addOption (OPT_NOSYNC, "nosync", 0, "do not synchronize camera with telescope (don't block)");
	addOption (OPT_IGNORE_BLOCK, "ignore-block", 0, "ignore block of camera exposure by device (moving telescope,...)");
	addOption (OPT_DARK, "dark", 0, "create dark images");
	addOption ('c', NULL, 0, "takes only center images");
	addOption ('b', NULL, 1, "default binning (ussually 1, depends on camera setting)");
	addOption ('Q', NULL, 0, "query after image end to user input (changing focusing etc..");
	addOption ('R', NULL, 1, "target ra (must come with dec - -D)");
	addOption ('D', NULL, 1, "target dec (must come with ra - -R)");
	addOption ('X', NULL, 1, "x pixel offset");
	addOption ('Y', NULL, 1, "y pixel offset");
	addOption ('W', NULL, 1, "image width");
	addOption ('H', NULL, 1, "image height");
	addOption ('F', NULL, 1, "image processing script (default to NULL - no image processing will be done");
	addOption ('o', NULL, 1, "save results to given file");
	addOption (OPT_PHOTOMETER_TIME, "photometer_time", 1, "photometer integration time (in seconds); default to 1 second");
	addOption (OPT_CHANGE_FILTER, "change_filter", 1, "change filter on photometer after taking n counts; default to 0 (don't change)");
	addOption (OPT_SKIP_FILTER, "skip_filter", 1, "Skip that filter number");
}

FocusClient::~FocusClient (void)
{
	std::cout << std::endl << "Ending program." << std::endl;
}

int FocusClient::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_CONFIG:
			configFile = optarg;
			break;
		case 'd':
			cameraNames.push_back (optarg);
			break;
		case 'e':
			defExposure = atof (optarg);
			optExposureCount++;
			break;
		case OPT_NOSYNC:
			bop = 0;
			break;
		case OPT_IGNORE_BLOCK:
			bop = -1;
			break;
		case OPT_DARK:
			darks = true;
			break;
		case 'b':
			defBin = atoi (optarg);
			optBinCount++;
			break;
		case 'Q':
			query = 1;
			break;
		case 'R':
			tarRa = atof (optarg);
			break;
		case 'D':
			tarDec = atof (optarg);
			break;
		case 'X':
			xOffset = atoi (optarg);
			optXCount++;
			break;
		case 'Y':
			yOffset = atoi (optarg);
			optYCount++;
			break;
		case 'W':
			imageWidth = atoi (optarg);
			optWidthCount++;
			break;
		case 'H':
			imageHeight = atoi (optarg);
			optHeightCount++;
			break;
		case 'c':
			defCenter = 1;
			break;
		case 'F':
			focExe = optarg;
			break;
		case 'o':
			photometerFile = optarg;
			break;
		case OPT_PHOTOMETER_TIME:
			photometerTime = atof (optarg);
			break;
		case OPT_CHANGE_FILTER:
			photometerFilterChange = atoi (optarg);
			break;
		case OPT_SKIP_FILTER:
			skipFilters.push_back (atoi (optarg));
			break;
		default:
			return rts2core::Client::processOption (in_opt);
	}
	return 0;
}

FocusCameraClient * FocusClient::createFocCamera (rts2core::Connection * conn)
{
	return new FocusCameraClient (conn, this);
}

bool FocusClient::isNamedCamera (const char *camName)
{
	std::vector < char *>::iterator cam_iter;
	for (cam_iter = cameraNames.begin (); cam_iter != cameraNames.end (); cam_iter++)
	{
		if (!strcmp (*cam_iter, camName))
			return true;
	}
	return false;
}

// Applies the window/exposure-time/binning/dark-shutter settings and
// triggers the actual exposure on cam. Only ever called for a camera this
// process has decided it owns - either named explicitly via -d, or (see
// the EVENT_CAMERA_CHOICE handler below) the single camera auto-picked
// when -d was omitted. Never called for a camera we're only passively
// watching (see the "base note" on the -d omitted case in createOtherType
// - classic/pre-fix code applied these settings to every connected camera
// regardless of -d, a real bug: running e.g. `-d C0 -b 2` while an
// unrelated C1 also happened to be connected silently changed C1's
// binning too, without ever exposing it).
void FocusClient::armCamera (FocusCameraClient * cam)
{
	if (defCenter)
	{
		cam->center (imageWidth, imageHeight);
	}
	else if (xOffset >= 0 || yOffset >= 0 || imageWidth >= 0 || imageHeight >= 0)
	{
		cam->queCommand (new rts2core::CommandBox (cam, xOffset, yOffset, imageWidth, imageHeight));
	}
	if (!std::isnan (defExposure))
	{
		cam->queCommand (new rts2core::CommandChangeValue (cam->getMaster (), "exposure", '=', defExposure));
	}
	if (darks)
	{
		cam->queCommand (new rts2core::CommandChangeValue (cam->getMaster (), "SHUTTER", '=', 1));
	}
	if (defBin >= 0)
	{
		cam->queCommand (new rts2core::CommandChangeValue (cam->getMaster (), "binning", '=', defBin));
	}
	logStream (MESSAGE_DEBUG) << "exposing on " << cam->getName () << sendLog;
	cam->queCommand (new rts2core::CommandExposure (this, cam, bop >= 0 ? bop : 0));
	armedCameras.push_back (std::string (cam->getName ()));
}

rts2core::DevClient *FocusClient::createOtherType (rts2core::Connection * conn, int other_device_type)
{
	switch (other_device_type)
	{
		case DEVICE_TYPE_CCD:
		{
			FocusCameraClient *cam = createFocCamera (conn);
			cam->setSaveImage (autoSave || focExe);
			// If -d was given, we already know which camera(s) we want and
			// can act the moment each one connects. If it wasn't, we don't
			// yet know how many cameras even exist - EVENT_CAMERA_CHOICE
			// (armed once from init()) makes that decision after giving
			// centrald a moment to report every currently connected device.
			if (!cameraNames.empty () && isNamedCamera (cam->getName ()))
				armCamera (cam);
			return cam;
		}
		case DEVICE_TYPE_MOUNT:
			return new rts2image::DevClientTelescopeImage (conn);
		case DEVICE_TYPE_FOCUS:
			return new rts2image::DevClientFocusFoc (conn);
		case DEVICE_TYPE_PHOT:
			return new rts2image::DevClientPhotFoc (conn, photometerFile, photometerTime, photometerFilterChange, skipFilters);
		case DEVICE_TYPE_DOME:
		case DEVICE_TYPE_MIRROR:
		case DEVICE_TYPE_SENSOR:
		case DEVICE_TYPE_ROTATOR:
			return new rts2image::DevClientWriteImage (conn);
		default:
			return rts2core::Client::createOtherType (conn, other_device_type);
	}
}

static void signal_winch (int sig)
{
	setupterm (NULL, 2, NULL);
}

// -e/-b/-X/-Y/-W/-H are single scalars, applied identically to every
// camera named via -d regardless of where they appear on the command
// line relative to -d - there is no per-camera settings scoping (see
// UPSTREAM_BUGS.md). A `-d C0 -e 1 -d C1 -e 5` command line reads as if
// C0 got exposure=1 and C1 got exposure=5, but both actually get
// exposure=5 (the last -e wins, for every named camera). Given more than
// one -d and any of these options repeated, that's the most likely
// explanation, so warn loudly rather than silently doing something
// different from what the command line visually suggests.
void FocusClient::warnIfSettingsAmbiguous ()
{
	if (cameraNames.size () <= 1)
		return;
	if (optExposureCount <= 1 && optBinCount <= 1 && optXCount <= 1
		&& optYCount <= 1 && optWidthCount <= 1 && optHeightCount <= 1)
		return;
	std::cerr << "warning: -e/-b/-X/-Y/-W/-H are not scoped per -d camera - "
		"each is a single value applied to every named camera, and the "
		"last occurrence of each on the command line wins for all of "
		"them. This command line repeats at least one of those options "
		"while naming more than one camera (-d given " << cameraNames.size ()
		<< " times) - if you intended different settings per camera, run "
		"rts2-focusc once per camera instead." << std::endl;
}

int FocusClient::init ()
{
	rts2core::Configuration *config;
	int ret;

	ret = rts2core::Client::init ();
	if (ret)
		return ret;

	warnIfSettingsAmbiguous ();

	setupterm (NULL, 2, NULL);

	signal (SIGWINCH, signal_winch);

	config = rts2core::Configuration::instance ();
	ret = config->loadFile (configFile);
	if (ret)
	{
		std::cerr << "Cannot load configuration file '"
			<< (configFile ? configFile : "/etc/rts2/rts2.ini")
			<< ")" << std::endl;
		return ret;
	}
	addTimer (CHECK_TIMER, new rts2core::Event (EVENT_EXP_CHECK));
	if (cameraNames.empty ())
		addTimer (CAMERA_CHOICE_DELAY, new rts2core::Event (EVENT_CAMERA_CHOICE));
	return 0;
}

void FocusClient::postEvent (rts2core::Event *event)
{
	switch (event->getType ())
	{
		case EVENT_EXP_CHECK:
			addTimer (CHECK_TIMER, new rts2core::Event (event));
			break;
		case EVENT_CAMERA_CHOICE:
		{
			// No -d was given. Decide, once, what that means: exactly one
			// camera connected -> use it; none yet -> stay passive (autoSave
			// still saves anything another agency happens to expose); more
			// than one -> refusing to guess is safer than guessing wrong on
			// someone else's telescope, so ask and touch nothing.
			std::vector < rts2core::Connection * > ccds;
			for (rts2core::connections_t::iterator iter = getConnections ()->begin (); iter != getConnections ()->end (); iter++)
			{
				if ((*iter)->getOtherType () == DEVICE_TYPE_CCD)
					ccds.push_back (*iter);
			}
			if (ccds.empty ())
			{
				std::cerr << "no -d given and no camera connected - staying passive (pass -d <name> to actively expose one)" << std::endl;
			}
			else if (ccds.size () > 1)
			{
				std::cerr << "no -d given and more than one camera connected (";
				for (size_t i = 0; i < ccds.size (); i++)
					std::cerr << (i ? ", " : "") << ccds[i]->getName ();
				std::cerr << ") - pass -d <name> to pick one; nothing was touched" << std::endl;
				setEndLoop (true);
			}
			else
			{
				std::cerr << "no -d given and exactly one camera (" << ccds[0]->getName () << ") connected - using it" << std::endl;
				armCamera ((FocusCameraClient *) ccds[0]->getOtherDevClient ());
			}
			break;
		}
	}
	rts2core::Client::postEvent (event);
}

int FocusClient::run ()
{
	int ret = rts2core::Client::run ();
	stopOwnedExposures ();
	return ret;
}

// Ctrl-C (or any other graceful shutdown) must not just vanish and leave a
// camera we ourselves commanded still exposing for nobody - see the
// UPSTREAM_BUGS.md entry for why: classic/pre-fix code relied entirely on
// the OS closing the socket, which stops nothing on the device side.
void FocusClient::stopOwnedExposures ()
{
	bool sentAny = false;
	for (rts2core::connections_t::iterator iter = getConnections ()->begin (); iter != getConnections ()->end (); iter++)
	{
		rts2core::Connection *conn = *iter;
		if (conn->getOtherType () != DEVICE_TYPE_CCD)
			continue;
		if (std::find (armedCameras.begin (), armedCameras.end (), std::string (conn->getName ())) == armedCameras.end ())
			continue;
		if (!(conn->getState () & CAM_WORKING))
			continue;
		rts2core::DevClient *dc = conn->getOtherDevClient ();
		if (!dc)
			continue;
		std::cerr << "stopping exposure on " << conn->getName () << " before exit" << std::endl;
		dc->queCommand (new rts2core::CommandStopExposure ((rts2core::DevClientCamera *) dc));
		sentAny = true;
	}
	if (sentAny)
	{
		double deadline = getNow () + STOP_EXPOSURE_GRACE;
		while (getNow () < deadline)
			oneRunLoop ();
	}
}
