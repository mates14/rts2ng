/*
 * Caring-loop transport for the Losmandy Gemini-2 UDP protocol.
 * Copyright (C) 2026 Martin Jelinek <mates@iaa.es>
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

#include "geminicaringloop.h"
#include "hms.h"

#include <libnova/libnova.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace rts2teld;

namespace
{
	constexpr double MOVE_STABLE_DEG = 0.01;
	constexpr double MOVE_MIN_SETTLE_SEC = 3.0;
	// forces a stop-and-check past this even if nobody called requestAbort().
	// Not a slew duration estimate: Gemini sequences the axes of some moves
	// (on SBT, a meridian flip swung Dec over the pole for 30 s before the
	// RA axis started - 2026-09-14), so a long flip can take minutes.
	constexpr double MOVE_MAX_SEC = 300.0;
	constexpr double ARRIVAL_TOLERANCE_DEG = 0.5;	// generous vs. the ~0.01-0.02 deg errors seen on real successful slews - catches "stopped nowhere near the target", not normal settling wobble
	constexpr double COMMAND_TIMEOUT_SEC = 1.0;
	constexpr int RESYNC_ATTEMPTS = 5;

	// one extra handshake + tracking-rate read every this many normal poll
	// cycles (so ~15s at the default 1s poll interval). Cheap insurance
	// against the mount rebooting under a running driver - which is what
	// the whole startup-handshake machinery exists to survive - without
	// doubling the routine datagram rate.
	constexpr int SLOW_POLL_EVERY = 15;

	// how many consecutive polls have to show the mount moving away from
	// its target before GeminiStatus::moveWrongWay is raised
	constexpr int WRONG_WAY_POLLS = 3;

	// cap on bR#/bW#/bC# selections per boot: if the mount is still sitting
	// in its boot menu after this many, something is wrong with the
	// selection itself and repeating it forever helps nobody - leave the
	// state visible instead and let the driver surface it.
	constexpr int MAX_STARTUP_SELECTIONS = 5;

	double nowSeconds ()
	{
		return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
	}

	double angularSeparationDeg (double ra1, double dec1, double ra2, double dec2)
	{
		if (std::isnan (ra1) || std::isnan (dec1) || std::isnan (ra2) || std::isnan (dec2))
			return NAN;
		struct ln_equ_posn a, b;
		a.ra = ra1;
		a.dec = dec1;
		b.ra = ra2;
		b.dec = dec2;
		return ln_get_angular_separation (&a, &b);
	}

	// the three boot-menu selections Gemini accepts while its handshake
	// answers 'b' - same commands as base/teld/gemini/gemini.cpp's
	// tel_gemini_reset(); nullptr for STARTUP_NONE
	const char *startupSelectionCommand (int mode)
	{
		switch (mode)
		{
			case GeminiCaringLoop::STARTUP_RESTART: return "bR#";
			case GeminiCaringLoop::STARTUP_WARM: return "bW#";
			case GeminiCaringLoop::STARTUP_COLD: return "bC#";
			default: return nullptr;
		}
	}

	void putLE32 (uint8_t *p, uint32_t v)
	{
		p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
	}

	uint32_t getLE32 (const uint8_t *p)
	{
		return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
	}

	// same wire format as base/teld/src/tellx200.cpp's tel_write_ra/dec -
	// proven against real hardware, see STATUS.md
	bool formatTargetCommands (double raDeg, double decDeg, std::string &sr, std::string &sd)
	{
		if (std::isnan (raDeg) || std::isnan (decDeg) || raDeg < 0 || raDeg > 360.0)
			return false;

		int h, m, s;
		dtoints (raDeg / 15.0, &h, &m, &s);
		char buf[32];
		snprintf (buf, sizeof (buf), ":Sr %02d:%02d:%02d#", h, m, s);
		sr = buf;

		char sign = '+';
		double dec = decDeg;
		if (dec < 0)
		{
			sign = '-';
			dec = -dec;
		}
		struct ln_dms dh;
		ln_deg_to_dms (dec, &dh);
		snprintf (buf, sizeof (buf), ":Sd%c%02d*%02d:%02.0f#", sign, dh.degrees, dh.minutes, dh.seconds);
		sd = buf;
		return true;
	}

	// see base/teld/gemini/gemini.cpp's Gemini::tel_gemini_checksum - same
	// XOR-then-mod128-then-+64 algorithm, used for commands Gemini expects
	// to be checksummed on the way in (>ID:VAL#).
	unsigned char geminiChecksum1 (const std::string &s)
	{
		unsigned char c = 0;
		for (char ch : s)
			c ^= (unsigned char) ch;
		c %= 128;
		c += 64;
		return c;
	}

	std::string buildNativeSet (int id, const std::string &valueStr)
	{
		std::string buf = ">" + std::to_string (id) + ":" + valueStr;
		buf += (char) geminiChecksum1 (buf);
		buf += "#";
		return buf;
	}

	// same XOR, but +64-then-mod128 (operations swapped) - see
	// Gemini::tel_gemini_checksum2's comment in gemini.cpp: a firmware
	// quirk where Gemini's OWN replies are checksummed with the operations
	// in the opposite order from what it expects on commands sent to it.
	unsigned char geminiChecksum2 (const std::string &s)
	{
		unsigned char c = 0;
		for (char ch : s)
			c ^= (unsigned char) ch;
		c += 64;
		c %= 128;
		return c;
	}

	std::string buildNativeGet (int id)
	{
		std::string buf = "<" + std::to_string (id) + ":";
		buf += (char) geminiChecksum1 (buf);
		buf += "#";
		return buf;
	}

	// strips the trailing '#', then the checksum2 byte, verifying it -
	// shared by readNativeRaw() (RTS2-thread-facing, synchronous) and
	// pollTrackingLimit() (caring-thread-internal, routine polling)
	bool parseNativeGetReply (const std::string &raw, std::string &value)
	{
		std::string resp = raw;
		if (!resp.empty () && resp.back () == '#')
			resp.pop_back ();
		if (resp.empty ())
			return false;

		char checksum = resp.back ();
		std::string valueStr = resp.substr (0, resp.size () - 1);
		if ((char) geminiChecksum2 (valueStr) != checksum)
			return false;

		value = valueStr;
		return true;
	}

	// :MS#/:MM# response codes, per the official Gemini Level 5/2.1 and
	// Level 6/1.02 Serial Interface Command References (gemini-2.com) -
	// on this firmware the wire reply is just the bare digit, not the
	// English text the docs describe it as. "3" (Manual Control) is
	// treated as an accepted slew, matching
	// base/teld/gemini/gemini.cpp's Gemini::tel_start_move() precedent
	// (retstr == '3' -> return 0, not an error).
	const char *msResponseMeaning (char code)
	{
		switch (code)
		{
			case '0': return "success";
			case '1': return "object below horizon";
			case '2': return "no object selected";
			case '3': return "manual control";
			case '4': return "position unreachable";
			case '5': return "not aligned";
			case '6': return "outside limits (HA/meridian safety limit - see native params ~220-223)";
			case '7': return "mount is parked";
			default: return "undocumented response code";
		}
	}

	// Splits the 0x05 ENQ macro's composite status line and fills the
	// verified fields (see STATUS.md for how these were worked out
	// against live samples - LST = HA + RA checks out exactly, pier side
	// matches the documented :Gm# W/E format). Fields beyond what's
	// verified are kept as an opaque raw string rather than guessing
	// wrong labels on a live mount.
	bool parseEnq (const std::string &s, GeminiStatus &st)
	{
		std::vector<std::string> f;
		size_t start = 0;
		for (size_t i = 0; i <= s.size (); i++)
		{
			if (i == s.size () || s[i] == ';')
			{
				f.push_back (s.substr (start, i - start));
				start = i + 1;
			}
		}
		if (f.size () < 12)
			return false;

		try
		{
			st.praRaw = std::stol (f[0]);
			st.pdecRaw = std::stol (f[1]);
			st.ra = std::stod (f[2]) * 15.0;	// hours -> degrees
			st.dec = std::stod (f[3]);
			st.ha = std::stod (f[4]) * 15.0;	// hours -> degrees
			st.az = std::stod (f[5]);
			st.alt = std::stod (f[6]);
			st.moveRate = f[7].empty () ? '?' : f[7][0];
			st.pierSide = f[10].empty () ? '?' : f[10][0];
			st.lst = std::stod (f[11]) * 15.0;	// hours -> degrees
		}
		catch (const std::exception &)
		{
			return false;
		}

		std::string raw;
		for (size_t i = 12; i < f.size (); i++)
		{
			if (i > 12)
				raw += ';';
			raw += f[i];
		}
		st.rawExtended = raw;
		return true;
	}

	// "<a>;<b>" as native 231/235-239 reply it
	bool parseTickPair (const std::string &s, int32_t &a, int32_t &b)
	{
		long la, lb;
		if (sscanf (s.c_str (), "%ld;%ld", &la, &lb) != 2)
			return false;
		a = (int32_t) la;
		b = (int32_t) lb;
		return true;
	}

	// "<ddd>d<mm>" as native 221-223/227/228 reply it
	bool parseDegMin (const std::string &s, double &deg)
	{
		int d, m;
		if (sscanf (s.c_str (), "%d%*[dD:]%d", &d, &m) != 2)
			return false;
		deg = d + m / 60.0;
		return true;
	}
}

// See the header, and ~/tmp/gemini/FLIP_LOGIC.md for the firmware routine
// this mirrors. The firmware computes the target's RA axis position as
// (RA - reference) mod full circle, where the reference follows sidereal
// time and jumps by 12h whenever the Dec axis crosses its half circle - so
// relative to where the RA axis is now, a target dRA further east sits dRA
// further along the axis, on the side the Dec axis is currently on.
GeminiSidePrediction rts2teld::predictGotoSide (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, double curRaDeg, double targetRaDeg, bool preferOther)
{
	GeminiSidePrediction p;
	if (!geo.valid)
	{
		p.reason = "mount geometry (native 238/231/223) not read";
		return p;
	}
	if (std::isnan (curRaDeg) || std::isnan (targetRaDeg))
	{
		p.reason = "no current or target RA";
		return p;
	}

	double full = 2.0 * geo.raHalf;
	double dRa = ln_range_degrees (targetRaDeg - curRaDeg);
	if (dRa > 180.0)
		dRa -= 360.0;

	double here = fmod (raTicks + dRa * geo.ticksPerDeg (), full);
	if (here < 0)
		here += full;
	double there = here < geo.raHalf ? here + geo.raHalf : here - geo.raHalf;

	auto marginDeg = [&geo] (double ticks)
	{
		double m = ticks - geo.windowLow ();
		if (geo.windowHigh () - ticks < m)
			m = geo.windowHigh () - ticks;
		return m / geo.ticksPerDeg ();
	};

	p.sideBefore = decTicks >= geo.decHalf ? 'E' : 'W';
	char otherSide = p.sideBefore == 'E' ? 'W' : 'E';

	p.firstMarginDeg = marginDeg (preferOther ? there : here);
	p.secondMarginDeg = marginDeg (preferOther ? here : there);
	char firstSide = preferOther ? otherSide : p.sideBefore;
	char secondSide = preferOther ? p.sideBefore : otherSide;

	if (p.firstMarginDeg > 0)
		p.sideAfter = firstSide;
	else if (p.secondMarginDeg > 0)
		p.sideAfter = secondSide;
	else
	{
		p.outcome = GeminiSidePrediction::REFUSE;
		p.sideAfter = p.sideBefore;
		return p;
	}
	p.outcome = p.sideAfter == p.sideBefore ? GeminiSidePrediction::STAY : GeminiSidePrediction::FLIP;
	return p;
}

GeminiLimitDecision rts2teld::decideTrackingLimit (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, double curRaDeg,
	double secToLimit, double marginDeg, double earliestSec)
{
	if (std::isnan (secToLimit) || secToLimit >= earliestSec)
		return LIMIT_WAIT;
	if (!geo.valid)
		return LIMIT_FLIP;	// no geometry: try the guarded :MM#, which falls back to a park

	// the other side's goto window and the tracking side's hard limit
	// overlap by (east + west - 180) deg of hour angle
	double overlapDeg = (geo.eastLimit - geo.westLimit) / geo.ticksPerDeg () - 180.0;
	if (overlapDeg <= marginDeg)
		return LIMIT_PARK;

	GeminiSidePrediction p = predictGotoSide (geo, raTicks, decTicks, curRaDeg, curRaDeg, true);
	if (p.outcome == GeminiSidePrediction::FLIP && !p.ambiguous (marginDeg))
		return LIMIT_FLIP;
	if (secToLimit < 30.0)
		return LIMIT_PARK;
	return LIMIT_WAIT;
}

GeminiCounterError rts2teld::computeCounterError (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, char decSide,
	double lstDeg, double mountRaDeg, double mountDecDeg)
{
	GeminiCounterError e;
	double k = geo.ticksPerDeg ();
	double kd = geo.decHalf / 180.0;
	double ha = ln_range_degrees (lstDeg - mountRaDeg);
	double raWant = ln_range_degrees ((decSide == 'E' ? 270.0 : 90.0) - ha) * k;
	double decWant = (decSide == 'E' ? 270.0 - mountDecDeg : mountDecDeg + 90.0) * kd;

	e.raTicks = raWant - raTicks;
	if (e.raTicks > geo.raHalf)
		e.raTicks -= 2.0 * geo.raHalf;
	if (e.raTicks < -geo.raHalf)
		e.raTicks += 2.0 * geo.raHalf;
	e.decTicks = decWant - decTicks;
	e.raDeg = e.raTicks / k;
	e.decDeg = e.decTicks / kd;
	e.rezeroRa = (int32_t) lround (geo.raHalf - e.raTicks);
	e.rezeroDec = (int32_t) lround (geo.decHalf - e.decTicks);
	return e;
}

bool GeminiSidePrediction::ambiguous (double threshold) const
{
	if (outcome == UNKNOWN)
		return true;
	if (fabs (firstMarginDeg) < threshold)
		return true;
	// the second candidate only matters when the first one is out
	return firstMarginDeg <= 0 && fabs (secondMarginDeg) < threshold;
}

double GeminiSidePrediction::marginDeg () const
{
	return firstMarginDeg > 0 ? firstMarginDeg : secondMarginDeg;
}

std::string GeminiSidePrediction::describe () const
{
	char buf[160];
	switch (outcome)
	{
		case STAY:
			snprintf (buf, sizeof (buf), "STAY %c (window margins %.2f / %.2f deg)", sideAfter, firstMarginDeg, secondMarginDeg);
			break;
		case FLIP:
			snprintf (buf, sizeof (buf), "FLIP %c->%c (window margins %.2f / %.2f deg)", sideBefore, sideAfter, firstMarginDeg, secondMarginDeg);
			break;
		case REFUSE:
			snprintf (buf, sizeof (buf), "REFUSE, fits neither side (window margins %.2f / %.2f deg)", firstMarginDeg, secondMarginDeg);
			break;
		default:
			return "UNKNOWN: " + reason;
	}
	return buf;
}

GeminiCaringLoop::GeminiCaringLoop (const char *_hostname, int _port):
	hostname (_hostname), port (_port), sock (-1), stopFlag (false),
	lastPollRa (NAN), lastPollDec (NAN), stableCount (0), moveStartedAt (0), moveDeadline (0),
	activeMoveTargetRa (NAN), activeMoveTargetDec (NAN),
	abortRequested (false), parkRequested (false), parkAtStartupPosition (false), rebootRequested (false), rebootCold (false),
	startupMode ((int) STARTUP_NONE), forcedSelection ((int) STARTUP_NONE), pollIntervalSec (1.0), wrongWayMarginDeg (15.0),
	flipAmbiguityMarginDeg (0.5), gotoPrestop ((int) PRESTOP_STOP),
	moveMinSeparation (NAN), wrongWayCount (0), moveStartPierSide ('?'), moveStartDecSide ('?'), movePierChangedFlag (false),
	slowPollCounter (0), nextDatagramNumber (0)
{
}

GeminiCaringLoop::~GeminiCaringLoop ()
{
	stop ();
}

bool GeminiCaringLoop::openSocket ()
{
	sock = socket (AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return false;

	memset (&destAddr, 0, sizeof (destAddr));
	destAddr.sin_family = AF_INET;
	destAddr.sin_port = htons (port);
	if (inet_pton (AF_INET, hostname.c_str (), &destAddr.sin_addr) != 1)
	{
		close (sock);
		sock = -1;
		return false;
	}
	return true;
}

bool GeminiCaringLoop::start ()
{
	if (!openSocket ())
		return false;
	stopFlag = false;
	worker = std::thread (&GeminiCaringLoop::threadMain, this);
	return true;
}

void GeminiCaringLoop::stop ()
{
	stopFlag = true;
	if (worker.joinable ())
		worker.join ();
	if (sock >= 0)
	{
		close (sock);
		sock = -1;
	}
}

GeminiStatus GeminiCaringLoop::getStatus ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	return status;
}

void GeminiCaringLoop::requestAbort ()
{
	abortRequested = true;
}

void GeminiCaringLoop::requestPark (bool atStartupPosition)
{
	parkAtStartupPosition = atStartupPosition;
	// set parking=true HERE, synchronously, before returning - not in
	// handlePark() on the caring thread. Otherwise there's a real race:
	// isParking() reads the registry's default parking=false until the
	// caring thread actually gets around to processing parkRequested,
	// which can be several idle() ticks later - and "not started yet"
	// looks identical to "already finished" from that side. Confirmed on
	// real hardware: park was reported done within 2-3ms while the mount
	// was still visibly slewing to the park position for real.
	{
		std::lock_guard<std::mutex> lock (mutex_);
		status.parking = true;
		status.parkFailed = false;
		status.parkStatus = '?';
	}
	parkRequested = true;
}

void GeminiCaringLoop::requestReboot (bool cold, StartupMode selection)
{
	// drop out of "the mount is up and usable" the moment the reboot is
	// asked for, not when the caring thread gets round to sending it: the
	// same race requestPark() documents, with the same consequence if it's
	// got wrong (a goto accepted into a mount that is on its way down).
	{
		std::lock_guard<std::mutex> lock (mutex_);
		status.startupComplete = false;
		status.startupState = '?';
		status.startupSelections = 0;
		status.valid = false;
		status.moveInProgress = false;
		status.parking = false;
		// read again once it is back: a CMOS reset resets the limits too
		status.geometry.valid = false;
		status.axisValid = false;
		status.bootObserved = true;
		status.bootSelection = '?';
	}
	rebootCold = cold;
	if (selection != STARTUP_NONE)
		forcedSelection = (int) selection;
	rebootRequested = true;
}

void GeminiCaringLoop::setStartupMode (StartupMode mode)
{
	startupMode = (int) mode;
	std::lock_guard<std::mutex> lock (mutex_);
	status.startupSelections = 0;
}

void GeminiCaringLoop::selectStartup (StartupMode mode)
{
	forcedSelection = (int) mode;
	std::lock_guard<std::mutex> lock (mutex_);
	status.startupSelections = 0;
}

void GeminiCaringLoop::queueNativeSet (int id, int32_t value)
{
	std::lock_guard<std::mutex> lock (mutex_);
	commandQueue.push_back ({ id, std::to_string (value) });
}

void GeminiCaringLoop::queueNativeSet (int id, double value)
{
	char buf[32];
	snprintf (buf, sizeof (buf), "%0.1f", value);
	std::lock_guard<std::mutex> lock (mutex_);
	commandQueue.push_back ({ id, buf });
}

void GeminiCaringLoop::queuePulseGuide (char direction, unsigned int magnitude)
{
	if (magnitude > 255)
		return;	// matches gemini2ser.cpp's performGuide() - reject, don't clamp, see header
	char buf[16];
	snprintf (buf, sizeof (buf), ":Mi%c%u#", direction, magnitude);
	std::lock_guard<std::mutex> lock (mutex_);
	rawCommandQueue.push_back (buf);
}

bool GeminiCaringLoop::gotoRaDec (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec, GotoSideMode sideMode, GeminiSidePrediction *prediction)
{
	std::unique_lock<std::mutex> lock (mutex_);
	gotoTargetRa = raDeg;
	gotoTargetDec = decDeg;
	gotoSideMode = sideMode;
	gotoDone = false;
	gotoCancelled = false;
	gotoRequested = true;

	bool signaled = cv_.wait_for (lock, std::chrono::duration<double> (waitTimeoutSec), [this] { return gotoDone; });
	if (!signaled)
	{
		// handleGoto() checks this right before the slew command goes out:
		// a goto we have already reported as failed must not start moving
		// the mount a moment later
		gotoCancelled = true;
		errorMessage = "timed out waiting for the caring loop to process the goto";
		return false;
	}
	if (prediction)
		*prediction = gotoPrediction;
	errorMessage = gotoMessage;
	return gotoAccepted;
}

bool GeminiCaringLoop::sendRawSync (const std::string &geminiData, std::string &response, double waitTimeoutSec)
{
	std::unique_lock<std::mutex> lock (mutex_);
	syncQueryCommand = geminiData;
	syncQueryDone = false;
	syncQueryRequested = true;

	bool signaled = cv_.wait_for (lock, std::chrono::duration<double> (waitTimeoutSec), [this] { return syncQueryDone; });
	if (!signaled || !syncQueryOk)
		return false;

	response = syncQueryResponse;
	return true;
}

bool GeminiCaringLoop::readNativeRaw (int id, std::string &value, double waitTimeoutSec)
{
	std::string response;
	if (!sendRawSync (buildNativeGet (id), response, waitTimeoutSec))
		return false;
	return parseNativeGetReply (response, value);
}

namespace
{
	// the three commands matchTimeUtc()/matchTimeUtcInternal() send, built
	// once here so the RTS2-thread-facing and caring-thread-facing versions
	// can never drift apart. labels[] is only for the error messages.
	void buildMatchTimeCommands (std::string cmds[3], const char *labels[3])
	{
		time_t t = time (nullptr);
		struct tm ts;
		gmtime_r (&t, &ts);

		char buf[32];
		// 1) zero the UTC-offset register, so Gemini's "local time" is UTC
		cmds[0] = ":SG+00.0#";
		labels[0] = "cannot set UTC offset (:SG+00.0#)";
		// 2) set time
		snprintf (buf, sizeof (buf), ":SL%02d:%02d:%02d#", ts.tm_hour, ts.tm_min, ts.tm_sec);
		cmds[1] = buf;
		labels[1] = "cannot set time (:SL#)";
		// 3) set date
		snprintf (buf, sizeof (buf), ":SC%02d/%02d/%02d#", ts.tm_mon + 1, ts.tm_mday, ts.tm_year - 100);
		cmds[2] = buf;
		labels[2] = "cannot set date (:SC#)";
	}
}

bool GeminiCaringLoop::matchTimeUtc (std::string &errorMessage, double waitTimeoutSec)
{
	std::string cmds[3];
	const char *labels[3];
	buildMatchTimeCommands (cmds, labels);

	std::string response;
	for (int i = 0; i < 3; i++)
	{
		if (!sendRawSync (cmds[i], response, waitTimeoutSec) || response.empty () || response[0] != '1')
		{
			errorMessage = labels[i];
			return false;
		}
	}
	return true;
}

bool GeminiCaringLoop::syncTo (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec)
{
	std::string sr, sd;
	if (!formatTargetCommands (raDeg, decDeg, sr, sd))
	{
		errorMessage = "bad RA/Dec for sync";
		return false;
	}

	std::string response;
	if (!sendRawSync (sr + sd, response, waitTimeoutSec))
	{
		errorMessage = "no response to sync target set (:Sr/:Sd)";
		return false;
	}
	if (response.size () < 2 || response[0] != '1' || response[1] != '1')
	{
		errorMessage = "mount rejected sync target (:Sr/:Sd expected two '1' acks), reply=\"" + response + "\"";
		return false;
	}

	// sent separately from :Sr/:Sd, reply not strictly parsed - see header
	if (!sendRawSync (":CI#", response, waitTimeoutSec))
	{
		errorMessage = "target was set, but no response to sync command (:CI#) - sync may not have completed";
		return false;
	}

	return true;
}

// ---- everything below this line runs on the caring-loop thread only ----

bool GeminiCaringLoop::sendAndReceive (const std::string &payload, std::string &response, double timeoutSec, int maxResyncAttempts)
{
	auto transmit = [this] (uint32_t datagramNumber, uint32_t lastDatagramNumber, const void *data, size_t len)
	{
		std::vector<uint8_t> pkt (8 + len);
		putLE32 (pkt.data (), datagramNumber);
		putLE32 (pkt.data () + 4, lastDatagramNumber);
		memcpy (pkt.data () + 8, data, len);
		sendto (sock, pkt.data (), pkt.size (), 0, (struct sockaddr *) &destAddr, sizeof (destAddr));
	};

	auto receive = [this] (double timeout, uint32_t &replyNumber, uint32_t &lastReplyNumber, std::string &out) -> bool
	{
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (sock, &fds);
		struct timeval tv;
		tv.tv_sec = (long) timeout;
		tv.tv_usec = (long) ((timeout - tv.tv_sec) * 1e6);
		int r = select (sock + 1, &fds, nullptr, nullptr, &tv);
		if (r <= 0)
			return false;
		uint8_t buf[600];
		ssize_t n = recv (sock, buf, sizeof (buf), 0);
		if (n < 8)
			return false;
		replyNumber = getLE32 (buf);
		lastReplyNumber = getLE32 (buf + 4);
		out.assign ((char *) buf + 8, n - 8);
		if (!out.empty () && out.back () == '\0')
			out.pop_back ();
		return true;
	};

	uint32_t cmdNum = nextDatagramNumber++;
	transmit (cmdNum, 0, payload.data (), payload.size () + 1);	// +1: NUL terminator, per spec

	for (int attempt = 0; attempt < maxResyncAttempts; attempt++)
	{
		uint32_t rNum, rLast;
		std::string rPayload;

		if (receive (timeoutSec, rNum, rLast, rPayload) && rNum == cmdNum)
		{
			response = rPayload;
			return true;
		}

		// timeout, or an unrelated/stale datagram - either way, resync per
		// the spec's Appendix 1 flowchart
		uint32_t nackNum = nextDatagramNumber++;
		uint8_t nak = 0x15;
		transmit (nackNum, 0, &nak, 1);	// no NUL terminator on a NACK, see spec footnote

		if (receive (timeoutSec, rNum, rLast, rPayload) && rNum == nackNum)
		{
			if (rLast == cmdNum)
			{
				// Gemini did receive/process the original command - this
				// datagram carries its deferred response
				response = rPayload;
				return true;
			}
			// Gemini never received it - resend fresh under a new number
			cmdNum = nextDatagramNumber++;
			transmit (cmdNum, 0, payload.data (), payload.size () + 1);
			attempt = -1;	// reset the budget: this is a fresh wait for a fresh command, not a NACK retry
			continue;
		}
		// NACK's own reply also timed out - loop tries another NACK unless attempts are exhausted
	}
	return false;
}

// Copies across everything a fresh ENQ parse does not itself produce.
// Getting this wrong is silent and annoying to chase: the field just reads
// as its default for one poll cycle in three, or forever. (It cost a real
// bug before this existed - requestPark() sets status.parking on the RTS2
// thread, and a pollStatus() that happened to be blocked in recv() at that
// moment wiped the flag on its way out, so isParking() saw "not parking"
// and declared the park finished within milliseconds.)
void GeminiCaringLoop::carryPersistentFields (const GeminiStatus &from, GeminiStatus &to)
{
	to.moveInProgress = from.moveInProgress;
	to.moveFailed = from.moveFailed;
	to.moveFailReason = from.moveFailReason;
	to.moveWrongWay = from.moveWrongWay;
	to.moveAborted = from.moveAborted;
	to.lastMoveSeparation = from.lastMoveSeparation;
	to.moveEndReason = from.moveEndReason;
	to.moveEndSerial = from.moveEndSerial;
	to.movePierChanged = from.movePierChanged;

	to.parking = from.parking;
	to.parkFailed = from.parkFailed;
	to.parkStatus = from.parkStatus;

	// polled separately (native 226), and deliberately sticky: a single
	// dropped datagram must not erase a tracking-limit warning that is
	// already showing - see pollTrackingLimit()
	to.trackingSecToWestLimit = from.trackingSecToWestLimit;
	to.trackingRate = from.trackingRate;

	to.startupState = from.startupState;
	to.startupComplete = from.startupComplete;
	to.startupCount = from.startupCount;
	to.startupSelections = from.startupSelections;
	to.clockMatched = from.clockMatched;
	to.bootObserved = from.bootObserved;
	to.bootSelection = from.bootSelection;
	to.clockOffsetSec = from.clockOffsetSec;
	to.axisTimestamp = from.axisTimestamp;

	to.limitsValid = from.limitsValid;
	to.limitBothRaw = from.limitBothRaw;
	to.limitEastRaw = from.limitEastRaw;
	to.limitWestRaw = from.limitWestRaw;
	to.limitWestGotoRaw = from.limitWestGotoRaw;

	to.geometry = from.geometry;
	to.axisValid = from.axisValid;
	to.raAxisTicks = from.raAxisTicks;
	to.decAxisTicks = from.decAxisTicks;
	to.gotoSerial = from.gotoSerial;
	to.lastPrediction = from.lastPrediction;
}

void GeminiCaringLoop::pollStatus ()
{
	std::string response;
	bool ok = sendAndReceive (std::string (1, '\x05'), response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	GeminiStatus fresh;
	bool parsed = ok && parseEnq (response, fresh);

	std::lock_guard<std::mutex> lock (mutex_);

	status.connected = ok;
	if (!parsed)
		return;

	fresh.connected = true;
	fresh.valid = true;
	fresh.timestamp = nowSeconds ();
	carryPersistentFields (status, fresh);

	if (fresh.moveInProgress)
	{
		// ---- is the mount actually going where it was told to? ----
		// A real meridian flip legitimately swings the reported RA/Dec far
		// away from both ends of the move (the mount crosses the pole with
		// the counterweight going over), so as soon as the pier side
		// changes mid-move this check stands down for the rest of it
		// rather than reporting the flip itself as a fault.
		// Only as a fallback when the Dec axis side is unknown: the ENQ pier
		// side is the RA axis relative to CWD, which changes without any flip
		// - out of the park position at the pole, every goto flips it - and
		// would switch the wrong-way check off for the whole move. With the
		// axes read, pollAxisPosition() watches the real flip state.
		if (status.decSide () == '?' && fresh.pierSide != '?' && moveStartPierSide != '?' && fresh.pierSide != moveStartPierSide)
			movePierChangedFlag = true;
		fresh.movePierChanged = movePierChangedFlag;

		fresh.moveSeparation = angularSeparationDeg (fresh.ra, fresh.dec, activeMoveTargetRa, activeMoveTargetDec);
		fresh.lastMoveSeparation = fresh.moveSeparation;
		if (!std::isnan (fresh.moveSeparation))
		{
			if (std::isnan (moveMinSeparation) || fresh.moveSeparation < moveMinSeparation)
				moveMinSeparation = fresh.moveSeparation;

			if (!movePierChangedFlag && fresh.moveSeparation > moveMinSeparation + wrongWayMarginDeg.load ())
				wrongWayCount++;
			else
				wrongWayCount = 0;

			if (wrongWayCount >= WRONG_WAY_POLLS)
				fresh.moveWrongWay = true;	// sticky until the next accepted goto clears it
		}

		if (!std::isnan (lastPollRa) &&
			raDistanceDeg (fresh.ra, lastPollRa) < MOVE_STABLE_DEG &&
			fabs (fresh.dec - lastPollDec) < MOVE_STABLE_DEG)
			stableCount++;
		else
			stableCount = 0;

		// never while the mount itself still reports slewing/centering: a
		// sequenced move can hold one axis still while the other waits
		bool stoppedChanging = stableCount >= 2 && (fresh.timestamp - moveStartedAt) >= MOVE_MIN_SETTLE_SEC
			&& fresh.moveRate != 'S' && fresh.moveRate != 'C';
		bool timedOut = fresh.timestamp > moveDeadline;

		// "stopped changing" is NOT the same thing as "arrived" - a move
		// that stalls partway (mount-side limit, another client sending
		// conflicting commands, ...) also looks like a stable position.
		// Always check against the actual requested target before ever
		// declaring success - see UPSTREAM_BUGS.md/STATUS.md for the real
		// live-hardware incident this fixes (a 30+ degree miss was
		// reported as a successful move because nothing checked).
		if (stoppedChanging || timedOut)
		{
			// Judge arrival on the true angular distance to the target,
			// not on per-axis differences. Two reasons, both of them real
			// on this mount: RA differences do not mean what they look
			// like near the pole (the park position is AT the pole, where
			// any RA whatsoever is the same point on the sky), and a
			// per-axis RA test has to get the 0/360 wrap right, which is
			// exactly what went wrong before - see raDistanceDeg()'s
			// comment. moveSeparation is already computed above, from
			// this same snapshot, by the wrong-way check.
			double dRa = raDistanceDeg (fresh.ra, activeMoveTargetRa);
			double dDec = fabs (fresh.dec - activeMoveTargetDec);
			double miss = std::isnan (fresh.moveSeparation) ? (dRa > dDec ? dRa : dDec) : fresh.moveSeparation;

			fresh.moveInProgress = false;
			{
				char end[200];
				snprintf (end, sizeof (end), "%s after %.0f s, %.3f deg from target (dRA=%.3f dDec=%.3f), mount rate '%c'",
					miss < ARRIVAL_TOLERANCE_DEG ? "arrived" : (timedOut ? "timed out" : "stopped moving"),
					fresh.timestamp - moveStartedAt, miss, dRa, dDec, fresh.moveRate);
				fresh.moveEndReason = end;
				fresh.moveEndSerial = status.gotoSerial;
			}
			if (miss < ARRIVAL_TOLERANCE_DEG)
			{
				fresh.moveFailed = false;
			}
			else
			{
				fresh.moveFailed = true;
				char buf[256];
				snprintf (buf, sizeof (buf), "%s %.3f deg from target (dRA=%.3f dDec=%.3f) - possible mount-side limit, obstruction, or another client sending conflicting commands",
					timedOut ? "move timed out" : "move stopped", miss, dRa, dDec);
				fresh.moveFailReason = buf;
			}
		}
	}

	lastPollRa = fresh.ra;
	lastPollDec = fresh.dec;

	status = fresh;
}

// native register 226 - see GeminiStatus::trackingSecToWestLimit's doc
// comment. A miss just leaves the previous value in place rather than
// resetting to NAN - a single dropped/resynced datagram shouldn't erase a
// real warning that was already showing.
void GeminiCaringLoop::pollTrackingLimit ()
{
	std::string response;
	bool ok = sendAndReceive (buildNativeGet (226), response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::string value;
	if (!ok || !parseNativeGetReply (response, value))
		return;

	try
	{
		double sec = std::stod (value);
		std::lock_guard<std::mutex> lock (mutex_);
		status.trackingSecToWestLimit = sec;
	}
	catch (const std::exception &)
	{
	}
}

// native register 239 - both axes in motor ticks. The Dec axis side is the
// flip state the firmware's own goto decision works from, so a change of it
// during a move is the direct evidence of a flip (the ENQ pier side is the
// RA axis relative to CWD, which also changes without any flip when the
// telescope tracks past 6h from the meridian).
void GeminiCaringLoop::pollAxisPosition ()
{
	std::string value;
	int32_t ra, dec;
	if (!readNativeInternal (239, value) || !parseTickPair (value, ra, dec))
		return;

	std::lock_guard<std::mutex> lock (mutex_);
	status.axisValid = true;
	status.raAxisTicks = ra;
	status.decAxisTicks = dec;
	status.axisTimestamp = nowSeconds ();

	char side = status.decSide ();
	if (status.moveInProgress && side != '?' && moveStartDecSide != '?' && side != moveStartDecSide)
	{
		movePierChangedFlag = true;
		status.movePierChanged = true;
	}
}

bool GeminiCaringLoop::readGeometryInternal ()
{
	GeminiAxisGeometry geo;
	std::string value;

	if (!readNativeInternal (238, value) || !parseTickPair (value, geo.raHalf, geo.decHalf))
		return false;
	if (!readNativeInternal (231, value) || !parseTickPair (value, geo.eastLimit, geo.westLimit))
		return false;
	// stored as arcseconds and subtracted from the western safety limit
	// (firmware FUN_0000a3bc) - "000d00" really means none, not a default
	if (!readNativeInternal (223, value) || !parseDegMin (value, geo.westGotoDeg))
		return false;
	if (readNativeInternal (229, value))
	{
		try
		{
			geo.flipPoints = std::stoi (value);
		}
		catch (const std::exception &)
		{
		}
	}

	// sanity: CWD must lie inside both safety limits, and the goto window
	// must not be empty - anything else means a reply was misread
	if (geo.raHalf <= 0 || geo.decHalf <= 0 || !(geo.westLimit < geo.raHalf && geo.raHalf < geo.eastLimit)
		|| geo.windowLow () >= geo.windowHigh ())
		return false;

	geo.valid = true;
	std::lock_guard<std::mutex> lock (mutex_);
	status.geometry = geo;
	return true;
}

GeminiSidePrediction GeminiCaringLoop::predictInternal (double targetRaDeg, bool preferOther)
{
	GeminiAxisGeometry geo;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		geo = status.geometry;
	}

	GeminiSidePrediction p;
	if (!geo.valid)
	{
		p.reason = "mount geometry (native 238/231/223) not read";
		return p;
	}

	// fresh, back to back: the RA axis position and the RA the mount
	// reports have to describe the same moment, which a snapshot up to a
	// poll interval old does not guarantee while the mount is moving
	std::string response, value;
	GeminiStatus enq;
	int32_t ra, dec;
	if (!sendAndReceive (std::string (1, '\x05'), response, COMMAND_TIMEOUT_SEC, 2) || !parseEnq (response, enq))
	{
		p.reason = "fresh ENQ read failed";
		return p;
	}
	if (!sendAndReceive (buildNativeGet (239), response, COMMAND_TIMEOUT_SEC, 2) || !parseNativeGetReply (response, value)
		|| !parseTickPair (value, ra, dec))
	{
		p.reason = "fresh native 239 read failed";
		return p;
	}
	return predictGotoSide (geo, ra, dec, enq.ra, targetRaDeg, preferOther);
}

// native register 130 - the mount's own idea of which tracking rate it is
// running (131 sidereal, 135 terrestrial/off, ...). Read with the slow poll
// group rather than every cycle: nothing changes it behind our back at
// second resolution, and it exists mostly so an operator (and the safety
// watchdog) can see that a requested "start tracking" really landed.
void GeminiCaringLoop::pollTrackingRate ()
{
	std::string value;
	if (!readNativeInternal (130, value))
		return;
	try
	{
		int rate = std::stoi (value);
		std::lock_guard<std::mutex> lock (mutex_);
		status.trackingRate = rate;
	}
	catch (const std::exception &)
	{
	}
}

// The missing half of "connect to the mount" - see GeminiStatus::
// startupState. A Gemini that has just been powered on sits in its boot
// menu answering status queries quite normally while ignoring every single
// motion command, which from the client side is indistinguishable from a
// broken driver. base/teld/gemini/gemini.cpp's tel_gemini_reset() does this
// same 0x06 handshake over RS232; this is the UDP equivalent, with two
// differences: it runs on every poll cycle until the mount is up (rather
// than once, blocking, at connect), and it keeps running - at
// SLOW_POLL_EVERY - afterwards, so a mount that reboots under a running
// driver is noticed and brought back up instead of silently going deaf.
void GeminiCaringLoop::pollStartupState ()
{
	std::string response;
	bool ok = sendAndReceive (std::string (1, '\x06'), response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	if (!ok || response.empty ())
	{
		std::lock_guard<std::mutex> lock (mutex_);
		status.connected = ok;
		return;
	}

	char state = response[0];

	const char *selection = nullptr;
	char selectionLetter = '?';
	{
		std::lock_guard<std::mutex> lock (mutex_);
		status.connected = true;
		status.startupState = state;

		switch (state)
		{
			case 'G':	// startup complete, German equatorial
			case 'A':	// startup complete, Alt/Az
				if (status.startupComplete)
					return;	// already up and known to be up - nothing to do
				break;
			case 'b':	// boot menu, waiting for a startup-mode selection
			{
				if (status.startupComplete || !status.bootObserved)
					status.bootSelection = '?';	// a new boot
				status.startupComplete = false;
				status.bootObserved = true;
				// answered only when somebody decided the answer: the
				// standing startup_mode (NONE by default), or a one-shot
				// from a human's "position" command or our own reboot
				int mode = forcedSelection.load () != (int) STARTUP_NONE ? forcedSelection.load () : startupMode.load ();
				selection = startupSelectionCommand (mode);
				if (selection != nullptr && status.startupSelections < MAX_STARTUP_SELECTIONS)
				{
					status.startupSelections++;
					selectionLetter = selection[1];
				}
				else
				{
					selection = nullptr;
				}
				break;
			}
			default:	// 'B' startup message on screen, 'S' cold start running, or something undocumented
				if (status.startupComplete || !status.bootObserved)
					status.bootSelection = '?';
				status.startupComplete = false;
				status.bootObserved = true;
				return;
		}
	}

	if (state == 'b')
	{
		if (selection != nullptr)
		{
			// no meaningful reply to parse - a command Gemini has no
			// response for comes back as the ACK substitution (see the
			// protocol reference, "Commands with no serial response").
			// The next poll cycle re-reads the handshake and sees whether
			// it took.
			std::string ignored;
			sendAndReceive (selection, ignored, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);
			std::lock_guard<std::mutex> lock (mutex_);
			status.bootSelection = selectionLetter;
		}
		return;
	}

	// fell through the 'G'/'A' case with startupComplete still false: the
	// mount has just finished starting up (or we have just connected to one
	// that was already up)
	runPostStartupSequence ();
}

// Everything worth reading from the mount once per startup, done here on
// the caring thread rather than from GeminiUDP::initValues() for two
// reasons: it must also run after a *re*start (a power cycle, somebody
// else's reboot, a re-zero), and doing it here keeps it off the RTS2 thread.
//
// Deliberately read-only apart from the clock, and the clock only when it
// is wrong: this runs every time the driver (re)connects, including to a
// mount that is in the middle of the night's work, and a driver restart
// must not change anything about a mount that is fine. In particular no
// :hW# - in this firmware it is "unpark and start tracking" (it clears the
// park flag and starts the worm), which on a parked mount means tracking
// away from the park position for as long as nobody notices.
void GeminiCaringLoop::runPostStartupSequence ()
{
	// every HA/LST-derived decision this driver makes trusts the mount's
	// own clock; set it only when it is off, since even a correct setting
	// makes the mount's idea of the sky jump by up to a second's worth
	double clockOffset = NAN;
	bool clockRead = readClockOffsetInternal (clockOffset);
	bool timeOk = clockRead && fabs (clockOffset) <= 2.0;
	if (!timeOk)
		timeOk = matchTimeUtcInternal ();

	std::string both, east, west, westGoto;
	bool limitsOk = readNativeInternal (220, both)
		&& readNativeInternal (221, east)
		&& readNativeInternal (222, west)
		&& readNativeInternal (223, westGoto);

	// failure is not fatal here - the slow poll keeps retrying it, and
	// until it lands gotos go out unpredicted
	readGeometryInternal ();
	pollAxisPosition ();

	int rate = 0;
	std::string rateStr;
	if (readNativeInternal (130, rateStr))
	{
		try
		{
			rate = std::stoi (rateStr);
		}
		catch (const std::exception &)
		{
		}
	}

	// the parked flag survives power cycles (battery-backed), so this is
	// how a restarted driver learns the mount is parked
	std::string parkResponse;
	bool parkRead = sendAndReceive (":h?#", parkResponse, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS) && !parkResponse.empty ();

	std::lock_guard<std::mutex> lock (mutex_);
	if (limitsOk)
	{
		status.limitBothRaw = both;
		status.limitEastRaw = east;
		status.limitWestRaw = west;
		status.limitWestGotoRaw = westGoto;
		status.limitsValid = true;
	}
	if (rate != 0)
		status.trackingRate = rate;
	if (parkRead && !status.parking)
		status.parkStatus = parkResponse[0];
	status.clockOffsetSec = clockRead ? clockOffset : NAN;
	status.clockMatched = timeOk;
	status.startupComplete = true;
	status.startupSelections = 0;
	status.startupCount++;
	forcedSelection = (int) STARTUP_NONE;
}

// :GG# is the hours to add to the mount's local time to get UTC, :GL#/:GC#
// its local time and date. Both of the latter are read back to back and a
// date rollover in between just shows up as a large offset, which only
// makes the caller set the clock - the safe direction.
bool GeminiCaringLoop::readClockOffsetInternal (double &offsetSec)
{
	std::string gg, gl, gc;
	if (!sendAndReceive (":GG#", gg, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS)
		|| !sendAndReceive (":GL#", gl, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS)
		|| !sendAndReceive (":GC#", gc, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS))
		return false;
	time_t now = time (nullptr);

	double utcOffsetHours;
	int hh, mm, ss, mon, day, yy;
	if (sscanf (gg.c_str (), "%lf", &utcOffsetHours) != 1
		|| sscanf (gl.c_str (), "%d:%d:%d", &hh, &mm, &ss) != 3
		|| sscanf (gc.c_str (), "%d/%d/%d", &mon, &day, &yy) != 3)
		return false;

	struct tm ts;
	memset (&ts, 0, sizeof (ts));
	ts.tm_year = yy + 100;
	ts.tm_mon = mon - 1;
	ts.tm_mday = day;
	ts.tm_hour = hh;
	ts.tm_min = mm;
	ts.tm_sec = ss;
	time_t mountLocal = timegm (&ts);
	if (mountLocal == (time_t) -1)
		return false;
	offsetSec = difftime (mountLocal, now) + utcOffsetHours * 3600.0;
	return true;
}

bool GeminiCaringLoop::matchTimeUtcInternal ()
{
	std::string cmds[3];
	const char *labels[3];
	buildMatchTimeCommands (cmds, labels);

	for (int i = 0; i < 3; i++)
	{
		std::string response;
		if (!sendAndReceive (cmds[i], response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS) || response.empty () || response[0] != '1')
			return false;
	}
	return true;
}

bool GeminiCaringLoop::readNativeInternal (int id, std::string &value)
{
	std::string response;
	if (!sendAndReceive (buildNativeGet (id), response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS))
		return false;
	return parseNativeGetReply (response, value);
}

// 65533 "reboot enforcing a Cold Start" / 65535 "reboot" - the same pair,
// picked the same way, as base/teld/gemini/gemini.cpp's resetMount().
// requestReboot() has already marked the snapshot as not-started-up, so by
// the time this returns the caring loop is back in handshake mode and will
// walk the mount up through its boot menu on its own.
void GeminiCaringLoop::handleReboot ()
{
	std::string response;
	sendAndReceive (buildNativeSet (rebootCold.load () ? 65533 : 65535, "0"), response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);
}

void GeminiCaringLoop::handleGoto ()
{
	double ra, dec;
	bool started;
	char pierSideNow, decSideNow, startupStateNow;
	GotoSideMode sideMode;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		gotoRequested = false;
		ra = gotoTargetRa;
		dec = gotoTargetDec;
		sideMode = gotoSideMode;
		started = status.startupComplete;
		pierSideNow = status.pierSide;
		decSideNow = status.decSide ();
		startupStateNow = status.startupState;
	}

	std::string sr, sd;
	bool accepted = false;
	std::string message;
	GeminiSidePrediction prediction;
	double ambiguityMargin = flipAmbiguityMarginDeg.load ();
	const char *slewCommand = sideMode == GOTO_FLIP ? ":MM#" : ":MS#";

	bool targetOk = formatTargetCommands (ra, dec, sr, sd);
	if (started && targetOk)
		prediction = predictInternal (ra, sideMode == GOTO_FLIP);

	bool cancelled;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		cancelled = gotoCancelled;
	}

	if (!started)
	{
		// the failure mode this refusal exists to make visible: a Gemini
		// still in its boot menu acks :Sr/:Sd and answers :MS# perfectly
		// politely, and then does nothing at all. Far better to reject the
		// move here, with a reason, than to let the framework believe a
		// slew is under way for its full timeout.
		message = "mount startup is not complete (handshake state '"
			+ std::string (1, startupStateNow) + "') - not sending a slew to a mount that will ignore it";
	}
	else if (!targetOk)
	{
		message = "bad target RA/Dec";
	}
	else if (sideMode == GOTO_KEEP_SIDE && (prediction.outcome != GeminiSidePrediction::STAY || prediction.ambiguous (ambiguityMargin)))
	{
		message = "not sent: the caller needs the mount to stay on its pier side, predicted " + prediction.describe ();
	}
	else if (sideMode == GOTO_FLIP && (prediction.outcome != GeminiSidePrediction::FLIP || prediction.ambiguous (ambiguityMargin)))
	{
		message = "not sent: the caller needs a pier flip, predicted " + prediction.describe ();
	}
	else if (cancelled)
	{
		// the prediction reads above take real round trips, long enough
		// for gotoRaDec() to have stopped waiting and reported a failure
		message = "not sent: the requester already gave up waiting";
	}
	else
	{
		std::string response;
		// Both slew commands run the same firmware routine and differ in
		// one flag (FUN_00048ed0, see ~/tmp/gemini/FLIP_LOGIC.md): :MS#
		// keeps the pier side the Dec axis is on whenever the target fits
		// the RA window there, :MM# tries the other side first. :MS# is
		// the default for every ordinary move - an :MM# for all moves flips
		// on every goto that the other side can reach, which is how the
		// one early experiment with it ended in a stuck mount. :MM# is used
		// only through GOTO_FLIP, where a flip is the point.
		// See GotoPrestop. The production driver (gemini2ser.cpp) always
		// sends :Q# ahead of a goto; on SBT (2026-09-14) two meridian flips
		// sent to a tracking mount without it ran the RA axis at ~21x
		// sidereal for the whole move instead of slewing.
		int prestop = gotoPrestop.load ();
		if (prestop == PRESTOP_STOP_TRACKING)
		{
			std::string ignored;
			sendAndReceive (buildNativeSet (135, "1"), ignored, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);
		}
		if (prestop != PRESTOP_NONE)
		{
			std::string ignored;
			sendAndReceive (":Q#", ignored, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);
			std::this_thread::sleep_for (std::chrono::milliseconds (300));
		}
		if (!sendAndReceive (sr + sd + slewCommand, response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS))
		{
			message = std::string ("no response to goto command (:Sr/:Sd/") + slewCommand + ")";
		}
		else if (response.size () < 3 || response[0] != '1' || response[1] != '1')
		{
			message = "mount rejected target RA/Dec set (:Sr/:Sd expected two '1' acks), reply=\"" + response + "\"";
		}
		else
		{
			std::string msResp = response.substr (2);
			if (!msResp.empty () && msResp.back () == '#')
				msResp.pop_back ();
			char code = msResp.empty () ? '?' : msResp[0];

			if (code == '0' || code == '3')
			{
				accepted = true;
			}
			else
			{
				message = "mount refused slew: code " + msResp + " (" + msResponseMeaning (code) + ")";
			}
		}
	}

	if (accepted)
	{
		moveStartedAt = nowSeconds ();
		moveDeadline = moveStartedAt + MOVE_MAX_SEC;
		stableCount = 0;
		lastPollRa = NAN;
		lastPollDec = NAN;
		activeMoveTargetRa = ra;
		activeMoveTargetDec = dec;

		moveMinSeparation = NAN;
		wrongWayCount = 0;
		moveStartPierSide = pierSideNow;
		moveStartDecSide = prediction.sideBefore != '?' ? prediction.sideBefore : decSideNow;
		// a flip that is expected, or can't be ruled out, suspends the
		// wrong-way check from the start rather than from the pole
		// crossing - by then the distance to the target has long been
		// growing. An UNKNOWN prediction keeps the after-the-fact
		// detection only, as before predictions existed.
		movePierChangedFlag = prediction.outcome == GeminiSidePrediction::FLIP
			|| (prediction.outcome != GeminiSidePrediction::UNKNOWN && prediction.ambiguous (ambiguityMargin));
	}

	std::lock_guard<std::mutex> lock (mutex_);
	gotoAccepted = accepted;
	gotoMessage = message;
	gotoPrediction = prediction;
	gotoDone = true;
	if (accepted)
	{
		status.moveInProgress = true;
		status.moveFailed = false;
		status.moveFailReason.clear ();
		status.moveWrongWay = false;
		status.moveAborted = false;
		status.movePierChanged = movePierChangedFlag;
		status.moveSeparation = NAN;
		status.parkStatus = '0';	// the firmware clears its park status on every goto
		status.gotoSerial++;
		status.lastPrediction = prediction;
	}
	cv_.notify_all ();
}

void GeminiCaringLoop::handleAbort ()
{
	std::string response;
	sendAndReceive (":Q#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::lock_guard<std::mutex> lock (mutex_);
	if (status.moveInProgress)
	{
		status.moveAborted = true;
		char end[160];
		snprintf (end, sizeof (end), "stopped by :Q# after %.0f s, %.3f deg from target, mount rate '%c'",
			nowSeconds () - moveStartedAt, status.moveSeparation, status.moveRate);
		status.moveEndReason = end;
		status.moveEndSerial = status.gotoSerial;
	}
	status.moveInProgress = false;
}

void GeminiCaringLoop::handlePark ()
{
	// parking/parkFailed/parkStatus are already set by requestPark() -
	// see its comment for why that has to happen there, not here
	std::string response;
	sendAndReceive (parkAtStartupPosition.load () ? ":hC#" : ":hP#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::lock_guard<std::mutex> lock (mutex_);
	status.moveInProgress = false;	// matches gemini2ser.cpp's startPark() calling stopMove() first
}

// :h?# ("query park status") response chars, per gemini2ser.cpp's
// isParking(): '1' done, '2'/' ' still parking, '0' "called without park
// command" (a real error - logged by GeminiUDP::isParking()). Anything
// else is left alone rather than guessed at, matching this driver's usual
// discipline around undocumented codes - just keep polling.
void GeminiCaringLoop::pollParkStatus ()
{
	std::string response;
	bool ok = sendAndReceive (":h?#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::lock_guard<std::mutex> lock (mutex_);
	status.connected = ok;
	if (!ok || response.empty ())
		return;

	char c = response[0];
	status.parkStatus = c;
	if (c == '1')
	{
		status.parking = false;
		status.parkFailed = false;
	}
	else if (c == '0')
	{
		status.parking = false;
		status.parkFailed = true;
	}
	// else: still parking ('2'/' ') or an undocumented code - keep polling
}

void GeminiCaringLoop::handleQueuedCommand ()
{
	NativeSetCommand cmd;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		if (commandQueue.empty ())
			return;
		cmd = commandQueue.front ();
		commandQueue.pop_front ();
	}

	std::string wire = buildNativeSet (cmd.id, cmd.valueStr);
	std::string response;
	sendAndReceive (wire, response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);	// fire-and-forget: result intentionally unused, see header
}

void GeminiCaringLoop::handleQueuedRawCommand ()
{
	std::string cmd;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		if (rawCommandQueue.empty ())
			return;
		cmd = rawCommandQueue.front ();
		rawCommandQueue.pop_front ();
	}

	std::string response;
	sendAndReceive (cmd, response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);	// fire-and-forget: result intentionally unused, see header
}

void GeminiCaringLoop::handleSyncQuery ()
{
	std::string cmd;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		syncQueryRequested = false;
		cmd = syncQueryCommand;
	}

	std::string response;
	bool ok = sendAndReceive (cmd, response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::lock_guard<std::mutex> lock (mutex_);
	syncQueryOk = ok;
	syncQueryResponse = response;
	syncQueryDone = true;
	cv_.notify_all ();
}

void GeminiCaringLoop::threadMain ()
{
	double lastPoll = 0;

	while (!stopFlag.load ())
	{
		// ahead of the abort: a reboot is the heaviest thing we can ask of
		// the mount and the recovery sequence that issues one has already
		// stopped and parked it by this point
		if (rebootRequested.exchange (false))
		{
			handleReboot ();
			continue;
		}

		if (abortRequested.exchange (false))
		{
			handleAbort ();
			continue;
		}

		bool haveGoto;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			haveGoto = gotoRequested;
		}
		if (haveGoto)
		{
			handleGoto ();
			continue;
		}

		if (parkRequested.exchange (false))
		{
			handlePark ();
			continue;
		}

		bool haveSyncQuery;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			haveSyncQuery = syncQueryRequested;
		}
		if (haveSyncQuery)
		{
			handleSyncQuery ();
			continue;
		}

		bool haveRawQueued;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			haveRawQueued = !rawCommandQueue.empty ();
		}
		if (haveRawQueued)
		{
			handleQueuedRawCommand ();
			continue;
		}

		bool haveQueued;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			haveQueued = !commandQueue.empty ();
		}
		if (haveQueued)
		{
			handleQueuedCommand ();
			continue;
		}

		bool parking, started;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			parking = status.parking;
			started = status.startupComplete;
		}

		double now = nowSeconds ();
		if (now - lastPoll >= pollIntervalSec.load ())
		{
			lastPoll = now;
			if (!started)
			{
				// nothing else is worth asking a mount that is still
				// booting - and while it sits in its boot menu this is
				// what walks it out of there
				pollStartupState ();
			}
			else if (parking)
			{
				pollParkStatus ();
			}
			else
			{
				bool wasConnected;
				{
					std::lock_guard<std::mutex> lock (mutex_);
					wasConnected = status.connected;
				}
				// silence is how a power cycle looks from here: ask where it
				// is in its startup first, not up to SLOW_POLL_EVERY later
				if (!wasConnected)
				{
					pollStartupState ();
					std::lock_guard<std::mutex> lock (mutex_);
					if (!status.startupComplete)
						continue;
				}
				pollStatus ();
				pollTrackingLimit ();
				pollAxisPosition ();
				if (++slowPollCounter >= SLOW_POLL_EVERY)
				{
					slowPollCounter = 0;
					pollStartupState ();	// catches a mount that rebooted under us
					pollTrackingRate ();

					bool haveGeometry;
					{
						std::lock_guard<std::mutex> lock (mutex_);
						haveGeometry = status.geometry.valid;
					}
					if (!haveGeometry)
						readGeometryInternal ();
				}
			}
		}
		else
		{
			std::this_thread::sleep_for (std::chrono::milliseconds (100));
		}
	}
}
