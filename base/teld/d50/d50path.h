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

// base note: classic had all of this inside D50 (src/teld/d50.cpp), reading
// the drives, the clock and the master state directly. It is a class of its
// own here so that it can be tested without the mount (teld/tests/
// test_d50path.cpp); D50 fills in the drive parameters, the time and the
// limits before each findPath (). Three changes against classic, all
// [JS 2026-11-07]:
// - findPath () searches a grid of intermediate safe targets when the three
//   classic strategies (straight, HA first, Dec first) all fail. On
//   2026-11-06 11:21 a noon park (to HA 0, Dec -40, under the Sun) failed
//   that way and the roof could not close until a manual intermediate move.
// - the Sun is avoided while it is above the horizon (upper limb, with
//   refraction), not in every master state but night: a Sun below the
//   horizon blocked moves at dusk.
// - the Sun position is computed once per findPath (), not at every 0.2 s
//   sample of every passage.
// - sunApproach (): D50 parks a standing mount the Sun drifts towards (new).
// - a mount already in the Sun's proximity may leave it, moving away from
//   the Sun from the first step on (checkPassage (), new).

#pragma once

#include "objectcheck.h"

#include <libnova/libnova.h>

#include <cstdint>

// timestep [s] for tracing path in checkPassage routine
#define CHECKPASSAGE_TIME_RESOLUTION	0.2
// ... and while leaving the Sun's proximity
#define CHECKPASSAGE_ESCAPE_RESOLUTION	0.01
// timestep [s] for examining path in findPath routine
#define FINDPATH_TIME_RESOLUTION	1.0
// grid step in degrees of the intermediate targets findPath searches
#define FINDPATH_MIN_STEP_SIZE		5
#define MAX_NUMBER_OF_SUCCESSIVE_POINTS	20

// We should never get closer to Sun than... [deg]
#define SUN_AVOID_DISTANCE 10.0
// how far ahead [s] a standing mount is checked for the Sun drifting towards it
#define SUN_APPROACH_LEAD 600.0
// ...while the Sun is above the horizon: centre altitude of a Sun whose upper
// limb touches the horizon, refraction included [deg]
#define SUN_AVOID_ALTITUDE (-0.833)

namespace rts2teld
{

class D50Path
{
	public:
		D50Path ();

		// axis parameters, 0 = HA, 1 = Dec: speed [counts/s], acceleration and
		// deceleration [counts/s^2], all positive
		double vMax[2];
		double accel[2];
		double decel[2];

		// raw counts <-> HA/Dec [deg]: HA = AC / haCpd + haZero, Dec = DC / decCpd + decZero
		double haCpd, decCpd, haZero, decZero;
		int32_t acMin, acMax, dcMin, dcMax;

		struct ln_lnlat_posn observer;
		ObjectCheck *hardHorizon;
		ObjectCheck *hardCeiling;	// may be nullptr
		bool checkCeiling;		// the dome is not open: stay below the ceiling (the Sun cannot reach us then)

		/**
		 * Set the time of the path: computes the Sun position and whether it is
		 * to be avoided. Call before findPath ().
		 */
		void setTime (double JD);

		// under a not open roof the Sun is not checked (the ceiling is)
		bool avoidingSun () { return checkSunDistance && !checkCeiling; }

		/**
		* Try to find path from (acStart, dcStart) to (acFin, dcFin).
		* Sets values to successivePointsAC[], successivePointsDC[] and
		* successivePointsIntervals[]: the drives get target 0 at the start
		* (after the backlash), target k + 1 successivePointsIntervals[k] seconds
		* after target k. Every target is safe to stop at: if the next one is
		* never set, the mount stays clear of the horizon, ceiling and Sun.
		*
		* @return <0 when path not found, =0 when simple ("straight") movement strategy
		* is suitable, >0 returns number of successive points used to reach the target.
		*/
		int findPath (int32_t acStart, int32_t dcStart, int32_t acFin, int32_t dcFin);

		int32_t successivePointsAC[MAX_NUMBER_OF_SUCCESSIVE_POINTS + 1], successivePointsDC[MAX_NUMBER_OF_SUCCESSIVE_POINTS + 1];
		double successivePointsIntervals[MAX_NUMBER_OF_SUCCESSIVE_POINTS];

		/**
		* Find shortest possible successivePointInterval, after which can be movement from*->to* updated to target next*.
		*
		* @param fromAC		initial position of HA [counts] of preceding movement
		* @param fromDC		initial position of Dec [counts] of preceding movement
		* @param toAC		final position of HA [counts] of preceding movement
		* @param toDC		final position of Dec [counts] of preceding movement
		* @param nextAC		next position of HA [counts]
		* @param nextDC		next position of Dec [counts]
		* @param initSpeedAC	initial speed in HA [counts/s] of preceding movement
		* @param initSpeedDC	initial speed in Dec [counts/s] of preceding movement
		* @param switchAC	when not nullptr, HA position [counts] at the switch
		* @param switchDC	when not nullptr, Dec position [counts] at the switch
		* @param switchSpeedAC	when not nullptr, HA speed [counts/s] at the switch
		* @param switchSpeedDC	when not nullptr, Dec speed [counts/s] at the switch
		*
		* @return shortest found interval [s] for realizing the target transition, negative number when nothing found
		*/
		double computeSuccessivePointInterval (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, int32_t nextAC, int32_t nextDC, double initSpeedAC = 0, double initSpeedDC = 0, int32_t *switchAC = nullptr, int32_t *switchDC = nullptr, double *switchSpeedAC = nullptr, double *switchSpeedDC = nullptr);

		/**
		* Check, whether the considered simple "two points" passage of the telescope
		* will not conflict with the defined hard-horizon (and ceiling, and the Sun's
		* proximity - which a passage starting in it may only leave, getting farther
		* from the Sun at every step).
		* Also returns the expected time of movement.
		* Takes into account current values of acc/decc and max speed for both axes, as well as initial speeds in both axes.
		*
		* @param fromAC		initial position of HA [counts]
		* @param fromDC		initial position of Dec [counts]
		* @param toAC		final position of HA [counts]
		* @param toDC		final position of Dec [counts]
		* @param initSpeedAC	initial speed in HA [counts/s]
		* @param initSpeedDC	initial speed in Dec [counts/s]
		*
		* @return >0 success, expected time needed to finish movement [s]
		* @return <=0 conflict with horizon recognized at fabs() [s] from start
		*/
		double checkPassage (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, double initSpeedAC = 0, double initSpeedDC = 0);

		/**
		* Simply return time needed to fully complete complex both-axis movement, no checks performed.
		*
		* @return time [s] till the movement is done
		*/
		double computeFullMovementTime (int32_t fromAC, int32_t fromDC, int32_t toAC, int32_t toDC, double initSpeedAC = 0, double initSpeedDC = 0);

		/**
		* Compute actual position of specified movement at specific time inspectionTime, for one axis only.
		* Negative value of inspectionTime changes meaning, in this case return time in miliseconds needed to travel the full distance.
		* When double * speedAtInspectionTime is defined, also compute an actual speed.
		* The initial speed is taken in the direction of the movement: a movement
		* that reverses a moving axis is not modelled (findPath never asks for one).
		*
		* @param isDec				0 for HA, other (typically 1) for Dec axis
		* @param fromCount			initial position [counts]
		* @param toCount			final position [counts]
		* @param initSpeed			initial speed [counts/s]
		* @param inspectionTime			time (from the beginning of movement) for which the result is requested, value <0 means full travel time requested
		* @param *speedAtInspectionTime		where the computed speed at inspectionTime is stored (only when pointer differs from nullptr)
		*
		* @return computed position [counts] at inspectionTime, or time [ms] needed to complete all movemens, if inspectionTime<0
		*/
		int32_t computeMovement (int isDec, int32_t fromCount, int32_t toCount, double initSpeed, double inspectionTime, double* speedAtInspectionTime = nullptr);

		/**
		* Check potential conflict with a hard-horizon, Sun distance and dome's ceiling for the position in raw coordinates.
		*
		* @param sunDist	when not nullptr, the Sun is not checked but its distance stored there (INFINITY when not avoided)
		*
		* @return 1 if position is ok, 0 when collision with hard-horizon, Sun proximity or dome's ceiling (when checked) detected
		*/
		int checkRawPosition (int32_t AC, int32_t DC, double *sunDist = nullptr);

		/**
		* Distance [deg] of the position from the Sun, INFINITY when the Sun is not avoided (below the horizon, or the roof is not open).
		*/
		double sunDistance (int32_t AC, int32_t DC);

		/**
		 * Will the Sun come within SUN_AVOID_DISTANCE of a mount standing at
		 * (AC, DC) in the next lead seconds after the time of setTime ()? Without
		 * tracking the mount keeps its HA/Dec and the Sun crosses it at ~15 deg/h;
		 * with tracking it keeps its RA/Dec. Only while the Sun is above the horizon.
		 *
		 * @param distanceNow	distance from the Sun at the time of setTime () [deg]
		 *
		 * @return seconds until the Sun is within SUN_AVOID_DISTANCE (0 = already is), <0 if it is not in lead seconds
		 */
		double sunApproach (int32_t AC, int32_t DC, bool tracking, double lead, double &distanceNow);

		// number of grid targets the last search examined (for the log and the tests)
		int searchedNodes;

	private:
		int findPathOnGrid (int32_t acStart, int32_t dcStart, int32_t acFin, int32_t dcFin);
		int planIntervals (int32_t acStart, int32_t dcStart, int n);

		double JD;
		double gmst;			// mean sidereal time at JD [deg]
		struct ln_equ_posn sun;
		bool checkSunDistance;
};

}
