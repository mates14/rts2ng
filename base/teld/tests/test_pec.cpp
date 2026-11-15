// D50 PEC: the worm model file, the blend between the sides, and the step
// frequency factor against the error it has to cancel. [JS 2026-11-14]

#include "pec.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace rts2teld;

static std::string writeFile (const char *text)
{
	char name[] = "/tmp/test_pec_XXXXXX";
	int fd = mkstemp (name);
	assert (fd >= 0);
	close (fd);
	std::ofstream (name) << text;
	return name;
}

static void testLoad ()
{
	PecModel m;
	std::string err;
	assert (!m.load ("/nonexistent/pec.txt", err) && m.empty ());
	std::string f = writeFile ("# comment\neast 1 1.0 0.5\n\nwest 1 -1.0 0.2  # after\nwest 3 0.1 0.0\n");
	assert (m.load (f, err) && !m.empty ());
	// east alone: 1.0 cos + 0.5 sin at phi 0 and pi/2
	assert (fabs (m.error (0, 0) - 1.0) < 1e-12);
	assert (fabs (m.error (M_PI / 2, 0) - 0.5) < 1e-12);
	// west: -1.0 + 0.1 at phi 0
	assert (fabs (m.error (0, 1) + 0.9) < 1e-12);
	unlink (f.c_str ());
	// a bad line or one side missing leaves the model empty
	for (const char *bad : {"east 1 1.0\nwest 1 1 1\n", "north 1 1 1\nwest 1 1 1\n", "east 0 1 1\nwest 1 1 1\n", "east 1 1 1 9\nwest 1 1 1\n", "east 1 1 1\n"})
	{
		f = writeFile (bad);
		assert (!m.load (f, err) && m.empty () && !err.empty ());
		unlink (f.c_str ());
	}
}

static void testWeight ()
{
	assert (PecModel::westWeight (-1.0, -0.3, 0.6) == 0);
	assert (PecModel::westWeight (-0.3, -0.3, 0.6) == 0);
	assert (fabs (PecModel::westWeight (0.15, -0.3, 0.6) - 0.5) < 1e-12);
	assert (PecModel::westWeight (0.6, -0.3, 0.6) == 1);
	assert (PecModel::westWeight (5.0, -0.3, 0.6) == 1);
	// a degenerate range switches at the one HA
	assert (PecModel::westWeight (0.1, 0.0, 0.0) == 1);
}

static void testFactor ()
{
	// the 2026-11-13 GRB west shape, h1-h3 (amplitude/phase -> c, s)
	std::string f = writeFile ("east 1 -0.80 -1.45\neast 2 -0.05 0.26\nwest 1 -1.65 1.30\nwest 2 0.26 0.04\nwest 3 0.29 -0.32\n");
	PecModel m;
	std::string err;
	assert (m.load (f, err));
	unlink (f.c_str ());
	// f - 1 = -2 pi / 2700 * dp/dphi: check against a numerical derivative,
	// and that a steady worm then leaves the sky still: the extra step motion
	// over a turn, in arcsec of HA, cancels p
	for (double w : {0.0, 0.4, 1.0})
	{
		for (double phi = 0; phi < 2 * M_PI; phi += 0.1)
		{
			double d = 1e-6;
			double dp = (m.error (phi + d, w) - m.error (phi - d, w)) / (2 * d);
			assert (fabs (m.factor (phi, w) - (1 - 2 * M_PI / PEC_HA_PER_TURN * dp)) < 1e-9);
		}
		// integrate the extra motion over the turn: (f - 1) in arcsec of HA
		// per radian of worm is 2700 / 2 pi; the sum is -(p (phi) - p (0))
		double extra = 0, step = 2 * M_PI / 100000;
		for (double phi = 0; phi < 2.0; phi += step)
			extra += (m.factor (phi + step / 2, w) - 1) * PEC_HA_PER_TURN / (2 * M_PI) * step;
		assert (fabs (extra + (m.error (2.0, w) - m.error (0, w))) < 1e-3);
	}
	// the real model stays well inside the limit
	for (double phi = 0; phi < 2 * M_PI; phi += 0.01)
		assert (fabs (m.factor (phi, 1) - 1) < 0.01);
	// a model that asks for more than 5 % is refused
	f = writeFile ("east 1 0 0\nwest 8 40 0\n");
	assert (m.load (f, err));
	unlink (f.c_str ());
	bool refused = false;
	for (double phi = 0; phi < 2 * M_PI; phi += 0.01)
		refused |= std::isnan (m.factor (phi, 1));
	assert (refused && !std::isnan (m.factor (0.3, 0)));
}

// the motor terms: optional, the same on both sides, and in the factor 4.8x
// as steep as a worm term, since theta turns 4.8 times per worm turn
static void testMotor ()
{
	PecModel m;
	std::string err;
	std::string f = writeFile ("east 1 -0.80 -1.45\nwest 1 -1.65 1.30\nmotor 1 0.37 0.12\nmotor 2 -0.21 0\n");
	assert (m.load (f, err) && m.hasMotor ());
	unlink (f.c_str ());
	// theta NAN: the worm terms alone
	assert (fabs (m.error (0.7, 1) - m.error (0.7, 1, NAN)) < 1e-12);
	assert (fabs (m.error (0.7, 1, 0) - m.error (0.7, 1) - (0.37 - 0.21)) < 1e-12);
	// along the drive, theta = theta0 + 4.8 phi: the factor against a numerical
	// derivative of the whole error, and the extra motion cancelling it
	for (double w : {0.0, 1.0})
	{
		double th0 = 1.3;
		auto p = [&] (double phi) { return m.error (phi, w, th0 + PEC_MOTOR_PER_TURN * phi); };
		for (double phi = 0; phi < 2 * M_PI; phi += 0.05)
		{
			double d = 1e-6;
			double dp = (p (phi + d) - p (phi - d)) / (2 * d);
			assert (fabs (m.factor (phi, w, th0 + PEC_MOTOR_PER_TURN * phi) - (1 - 2 * M_PI / PEC_HA_PER_TURN * dp)) < 1e-9);
		}
		double extra = 0, step = 2 * M_PI / 100000;
		for (double phi = 0; phi < 2.0; phi += step)
			extra += (m.factor (phi + step / 2, w, th0 + PEC_MOTOR_PER_TURN * (phi + step / 2)) - 1) * PEC_HA_PER_TURN / (2 * M_PI) * step;
		assert (fabs (extra + (p (2.0) - p (0))) < 1e-3);
	}
	// the motor angle of a servo position, negative counts included
	assert (fabs (PecModel::motorAngle (0)) < 1e-12);
	assert (fabs (PecModel::motorAngle (PEC_MOTOR_COUNTS / 4) - M_PI / 2) < 1e-12);
	assert (fabs (PecModel::motorAngle (-PEC_MOTOR_COUNTS / 4) - 3 * M_PI / 2) < 1e-12);
	assert (fabs (PecModel::motorAngle (-6658945.0 - 102 * PEC_MOTOR_COUNTS) - PecModel::motorAngle (-6658945.0)) < 1e-9);
	// a model without motor lines has none
	f = writeFile ("east 1 1 0\nwest 1 1 0\n");
	assert (m.load (f, err) && !m.hasMotor ());
	assert (m.factor (0.5, 1, 2.0) == m.factor (0.5, 1));
	unlink (f.c_str ());
}

// the model in the repository, as T0 reads it
static void testRepoModel ()
{
	PecModel m;
	std::string err;
	assert (m.load (PEC_MODEL_FILE, err));
	double worst = 0;
	for (double w : {0.0, 0.5, 1.0})
		for (double phi = 0; phi < 2 * M_PI; phi += 0.001)
			for (double th : {(double) NAN, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0})
			{
				double f = m.factor (phi, w, th);
				assert (!std::isnan (f));
				worst = fmax (worst, fabs (f - 1));
			}
	printf ("test_pec: %s needs up to %.2f %% of the step frequency\n", PEC_MODEL_FILE, 100 * worst);
}

int main ()
{
	testLoad ();
	testRepoModel ();
	testWeight ();
	testFactor ();
	testMotor ();
	printf ("test_pec: OK\n");
	return 0;
}
