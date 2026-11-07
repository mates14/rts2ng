/*
 * D50 path finding - the "Successive Safe Targets" strategy.
 * Copyright (C) 2010-2022 Jan Strobl
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
 */

#include "d50path.h"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <vector>

using namespace rts2teld;

D50Path::D50Path ()
{
	for (int i = 0; i < 2; i++)
		vMax[i] = accel[i] = decel[i] = NAN;
	haCpd = decCpd = haZero = decZero = NAN;
	acMin = acMax = dcMin = dcMax = 0;
	observer.lng = observer.lat = 0;
	hardHorizon = nullptr;
	hardCeiling = nullptr;
	checkCeiling = false;
	searchedNodes = 0;
	JD = gmst = NAN;
	sun.ra = sun.dec = 0;
	checkSunDistance = true;
}

void D50Path::slopeBins (ObjectCheck *oc, std::vector <double> &bins)
{
	// bins[i]: the steepest slope [deg alt / deg az] of the horizon line in azimuths i .. i + 1
	bins.assign (360, 0);
	if (oc == nullptr || oc->begin () == oc->end ())
		return;
	for (auto it = oc->begin (); it != oc->end (); it++)
	{
		auto next = std::next (it);
		if (next == oc->end ())
			next = oc->begin ();
		double a1 = (*it).hrz.az, len = ln_range_degrees ((*next).hrz.az - a1);
		if (len <= 0)
			continue;
		double slope = fabs ((*next).hrz.alt - (*it).hrz.alt) / len;
		for (int i = (int) floor (a1); i <= (int) floor (a1 + len); i++)
			bins[((i % 360) + 360) % 360] = std::max (bins[((i % 360) + 360) % 360], slope);
	}
}

double D50Path::steepest (const std::vector <double> &bins, double az, double daz)
{
	if (bins.empty ())
		return 0;
	if (daz >= 180)
		return *std::max_element (bins.begin (), bins.end ());
	double slope = 0;
	for (int i = (int) floor (az - daz); i <= (int) floor (az + daz); i++)
		slope = std::max (slope, bins[((i % 360) + 360) % 360]);
	return slope;
}

void D50Path::setTime (double _JD)
{
	JD = _JD;
	gmst = ln_get_mean_sidereal_time (JD) * 15.0;
	ln_get_solar_equ_coords (JD, &sun);
	struct ln_hrz_posn sunHrz;
	ln_get_hrz_from_equ (&sun, &observer, JD, &sunHrz);
	checkSunDistance = sunHrz.alt > SUN_AVOID_ALTITUDE;
	slopeBins (hardHorizon, horizonBins);
	slopeBins (hardCeiling, ceilingBins);
}

double D50Path::safeStep (int32_t AC, int32_t DC, double sunDist)
{
	// the fastest the mount moves on the sky [deg/s]; the Sun's distance changes no faster
	const double v = hypot (vMax[0] / fabs (haCpd), vMax[1] / fabs (decCpd));
	double dt = CHECKPASSAGE_TIME_RESOLUTION;
	if (std::isfinite (sunDist))
		dt = std::min (dt, (sunDist - SUN_AVOID_DISTANCE) / v);
	struct ln_equ_posn equPosition;
	struct ln_hrz_posn hrzPosition;
	equPosition.ra = - ( (double) (AC / haCpd) + haZero);
	equPosition.dec = (double) (DC / decCpd) + decZero;
	ln_get_hrz_from_equ_sidereal_time (&equPosition, &observer, -observer.lng/15.0, &hrzPosition);
	// within a coarse step the azimuth changes by at most step / cos (alt) (bounded near the zenith, where no horizon
	// line is), and the margin to a horizon line by v (1 + its steepest slope there / cos (alt)) per second
	const double step = v * CHECKPASSAGE_TIME_RESOLUTION;
	const double c = std::max (cos ((fabs (hrzPosition.alt) + step) * M_PI / 180.0), 0.2);
	const double daz = step / c;
	dt = std::min (dt, (hrzPosition.alt - hardHorizon->getHorizonHeight (&hrzPosition, 0)) / (v * (1 + steepest (horizonBins, hrzPosition.az, daz) / c)));
	if (checkCeiling && hardCeiling != nullptr)
		dt = std::min (dt, (hardCeiling->getHorizonHeight (&hrzPosition, 0) - hrzPosition.alt) / (v * (1 + steepest (ceilingBins, hrzPosition.az, daz) / c)));
	return std::max (dt, CHECKPASSAGE_FINE_RESOLUTION);
}

int D50Path::findPath (int32_t acStart, int32_t dcStart, int32_t acFin, int32_t dcFin)
{
	double acOnlyMoveTime, dcOnlyMoveTime, movementDelay;

	searchedNodes = 0;

	// The essential approach used for dealing the problematic movements is to change the final target positon of servos gradually,
	// while all the temporary targets (as well as the way to them) are safe and don't collide with the hard-horizon (as we cannot
	// guarantee that the following change of coordinates will really happen). We call this approach "Successive Safe Targets" strategy.
	//
	// The simple strategies come first, as they always did: a straight movement, then "HA-first, Dec-delayed" and
	// "Dec-first, HA-delayed", each with one successive point. Only when all of them fail (the Sun between us and the target,
	// typically a park around noon) the grid of intermediate targets is searched.

	if (checkPassage(acStart, dcStart, acFin, dcFin) > 0)	// simple (straight) movement is possible
	{
		successivePointsAC[0] = acFin;
		successivePointsDC[0] = dcFin;
		return 0;
	}

	acOnlyMoveTime = checkPassage(acStart, dcStart, acFin, dcStart);

	if (acOnlyMoveTime > 0)	// HA-only movement is possible, we will follow a "HA-first, Dec-delayed" strategy
	{
		movementDelay = computeSuccessivePointInterval (acStart, dcStart, acFin, dcStart, acFin, dcFin);
		if (movementDelay > 0)
		{
			successivePointsAC[0] = acFin;
			successivePointsDC[0] = dcStart;
			successivePointsIntervals[0] = movementDelay;
			successivePointsAC[1] = acFin;
			successivePointsDC[1] = dcFin;
			return 1;
		}
	}

	dcOnlyMoveTime = checkPassage(acStart, dcStart, acStart, dcFin);
	if (dcOnlyMoveTime > 0)	// Dec-only movement is possible, we will follow "Dec-first, HA-delayed" strategy
	{
		movementDelay = computeSuccessivePointInterval (acStart, dcStart, acStart, dcFin, acFin, dcFin);
		if (movementDelay > 0)
		{
			successivePointsAC[0] = acStart;
			successivePointsDC[0] = dcFin;
			successivePointsIntervals[0] = movementDelay;
			successivePointsAC[1] = acFin;
			successivePointsDC[1] = dcFin;
			return 1;
		}
	}

	return findPathOnGrid (acStart, dcStart, acFin, dcFin);
}

int D50Path::findPathOnGrid (int32_t acStart, int32_t dcStart, int32_t acFin, int32_t dcFin)
{
	// Intermediate safe targets on a grid of FINDPATH_MIN_STEP_SIZE degrees over the whole reachable HA/Dec range. A step
	// between neighbouring targets (and from a target straight to the final one) is allowed when checkPassage () accepts it
	// starting at rest - every target is a safe place to stop. A* over the movement times finds the fastest chain, then the
	// targets a direct safe passage can skip are dropped, and planIntervals () decides when each next target is set.
	// The HA range keeps one step from its limits, as targets do (Fork::sky2counts, acMargin of 5 degrees).
	const double stepA = fabs (haCpd) * FINDPATH_MIN_STEP_SIZE;
	const double stepD = fabs (decCpd) * FINDPATH_MIN_STEP_SIZE;
	const int32_t acLo = std::min (acMin, acMax) + (int32_t) stepA, acHi = std::max (acMin, acMax) - (int32_t) stepA;
	const int32_t dcLo = std::min (dcMin, dcMax), dcHi = std::max (dcMin, dcMax);
	const int na = (int) floor ((double) (acHi - acLo) / stepA) + 1;
	const int nd = (int) floor ((double) (dcHi - dcLo) / stepD) + 1;
	const int nGrid = na * nd;
	const int START = nGrid, TARGET = nGrid + 1;

	auto posAC = [&] (int id) -> int32_t { return id == START ? acStart : (id == TARGET ? acFin : acLo + (int32_t) ((id / nd) * stepA)); };
	auto posDC = [&] (int id) -> int32_t { return id == START ? dcStart : (id == TARGET ? dcFin : dcLo + (int32_t) ((id % nd) * stepD)); };

	std::vector <signed char> safe (nGrid, -1);
	auto isSafe = [&] (int id) -> bool
	{
		if (safe[id] < 0)
			safe[id] = checkRawPosition (posAC (id), posDC (id)) ? 1 : 0;
		return safe[id] == 1;
	};

	std::vector <double> g (nGrid + 2, INFINITY);
	std::vector <int> parent (nGrid + 2, -1);
	std::vector <bool> closed (nGrid + 2, false);
	typedef std::pair <double, int> qitem;
	std::priority_queue <qitem, std::vector <qitem>, std::greater <qitem> > open;

	auto relax = [&] (int u, int v)
	{
		double t = checkPassage (posAC (u), posDC (u), posAC (v), posDC (v));
		if (t <= 0 || g[u] + t >= g[v])
			return;
		g[v] = g[u] + t;
		parent[v] = u;
		open.push (qitem (g[v] + computeFullMovementTime (posAC (v), posDC (v), acFin, dcFin), v));
	};

	g[START] = 0;
	open.push (qitem (computeFullMovementTime (acStart, dcStart, acFin, dcFin), START));
	while (!open.empty ())
	{
		int u = open.top ().second;
		open.pop ();
		if (closed[u])
			continue;
		closed[u] = true;
		if (u == TARGET)
			break;
		searchedNodes++;

		relax (u, TARGET);

		int ia, id, r;
		if (u == START)
		{
			// the grid targets around the start - farther ones too when it has to leave the Sun's proximity first,
			// as the nearest ones may still be in it
			ia = (int) floor ((acStart - acLo) / stepA);
			id = (int) floor ((dcStart - dcLo) / stepD);
			r = sunDistance (acStart, dcStart) < SUN_AVOID_DISTANCE ? 4 : 2;
		}
		else
		{
			ia = u / nd;
			id = u % nd;
			r = 1;
		}
		for (int i = ia - r + (u == START ? 1 : 0); i <= ia + r; i++)
		{
			for (int j = id - r + (u == START ? 1 : 0); j <= id + r; j++)
			{
				if (i < 0 || i >= na || j < 0 || j >= nd)
					continue;
				int v = i * nd + j;
				if (v == u || closed[v] || !isSafe (v))
					continue;
				relax (u, v);
			}
		}
	}

	if (parent[TARGET] < 0)
		return -1;

	std::vector <int> chain;
	for (int v = TARGET; v != -1; v = parent[v])
		chain.push_back (v);
	std::reverse (chain.begin (), chain.end ());

	// drop the targets a direct safe passage from an earlier one skips
	std::vector <int> points;
	size_t i = 0;
	while (i + 1 < chain.size ())
	{
		size_t j = chain.size () - 1;
		while (j > i + 1 && checkPassage (posAC (chain[i]), posDC (chain[i]), posAC (chain[j]), posDC (chain[j])) <= 0)
			j--;
		points.push_back (chain[j]);
		i = j;
	}

	int n = (int) points.size () - 1;
	if (n > MAX_NUMBER_OF_SUCCESSIVE_POINTS)
		return -1;
	for (int k = 0; k <= n; k++)
	{
		successivePointsAC[k] = posAC (points[k]);
		successivePointsDC[k] = posDC (points[k]);
	}
	return planIntervals (acStart, dcStart, n);
}

int D50Path::planIntervals (int32_t acStart, int32_t dcStart, int n)
{
	// Each next target is set as soon as the rest of the way to it is safe from where the mount is by then (and how fast it
	// goes) - at the latest when it has stopped at the current target, from where the passage was checked at rest.
	int32_t ac = acStart, dc = dcStart;
	double vac = 0, vdc = 0;
	for (int k = 0; k < n; k++)
	{
		double interval = computeSuccessivePointInterval (ac, dc, successivePointsAC[k], successivePointsDC[k], successivePointsAC[k + 1], successivePointsDC[k + 1], vac, vdc, &ac, &dc, &vac, &vdc);
		if (interval < 0)
			return -1;
		successivePointsIntervals[k] = interval;
	}
	return n;
}

double D50Path::computeSuccessivePointInterval (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, int32_t nextAC, int32_t nextDC, double initSpeedAC, double initSpeedDC, int32_t *switchAC, int32_t *switchDC, double *switchSpeedAC, double *switchSpeedDC)
{
	double initialMoveTime, movementDelay, acSpeedTemp, dcSpeedTemp;
	int32_t acTemp, dcTemp;

	initialMoveTime = computeFullMovementTime (fromAC, fromDC, toAC, toDC, initSpeedAC, initSpeedDC);

	// computeMovement () takes the initial speed in the direction of the new movement, so a moving axis may only be given a
	// target further on in its direction, and far enough to stop in. base note: classic did not check this; its two
	// strategies never asked otherwise. Classic also never tried the moment of arrival itself (1 s steps up to it).
	auto canContinue = [&] (int isDec, int32_t from, int32_t to, int32_t pos, int32_t next, double speed)
	{
		double stopDistance = speed * speed / (2 * decel[isDec]);
		if (stopDistance <= 2)		// at rest (to the rounding of the movement times)
			return true;
		double dist = (double) (next - pos) * (to > from ? 1 : -1);
		return dist + 2 >= stopDistance;
	};

	for (int step = 1; ; step++)
	{
		bool arrived = false;
		movementDelay = step * FINDPATH_TIME_RESOLUTION;
		if (movementDelay >= initialMoveTime)
		{
			movementDelay = initialMoveTime;
			arrived = true;
		}
		acTemp = computeMovement (0, fromAC, toAC, initSpeedAC, movementDelay, &acSpeedTemp);
		dcTemp = computeMovement (1, fromDC, toDC, initSpeedDC, movementDelay, &dcSpeedTemp);
		if (canContinue (0, fromAC, toAC, acTemp, nextAC, acSpeedTemp) && canContinue (1, fromDC, toDC, dcTemp, nextDC, dcSpeedTemp)
			&& checkPassage(acTemp, dcTemp, nextAC, nextDC, acSpeedTemp, dcSpeedTemp) > 0)	// we've found the earliest delay of setting the next target!
		{
			if (switchAC)
				*switchAC = acTemp;
			if (switchDC)
				*switchDC = dcTemp;
			if (switchSpeedAC)
				*switchSpeedAC = acSpeedTemp;
			if (switchSpeedDC)
				*switchSpeedDC = dcSpeedTemp;
			return movementDelay;
		}
		if (arrived)
			break;
	}

	return -1;
}

double D50Path::checkPassage (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, double initSpeedAC, double initSpeedDC)
{
	double fullMovementTime, inspectionTime, sunDist;
	int32_t posAC, posDC;

	// Leaving the Sun's proximity: a mount already closer than SUN_AVOID_DISTANCE (the Sun came to it, or it was moved
	// there by hand) may move only away from the Sun - every sample farther than the one before, from the first one on -
	// until it is out; from then on the usual rules. Horizon and ceiling hold all the time. [JS 2026-11-07]
	double previous = sunDistance (fromAC, fromDC);
	bool escaping = previous < SUN_AVOID_DISTANCE;

	fullMovementTime = computeFullMovementTime (fromAC, fromDC, toAC, toDC, initSpeedAC, initSpeedDC);

	// The next sample comes as late as no limit can be reached before it: up to 1 s when none is near, down to 5 ms at
	// a limit (there at most 0.035 deg on the sky), and 5 ms while leaving the Sun's proximity, where the way could
	// curve towards the Sun between two samples. Classic sampled every 0.2 s, up to 1.4 deg at full speed, with no
	// bound in between. [JS 2026-11-07]
	double dt = escaping ? CHECKPASSAGE_FINE_RESOLUTION : safeStep (fromAC, fromDC, previous);
	for (inspectionTime = dt; ; inspectionTime += dt)
	{
		bool last = false;
		if (inspectionTime >= fullMovementTime)
		{
			// base note: classic stopped at the last 0.2 s sample, which can fall short of the target itself
			inspectionTime = fullMovementTime;
			last = true;
		}
		posAC = computeMovement (0, fromAC, toAC, initSpeedAC, inspectionTime);
		posDC = computeMovement (1, fromDC, toDC, initSpeedDC, inspectionTime);
		if (!checkRawPosition (posAC, posDC, &sunDist))
			return -1.0 * inspectionTime;
		if (escaping)
		{
			// positions are whole counts (2.4e-6 deg in HA): at 5 ms steps from rest a few counts' rounding is no approach
			if (sunDist < previous - SUN_ESCAPE_ROUNDING)
				return -1.0 * inspectionTime;
			previous = sunDist;
			escaping = sunDist < SUN_AVOID_DISTANCE;
		}
		else if (sunDist < SUN_AVOID_DISTANCE)
		{
			return -1.0 * inspectionTime;
		}
		if (last)
			break;
		dt = escaping ? CHECKPASSAGE_FINE_RESOLUTION : safeStep (posAC, posDC, sunDist);
	}
	// the target itself must be out of the Sun's proximity
	if (escaping)
		return -1.0 * fullMovementTime;

	return fullMovementTime;
}

double D50Path::computeFullMovementTime (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, double initSpeedAC, double initSpeedDC)
{
	double movementTimeAC, movementTimeDC;

	movementTimeAC = (double) computeMovement (0, fromAC, toAC, initSpeedAC, -1) / 1000.0;
	movementTimeDC = (double) computeMovement (1, fromDC, toDC, initSpeedDC, -1) / 1000.0;
	if (movementTimeAC > movementTimeDC)
		return movementTimeAC;
	return movementTimeDC;
}

int32_t D50Path::computeMovement (int isDec, int32_t fromCount, int32_t toCount, double v0, double inspectionTime, double* speedAtInspectionTime)
{
	if (fromCount == toCount)
	{
		// base note: classic left *speedAtInspectionTime unset here, so the "HA-first" strategy handed checkPassage ()
		// an uninitialised Dec speed (UPSTREAM_BUGS.md)
		if (speedAtInspectionTime != nullptr)
			*speedAtInspectionTime = 0.0;
		if (inspectionTime < 0)         // i.e., only the time of whole movement is expected from this routine
			return 0;
		return toCount;
	}

	double v_max = vMax[isDec], a1 = accel[isDec], a2 = decel[isDec];

	double T_ramp1, T_ramp2;
	int32_t S_ramp1, S_ramp2, S_const, movementDirection;

	T_ramp1 = (v_max - v0) / a1;
	T_ramp2 = v_max / a2;
	S_ramp1 = T_ramp1 * (v_max + v0) / 2;
	S_ramp2 = T_ramp2 * v_max / 2;
	S_const = abs (toCount - fromCount) - S_ramp1 - S_ramp2;

	if (toCount > fromCount)
		movementDirection = (int32_t) 1;
	else
		movementDirection = (int32_t) -1;

	if (S_const >= 0)	// i.e., we reach the full speed during the transition
	{
		double T_const = (double) S_const/v_max;
		if (inspectionTime < 0)		// i.e., only the time of whole movement is expected from this routine
		{
			return (int32_t) ((T_ramp1 + T_ramp2 + T_const) * 1000.0 + 0.5);
		}

		if (inspectionTime <= T_ramp1)
		{
			if (speedAtInspectionTime != nullptr)
				*speedAtInspectionTime = v0 + a1 * inspectionTime;
			return fromCount + movementDirection * (int32_t) ((v0 + a1 * inspectionTime / 2) * inspectionTime + 0.5);
		}
		if (inspectionTime <= T_ramp1 + T_const)
		{
			if (speedAtInspectionTime != nullptr)
				*speedAtInspectionTime = v_max;
			return fromCount + movementDirection * (int32_t) (S_ramp1 + v_max * (inspectionTime - T_ramp1) + 0.5);
		}
		if (inspectionTime <= T_ramp1 + T_const + T_ramp2)
		{
			double T_reversed = T_ramp1 + T_ramp2 + T_const - inspectionTime;
			if (speedAtInspectionTime != nullptr)
				*speedAtInspectionTime = a2 * T_reversed;
			return fromCount + movementDirection * (int32_t) (S_ramp1 + S_const + S_ramp2 - a2 * T_reversed * T_reversed / 2.0 + 0.5);
		}
		if (speedAtInspectionTime != nullptr)
			*speedAtInspectionTime = 0.0;
		return toCount;
	}
	// case when S_const<0, i.e., full speed is not reached during the transition
	double T_full = ((1.0 + a1/a2) * sqrt ((v0*v0 + 2*a1*abs(toCount-fromCount)) / (1.0 + a1/a2)) - v0) / a1;
	double T1 = (T_full * a2 - v0)/(a1 + a2);
	if (inspectionTime < 0)		// i.e., only the time of whole movement is expected from this routine
		return (int32_t) (T_full * 1000.0 + 0.5);
	if (inspectionTime <=  T1)
	{
		if (speedAtInspectionTime != nullptr)
			*speedAtInspectionTime = v0 + a1 * inspectionTime;
		return fromCount + movementDirection * (int32_t) ((v0 + a1 * inspectionTime / 2) * inspectionTime + 0.5);
	}
	if (inspectionTime <=  T_full)
	{
		double T_reversed = T_full - inspectionTime;
		if (speedAtInspectionTime != nullptr)
			*speedAtInspectionTime = a2 * T_reversed;
		return fromCount + movementDirection * (int32_t) (abs(toCount-fromCount) - a2 * T_reversed * T_reversed / 2.0 + 0.5);
	}
	if (speedAtInspectionTime != nullptr)
		*speedAtInspectionTime = 0.0;
	return toCount;
}

double D50Path::sunDistance (int32_t AC, int32_t DC)
{
	if (checkCeiling || !checkSunDistance)
		return INFINITY;
	struct ln_equ_posn equPosition;
	equPosition.ra = - ( (double) (AC / haCpd) + haZero) + gmst + observer.lng;
	equPosition.dec = (double) (DC / decCpd) + decZero;
	return ln_get_angular_separation (&sun, &equPosition);
}

int D50Path::checkRawPosition (int32_t AC, int32_t DC, double *sunDist)
{
	struct ln_equ_posn equPosition;
	struct ln_hrz_posn hrzPosition;

	equPosition.ra = - ( (double) (AC / haCpd) + haZero);
	equPosition.dec = (double) (DC / decCpd) + decZero;

	ln_get_hrz_from_equ_sidereal_time (&equPosition, &observer, -observer.lng/15.0, &hrzPosition);

	if (!hardHorizon->is_good (&hrzPosition))
		return 0;

	if (checkCeiling)
	{
		if (hardCeiling != nullptr && hardCeiling->is_good (&hrzPosition))		// !!! we use the "horizon" object, but need a ceiling test, so the logic is reversed
			return 0;
	}
	double d = sunDistance (AC, DC);
	if (sunDist != nullptr)
	{
		*sunDist = d;
		return 1;
	}
	return d >= SUN_AVOID_DISTANCE;
}

double D50Path::sunApproach (int32_t AC, int32_t DC, bool tracking, double lead, double &distanceNow)
{
	struct ln_equ_posn tel, sunT;
	struct ln_hrz_posn sunHrz;
	double ha = (double) (AC / haCpd) + haZero;
	double ra0 = gmst + observer.lng - ha;
	tel.dec = (double) (DC / decCpd) + decZero;
	distanceNow = NAN;
	for (double t = 0; t <= lead; t += 30.0)
	{
		double JDt = JD + t / 86400.0;
		ln_get_solar_equ_coords (JDt, &sunT);
		// a standing mount keeps its hour angle, a tracking one its right ascension
		tel.ra = tracking ? ra0 : ln_get_mean_sidereal_time (JDt) * 15.0 + observer.lng - ha;
		double d = ln_get_angular_separation (&sunT, &tel);
		if (t == 0)
			distanceNow = d;
		ln_get_hrz_from_equ (&sunT, &observer, JDt, &sunHrz);
		if (sunHrz.alt > SUN_AVOID_ALTITUDE && d < SUN_AVOID_DISTANCE)
			return t;
	}
	return -1;
}
