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
	constexpr double MOVE_MAX_SEC = 60.0;	// forces a stop-and-check past this even if nobody called requestAbort()
	constexpr double ARRIVAL_TOLERANCE_DEG = 0.5;	// generous vs. the ~0.01-0.02 deg errors seen on real successful slews - catches "stopped nowhere near the target", not normal settling wobble
	constexpr double COMMAND_TIMEOUT_SEC = 1.0;
	constexpr int RESYNC_ATTEMPTS = 5;

	double nowSeconds ()
	{
		return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
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
}

GeminiCaringLoop::GeminiCaringLoop (const char *_hostname, int _port):
	hostname (_hostname), port (_port), sock (-1), stopFlag (false),
	lastPollRa (NAN), lastPollDec (NAN), stableCount (0), moveStartedAt (0), moveDeadline (0),
	activeMoveTargetRa (NAN), activeMoveTargetDec (NAN),
	abortRequested (false), parkRequested (false), pollIntervalSec (1.0), nextDatagramNumber (0)
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

void GeminiCaringLoop::requestPark ()
{
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

void GeminiCaringLoop::queuePulseGuide (char direction, unsigned int durationMs)
{
	if (durationMs > 9999)
		durationMs = 9999;	// DDDD is a 4-digit field, see :Mg's doc comment
	char buf[16];
	snprintf (buf, sizeof (buf), ":Mg%c%u#", direction, durationMs);
	std::lock_guard<std::mutex> lock (mutex_);
	rawCommandQueue.push_back (buf);
}

bool GeminiCaringLoop::gotoRaDec (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec)
{
	std::unique_lock<std::mutex> lock (mutex_);
	gotoTargetRa = raDeg;
	gotoTargetDec = decDeg;
	gotoDone = false;
	gotoRequested = true;

	bool signaled = cv_.wait_for (lock, std::chrono::duration<double> (waitTimeoutSec), [this] { return gotoDone; });
	if (!signaled)
	{
		errorMessage = "timed out waiting for the caring loop to process the goto";
		return false;
	}
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

bool GeminiCaringLoop::matchTimeUtc (std::string &errorMessage, double waitTimeoutSec)
{
	time_t t = time (nullptr);
	struct tm ts;
	gmtime_r (&t, &ts);

	std::string response;
	char buf[32];

	// 1) zero the UTC-offset register, so Gemini's "local time" is UTC
	if (!sendRawSync (":SG+00.0#", response, waitTimeoutSec) || response.empty () || response[0] != '1')
	{
		errorMessage = "cannot set UTC offset (:SG+00.0#)";
		return false;
	}

	// 2) set time
	snprintf (buf, sizeof (buf), ":SL%02d:%02d:%02d#", ts.tm_hour, ts.tm_min, ts.tm_sec);
	if (!sendRawSync (buf, response, waitTimeoutSec) || response.empty () || response[0] != '1')
	{
		errorMessage = "cannot set time (:SL#)";
		return false;
	}

	// 3) set date
	snprintf (buf, sizeof (buf), ":SC%02d/%02d/%02d#", ts.tm_mon + 1, ts.tm_mday, ts.tm_year - 100);
	if (!sendRawSync (buf, response, waitTimeoutSec) || response.empty () || response[0] != '1')
	{
		errorMessage = "cannot set date (:SC#)";
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
	fresh.moveInProgress = status.moveInProgress;
	fresh.moveFailed = status.moveFailed;
	fresh.moveFailReason = status.moveFailReason;

	if (fresh.moveInProgress)
	{
		if (!std::isnan (lastPollRa) &&
			fabs (ln_range_degrees (fresh.ra - lastPollRa)) < MOVE_STABLE_DEG &&
			fabs (fresh.dec - lastPollDec) < MOVE_STABLE_DEG)
			stableCount++;
		else
			stableCount = 0;

		bool stoppedChanging = stableCount >= 2 && (fresh.timestamp - moveStartedAt) >= MOVE_MIN_SETTLE_SEC;
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
			double dRa = fabs (ln_range_degrees (fresh.ra - activeMoveTargetRa));
			double dDec = fabs (fresh.dec - activeMoveTargetDec);

			fresh.moveInProgress = false;
			if (dRa < ARRIVAL_TOLERANCE_DEG && dDec < ARRIVAL_TOLERANCE_DEG)
			{
				fresh.moveFailed = false;
			}
			else
			{
				fresh.moveFailed = true;
				char buf[256];
				snprintf (buf, sizeof (buf), "%s %.3f deg from target (dRA=%.3f dDec=%.3f) - possible mount-side limit, obstruction, or another client sending conflicting commands",
					timedOut ? "move timed out" : "move stopped", dRa > dDec ? dRa : dDec, dRa, dDec);
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

void GeminiCaringLoop::handleGoto ()
{
	double ra, dec;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		gotoRequested = false;
		ra = gotoTargetRa;
		dec = gotoTargetDec;
	}

	std::string sr, sd;
	bool accepted = false;
	std::string message;

	if (!formatTargetCommands (ra, dec, sr, sd))
	{
		message = "bad target RA/Dec";
	}
	else
	{
		std::string response;
		// :MS# - despite the official docs' :MM# entry describing itself
		// as the one "doing a meridian flip if possible" (implying by
		// contrast that :MS# doesn't), :MS#'s own verbatim doc text says
		// nothing about flip behavior either way, and live-hardware
		// evidence settles it: a real Dec 80->-10 walk at fixed RA, using
		// :MS# exclusively, crossed a real pier-side flip (W->E) with zero
		// rejections - see STATUS.md. We briefly tried :MM# instead
		// (reasoning that it was the "correct" flip-capable command) and
		// hit a real stuck-mount incident on the very first attempt; :MS#
		// has substantial successful live mileage including a real flip
		// and :MM# has exactly one data point and it went badly, so :MS#
		// is the better-evidenced choice, not because :MM# is proven bad.
		if (!sendAndReceive (sr + sd + ":MS#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS))
		{
			message = "no response to goto command (:Sr/:Sd/:MS)";
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
	}

	std::lock_guard<std::mutex> lock (mutex_);
	gotoAccepted = accepted;
	gotoMessage = message;
	gotoDone = true;
	if (accepted)
	{
		status.moveInProgress = true;
		status.moveFailed = false;
		status.moveFailReason.clear ();
	}
	cv_.notify_all ();
}

void GeminiCaringLoop::handleAbort ()
{
	std::string response;
	sendAndReceive (":Q#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

	std::lock_guard<std::mutex> lock (mutex_);
	status.moveInProgress = false;
}

void GeminiCaringLoop::handlePark ()
{
	// parking/parkFailed/parkStatus are already set by requestPark() -
	// see its comment for why that has to happen there, not here
	std::string response;
	sendAndReceive (":hP#", response, COMMAND_TIMEOUT_SEC, RESYNC_ATTEMPTS);

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

		bool parking;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			parking = status.parking;
		}

		double now = nowSeconds ();
		if (now - lastPoll >= pollIntervalSec.load ())
		{
			lastPoll = now;
			if (parking)
				pollParkStatus ();
			else
			{
				pollStatus ();
				pollTrackingLimit ();
			}
		}
		else
		{
			std::this_thread::sleep_for (std::chrono::milliseconds (100));
		}
	}
}
