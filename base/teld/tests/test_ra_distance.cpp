// rts2teld::raDistanceDeg() - the shortest-distance-between-two-RAs helper.
//
// This test exists because of a specific live failure at SBT on 2026-09-14:
// a slew arrived dead on target and was reported as "move stopped 360.000
// deg from target", which tripped the safety watchdog, parked the mount and
// marked its position LOST. The cause was `fabs (ln_range_degrees (a - b))`
// used as a distance - it is not one, because ln_range_degrees() normalizes
// into 0..360 and fabs() cannot pull a ~360 back down to ~0. The cases
// below are mostly about the wrap; the first two are the exact shape of
// that failure.

#include "geminicaringloop.h"

#include <cassert>
#include <cmath>

using rts2teld::raDistanceDeg;

static bool near (double a, double b, double tol = 1e-9)
{
	return fabs (a - b) < tol;
}

int main ()
{
	// the live failure: mount a hair BELOW the target RA. The broken
	// idiom returned ~360 for this; it is 2e-5 deg away.
	assert (near (raDistanceDeg (164.99998, 165.0), 0.00002, 1e-12));
	// and a hair above, which the broken idiom got right - hence the
	// coin-flip behaviour, one slew in two
	assert (near (raDistanceDeg (165.00002, 165.0), 0.00002, 1e-12));

	// symmetric
	assert (near (raDistanceDeg (10.0, 350.0), raDistanceDeg (350.0, 10.0)));

	// across 0h, the case a naive subtraction gets wrong
	assert (near (raDistanceDeg (359.0, 1.0), 2.0));
	assert (near (raDistanceDeg (1.0, 359.0), 2.0));

	// identical, and a full turn apart, are both zero distance
	assert (near (raDistanceDeg (123.456, 123.456), 0.0));
	assert (near (raDistanceDeg (123.456, 123.456 + 360.0), 0.0));
	assert (near (raDistanceDeg (123.456, 123.456 - 720.0), 0.0));

	// never exceeds half a turn, whichever way round
	assert (near (raDistanceDeg (0.0, 180.0), 180.0));
	assert (near (raDistanceDeg (0.0, 181.0), 179.0));
	assert (near (raDistanceDeg (181.0, 0.0), 179.0));
	assert (near (raDistanceDeg (0.0, 270.0), 90.0));

	// unnormalized inputs
	assert (near (raDistanceDeg (-5.0, 5.0), 10.0));
	assert (near (raDistanceDeg (365.0, 5.0), 0.0));

	for (double a = -720.0; a <= 720.0; a += 7.3)
	{
		for (double b = -720.0; b <= 720.0; b += 11.7)
		{
			double d = raDistanceDeg (a, b);
			assert (d >= 0.0 && d <= 180.0 + 1e-9);
			assert (near (d, raDistanceDeg (b, a), 1e-9));
		}
	}

	// NaN propagates rather than reading as "arrived"
	assert (std::isnan (raDistanceDeg (NAN, 10.0)));
	assert (std::isnan (raDistanceDeg (10.0, NAN)));

	return 0;
}
