/*
 * State - display day states for rts2.
 * Copyright (C) 2003 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2011,2012 Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
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

// base note: ported from the classic tree's src/centrald/stateapp.cpp.
// It is the one RTS2 tool that answers "is it night?" without a running
// centrald - everything it prints comes from riseset.cpp plus the
// observatory position in rts2.ini - so shell/cron scripts use it where
// rts2-jsonclient (which needs rts2-httpd and a live centrald) cannot be
// used. Ported as-is apart from the usual modernization; the one behavior
// fix is restoring the stream flags with flags() instead of setf() in
// printAltTable, which only ever set bits back and never cleared them.

#include <ctime>
#include <iostream>
#include <iomanip>
#include <sstream>

#include "expander.h"
#include "riseset.h"
#include "libnova_cpp.h"
#include "cliapp.h"
#include "configuration.h"
#include "centralstate.h"

#define OPT_LAT              OPT_LOCAL + 230
#define OPT_LONG             OPT_LOCAL + 231
#define OPT_SUN_ALTITUDE     OPT_LOCAL + 232
#define OPT_SUN_AZIMUTH      OPT_LOCAL + 233
#define OPT_SUN_BELOW        OPT_LOCAL + 234
#define OPT_SUN_ABOVE        OPT_LOCAL + 235

namespace rts2centrald
{

/*!
 * Prints current state on STDOUT.
 * States are (as described in kernel/include/status.h):
 *
 * 0 - day
 * 1 - evening
 * 2 - dusk
 * 3 - night
 * 4 - dawn
 * 5 - morning
 *
 * Configuration file (rts2.ini) is used.
 */
class StateApp:public rts2core::CliApp
{
	public:
		StateApp (int in_argc, char **in_argv);

		virtual int init ();
	protected:
		virtual int doProcessing ();
		virtual void help ();
		virtual int processOption (int in_opt);
	private:
		int verbose;			 // verbosity level
		double lng;
		double lat;
		double night_horizon;
		double day_horizon;

		int eve_time;
		int mor_time;

		const char *conff;

		time_t currTime;
		double JD;
		void printAltTable (std::ostream & _os, double jd_start, double h_start, double h_end, double h_step = 1.0);
		void printAltTable (std::ostream & _os);
		void printDayStates (std::ostream & _os);

		enum {NONE, SUN_ALT, SUN_AZ, SUN_ABOVE, SUN_BELOW } calculateSun;
		bool stateOnly;
		double sunLimit;

		const char *expandString;
};

}

using namespace rts2centrald;

void StateApp::printAltTable (std::ostream & _os, double jd_start, double h_start, double h_end, double h_step)
{
	double i;
	struct ln_hrz_posn hrz;
	struct ln_equ_posn pos;
	double jd;
	int old_precison = _os.precision (0);
	std::ios_base::fmtflags old_settings = _os.flags ();
	_os.setf (std::ios_base::fixed, std::ios_base::floatfield);

	char old_fill = _os.fill ('0');

	jd_start += h_start / 24.0;

	_os << "H   ";

	for (i = h_start; i <= h_end; i += h_step)
	{
		_os << "  " << std::setw (2) << i;
	}
	_os.fill (old_fill);
	_os << std::endl;

	// print sun position
	std::ostringstream _os3;

	_os << "SAL ";
	_os3 << "SAZ ";

	_os3.precision (0);
	_os3.setf (std::ios_base::fixed, std::ios_base::floatfield);

	jd = jd_start;
	for (i = h_start; i <= h_end; i += h_step, jd += h_step / 24.0)
	{
		ln_get_solar_equ_coords (jd, &pos);
		ln_get_hrz_from_equ (&pos, rts2core::Configuration::instance ()->getObserver (), jd, &hrz);
		_os << " " << std::setw (3) << hrz.alt;
		_os3 << " " << std::setw (3) << hrz.az;
	}

	_os << std::endl << _os3.str () << std::endl << std::endl;

	_os.flags (old_settings);
	_os.precision (old_precison);
}

void StateApp::printAltTable (std::ostream & _os)
{
	double jd_start = ((int) JD) - 0.5;

	_os << std::endl << "** SUN POSITION table from "
		<< TimeJD (jd_start) << " **" << std::endl << std::endl;

	printAltTable (_os, jd_start, -1, 11);
	_os << std::endl;
	printAltTable (_os, jd_start, 12, 24);
}

void StateApp::printDayStates (std::ostream & _os)
{
	time_t ev_time, curr_time;
	rts2_status_t curr_type, next_type;

	curr_time = currTime;

	while (curr_time < (currTime + 87000.0))
	{
		if (next_event (rts2core::Configuration::instance ()->getObserver (), &curr_time, &curr_type, &next_type, &ev_time, night_horizon, day_horizon, eve_time, mor_time, verbose > 0))
		{
			std::cerr << "Error getting next type" << std::endl;
			return;
		}
		if (curr_time == currTime)
		{
			_os << Timestamp (currTime) << " " << rts2core::CentralState (curr_type) << " (current)" << std::endl;
		}
		_os << Timestamp (ev_time) << " " << rts2core::CentralState (next_type) << std::endl;

		curr_time = ev_time + 1;
	}
}

void StateApp::help ()
{
	std::cout << "Observing state display tool." << std::endl;
	rts2core::CliApp::help ();
}

int StateApp::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_LAT:
			lat = atof (optarg);
			break;
		case OPT_LONG:
			lng = atof (optarg);
			break;
		case 'N':
			std::cout << pureNumbers;
			break;
		case OPT_SUN_ALTITUDE:
			calculateSun = SUN_ALT;
			break;
		case OPT_SUN_AZIMUTH:
			calculateSun = SUN_AZ;
			break;
		case OPT_SUN_BELOW:
			calculateSun = SUN_BELOW;
			sunLimit = atof (optarg);
			break;
		case OPT_SUN_ABOVE:
			calculateSun = SUN_ABOVE;
			sunLimit = atof (optarg);
			break;
		case 'e':
			expandString = optarg;
			break;
		case 'c':
			stateOnly = true;
			break;
		case 't':
			currTime = atoi (optarg);
			if (!currTime)
			{
				std::cerr << "Invalid time: " << optarg << std::endl;
				return -1;
			}
			break;
		case 'd':
			if (parseDate (optarg, &currTime))
			{
				std::cerr << "Invalid date: " << optarg << std::endl;
				return -1;
			}
			break;
		case 'v':
			verbose++;
			break;
		case OPT_CONFIG:
			conff = optarg;
			break;
		default:
			return rts2core::CliApp::processOption (in_opt);
	}
	return 0;
}

StateApp::StateApp (int argc, char **argv):rts2core::CliApp (argc, argv)
{
	lng = lat = -1000;
	verbose = 0;

	night_horizon = -10;
	day_horizon = 0;

	eve_time = 7200;
	mor_time = 1800;

	conff = nullptr;

	JD = NAN;

	calculateSun = NONE;
	stateOnly = false;
	sunLimit = 0;

	expandString = nullptr;

	time (&currTime);

	addOption (OPT_CONFIG, "config", 1, "configuration file");
	addOption (OPT_LAT, "latitude", 1, "set latitude (overwrites config file)");
	addOption (OPT_LONG, "longtitude", 1, "set longtitude (overwrites config file). Negative for west from Greenwich)");
	addOption ('e', nullptr, 1, "expand string given as argument");
	addOption ('c', nullptr, 0,  "print current state (one number) and exits");
	addOption ('d', nullptr, 1, "print for given date (in YYYY-MM-DD[Thh:mm:ss.sss] format)");
	addOption ('t', nullptr, 1, "print for given time (in unix time)");
	addOption ('N', nullptr, 0, "do not pretty print");
	addOption (OPT_SUN_ALTITUDE, "sun-altitude", 0, "return current sun altitude (in degrees)");
	addOption (OPT_SUN_AZIMUTH, "sun-azimuth", 0, "return current sun azimuth (in degrees)");
	addOption (OPT_SUN_BELOW, "sun-below", 1, "prints time when sun altitude goes below specified altitude (in degrees)");
	addOption (OPT_SUN_ABOVE, "sun-above", 1, "prints time when sun altitude goes above specified altitude (in degrees)");
	addOption ('v', nullptr, 0, "be verbose");
}

int StateApp::init ()
{
	int ret = rts2core::CliApp::init ();
	if (ret)
		return ret;

	JD = ln_get_julian_from_timet (&currTime);

	rts2core::Configuration *config = rts2core::Configuration::instance ();

	if (config->loadFile (conff) == -1)
	{
		std::cerr << "Cannot read configuration file, exiting." << std::endl;
		return -1;
	}

	if (lng != -1000)
		config->getObserver ()->lng = lng;
	if (lat != -1000)
		config->getObserver ()->lat = lat;

	night_horizon = config->getDoubleDefault ("observatory", "night_horizon", -10);
	day_horizon = config->getDoubleDefault ("observatory", "day_horizon", 0);

	eve_time = config->getIntegerDefault ("observatory", "evening_time", 7200);
	mor_time = config->getIntegerDefault ("observatory", "morning_time", 1800);

	return 0;
}

int StateApp::doProcessing ()
{
	time_t ev_time;
	rts2_status_t curr_type, next_type;

	struct ln_lnlat_posn *obs = rts2core::Configuration::instance ()->getObserver ();

	if (verbose > 0)
		std::cout << "Position: " << LibnovaPos (obs) << " Time: " << Timestamp (currTime) << std::endl;

	if (expandString)
	{
		rts2core::Expander ex;
		std::cout << ex.expand (std::string (expandString), false) << std::endl;
		return 0;
	}

	if (calculateSun != NONE)
	{
		struct ln_equ_posn pos;
		struct ln_hrz_posn hrz;
		ln_get_solar_equ_coords (JD, &pos);
		ln_get_hrz_from_equ (&pos, obs, JD, &hrz);

		if (calculateSun == SUN_ALT || calculateSun == SUN_AZ)
		{
			if (calculateSun == SUN_ALT)
				std::cout << hrz.alt << std::endl;
			else
				std::cout << hrz.az << std::endl;
		}
		else
		{
			if (next_event (obs, &currTime, &curr_type, &next_type, &ev_time, sunLimit, sunLimit, 0, 0, verbose > 0))
			{
				std::cerr << "cannot find rise/set times for horizon " << sunLimit << " degrees." << std::endl;
				return -1;
			}
			if (calculateSun == SUN_BELOW)
			{
				if (hrz.alt <= sunLimit + LN_SOLAR_STANDART_HORIZON)
				{
					std::cout << Timestamp (currTime) << std::endl;
					return 1;
				}
				std::cout << Timestamp (ev_time) << std::endl;
			}
			else
			{
				if (hrz.alt >= sunLimit - LN_SOLAR_STANDART_HORIZON)
				{
					std::cout << Timestamp (currTime) << std::endl;
					return 1;
				}
				std::cout << Timestamp (ev_time) << std::endl;
			}
		}
		return 0;
	}

	if (next_event (obs, &currTime, &curr_type, &next_type, &ev_time, night_horizon, day_horizon, eve_time, mor_time, verbose > 0))
	{
		std::cerr << "Error getting next type" << std::endl;
		return -1;
	}

	if (stateOnly)
	{
		std::cout << curr_type << std::endl;
	}
	else
	{
		printAltTable (std::cout);
		printDayStates (std::cout);
		std::cout << std::endl;
	}
	return 0;
}

int main (int argc, char **argv)
{
	StateApp app (argc, argv);
	return app.run ();
}
