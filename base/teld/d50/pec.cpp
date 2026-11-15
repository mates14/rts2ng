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

#include "pec.h"

#include <cmath>
#include <fstream>
#include <sstream>

using namespace rts2teld;

bool PecModel::load (const std::string &path, std::string &err)
{
	east.clear ();
	west.clear ();
	motor.clear ();
	std::ifstream in (path);
	if (!in)
	{
		err = "cannot open " + path;
		return false;
	}
	std::string line;
	int n = 0;
	while (std::getline (in, line))
	{
		n++;
		size_t hash = line.find ('#');
		if (hash != std::string::npos)
			line.erase (hash);
		std::istringstream ls (line);
		std::string side;
		if (!(ls >> side))
			continue;
		Term t;
		std::string rest;
		if (!(ls >> t.h >> t.c >> t.s) || (ls >> rest) || t.h < 1 || t.h > 64
			|| !std::isfinite (t.c) || !std::isfinite (t.s) || (side != "east" && side != "west" && side != "motor"))
		{
			err = path + ":" + std::to_string (n) + ": expected \"east|west|motor <h> <c> <s>\"";
			east.clear ();
			west.clear ();
			motor.clear ();
			return false;
		}
		(side == "east" ? east : side == "west" ? west : motor).push_back (t);
	}
	if (east.empty () || west.empty ())
	{
		err = path + ": needs terms for both east and west";
		east.clear ();
		west.clear ();
		motor.clear ();
		return false;
	}
	return true;
}

double PecModel::value (const std::vector<Term> &t, double phi)
{
	double v = 0;
	for (const Term &x : t)
		v += x.c * cos (x.h * phi) + x.s * sin (x.h * phi);
	return v;
}

double PecModel::slope (const std::vector<Term> &t, double phi)
{
	double v = 0;
	for (const Term &x : t)
		v += x.h * (x.s * cos (x.h * phi) - x.c * sin (x.h * phi));
	return v;
}

double PecModel::error (double phi, double w, double theta) const
{
	double v = (1 - w) * value (east, phi) + w * value (west, phi);
	if (std::isfinite (theta))
		v += value (motor, theta);
	return v;
}

double PecModel::factor (double phi, double w, double theta) const
{
	double dp = (1 - w) * slope (east, phi) + w * slope (west, phi);
	// d/dphi of a motor term: theta turns PEC_MOTOR_PER_TURN times per phi
	if (std::isfinite (theta))
		dp += PEC_MOTOR_PER_TURN * slope (motor, theta);
	double f = 1 - 2 * M_PI / PEC_HA_PER_TURN * dp;
	if (!std::isfinite (f) || fabs (f - 1) > PEC_MAX_CHANGE)
		return NAN;
	return f;
}

double PecModel::motorAngle (double counts)
{
	double r = fmod (counts, (double) PEC_MOTOR_COUNTS);
	if (r < 0)
		r += PEC_MOTOR_COUNTS;
	return 2 * M_PI * r / PEC_MOTOR_COUNTS;
}

double PecModel::westWeight (double ha, double haEast, double haWest)
{
	if (ha <= haEast)
		return 0;
	if (ha >= haWest || haWest <= haEast)
		return 1;
	return (ha - haEast) / (haWest - haEast);
}
