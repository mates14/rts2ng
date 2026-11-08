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

#include "wormencoder.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

using namespace rts2teld;

namespace
{
	// a reading older than this is no position
	const double WORM_STALE = 0.5;
	// positions kept for the rate: one per WORM_RATE_STEP s over WORM_RATE_WINDOW s
	const double WORM_RATE_WINDOW = 30.0;
	const double WORM_RATE_STEP = 0.1;
	// the log gets a position at most once a second, and while the worm stands
	// (within WORM_LOG_MOVE counts, ~2" of worm angle) once a minute
	const double WORM_LOG_EVERY = 1.0;
	const double WORM_LOG_IDLE = 60.0;
	const int64_t WORM_LOG_MOVE = 100;
	// garbled lines right after opening are the flushed buffer's tail, no fault
	const double WORM_OPEN_SETTLE = 0.2;
}

WormParse rts2teld::parseWormLine (const std::string &line, WormReading &r)
{
	unsigned m, s, e, w, c, cs;
	char tail;
	// %c after the last field: anything left over makes it no reading
	int n = sscanf (line.c_str (), "M:%x S:%x E:%u W:%u C:%x S:%x %c", &m, &s, &e, &w, &c, &cs, &tail);
	if (n != 6 || m > 0xffff || s >= (unsigned) WORM_COUNTS || e > 1 || w > 1 || c > 0x3f || cs > 0x3f)
		return WORM_BAD_LINE;
	r.turns = m;
	r.pos = s;
	r.error = e == 0;
	r.warning = w == 0;
	r.crcRead = c;
	r.crcComputed = cs;
	if (c != cs)
		return WORM_BAD_CRC;
	if (r.error || r.warning)
		return WORM_BAD_FLAGS;
	return WORM_OK;
}

double rts2teld::wormRate (const std::vector<std::pair<double, int64_t>> &pts)
{
	if (pts.size () < 2)
		return NAN;
	// relative to the first point: unwrapped counts reach 2^39
	double t0 = pts.front ().first;
	int64_t p0 = pts.front ().second;
	double st = 0, sp = 0, stt = 0, stp = 0;
	for (auto &q : pts)
	{
		double t = q.first - t0, p = (double) (q.second - p0);
		st += t;
		sp += p;
		stt += t * t;
		stp += t * p;
	}
	double n = pts.size ();
	double d = n * stt - st * st;
	if (d <= 0)
		return NAN;
	return (n * stp - st * sp) / d;
}

WormEncoder::WormEncoder (const std::string &_device, const std::string &_logDir):device (_device), logDir (_logDir)
{
	fd = -1;
	running = false;
	unwrapped = 0;
	haveUnwrapped = false;
	logFile = nullptr;
	lastLogged = 0;
	lastLoggedPos = 0;
}

WormEncoder::~WormEncoder ()
{
	stop ();
}

double WormEncoder::now ()
{
	return std::chrono::duration<double> (std::chrono::system_clock::now ().time_since_epoch ()).count ();
}

void WormEncoder::start ()
{
	if (running)
		return;
	running = true;
	worker = std::thread (&WormEncoder::run, this);
}

void WormEncoder::stop ()
{
	running = false;
	if (worker.joinable ())
		worker.join ();
	closePort ();
	if (logFile)
		fclose (logFile);
	logFile = nullptr;
}

WormStatus WormEncoder::getStatus ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	WormStatus s = status;
	s.valid = s.readings > 0 && now () - s.t < WORM_STALE;
	std::vector<std::pair<double, int64_t>> pts (history.begin (), history.end ());
	// a rate needs most of the window, and readings up to now
	if (s.valid && pts.size () >= 2 && pts.back ().first - pts.front ().first > WORM_RATE_WINDOW / 2)
		s.rate = wormRate (pts);
	else
		s.rate = NAN;
	return s;
}

void WormEncoder::logEvent (const std::string &what)
{
	if (logDir.empty ())
		return;
	WormStatus s = getStatus ();
	char buf[200];
	if (s.valid)
		snprintf (buf, sizeof (buf), "%.3f %s %u %u", now (), what.c_str (), s.turns, s.pos);
	else
		snprintf (buf, sizeof (buf), "%.3f %s - -", now (), what.c_str ());
	std::lock_guard<std::mutex> lock (mutex_);
	pending.push_back (buf);
}

void WormEncoder::note (const std::string &msg)
{
	std::lock_guard<std::mutex> lock (mutex_);
	status.lastMessage = msg;
}

bool WormEncoder::openPort ()
{
	fd = open (device.c_str (), O_RDONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
	{
		note (std::string ("cannot open ") + device + ": " + strerror (errno));
		return false;
	}
	struct termios tio;
	if (tcgetattr (fd, &tio))
	{
		note (std::string ("tcgetattr ") + device + ": " + strerror (errno));
		closePort ();
		return false;
	}
	cfmakeraw (&tio);
	cfsetispeed (&tio, B921600);
	cfsetospeed (&tio, B921600);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~(CSTOPB | CRTSCTS | HUPCL);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr (fd, TCSANOW, &tio))
	{
		note (std::string ("tcsetattr ") + device + ": " + strerror (errno));
		closePort ();
		return false;
	}
	tcflush (fd, TCIFLUSH);
	std::lock_guard<std::mutex> lock (mutex_);
	status.open = true;
	status.lastMessage = "open";
	// a board unplugged meanwhile may have been turned by hand: start the unwrap anew
	haveUnwrapped = false;
	history.clear ();
	return true;
}

void WormEncoder::closePort ()
{
	if (fd >= 0)
		close (fd);
	fd = -1;
	std::lock_guard<std::mutex> lock (mutex_);
	status.open = false;
}

void WormEncoder::run ()
{
	std::string line;
	double opened = 0;
	char buf[4096];
	while (running)
	{
		if (fd < 0)
		{
			flushPending ();
			if (!openPort ())
			{
				for (int i = 0; i < 50 && running; i++)
					usleep (100000);
				continue;
			}
			line.clear ();
			opened = now ();
		}

		fd_set rf;
		FD_ZERO (&rf);
		FD_SET (fd, &rf);
		struct timeval tv = {0, 200000};
		int r = select (fd + 1, &rf, nullptr, nullptr, &tv);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			note (std::string ("select: ") + strerror (errno));
			closePort ();
			continue;
		}
		if (r > 0)
		{
			ssize_t got = read (fd, buf, sizeof (buf));
			if (got < 0 && errno != EAGAIN && errno != EINTR)
			{
				note (std::string ("read: ") + strerror (errno));
				closePort ();
				continue;
			}
			if (got == 0)
			{
				// readable and empty: the device went away (USB unplugged)
				note ("serial port hung up");
				closePort ();
				continue;
			}
			for (ssize_t i = 0; i < got; i++)
			{
				char c = buf[i];
				if (c == '\n' || c == '\r')
				{
					if (!line.empty ())
						handleLine (line, now () - opened < WORM_OPEN_SETTLE);
					line.clear ();
				}
				else if (line.size () < 200)
					line += c;
			}
		}
		else
		{
			note ("no data from the board");
		}
		sampleLog ();
		flushPending ();
	}
}

void WormEncoder::handleLine (const std::string &line, bool settling)
{
	WormReading rd;
	WormParse p = parseWormLine (line, rd);
	double t = now ();
	std::lock_guard<std::mutex> lock (mutex_);
	switch (p)
	{
		case WORM_BAD_LINE:
			if (!settling)
				status.badLines++;
			return;
		case WORM_BAD_CRC:
			status.badCrc++;
			return;
		case WORM_BAD_FLAGS:
			status.badFlags++;
			return;
		case WORM_OK:
			break;
	}
	int64_t raw = (int64_t) rd.turns * WORM_COUNTS + rd.pos;
	if (!haveUnwrapped)
	{
		unwrapped = raw;
		haveUnwrapped = true;
	}
	else
	{
		// the shortest way from the last reading, so the 16-bit turn
		// counter wrapping (every ~136 days of tracking) is no jump
		const int64_t span = (int64_t) 0x10000 * WORM_COUNTS;
		int64_t d = ((raw - unwrapped) % span + span) % span;
		if (d > span / 2)
			d -= span;
		unwrapped += d;
	}
	status.t = t;
	status.turns = rd.turns;
	status.pos = rd.pos;
	status.angle = rd.pos * 360.0 / WORM_COUNTS;
	status.readings++;
	if (history.empty () || t - history.back ().first >= WORM_RATE_STEP)
		history.push_back (std::make_pair (t, unwrapped));
	while (!history.empty () && history.front ().first < t - WORM_RATE_WINDOW)
		history.pop_front ();
}

void WormEncoder::sampleLog ()
{
	if (logDir.empty ())
		return;
	double t;
	unsigned turns, pos;
	int64_t u;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		if (status.readings == 0 || now () - status.t > WORM_STALE)
			return;
		t = status.t;
		turns = status.turns;
		pos = status.pos;
		u = unwrapped;
	}
	if (t - lastLogged < WORM_LOG_EVERY)
		return;
	if (t - lastLogged < WORM_LOG_IDLE && llabs (u - lastLoggedPos) < WORM_LOG_MOVE)
		return;
	char buf[100];
	snprintf (buf, sizeof (buf), "%.3f W %u %u", t, turns, pos);
	writeLog (t, buf);
	lastLogged = t;
	lastLoggedPos = u;
}

void WormEncoder::flushPending ()
{
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		lines.swap (pending);
	}
	for (auto &l : lines)
		writeLog (now (), l);
}

void WormEncoder::writeLog (double t, const std::string &line)
{
	time_t tt = (time_t) t;
	struct tm tm;
	gmtime_r (&tt, &tm);
	char name[40];
	strftime (name, sizeof (name), "worm-%Y%m%d.log", &tm);
	std::string path = logDir + "/" + name;
	if (path != logName || logFile == nullptr)
	{
		if (logFile)
			fclose (logFile);
		logFile = fopen (path.c_str (), "a");
		logName = path;
		if (logFile == nullptr)
		{
			// one note per failing file, and the reading goes on
			note (std::string ("cannot write ") + path + ": " + strerror (errno));
			return;
		}
	}
	fprintf (logFile, "%s\n", line.c_str ());
	fflush (logFile);
}
