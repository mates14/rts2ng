// D50 worm encoder: the line parser on real lines of the board, the rate fit,
// and the reader end to end on a pty posing as the board. [JS 2026-11-08]

#include "wormencoder.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <pty.h>
#include <string>
#include <unistd.h>

using namespace rts2teld;

static void testParse ()
{
	WormReading r;
	// lines as the board sent them on 2026-11-08, mount at rest
	assert (parseWormLine ("M:0577 S:0F0166 E:1 W:1 C:01 S:01", r) == WORM_OK);
	assert (r.turns == 0x577 && r.pos == 0x0F0166);
	assert (parseWormLine ("M:0577 S:0F0163 E:1 W:1 C:3D S:3D", r) == WORM_OK);
	assert (r.pos == 0x0F0163);
	// CRC from the encoder and the board's own differ
	assert (parseWormLine ("M:0577 S:0F0163 E:1 W:1 C:3D S:3C", r) == WORM_BAD_CRC);
	// error and warning flags are negated: 0 is the problem
	assert (parseWormLine ("M:0577 S:0F0163 E:0 W:1 C:3D S:3D", r) == WORM_BAD_FLAGS);
	assert (parseWormLine ("M:0577 S:0F0163 E:1 W:0 C:3D S:3D", r) == WORM_BAD_FLAGS);
	// the partial first line after opening, garbage, and a position beyond 23 bits
	assert (parseWormLine ("S:0F0165 E:1 W:1 C:15 S:15", r) == WORM_BAD_LINE);
	assert (parseWormLine ("M:0577 S:0F01", r) == WORM_BAD_LINE);
	assert (parseWormLine ("M:0577 S:800000 E:1 W:1 C:15 S:15", r) == WORM_BAD_LINE);
	assert (parseWormLine ("M:0577 S:0F0163 E:1 W:1 C:3D S:3D x", r) == WORM_BAD_LINE);
	assert (parseWormLine ("", r) == WORM_BAD_LINE);
}

static void testRate ()
{
	std::vector<std::pair<double, int64_t>> pts;
	assert (std::isnan (wormRate (pts)));
	// a worm turning once in 179.51 s, around 2^39 (turn counter near its top)
	double rate = WORM_COUNTS / 179.51;
	int64_t base = ((int64_t) 0xfff0) * WORM_COUNTS;
	for (int i = 0; i < 300; i++)
		pts.push_back (std::make_pair (1.8e9 + i * 0.1, base + (int64_t) llround (i * 0.1 * rate) + (i % 7) - 3));
	assert (fabs (wormRate (pts) / rate - 1) < 1e-4);
}

// the board on a pty: lines from turn 0xffff, position near the end of the
// turn, running forward over the turn and the 16-bit counter wrap
static void testReader ()
{
	int master, slave;
	char name[100];
	assert (openpty (&master, &slave, name, nullptr, nullptr) == 0);

	char dir[] = "/tmp/test_wormencoder.XXXXXX";
	assert (mkdtemp (dir));

	WormEncoder enc (name, dir);
	enc.start ();
	usleep (300000);

	unsigned m = 0xffff, s = WORM_COUNTS - 20000;
	auto line = [&] (const char *fmt, unsigned a, unsigned b, int e, int w, unsigned c, unsigned cs)
	{
		char buf[100];
		int n = snprintf (buf, sizeof (buf), fmt, a, b, e, w, c, cs);
		assert (write (master, buf, n) == n);
	};
	// a partial line - 0.3 s after opening, so a garbled line, not the
	// flushed buffer's tail
	line ("S:%06X E:%d W:%d C:%02X S:%02X\n", s, 1, 1, 0x15, 0x15, 0);
	for (int i = 0; i < 400; i++)
	{
		line ("M:%04X S:%06X E:%d W:%d C:%02X S:%02X\n", m, s, 1, 1, 0x19, 0x19);
		s += 100;
		if (s >= WORM_COUNTS)
		{
			s -= WORM_COUNTS;
			m = (m + 1) & 0xffff;
		}
		usleep (1000);
	}
	line ("M:%04X S:%06X E:%d W:%d C:%02X S:%02X\n", m, s, 1, 1, 0x19, 0x18);	// bad CRC
	line ("M:%04X S:%06X E:%d W:%d C:%02X S:%02X\n", m, s, 0, 1, 0x19, 0x19);	// error flag
	usleep (100000);

	WormStatus st = enc.getStatus ();
	assert (st.open && st.valid);
	assert (st.readings == 400);
	assert (st.badCrc == 1 && st.badFlags == 1 && st.badLines == 1);
	assert (st.turns == 0 && st.pos == s - 100);
	assert (fabs (st.angle - (s - 100) * 360.0 / WORM_COUNTS) < 1e-9);

	enc.logEvent ("P 250 1");
	usleep (400000);
	// silent board: no longer valid
	st = enc.getStatus ();
	assert (!st.valid);
	enc.stop ();

	// the log: a W line (the first reading) and the event, with the position
	char lname[40];
	time_t now = time (nullptr);
	struct tm tm;
	gmtime_r (&now, &tm);
	strftime (lname, sizeof (lname), "/worm-%Y%m%d.log", &tm);
	std::ifstream f (std::string (dir) + lname);
	std::string l, all;
	bool sawW = false, sawP = false;
	while (std::getline (f, l))
	{
		all += l + "\n";
		if (l.find (" W ") != std::string::npos)
			sawW = true;
		// logged after the board went quiet: no position
		if (l.find (" P 250 1 ") != std::string::npos)
			sawP = true;
	}
	if (!sawW || !sawP)
		fprintf (stderr, "log:\n%s", all.c_str ());
	assert (sawW && sawP);

	std::string rm = std::string ("rm -r ") + dir;
	assert (system (rm.c_str ()) == 0);
	close (master);
	close (slave);
}

int main ()
{
	testParse ();
	testRate ();
	testReader ();
	printf ("wormencoder: OK\n");
	return 0;
}
