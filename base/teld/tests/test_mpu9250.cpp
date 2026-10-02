// ImuMountModel against a simulated sensor: an arbitrary mounting, bias and
// gain, noisy readings at poses spread over the sky; the fit must find a
// model that agrees with the true one, and a counter error made after the
// calibration must show up - on the axis it was made on.

#include "mpu9250.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace rts2teld;

static const double DEG = M_PI / 180.0;

// the same GEM relation the model documents, written out independently:
// tube pointing from (HA, Dec) on the W side, up = zenith in tube frame
static void trueAcc (double lat, const double R[9], const double b[3], double s, double raAxis, double decAxis, double out[3])
{
	// mount frame unit vectors of the tube: Z = pointing, Y = Dec axis, X = Y x Z
	double a = (raAxis - 90) * DEG, d = (180 - decAxis) * DEG;
	double ca = cos (a), sa = sin (a), cd = cos (d), sd = sin (d);
	// T = Rz(a) Ry(d), columns are the tube axes
	double T[9] = {ca * cd, -sa, ca * sd, sa * cd, ca, sa * sd, -sd, 0, cd};
	double zen[3] = {cos (lat * DEG), 0, sin (lat * DEG)};
	double tube[3];
	for (int i = 0; i < 3; i++)
		tube[i] = T[i] * zen[0] + T[3 + i] * zen[1] + T[6 + i] * zen[2];
	for (int i = 0; i < 3; i++)
		out[i] = s * (R[i] * tube[0] + R[3 + i] * tube[1] + R[6 + i] * tube[2]) + b[i];
}

int main ()
{
	const double lat = 37.06;
	std::mt19937 rng (42);
	std::normal_distribution<double> noise (0, 0.002);
	std::uniform_real_distribution<double> ra (180 - 100, 180 + 100), dec (180 - 120, 180 + 120);

	// a mounting that is not square to anything: rotate about (1,2,3) by 50 deg
	double k[3] = {1 / sqrt (14), 2 / sqrt (14), 3 / sqrt (14)}, th = 50 * DEG;
	double c = cos (th), s = sin (th), C = 1 - c;
	double R[9] = {
		c + k[0] * k[0] * C, k[0] * k[1] * C - k[2] * s, k[0] * k[2] * C + k[1] * s,
		k[1] * k[0] * C + k[2] * s, c + k[1] * k[1] * C, k[1] * k[2] * C - k[0] * s,
		k[2] * k[0] * C - k[1] * s, k[2] * k[1] * C + k[0] * s, c + k[2] * k[2] * C};
	double bias[3] = {0.012, -0.020, 0.008};
	double gain = 1.015;

	std::vector<ImuMountModel::CalSample> cal;
	for (int i = 0; i < 20; i++)
	{
		ImuMountModel::CalSample cs;
		cs.raAxis = ra (rng);
		cs.decAxis = dec (rng);
		trueAcc (lat, R, bias, gain, cs.raAxis, cs.decAxis, cs.acc);
		for (int j = 0; j < 3; j++)
			cs.acc[j] += noise (rng);
		cal.push_back (cs);
	}

	ImuMountModel m;
	m.setLatitude (lat);
	assert (m.fit (cal));
	printf ("%s\n", m.describe ().c_str ());
	assert (m.rms () < 0.3);

	// fresh poses: the calibrated model agrees with the truth
	double worst = 0;
	for (int i = 0; i < 200; i++)
	{
		double r = ra (rng), d = dec (rng), a[3];
		trueAcc (lat, R, bias, gain, r, d, a);
		worst = std::max (worst, m.errorDeg (r, d, a));
	}
	printf ("worst agreement on fresh poses: %.3f deg\n", worst);
	assert (worst < 0.3);

	// counters off by 3 deg in RA, then in Dec, away from the degenerate
	// poses: the error is seen and solveAxes puts it on the right axis
	double r = 180 + 60, d = 180 - 40, a[3];
	trueAcc (lat, R, bias, gain, r, d, a);
	double sr, sd, cond;
	assert (m.errorDeg (r + 3, d, a) > 1.5);
	assert (m.solveAxes (a, r + 3, d, sr, sd, cond));
	printf ("RA counter +3: sensor says RA axis %+.3f, Dec axis %+.3f (cond %.2f)\n", sr - (r + 3), sd - d, cond);
	assert (fabs (sr - r) < 0.3 && fabs (sd - d) < 0.3);
	assert (m.errorDeg (r, d + 3, a) > 1.5);
	assert (m.solveAxes (a, r, d + 3, sr, sd, cond));
	printf ("Dec counter +3: sensor says RA axis %+.3f, Dec axis %+.3f (cond %.2f)\n", sr - r, sd - (d + 3), cond);
	assert (fabs (sr - r) < 0.3 && fabs (sd - d) < 0.3);

	// with no prior: both mirror solutions come back, and a known RA step
	// tells them apart - the second reading fits only the true one
	{
		double tr = 180 + 50, td = 180 + 70, a1[3], a2[3];
		trueAcc (lat, R, bias, gain, tr, td, a1);
		trueAcc (lat, R, bias, gain, tr + 3, td, a2);
		auto sols = m.solveAll (a1, 0.5);
		assert (sols.size () == 2);
		int truth = -1;
		for (size_t i = 0; i < sols.size (); i++)
		{
			double e2 = m.errorDeg (sols[i].raAxis + 3, sols[i].decAxis, a2);
			printf ("candidate RA axis %.2f Dec axis %.2f: fits %.3f, after the RA step %.3f deg\n", sols[i].raAxis, sols[i].decAxis, sols[i].errDeg, e2);
			if (e2 < 0.3)
			{
				assert (truth < 0);
				truth = i;
			}
		}
		assert (truth >= 0 && fabs (sols[truth].raAxis - tr) < 0.3 && fabs (sols[truth].decAxis - td) < 0.3);
	}

	// at CWD - on the merge line - one small step cannot decide, a step
	// well off the line can
	for (double stepDeg : {3.0, 25.0})
	{
		double a1[3], a2[3];
		trueAcc (lat, R, bias, gain, 180, 180, a1);
		trueAcc (lat, R, bias, gain, 180 + stepDeg, 180, a2);
		auto sols = m.solveJoint (a1, a2, stepDeg, 0, 1.0);
		printf ("CWD, %.0f deg step: %d fits, best RA axis %.2f Dec axis %.2f (%.3f, cond %.1f), next %.3f deg\n", stepDeg, (int) sols.size (),
			sols[0].raAxis, sols[0].decAxis, sols[0].errDeg, sols[0].condDeg, sols.size () > 1 ? sols[1].errDeg : NAN);
		if (stepDeg < 10)
			assert (sols[0].condDeg > 5);	// on the merge line: badly conditioned
		else
		{
			assert (sols[0].condDeg < 5);
			assert (fabs (sols[0].raAxis - 180) < 0.3 && fabs (sols[0].decAxis - 180) < 0.3);
			assert (sols.size () < 2 || sols[1].errDeg > std::max (3 * sols[0].errDeg, 0.5));
		}
	}

	// southern hemisphere / reversed counters: the fit finds the sense
	std::vector<ImuMountModel::CalSample> rev = cal;
	for (auto &cs : rev)
	{
		double rr = 360 - cs.raAxis;
		trueAcc (lat, R, bias, gain, cs.raAxis, cs.decAxis, cs.acc);
		cs.raAxis = rr;
	}
	ImuMountModel mr;
	mr.setLatitude (lat);
	assert (mr.fit (rev));
	printf ("reversed RA counter: %s\n", mr.describe ().c_str ());
	assert (mr.rms () < 0.1);

	// the tube at CWD points at the pole: altitude = latitude, whatever the RA axis
	assert (fabs (m.tubeAltitude (180, 180) - lat) < 0.01 && fabs (m.tubeAltitude (150, 180) - lat) < 0.01);
	// W side, Dec axis = Dec + 90, RA axis = 90 - HA: HA 0, Dec 0 culminates at 90 - lat
	assert (fabs (m.tubeAltitude (90, 90) - (90 - lat)) < 0.01);

	// raw line parsing
	ImuSample smp;
	assert (parseMpu9250Raw ("-180 132 16441 -160 70 20 2134 83 -201 -278 0", smp));
	assert (fabs (smp.acc[2] - 16441 / 16384.0) < 1e-9 && smp.magValid && !smp.magOverflow);
	assert (parseMpu9250Raw ("-180 132 16441 -160 70 20 2134 NA NA NA NA", smp) && !smp.magValid);
	assert (!parseMpu9250Raw ("# READY", smp));
	assert (!parseMpu9250Raw ("1 2 3", smp));
	assert (!parseMpu9250Raw ("1 2 3 4 5 6 7 8 9 10 11 12", smp));

	printf ("ok\n");
	return 0;
}
