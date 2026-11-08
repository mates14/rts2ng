/*
 * D50 RA worm encoder: Broadcom AS38-H39E (BiSS C) behind an STM32 NUCLEO-F411RE.
 * Copyright (C) 2026 Jan Strobl
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

// base note: wholly new, the first stage of periodic error correction for
// D50 [JS 2026-11-08]. The encoder sits on the HA worm (one turn ~179.5 s,
// 3 sidereal minutes). The board reads it and sends, without being asked,
// about 1070 lines a second at 921600 baud:
//	M:0577 S:0F0164 E:1 W:1 C:19 S:19
// M turns (16 bits), S position in the turn (23 bits), E and W the error
// and warning flags (1 = OK), C the CRC from the encoder, the last S the
// CRC the board computed. The board does not check them itself; a reading
// counts only with both CRCs equal and both flags 1.
//
// WormEncoder follows Mpu9250Reader: a thread owns the port and the log
// file, the RTS2 thread only takes a mutex and copies plain data. The
// position is read continuously, so a slow disk or a lost USB device never
// holds up the mount.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rts2teld
{

// counts in one worm turn (S, 23 bits)
const int64_t WORM_COUNTS = 1 << 23;

enum WormParse { WORM_OK, WORM_BAD_LINE, WORM_BAD_CRC, WORM_BAD_FLAGS };

struct WormReading
{
	unsigned turns = 0;		// M
	unsigned pos = 0;		// S, in the turn
	bool error = false;		// E == 0
	bool warning = false;		// W == 0
	unsigned crcRead = 0;		// C
	unsigned crcComputed = 0;	// the second S
};

/** parse one line of the board; WORM_OK only for a reading that can be used */
WormParse parseWormLine (const std::string &line, WormReading &r);

struct WormStatus
{
	bool open = false;		// port open
	bool valid = false;		// a good reading within the last WORM_STALE seconds
	double t = 0;			// time of the last good reading (unix)
	unsigned turns = 0;
	unsigned pos = 0;
	double angle = 0;		// deg, 0..360, in the turn
	double rate = NAN;		// counts/s over the last WORM_RATE_WINDOW s; NaN without enough of them
	unsigned readings = 0;		// good readings since start
	unsigned badLines = 0;		// unparseable (not counted in the first 0.2 s after opening)
	unsigned badCrc = 0;
	unsigned badFlags = 0;
	std::string lastMessage;	// last I/O error or note
};

/**
 * Least-squares slope [counts/s] of unwrapped positions; NaN for fewer than
 * 2 points or a zero time span. Exposed for the test.
 */
double wormRate (const std::vector<std::pair<double, int64_t>> &pts);

class WormEncoder
{
	public:
		/**
		 * @param device  serial port of the board
		 * @param logDir  directory for worm-YYYYMMDD.log (UTC date); empty: no log
		 */
		WormEncoder (const std::string &device, const std::string &logDir);
		~WormEncoder ();

		void start ();
		void stop ();

		WormStatus getStatus ();

		/**
		 * Queue one line for the log, time-stamped and followed by the current
		 * encoder reading (turns pos, or "- -" without a valid one). The thread
		 * writes it; this never touches the disk.
		 */
		void logEvent (const std::string &what);

		const std::string &getDevice () const { return device; }

		static double now ();

	private:
		std::string device;
		std::string logDir;
		int fd;
		std::thread worker;
		std::atomic<bool> running;

		std::mutex mutex_;
		WormStatus status;
		int64_t unwrapped;		// turns * WORM_COUNTS + pos, continued over the 16-bit turn wrap
		bool haveUnwrapped;
		std::deque<std::pair<double, int64_t>> history;	// decimated, for the rate
		std::vector<std::string> pending;		// log lines from logEvent ()

		FILE *logFile;
		std::string logName;
		double lastLogged;
		int64_t lastLoggedPos;

		void run ();
		bool openPort ();
		void closePort ();
		void note (const std::string &msg);
		void handleLine (const std::string &line, bool settling);
		void writeLog (double t, const std::string &line);
		void flushPending ();
		void sampleLog ();
};

}
