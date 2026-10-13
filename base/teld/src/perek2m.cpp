/*
 * Link to the Perek 2m telescope control system (Jan Fuchs' telescoped).
 * Copyright (C) 2026 Martin Jelinek
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

#include "perek2m.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <sys/time.h>

using namespace perek2m;

// state names as in Fuky's Observe client; raw r is entry r
static const char *oilStates[] = { "OFF", "START1", "START2", "START3", "ON", "OFF_DELAY" };
static const char *telStates[] = { "INIT", "OFF", "OFF_WAIT", "STOP", "TRACK", "OFF_REQ",
	"SS_CLU1", "SS_SLEW", "SS_DECC2", "SS_CLU2", "SS_DECC3", "SS_CLU3",
	"ST_DECC1", "ST_CLU1", "ST_SLEW", "ST_DECC2", "ST_CLU2", "ST_DECC3", "ST_CLU3" };
static const char *axisStates[] = { "STOP", "POSITION", "CA_CLU1", "CA_FAST", "CA_FASTBR", "CA_CLU2",
	"CA_SLOW", "MO_BR", "MO_CLU1", "MO_FAST", "MO_FASTBR", "MO_CLU2", "MO_SLOW", "MO_SLOWEST",
	"CENM_SLOWBR", "CENM_CLU3", "CENM_CEN", "CENM_BR", "CENM_CLU4",
	"CENA_SLOWBR", "CENA_CLU3", "CENA_CEN", "CENA_BR", "CENA_CLU4" };
static const char *focusStates[] = { "OFF", "STOP", "PLUS", "MINUS", "SLEW", "CAL1", "CAL2" };
static const char *domeStates[] = { "OFF", "STOP", "PLUS", "MINUS", "SLEW_PLUS", "SLEW_MINUS",
	"AUTO_STOP", "AUTO_PLUS", "AUTO_MINUS", "CALIBRATION" };
static const char *coverStates[] = { "UNDEF", "OPENING", "CLOSING", "OPEN", "CLOSE" };

#define NSTATES(a)  ((int) (sizeof (a) / sizeof (a[0])))

static void fieldTable (int field, const char **&names, int &n)
{
	switch (field)
	{
		case GLST_OIL: names = oilStates; n = NSTATES (oilStates); break;
		case GLST_TEL: names = telStates; n = NSTATES (telStates); break;
		// HA has no centring states, but the table is shared
		case GLST_HA: names = axisStates; n = 14; break;
		case GLST_DA: names = axisStates; n = NSTATES (axisStates); break;
		case GLST_FOCUS: names = focusStates; n = NSTATES (focusStates); break;
		case GLST_DOME: names = domeStates; n = NSTATES (domeStates); break;
		case GLST_SLIT:
		case GLST_TUBE_FLAP:
		case GLST_MIRROR_FLAP: names = coverStates; n = NSTATES (coverStates); break;
		default: names = nullptr; n = 0;
	}
}

const char *perek2m::glstName (int field, int raw)
{
	const char **names;
	int n;
	fieldTable (field, names, n);
	if (names == nullptr || raw < 0 || raw >= n)
		return "UNKNOWN";
	return names[raw];
}

int perek2m::glstStates (int field)
{
	const char **names;
	int n;
	fieldTable (field, names, n);
	return n;
}

TcsInfo::TcsInfo ()
{
	valid = false;
	fetched = 0;
	for (int i = 0; i < GLST_FIELDS; i++)
		glst[i] = -1;
	ra = dec = tarRa = tarDec = haAxis = decAxis = NAN;
	guideRa = guideDec = userRa = userDec = NAN;
	side = tarSide = -1;
	domeAz = focus = NAN;
	model = -1;
}

// "HHMMSS.ss" or "DDMMSS.ss" with an optional sign: first two digit pairs
// are the big units, the rest seconds
static double parseSexa (const std::string &in)
{
	std::string s = in;
	size_t b = s.find_first_not_of (" \t");
	if (b == std::string::npos)
		return NAN;
	s.erase (0, b);
	double sign = 1;
	if (s[0] == '-' || s[0] == '+')
	{
		if (s[0] == '-')
			sign = -1;
		s.erase (0, 1);
	}
	size_t dot = s.find ('.');
	size_t intLen = (dot == std::string::npos) ? s.size () : dot;
	if (intLen < 5 || intLen > 7)
		return NAN;
	for (size_t i = 0; i < intLen; i++)
		if (!isdigit ((unsigned char) s[i]))
			return NAN;
	// the big unit takes whatever is left of the last four digits
	size_t bigLen = intLen - 4;
	int big = atoi (s.substr (0, bigLen).c_str ());
	int min = atoi (s.substr (bigLen, 2).c_str ());
	char *end;
	double sec = strtod (s.c_str () + bigLen + 2, &end);
	// a TCS rounding 59.995 up may print 60.00
	if (min >= 60 || sec >= 61)
		return NAN;
	return sign * (big + min / 60.0 + sec / 3600.0);
}

double perek2m::parseHms (const std::string &s)
{
	double h = parseSexa (s);
	return std::isnan (h) ? NAN : h * 15.0;
}

double perek2m::parseDms (const std::string &s)
{
	return parseSexa (s);
}

static std::string formatSexa (double v, int bigWidth, bool sign)
{
	bool neg = v < 0;
	v = fabs (v);
	// round to 0.01 s first, so 59.999 carries
	long hundredths = lround (v * 360000.0);
	long big = hundredths / 360000;
	long min = (hundredths / 6000) % 60;
	double sec = (hundredths % 6000) / 100.0;
	char buf[40];
	snprintf (buf, sizeof (buf), "%s%0*ld%02ld%05.2f", (sign && neg) ? "-" : "", bigWidth, big, min, sec);
	return buf;
}

std::string perek2m::formatRa (double ra)
{
	ra = fmod (ra, 360.0);
	if (ra < 0)
		ra += 360.0;
	std::string r = formatSexa (ra / 15.0, 2, false);
	// 23:59:59.995 rounds to 24h
	if (r.compare (0, 2, "24") == 0)
		r = "000000.00";
	return r;
}

std::string perek2m::formatDec (double dec)
{
	return formatSexa (dec, 2, true);
}

static double toDouble (const std::string &s)
{
	char *end;
	double v = strtod (s.c_str (), &end);
	return end == s.c_str () ? NAN : v;
}

static std::vector <std::string> words (const std::string &s)
{
	std::vector <std::string> w;
	std::istringstream is (s);
	std::string x;
	while (is >> x)
		w.push_back (x);
	return w;
}

static std::string member (const rts2core::XmlRpcValue &v, const char *name)
{
	return v.has (name) ? v[name].asString () : std::string ();
}

void perek2m::parseInfo (const rts2core::XmlRpcValue &v, TcsInfo &info)
{
	if (v.getType () != rts2core::XmlRpcValue::STRUCT || !v.has ("glst"))
		throw rts2core::XmlRpcError ("telescope_info () reply without glst");

	TcsInfo n;

	std::vector <std::string> g = words (member (v, "glst"));
	if (g.size () < GLST_FIELDS)
		throw rts2core::XmlRpcError ("glst has " + std::to_string (g.size ()) + " fields, expected " + std::to_string (GLST_FIELDS));
	for (int i = 0; i < GLST_FIELDS; i++)
		n.glst[i] = atoi (g[i].c_str ());

	n.ut = member (v, "ut");

	std::vector <std::string> w = words (member (v, "trrd"));
	if (w.size () >= 2)
	{
		n.ra = parseHms (w[0]);
		n.dec = parseDms (w[1]);
	}
	if (w.size () >= 3)
		n.side = atoi (w[2].c_str ());

	w = words (member (v, "tsra"));
	if (w.size () >= 2)
	{
		n.tarRa = parseHms (w[0]);
		n.tarDec = parseDms (w[1]);
	}
	if (w.size () >= 3)
		n.tarSide = atoi (w[2].c_str ());

	w = words (member (v, "trhd"));
	if (w.size () >= 2)
	{
		n.haAxis = toDouble (w[0]);
		n.decAxis = toDouble (w[1]);
	}

	w = words (member (v, "trgv"));
	if (w.size () >= 2)
	{
		n.guideRa = toDouble (w[0]);
		n.guideDec = toDouble (w[1]);
	}

	w = words (member (v, "trus"));
	if (w.size () >= 2)
	{
		n.userRa = toDouble (w[0]);
		n.userDec = toDouble (w[1]);
	}

	n.domeAz = toDouble (member (v, "dopo"));
	n.focus = toDouble (member (v, "fopo"));
	std::string m = member (v, "trcs");
	n.model = m.empty () ? -1 : atoi (m.c_str ());
	n.object = member (v, "object");

	struct timeval tv;
	gettimeofday (&tv, nullptr);
	n.fetched = tv.tv_sec + tv.tv_usec / 1e6;
	n.valid = true;

	info = n;
}

Tcs::Tcs ()
{
	tcsHost = "192.168.193.195";
	tcsPort = 9999;
	pollInterval = 1.0;
	timeout = 5.0;
	running = false;
	connected = false;
}

Tcs::~Tcs ()
{
	stop ();
}

void Tcs::start ()
{
	if (running)
		return;
	running = true;
	worker = std::thread (&Tcs::threadMain, this);
}

void Tcs::stop ()
{
	running = false;
	if (worker.joinable ())
		worker.join ();
}

bool Tcs::getInfo (TcsInfo &out)
{
	std::lock_guard <std::mutex> g (lock);
	out = snapshot;
	return connected;
}

std::string Tcs::getLastError ()
{
	std::lock_guard <std::mutex> g (lock);
	return lastError;
}

void Tcs::threadMain ()
{
	// the log stream is not thread safe: this thread only records the
	// outcome, the daemons report it from their own loop
	rts2core::XmlRpcClient client (tcsHost, tcsPort, timeout);

	while (running)
	{
		auto started = std::chrono::steady_clock::now ();
		try
		{
			TcsInfo n;
			parseInfo (client.call ("telescope_info"), n);
			{
				std::lock_guard <std::mutex> g (lock);
				snapshot = n;
			}
			connected = true;
		}
		catch (rts2core::XmlRpcError &er)
		{
			std::lock_guard <std::mutex> g (lock);
			lastError = er.what ();
			connected = false;
		}

		// sleep in short steps, so stop () does not wait a whole interval
		auto until = started + std::chrono::milliseconds ((long) (pollInterval * 1000));
		while (running && std::chrono::steady_clock::now () < until)
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
	}
}

std::string Tcs::execute (const std::string &cmd)
{
	rts2core::XmlRpcClient client (tcsHost, tcsPort, timeout);
	std::vector <rts2core::XmlRpcValue> params;
	params.push_back (rts2core::XmlRpcValue (cmd));
	return client.call ("telescope_execute", params).asString ();
}

int Tcs::command (const std::string &cmd, std::string &err)
{
	try
	{
		std::string ret = execute (cmd);
		// tolerate the padding the daemons put around values
		size_t b = ret.find_first_not_of (" \t\r\n");
		size_t e = ret.find_last_not_of (" \t\r\n");
		ret = (b == std::string::npos) ? "" : ret.substr (b, e - b + 1);
		if (ret == "1")
			return 0;
		err = cmd + " refused (answer '" + ret + "')";
	}
	catch (rts2core::XmlRpcError &er)
	{
		err = cmd + " failed: " + er.what ();
	}
	return -1;
}

int Tcs::setCoordinates (double ra, double dec, int side, const std::string &object, std::string &err)
{
	try
	{
		rts2core::XmlRpcValue p = rts2core::XmlRpcValue::structure ();
		p.set ("ra", rts2core::XmlRpcValue (formatRa (ra)));
		p.set ("dec", rts2core::XmlRpcValue (formatDec (dec)));
		p.set ("position", rts2core::XmlRpcValue (side));
		p.set ("object", rts2core::XmlRpcValue (object));
		std::vector <rts2core::XmlRpcValue> params;
		params.push_back (p);
		rts2core::XmlRpcClient client (tcsHost, tcsPort, timeout);
		rts2core::XmlRpcValue r = client.call ("telescope_set_coordinates", params);
		if (r.asString () == "1" || (r.getType () == rts2core::XmlRpcValue::BOOL && r.asBool ()))
			return 0;
		err = "telescope_set_coordinates refused (answer '" + r.asString () + "')";
	}
	catch (rts2core::XmlRpcError &er)
	{
		err = std::string ("telescope_set_coordinates failed: ") + er.what ();
	}
	return -1;
}
