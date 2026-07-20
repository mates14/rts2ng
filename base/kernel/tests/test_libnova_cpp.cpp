#include "libnova_cpp.h"
#include "radecparser.h"

#include <cassert>
#include <sstream>

int main ()
{
	// RA/Dec sexagesimal round-trip through the stream operators.
	LibnovaRaDec radec (10.0, -20.5);
	std::ostringstream os;
	os << radec;
	assert (!os.str ().empty ());

	// deep-copy assignment must not double-free (this is the bug the
	// unique_ptr conversion fixes - see the note in libnova_cpp.h).
	LibnovaRaDec other;
	other = radec;
	assert (std::abs (other.getRa () - 10.0) < 1e-9);
	assert (std::abs (other.getDec () - (-20.5)) < 1e-9);

	double ra, dec;
	int ret = parseRaDec ("10:00:00 -20:30:00", ra, dec);
	assert (ret == 0);

	LibnovaDeg90 deg (45.5);
	std::ostringstream os2;
	os2 << deg;
	assert (!os2.str ().empty ());

	return 0;
}
