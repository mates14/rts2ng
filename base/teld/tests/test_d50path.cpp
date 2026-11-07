// rts2teld::D50Path - the D50 "Successive Safe Targets" path finding.
//
// The case behind it: 2026-11-06 11:21:15 UTC, after drying the dew, the
// dome asked T0 to park from HA +32.4, Dec +63.1 to the park position (HA 0,
// Dec -40.09, the south horizon) with the Sun at HA +9, Dec -16. The straight
// way passes 1.5 deg from the Sun, HA-first 8.8 deg, Dec-first goes below
// the horizon: findPath () failed, the roof could not close, and only a
// manual move east of the Sun let the park through. Then the dusk failure of
// the same day (15:50:45, Sun just set but still avoided).
//
// Every path found is replayed here on its own: the movement model is
// stepped every 5 ms through the successive targets with their intervals,
// and each sample is checked against the D50 hard horizon (and ceiling) and
// the Sun distance computed separately, not with D50Path::checkRawPosition ().

#include "d50path.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <unistd.h>

using rts2teld::D50Path;

// /etc/rts2/horizon-hard at D50 (2009-04-15), RTS2 azimuth (from south), as
// written there: tabs and signed altitudes (ObjectCheck reads "125 29" as
// 125 deg 29 arcmin)
static const char *D50_HARD_HORIZON =
	"AZ-ALT\n"
	"0\t\t-6\n"
	"30\t\t-1\n"
	"55.4825\t\t-1\n"
	"92.1321\t\t-1\n"
	"111.741\t\t+15.0323\n"
	"125\t\t+29\n"
	"140\t\t+44\n"
	"150\t\t+52\n"
	"160\t\t+56.5\n"
	"170\t\t+59.5\n"
	"179.338\t\t+60.5\n"
	"190\t\t+59.5\n"
	"201.417\t\t+57.0335\n"
	"235.988\t\t+42.8667\n"
	"256.286\t\t+5\n"
	"270\t\t-10\n"
	"282.058\t\t-1\n"
	"321.562\t\t-1\n"
	"323\t\t-1\n";

// /etc/rts2/horizon-ceiling at D50 (2022-02-24), as written there: with the roof not
// open the mount must stay below it
static const char *D50_CEILING =
	"AZ-ALT\n"
	"0\t\t+10.0\n"
	"30\t\t+8.0\n"
	"60\t\t+4.0\n"
	"75\t\t0.0\n"
	"85\t\t0.0\n"
	"90\t\t0.0\n"
	"96\t\t0.0\n"
	"115\t\t0.0\n"
	"125\t\t0.0\n"
	"140\t\t0.0\n"
	"150\t\t0.0\n"
	"160\t\t0.0\n"
	"170\t\t0.0\n"
	"180\t\t0.0\n"
	"190\t\t0.0\n"
	"200\t\t0.0\n"
	"210\t\t0.0\n"
	"220\t\t0.0\n"
	"240\t\t+10.0\n"
	"260\t\t+10.0\n"
	"280\t\t+10.0\n"
	"310\t\t+10.0\n"
	"320\t\t+10.0\n"
	"340\t\t+10.0\n";

static const double LNG = 14.7813631, LAT = 49.9093889;

static double julian (int y, int mo, int d, int h, int mi, double s)
{
	struct ln_date date;
	date.years = y; date.months = mo; date.days = d;
	date.hours = h; date.minutes = mi; date.seconds = s;
	return ln_get_julian_day (&date);
}

static ObjectCheck horizon;
static ObjectCheck ceiling;

static void setupD50 (D50Path &p)
{
	// as D50::D50 () / init () / updateLimits () and the T0 values at D50 (2026-11-07):
	// speed_full 5 deg/s, RA_ACCEL/DECEL 5/5, DEC_ACCEL/DECEL 20/10 rev/s^2, physical limit 61.67 rev/s
	p.haCpd = (int32_t) (-2304 * 65536) / 360.0;
	p.decCpd = (int32_t) (4000 * 65536) / 360.0;
	p.haZero = 0;
	p.decZero = - (90.0 - LAT);
	p.acMax = (int32_t) (p.haCpd * (-180.0 - p.haZero));
	p.acMin = (int32_t) (p.haCpd * (180.0 - p.haZero));
	p.dcMin = 0;
	p.dcMax = (int32_t) (p.decCpd * (90.0 - p.decZero));
	p.vMax[0] = std::min (5 * fabs (p.haCpd), 61.666666 * 65536);
	p.vMax[1] = std::min (5 * fabs (p.decCpd), 61.666666 * 65536);
	p.accel[0] = 5 * 65536.0;
	p.decel[0] = 5 * 65536.0;
	p.accel[1] = 20 * 65536.0;
	p.decel[1] = 10 * 65536.0;
	p.observer.lng = LNG;
	p.observer.lat = LAT;
	p.hardHorizon = &horizon;
	p.hardCeiling = nullptr;
	p.checkCeiling = false;
}

static int32_t acOf (D50Path &p, double ha) { return (int32_t) ((ha - p.haZero) * p.haCpd); }
static int32_t dcOf (D50Path &p, double dec) { return (int32_t) ((dec - p.decZero) * p.decCpd); }
static double haOf (D50Path &p, int32_t ac) { return ac / p.haCpd + p.haZero; }
static double decOf (D50Path &p, int32_t dc) { return dc / p.decCpd + p.decZero; }

// the Sun at JD as hour angle / declination and altitude, independently of D50Path
static void sunAt (double JD, double &ha, double &dec, double &alt)
{
	struct ln_equ_posn s;
	ln_get_solar_equ_coords (JD, &s);
	ha = ln_get_mean_sidereal_time (JD) * 15.0 + LNG - s.ra;
	dec = s.dec;
	struct ln_hrz_posn h;
	struct ln_lnlat_posn obs = { LNG, LAT };
	ln_get_hrz_from_equ (&s, &obs, JD, &h);
	alt = h.alt;
}

static double sepDeg (double ha1, double d1, double ha2, double d2)
{
	const double r = M_PI / 180.0;
	double c = sin (d1 * r) * sin (d2 * r) + cos (d1 * r) * cos (d2 * r) * cos ((ha1 - ha2) * r);
	return acos (std::max (-1.0, std::min (1.0, c))) / r;
}

static bool aboveHardHorizon (double ha, double dec)
{
	struct ln_equ_posn e = { -ha, dec };
	struct ln_lnlat_posn obs = { LNG, LAT };
	struct ln_hrz_posn h;
	ln_get_hrz_from_equ_sidereal_time (&e, &obs, -LNG / 15.0, &h);
	return horizon.is_good (&h);
}

static bool belowCeiling (double ha, double dec)
{
	struct ln_equ_posn e = { -ha, dec };
	struct ln_lnlat_posn obs = { LNG, LAT };
	struct ln_hrz_posn h;
	ln_get_hrz_from_equ_sidereal_time (&e, &obs, -LNG / 15.0, &h);
	return !ceiling.is_good (&h);
}

static void loadFile (ObjectCheck &oc, const char *content)
{
	char tmpl[] = "/tmp/test_d50path_XXXXXX";
	int fd = mkstemp (tmpl);
	assert (fd >= 0);
	close (fd);
	{
		std::ofstream f (tmpl);
		f << content;
	}
	assert (oc.loadHorizon (tmpl) == 0);
	unlink (tmpl);
}

// HA/Dec of a horizontal position at JD
static void haDecOf (double alt, double az, double JD, double &ha, double &dec)
{
	struct ln_hrz_posn h = { az, alt };
	struct ln_lnlat_posn obs = { LNG, LAT };
	struct ln_equ_posn e;
	ln_get_equ_from_hrz (&h, &obs, JD, &e);
	ha = ln_range_degrees (ln_get_mean_sidereal_time (JD) * 15.0 + LNG - e.ra + 180) - 180;
	dec = e.dec;
}

// replay the path through the movement model; returns the closest approach to the Sun [deg] (after leaving its
// proximity, for a start in it), -1 on a horizon hit, -2 when a start in the proximity got closer to the Sun
static double replay (D50Path &p, int n, int32_t ac, int32_t dc, double JD, bool sunChecked, int32_t acFin, int32_t dcFin)
{
	double sha, sdec, salt;
	sunAt (JD, sha, sdec, salt);
	double vac = 0, vdc = 0, closest = 360;
	// a start in the Sun's proximity: the distance must grow from the first step until it is out
	double previous = sepDeg (haOf (p, ac), decOf (p, dc), sha, sdec);
	bool escaping = sunChecked && previous < SUN_AVOID_DISTANCE;
	for (int k = 0; k <= n; k++)
	{
		int32_t tac = p.successivePointsAC[k], tdc = p.successivePointsDC[k];
		double full = p.computeFullMovementTime (ac, dc, tac, tdc, vac, vdc);
		double until = (k < n) ? p.successivePointsIntervals[k] : full;
		assert (until <= full + 1e-9);
		for (double t = 0; ; t += 0.005)
		{
			if (t > until)
				t = until;
			double sac, sdc;
			int32_t a = p.computeMovement (0, ac, tac, vac, t, &sac);
			int32_t d = p.computeMovement (1, dc, tdc, vdc, t, &sdc);
			double ha = haOf (p, a), dec = decOf (p, d);
			if (!aboveHardHorizon (ha, dec))
				return -1;
			if (p.checkCeiling && !belowCeiling (ha, dec))
				return -1;
			if (a < std::min (p.acMin, p.acMax) || a > std::max (p.acMin, p.acMax) || d < p.dcMin || d > p.dcMax)
				return -1;
			if (sunChecked)
			{
				double sd = sepDeg (ha, dec, sha, sdec);
				if (escaping)
				{
					// positions are whole counts (one is 2.4e-6 deg in HA): allow a few counts of rounding
					if (t > 0 && sd < previous - 1e-5)
					{
						fprintf (stderr, "closer by %.2g deg at segment %d t %.3f (distance %.4f)\n", previous - sd, k, t, sd);
						return -2;
					}
					previous = sd;
					escaping = sd < SUN_AVOID_DISTANCE;
				}
				else
				{
					closest = std::min (closest, sd);
				}
			}
			if (t >= until)
			{
				ac = a; dc = d; vac = sac; vdc = sdc;
				break;
			}
		}
	}
	assert (ac == acFin && dc == dcFin);
	assert (!escaping);
	return closest;
}

int main ()
{
	loadFile (horizon, D50_HARD_HORIZON);
	loadFile (ceiling, D50_CEILING);

	D50Path p;
	setupD50 (p);

	// --- 2026-11-06 11:21:15, the noon park
	{
		double JD = julian (2026, 11, 6, 11, 21, 15.6);
		double lst = ln_get_mean_sidereal_time (JD) * 15.0 + LNG;
		double ha = ln_range_degrees (lst - 198.347953 + 180) - 180;	// RA/Dec from the T0 log line
		int32_t ac0 = acOf (p, ha), dc0 = dcOf (p, 63.110642);
		int32_t acP = acOf (p, 0), dcP = 0;					// --park 0:0
		p.setTime (JD);
		assert (p.avoidingSun ());

		// what classic tried, all three refused
		assert (p.checkPassage (ac0, dc0, acP, dcP) <= 0);
		assert (p.checkPassage (ac0, dc0, acP, dc0) <= 0 || p.computeSuccessivePointInterval (ac0, dc0, acP, dc0, acP, dcP) < 0);
		assert (p.checkPassage (ac0, dc0, ac0, dcP) <= 0 || p.computeSuccessivePointInterval (ac0, dc0, ac0, dcP, acP, dcP) < 0);

		auto t0 = std::chrono::steady_clock::now ();
		int n = p.findPath (ac0, dc0, acP, dcP);
		double ms = std::chrono::duration <double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
		assert (n >= 1);
		assert (p.searchedNodes > 0);
		printf ("noon park 2026-11-06 11:21:15: %d successive points, %d targets tried, %.1f ms:", n, p.searchedNodes, ms);
		for (int k = 0; k <= n; k++)
		{
			printf (" %.1f/%.1f", haOf (p, p.successivePointsAC[k]), decOf (p, p.successivePointsDC[k]));
			if (k < n)
				printf (" (+%.1f s)", p.successivePointsIntervals[k]);
		}
		double closest = replay (p, n, ac0, dc0, JD, true, acP, dcP);
		printf ("; closest to the Sun %.1f deg\n", closest);
		assert (closest >= SUN_AVOID_DISTANCE);
	}

	// --- 2026-11-06 15:50:45, dusk: from the park to RA 346.42 Dec 50.82, the Sun set
	{
		double JD = julian (2026, 11, 6, 15, 50, 45.6);
		double lst = ln_get_mean_sidereal_time (JD) * 15.0 + LNG;
		double sha, sdec, salt;
		sunAt (JD, sha, sdec, salt);
		int32_t ac0 = acOf (p, ln_range_degrees (lst - 298.350399 + 180) - 180), dc0 = 0;
		int32_t ac1 = acOf (p, ln_range_degrees (lst - 346.420403 + 180) - 180), dc1 = dcOf (p, 50.824980);
		p.setTime (JD);
		assert (salt < SUN_AVOID_ALTITUDE);
		assert (!p.avoidingSun ());
		int n = p.findPath (ac0, dc0, ac1, dc1);
		printf ("dusk 2026-11-06 15:50:45 (Sun at %.1f deg altitude, not avoided): %d successive points\n", salt, n);
		assert (n >= 0);
		assert (replay (p, n, ac0, dc0, JD, false, ac1, dc1) >= 0);
	}

	// --- a standing mount the Sun drifts towards (sunApproach (), D50::checkSunApproach () parks then)
	{
		double JD = julian (2026, 11, 6, 11, 21, 15.6);
		p.setTime (JD);
		double sha, sdec, salt, now;
		sunAt (JD, sha, sdec, salt);
		sha = ln_range_degrees (sha + 180) - 180;

		// the drying position of that day: 81 deg away, nothing to do
		double lst = ln_get_mean_sidereal_time (JD) * 15.0 + LNG;
		int32_t acD = acOf (p, ln_range_degrees (lst - 198.347953 + 180) - 180), dcD = dcOf (p, 63.110642);
		assert (p.sunApproach (acD, dcD, false, SUN_APPROACH_LEAD, now) < 0);
		assert (now > 80);

		// 2 deg outside the limit, west of the Sun on its declination: the Sun closes in at ~15 deg/h * cos (dec)
		const double W = (SUN_AVOID_DISTANCE + 2) / cos (sdec * M_PI / 180);
		int32_t acW = acOf (p, sha + W), dcW = dcOf (p, sdec);
		double in = p.sunApproach (acW, dcW, false, SUN_APPROACH_LEAD, now);
		printf ("standing %.0f deg west of the Sun: within %g deg in %.0f s (now %.1f deg)\n", W, SUN_AVOID_DISTANCE, in, now);
		assert (now > SUN_AVOID_DISTANCE && in > 0 && in < SUN_APPROACH_LEAD);
		// tracking there, the Sun keeps its distance (1 deg a day)
		assert (p.sunApproach (acW, dcW, true, SUN_APPROACH_LEAD, now) < 0);
		// 10 deg farther: more than the lead away
		assert (p.sunApproach (acOf (p, sha + W + 10), dcW, false, SUN_APPROACH_LEAD, now) < 0);
		// 5 deg west: already too close
		assert (p.sunApproach (acOf (p, sha + 5), dcW, false, SUN_APPROACH_LEAD, now) == 0);
		// what D50 does then: park, around the Sun
		int n = p.findPath (acW, dcW, acOf (p, 0), 0);
		printf ("  and parks from there: %d successive points\n", n);
		assert (n >= 0);
		assert (replay (p, n, acW, dcW, JD, true, acOf (p, 0), 0) >= SUN_AVOID_DISTANCE);

		// the Sun below the horizon (that evening): never
		p.setTime (julian (2026, 11, 6, 17, 0, 0));
		assert (p.sunApproach (acW, dcW, false, SUN_APPROACH_LEAD, now) < 0);
	}

	// --- leaving the Sun's proximity: moving away from the Sun from the first step on
	{
		double JD = julian (2026, 11, 6, 11, 21, 15.6);
		p.setTime (JD);
		double sha, sdec, salt;
		sunAt (JD, sha, sdec, salt);
		sha = ln_range_degrees (sha + 180) - 180;
		struct { const char *name; double dha, ddec; } cases[] = {
			{ "5 deg west of the Sun", 5, 0 }, { "5 deg east", -5, 0 }, { "3 deg above", 0, 3 }, { "3 deg below", 0, -3 }, { "1 deg off", 0.7, 0.7 } };
		for (auto &c : cases)
		{
			int32_t ac0 = acOf (p, sha + c.dha), dc0 = dcOf (p, sdec + c.ddec);
			int n = p.findPath (ac0, dc0, acOf (p, 0), 0);
			printf ("park from %s (%.1f deg): %d successive points, %d targets tried:", c.name, p.sunDistance (ac0, dc0), n, p.searchedNodes);
			for (int k = 0; k <= n; k++)
				printf (" %.1f/%.1f", haOf (p, p.successivePointsAC[k]), decOf (p, p.successivePointsDC[k]));
			printf ("\n");
			assert (n >= 0);
			double closest = replay (p, n, ac0, dc0, JD, true, acOf (p, 0), 0);
			assert (closest >= SUN_AVOID_DISTANCE - 0.04);
		}
		// a target in the proximity is refused at once (D50 13:23 altaz 20 45: 727 grid targets tried before)
		assert (std::string (p.whyNot (acOf (p, sha + 5), dcOf (p, sdec))) == "too close to the Sun");
		assert (p.findPath (acOf (p, 0), 0, acOf (p, sha + 5), dcOf (p, sdec)) < 0 && p.searchedNodes == 0);
		// a move that would go closer first is refused: from 5 deg west straight across the Sun
		int32_t acW = acOf (p, sha + 5), dcW = dcOf (p, sdec);
		assert (p.checkPassage (acW, dcW, acOf (p, sha - 25), dcW) <= 0);

		// random starts in the proximity over a year, to the park and to random targets
		std::mt19937 rng (11);
		std::uniform_real_distribution <double> uday (0, 365), uhour (7, 15), ur (0.5, SUN_AVOID_DISTANCE - 0.1), uang (0, 2 * M_PI), uha (-175, 175), udec (-40, 89);
		int tried = 0, found = 0;
		double escMs = 0;
		for (int i = 0; i < 1500; i++)
		{
			double JDr = julian (2026, 1, 1, 0, 0, 0) + floor (uday (rng)) + uhour (rng) / 24.0;
			p.setTime (JDr);
			if (!p.avoidingSun ())
				continue;
			double sh, sd0, sa;
			sunAt (JDr, sh, sd0, sa);
			double r = ur (rng), ang = uang (rng);
			double ha0 = ln_range_degrees (sh + r * cos (ang) / cos (sd0 * M_PI / 180) + 180) - 180, d0 = sd0 + r * sin (ang);
			int32_t ac0 = acOf (p, ha0), dc0 = dcOf (p, d0);
			double sd;
			if (dc0 < p.dcMin || !p.checkRawPosition (ac0, dc0, &sd) || sd >= SUN_AVOID_DISTANCE)
				continue;
			int32_t ac1 = acOf (p, 0), dc1 = 0;
			if (i % 2)
			{
				ac1 = acOf (p, uha (rng));
				dc1 = dcOf (p, udec (rng));
				if (!p.checkRawPosition (ac1, dc1))
					continue;
			}
			tried++;
			auto t0 = std::chrono::steady_clock::now ();
			int n = p.findPath (ac0, dc0, ac1, dc1);
			escMs = std::max (escMs, std::chrono::duration <double, std::milli> (std::chrono::steady_clock::now () - t0).count ());
			if (n < 0)
			{
				struct ln_equ_posn e = { -ha0, d0 };
				struct ln_lnlat_posn obs = { LNG, LAT };
				struct ln_hrz_posn hz;
				ln_get_hrz_from_equ_sidereal_time (&e, &obs, -LNG / 15.0, &hz);
				printf ("  no way out: from alt %.1f az %.1f, %.1f deg from the Sun at alt %.1f, to %s\n", hz.alt, hz.az, sd, sa, i % 2 ? "a random target" : "the park");
				continue;
			}
			found++;
			double closest = replay (p, n, ac0, dc0, JDr, true, ac1, dc1);
			if (closest < SUN_AVOID_DISTANCE - 0.04)
			{
				fprintf (stderr, "case %d: from %.1f/%.1f (%.1f deg from the Sun) %d points, replay %.2f\n", i, ha0, d0, sd, n, closest);
				assert (false);
			}
		}
		printf ("random starts in the Sun's proximity: %d tried, %d left it safely, %d without a way; slowest findPath %.1f ms\n", tried, found, tried - found, escMs);
		assert (found > tried * 9 / 10);
	}

	// --- under the roof (dome not open): from the park along the band between the hard horizon and the ceiling
	{
		p.hardCeiling = &ceiling;
		p.checkCeiling = true;
		double JD = julian (2026, 11, 7, 12, 0, 0);
		p.setTime (JD);
		double ha, dec;

		// altaz 2 270 (east: hard horizon -10, ceiling +10) - the classic tries fail, the grid finds it
		haDecOf (2, 270, JD, ha, dec);
		int32_t ac1 = acOf (p, ha), dc1 = dcOf (p, dec);
		assert (p.checkRawPosition (ac1, dc1));
		int n = p.findPath (acOf (p, 0), 0, ac1, dc1);
		printf ("under the roof, park -> altaz 2 270: %d successive points, %d targets tried:", n, p.searchedNodes);
		for (int k = 0; k <= n && n >= 0; k++)
			printf (" %.1f/%.1f", haOf (p, p.successivePointsAC[k]), decOf (p, p.successivePointsDC[k]));
		printf ("\n");
		assert (n >= 1 && p.searchedNodes > 0);
		assert (replay (p, n, acOf (p, 0), 0, JD, false, ac1, dc1) >= 0);
		// and back to the park
		n = p.findPath (ac1, dc1, acOf (p, 0), 0);
		assert (n >= 0);
		assert (replay (p, n, ac1, dc1, JD, false, acOf (p, 0), 0) >= 0);

		// altaz 2 90 (west: the ceiling is at 0 there) - the target itself is above the ceiling, refused at once
		haDecOf (2, 90, JD, ha, dec);
		assert (!p.checkRawPosition (acOf (p, ha), dcOf (p, dec)));
		assert (std::string (p.whyNot (acOf (p, ha), dcOf (p, dec))) == "above the ceiling");
		assert (p.findPath (acOf (p, 0), 0, acOf (p, ha), dcOf (p, dec)) < 0 && p.searchedNodes == 0);
		// altaz -0.5 90, inside the west band (-1 .. 0), is reachable
		haDecOf (-0.5, 90, JD, ha, dec);
		ac1 = acOf (p, ha);
		dc1 = dcOf (p, dec);
		n = p.findPath (acOf (p, 0), 0, ac1, dc1);
		printf ("under the roof, park -> altaz -0.5 90: %d successive points, %d targets tried\n", n, p.searchedNodes);
		if (n >= 0)
			assert (replay (p, n, acOf (p, 0), 0, JD, false, ac1, dc1) >= 0);

		// random positions under the roof: every path found keeps between horizon and ceiling
		std::mt19937 rng (7);
		std::uniform_real_distribution <double> ualt (-10, 10), uaz (0, 360);
		int tried = 0, found = 0, grid = 0;
		for (int i = 0; i < 2000; i++)
		{
			double ha0, d0, ha1, d1;
			haDecOf (ualt (rng), uaz (rng), JD, ha0, d0);
			haDecOf (ualt (rng), uaz (rng), JD, ha1, d1);
			int32_t a0 = acOf (p, ha0), c0 = dcOf (p, d0), a1 = acOf (p, ha1), c1 = dcOf (p, d1);
			if (c0 < p.dcMin || c1 < p.dcMin || !p.checkRawPosition (a0, c0) || !p.checkRawPosition (a1, c1))
				continue;
			tried++;
			n = p.findPath (a0, c0, a1, c1);
			if (n < 0)
				continue;
			found++;
			if (p.searchedNodes > 0)
				grid++;
			assert (replay (p, n, a0, c0, JD, false, a1, c1) >= 0);
		}
		printf ("random moves under the roof: %d tried, %d found (%d on the grid), all between horizon and ceiling\n", tried, found, grid);

		p.hardCeiling = nullptr;
		p.checkCeiling = false;
	}

	// --- random daytime moves over a year: every path found is safe and ends on the target
	{
		std::mt19937 rng (20261107);
		std::uniform_real_distribution <double> uha (-175, 175), udec (-40, 89), uday (0, 365), uhour (6, 16);
		int tried = 0, classic = 0, grid = 0, none = 0, sunned = 0;
		double worstMs = 0, worstClosest = 360;
		for (int i = 0; i < 3000; i++)
		{
			double JD = julian (2026, 1, 1, 0, 0, 0) + floor (uday (rng)) + uhour (rng) / 24.0;
			double sha, sdec, salt;
			sunAt (JD, sha, sdec, salt);
			p.setTime (JD);
			double ha0 = uha (rng), d0 = udec (rng), ha1 = uha (rng), d1 = udec (rng);
			if (i % 3 == 0)
			{
				ha1 = 0;	// a park
				d1 = p.decZero;
			}
			int32_t ac0 = acOf (p, ha0), dc0 = dcOf (p, d0), ac1 = acOf (p, ha1), dc1 = i % 3 == 0 ? 0 : dcOf (p, d1);
			// only moves between positions the mount may stand at
			if (!p.checkRawPosition (ac0, dc0) || !p.checkRawPosition (ac1, dc1))
				continue;
			tried++;
			if (p.avoidingSun ())
				sunned++;
			auto t0 = std::chrono::steady_clock::now ();
			int n = p.findPath (ac0, dc0, ac1, dc1);
			worstMs = std::max (worstMs, std::chrono::duration <double, std::milli> (std::chrono::steady_clock::now () - t0).count ());
			if (n < 0)
			{
				none++;
				continue;
			}
			if (p.searchedNodes > 0)
				grid++;
			else
				classic++;
			double closest = replay (p, n, ac0, dc0, JD, p.avoidingSun (), ac1, dc1);
			if (closest < 0)
			{
				fprintf (stderr, "case %d: horizon hit, from %.1f/%.1f to %.1f/%.1f, %d points\n", i, ha0, d0, ha1, d1, n);
				assert (false);
			}
			// checkPassage () bounds the way between its samples; the replay samples every 5 ms: 0.035 deg at 7 deg/s
			if (closest < SUN_AVOID_DISTANCE - 0.04)
			{
				fprintf (stderr, "case %d: %.3f deg from the Sun\n", i, closest);
				assert (false);
			}
			worstClosest = std::min (worstClosest, closest);
		}
		printf ("random daytime moves: %d tried (%d with the Sun up), %d by the classic strategies, %d on the grid, %d without a safe way; closest to the Sun %.2f deg; slowest findPath %.1f ms\n",
			tried, sunned, classic, grid, none, worstClosest, worstMs);
		assert (grid > 0);
	}

	return 0;
}
