#include "configuration.h"
#include "iniparser.h"
#include "objectcheck.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main ()
{
	// write a small ini file and read it back
	char path[] = "/tmp/base_test_config_XXXXXX";
	int fd = mkstemp (path);
	assert (fd >= 0);
	FILE *f = fdopen (fd, "w");
	fprintf (f, "[observatory]\n"
		"longitude = 14.78\n"
		"latitude = 49.9\n"
		"altitude = 530\n"
		"sexadecimals = y\n"
		"[imgproc]\n"
		"astrometry_timeout = 120\n");
	fclose (f);

	rts2core::Configuration conf;
	int ret = conf.loadFile (path);
	assert (ret == 0);
	assert (conf.getObserver ()->lng == 14.78);
	assert (conf.getObserver ()->lat == 49.9);
	assert (conf.getObservatoryAltitude () == 530);
	assert (conf.getStoreSexadecimals () == true);
	assert (conf.getAstrometryTimeout () == 120);

	unlink (path);

	// ObjectCheck with no horizon file should always say "good"
	ObjectCheck checker;
	checker.loadHorizon ("-");
	struct ln_hrz_posn hrz;
	hrz.alt = 45;
	hrz.az = 180;
	assert (checker.is_good (&hrz) != 0);

	return 0;
}
