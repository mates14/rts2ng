/*
 * MPU-9250 USB IMU (Arduino Nano) reader and GEM gravity model.
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

#include "mpu9250.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

using namespace rts2teld;

namespace
{
	const double DEG = M_PI / 180.0;

	// how long the reader keeps samples for average()
	const double HISTORY_SEC = 60.0;

	// ---- 3x3 helpers, row-major ----
	void matMul (const double a[9], const double b[9], double out[9])
	{
		double r[9];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
		memcpy (out, r, sizeof (r));
	}

	// a^T v
	void matTVec (const double a[9], const double v[3], double out[3])
	{
		double r[3];
		for (int i = 0; i < 3; i++)
			r[i] = a[i] * v[0] + a[3 + i] * v[1] + a[6 + i] * v[2];
		memcpy (out, r, sizeof (r));
	}

	void rotZ (double deg, double out[9])
	{
		double c = cos (deg * DEG), s = sin (deg * DEG);
		double r[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
		memcpy (out, r, sizeof (r));
	}

	void rotY (double deg, double out[9])
	{
		double c = cos (deg * DEG), s = sin (deg * DEG);
		double r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
		memcpy (out, r, sizeof (r));
	}

	// rotation by the vector v (radians), Rodrigues
	void rodrigues (const double v[3], double out[9])
	{
		double th = sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (th < 1e-12)
		{
			double r[9] = {1, -v[2], v[1], v[2], 1, -v[0], -v[1], v[0], 1};
			memcpy (out, r, sizeof (r));
			return;
		}
		double k[3] = {v[0] / th, v[1] / th, v[2] / th};
		double c = cos (th), s = sin (th), C = 1 - c;
		double r[9] = {
			c + k[0] * k[0] * C, k[0] * k[1] * C - k[2] * s, k[0] * k[2] * C + k[1] * s,
			k[1] * k[0] * C + k[2] * s, c + k[1] * k[1] * C, k[1] * k[2] * C - k[0] * s,
			k[2] * k[0] * C - k[1] * s, k[2] * k[1] * C + k[0] * s, c + k[2] * k[2] * C};
		memcpy (out, r, sizeof (r));
	}

	double norm3 (const double v[3])
	{
		return sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	}

	double angleDeg (const double a[3], const double b[3])
	{
		double na = norm3 (a), nb = norm3 (b);
		if (na == 0 || nb == 0)
			return NAN;
		// atan2 of cross and dot: accurate for small angles too
		double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
		return atan2 (sqrt (cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / DEG;
	}

	// solve A x = b in place, A n x n; false if singular
	bool solveLinear (std::vector<double> A, std::vector<double> b, int n, std::vector<double> &x)
	{
		for (int c = 0; c < n; c++)
		{
			int p = c;
			for (int r = c + 1; r < n; r++)
				if (fabs (A[r * n + c]) > fabs (A[p * n + c]))
					p = r;
			if (fabs (A[p * n + c]) < 1e-300)
				return false;
			if (p != c)
			{
				for (int k = 0; k < n; k++)
					std::swap (A[c * n + k], A[p * n + k]);
				std::swap (b[c], b[p]);
			}
			for (int r = c + 1; r < n; r++)
			{
				double f = A[r * n + c] / A[c * n + c];
				for (int k = c; k < n; k++)
					A[r * n + k] -= f * A[c * n + k];
				b[r] -= f * b[c];
			}
		}
		x.assign (n, 0);
		for (int r = n - 1; r >= 0; r--)
		{
			double s = b[r];
			for (int k = r + 1; k < n; k++)
				s -= A[r * n + k] * x[k];
			x[r] = s / A[r * n + r];
		}
		return true;
	}

	// the 24 proper rotations mapping axes onto axes - starting points for
	// the fit, since a sensor is usually mounted square to something
	std::vector<std::vector<double>> cubeRotations ()
	{
		std::vector<std::vector<double>> out;
		int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
		for (auto &p : perm)
			for (int sg = 0; sg < 8; sg++)
			{
				std::vector<double> m (9, 0);
				for (int i = 0; i < 3; i++)
					m[i * 3 + p[i]] = (sg >> i) & 1 ? -1 : 1;
				double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
				if (det > 0)
					out.push_back (m);
			}
		return out;
	}
}

// ---------------------------------------------------------------- parsing

bool rts2teld::parseMpu9250Raw (const std::string &line, ImuSample &s)
{
	if (line.empty () || line[0] == '#')
		return false;
	std::istringstream is (line);
	std::string f[11];
	int n = 0;
	while (n < 11 && is >> f[n])
		n++;
	std::string extra;
	if (n != 11 || (is >> extra))
		return false;

	long v[11];
	for (int i = 0; i < 11; i++)
	{
		if (i >= 7 && f[i] == "NA")
		{
			v[i] = 0;
			continue;
		}
		char *end;
		v[i] = strtol (f[i].c_str (), &end, 10);
		if (*end != '\0' || f[i].empty ())
			return false;
	}
	for (int i = 0; i < 3; i++)
	{
		s.acc[i] = v[i] / 16384.0;
		s.gyro[i] = v[3 + i] / 131.0;
	}
	s.temp = v[6] / 333.87 + 21.0;
	s.magValid = f[7] != "NA" && f[8] != "NA" && f[9] != "NA";
	for (int i = 0; i < 3; i++)
		s.mag[i] = s.magValid ? v[7 + i] * 0.15 : 0;
	s.magOverflow = f[10] != "NA" && v[10] != 0;
	return true;
}

// ---------------------------------------------------------------- reader

Mpu9250Reader::Mpu9250Reader (const std::string &_device, double pollHz):device (_device)
{
	pollInterval = pollHz > 0 ? 1.0 / pollHz : 0.1;
	fd = -1;
	running = false;
}

Mpu9250Reader::~Mpu9250Reader ()
{
	stop ();
}

double Mpu9250Reader::now ()
{
	return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

bool Mpu9250Reader::start ()
{
	if (running)
		return true;
	running = true;
	worker = std::thread (&Mpu9250Reader::run, this);
	return true;
}

void Mpu9250Reader::stop ()
{
	running = false;
	if (worker.joinable ())
		worker.join ();
	closePort ();
}

ImuStatus Mpu9250Reader::getStatus ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	return status;
}

bool Mpu9250Reader::average (double windowSec, int minSamples, ImuAverage &out)
{
	std::lock_guard<std::mutex> lock (mutex_);
	double from = now () - windowSec;
	out = ImuAverage ();
	std::vector<const ImuSample *> win;
	for (auto it = history.rbegin (); it != history.rend () && it->t >= from; ++it)
		win.push_back (&*it);
	out.n = win.size ();
	if (win.empty ())
		return false;
	out.t1 = win.front ()->t;
	out.t0 = win.back ()->t;

	auto median = [] (std::vector<double> v)
	{
		std::nth_element (v.begin (), v.begin () + v.size () / 2, v.end ());
		return v[v.size () / 2];
	};

	// The serial line drops characters: on SBT a reply's field sometimes
	// arrives a digit short ("15018" as "1501"), a tenth of the true value,
	// and the line still parses - 2026-10-02 evening it was several samples
	// a second, read as 0.15 g of "vibration". The window's per-axis median
	// is safe; samples off it by more than any real rest scatter are dropped
	// before the mean and the scatter are taken.
	double accMed[3], gyroMed[3];
	for (int i = 0; i < 3; i++)
	{
		std::vector<double> a, g;
		for (auto *s : win)
		{
			a.push_back (s->acc[i]);
			g.push_back (s->gyro[i]);
		}
		accMed[i] = median (a);
		gyroMed[i] = median (g);
	}
	double sum[3] = {0, 0, 0}, sum2[3] = {0, 0, 0}, gsum[3] = {0, 0, 0}, gsum2[3] = {0, 0, 0};
	std::vector<double> mags[3], temps;
	int n = 0;
	for (auto *s : win)
	{
		bool good = true;
		for (int i = 0; i < 3; i++)
			if (fabs (s->acc[i] - accMed[i]) > 0.03 || fabs (s->gyro[i] - gyroMed[i]) > 2.0)
				good = false;
		if (!good)
		{
			out.accOutliers++;
			continue;
		}
		for (int i = 0; i < 3; i++)
		{
			sum[i] += s->acc[i];
			sum2[i] += s->acc[i] * s->acc[i];
			gsum[i] += s->gyro[i];
			gsum2[i] += s->gyro[i] * s->gyro[i];
		}
		temps.push_back (s->temp);
		n++;
	}
	for (auto *s : win)
		if (s->magValid && !s->magOverflow)
			for (int i = 0; i < 3; i++)
				mags[i].push_back (s->mag[i]);
	// mostly bad samples: the median itself is not to be trusted
	if (n < minSamples || n == 0 || 2 * n < (int) win.size ())
		return false;
	double va = 0, vg = 0;
	for (int i = 0; i < 3; i++)
	{
		out.acc[i] = sum[i] / n;
		out.gyro[i] = gsum[i] / n;
		va += std::max (0.0, sum2[i] / n - out.acc[i] * out.acc[i]);
		vg += std::max (0.0, gsum2[i] / n - out.gyro[i] * out.gyro[i]);
	}
	out.accStd = sqrt (va / 3);
	out.gyroStd = sqrt (vg / 3);
	out.temp = median (temps);
	// The magnetometer: a median, not a mean. At rest on SBT about one 2 s
	// window in eight had one axis pulled 6-18 uT off by single bad samples,
	// while clean windows scatter by 0.2-0.3 uT.
	size_t nm = mags[0].size ();
	out.magValid = nm > 0;
	if (nm > 0)
	{
		for (int i = 0; i < 3; i++)
		{
			std::vector<double> v = mags[i];
			std::nth_element (v.begin (), v.begin () + nm / 2, v.end ());
			out.mag[i] = v[nm / 2];
		}
		for (size_t k = 0; k < nm; k++)
			for (int i = 0; i < 3; i++)
				if (fabs (mags[i][k] - out.mag[i]) > 3.0)
				{
					out.magOutliers++;
					break;
				}
	}
	return true;
}

void Mpu9250Reader::note (const std::string &msg, bool error)
{
	std::lock_guard<std::mutex> lock (mutex_);
	status.lastMessage = msg;
	if (error)
		status.errors++;
}

bool Mpu9250Reader::openPort ()
{
	fd = open (device.c_str (), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
	{
		note (std::string ("cannot open ") + device + ": " + strerror (errno), true);
		return false;
	}
	struct termios tio;
	if (tcgetattr (fd, &tio))
	{
		note (std::string ("tcgetattr ") + device + ": " + strerror (errno), true);
		closePort ();
		return false;
	}
	cfmakeraw (&tio);
	cfsetispeed (&tio, B115200);
	cfsetospeed (&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~(CSTOPB | CRTSCTS);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr (fd, TCSANOW, &tio))
	{
		note (std::string ("tcsetattr ") + device + ": " + strerror (errno), true);
		closePort ();
		return false;
	}
	tcflush (fd, TCIOFLUSH);
	std::lock_guard<std::mutex> lock (mutex_);
	status.open = true;
	status.ready = false;
	return true;
}

void Mpu9250Reader::closePort ()
{
	if (fd >= 0)
		close (fd);
	fd = -1;
	std::lock_guard<std::mutex> lock (mutex_);
	status.open = false;
	status.ready = false;
}

bool Mpu9250Reader::readLine (std::string &line, double timeoutSec)
{
	line.clear ();
	double deadline = now () + timeoutSec;
	while (running)
	{
		double left = deadline - now ();
		if (left <= 0)
			return false;
		fd_set rf;
		FD_ZERO (&rf);
		FD_SET (fd, &rf);
		struct timeval tv;
		tv.tv_sec = (long) left;
		tv.tv_usec = (long) ((left - tv.tv_sec) * 1e6);
		int r = select (fd + 1, &rf, nullptr, nullptr, &tv);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			note (std::string ("select: ") + strerror (errno), true);
			return false;
		}
		if (r == 0)
			return false;
		char c;
		ssize_t got = read (fd, &c, 1);
		if (got < 0)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;
			note (std::string ("read: ") + strerror (errno), true);
			return false;
		}
		if (got == 0)
		{
			// select said readable, nothing came: the device went away (USB unplugged)
			note ("serial port hung up", true);
			return false;
		}
		if (c == '\n')
		{
			if (!line.empty ())
				return true;
			continue;
		}
		if (c != '\r')
			line += c;
		if (line.size () > 256)
			line.clear ();
	}
	return false;
}

void Mpu9250Reader::run ()
{
	int timeouts = 0;
	while (running)
	{
		if (fd < 0)
		{
			if (!openPort ())
			{
				for (int i = 0; i < 50 && running; i++)
					usleep (100000);
				continue;
			}
			// opening resets the board: give the bootloader its time and
			// wait for "# READY"; a board that does not reset (DTR held,
			// different USB chip) never says it, so carry on after 4 s
			std::string line;
			double until = now () + 4.0;
			while (running && now () < until)
			{
				if (readLine (line, until - now ()) && line == "# READY")
					break;
			}
			std::lock_guard<std::mutex> lock (mutex_);
			status.ready = true;
			status.lastMessage = line == "# READY" ? "ready" : "no \"# READY\" from the board, polling anyway";
			timeouts = 0;
		}

		double t0 = now ();
		if (write (fd, "r", 1) != 1)
		{
			note (std::string ("write: ") + strerror (errno), true);
			closePort ();
			continue;
		}

		// one data line, or a '#' line saying why there is none
		std::string line;
		bool gotData = false;
		ImuSample s;
		while (running && readLine (line, 0.5))
		{
			if (line[0] == '#')
			{
				note (line, line.compare (0, 5, "# ERR") == 0);
				if (line.compare (0, 5, "# ERR") == 0)
					break;
				continue;
			}
			if (parseMpu9250Raw (line, s))
				gotData = true;
			else
				note ("unparseable reply: " + line, true);
			break;
		}

		if (gotData)
		{
			timeouts = 0;
			s.t = now ();
			std::lock_guard<std::mutex> lock (mutex_);
			history.push_back (s);
			while (!history.empty () && history.front ().t < s.t - HISTORY_SEC)
				history.pop_front ();
			status.last = s;
			status.haveSample = true;
			status.samples++;
		}
		else if (line.empty ())
		{
			note ("no reply to 'r'", true);
			// a board unplugged and replugged comes back as a new device
			// node behind the same name - reopen after a few silences
			if (++timeouts >= 5)
				closePort ();
		}

		double left = pollInterval - (now () - t0);
		if (left > 0 && running)
			usleep ((useconds_t) (left * 1e6));
	}
}

// ---------------------------------------------------------------- model

ImuMountModel::ImuMountModel ()
{
	setLatitude (45);
	for (int i = 0; i < 9; i++)
		M[i] = i % 4 == 0 ? 1 : 0;
	bias[0] = bias[1] = bias[2] = 0;
	scale = 1;
	raSign = decSign = 1;
	fitted = false;
	fitRms = NAN;
	nFit = 0;
	fullModel = false;
	magFitted = false;
	for (double &c : magCoef)
		c = 0;
	magFitRms = NAN;
	nMagFit = 0;
}

void ImuMountModel::setLatitude (double latDeg)
{
	// the polar axis points at the elevated pole either way; a southern
	// mount differs in the sense its counters run, which the fit finds
	double l = fabs (latDeg) * DEG;
	zen[0] = cos (l);
	zen[1] = 0;
	zen[2] = sin (l);
}

void ImuMountModel::upFromAxes (const double Mr[9], int rs, int ds, double raAxis, double decAxis, double out[3]) const
{
	double ra = 180.0 + rs * (raAxis - 180.0);
	double dec = 180.0 + ds * (decAxis - 180.0);
	double Rz[9], Ry[9], T[9];
	rotZ (ra - 90.0, Rz);
	rotY (180.0 - dec, Ry);
	matMul (Rz, Ry, T);
	double tube[3];
	matTVec (T, zen, tube);
	matTVec (Mr, tube, out);
}

void ImuMountModel::predictUp (double raAxis, double decAxis, double out[3]) const
{
	upFromAxes (M, raSign, decSign, raAxis, decAxis, out);
}

double ImuMountModel::tubeAltitude (double raAxis, double decAxis) const
{
	// up in tube coordinates; the optical axis is the tube's z
	double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, up[3];
	upFromAxes (I, raSign, decSign, raAxis, decAxis, up);
	return asin (std::max (-1.0, std::min (1.0, up[2]))) / DEG;
}

void ImuMountModel::correctAcc (const double acc[3], double out[3]) const
{
	for (int i = 0; i < 3; i++)
		out[i] = (acc[i] - bias[i]) / scale;
	double n = norm3 (out);
	if (n > 0)
		for (int i = 0; i < 3; i++)
			out[i] /= n;
}

double ImuMountModel::errorDeg (double raAxis, double decAxis, const double acc[3]) const
{
	double p[3], m[3];
	predictUp (raAxis, decAxis, p);
	correctAcc (acc, m);
	return angleDeg (p, m);
}

// Levenberg-Marquardt from Mr (updated in place). Parameters: a small
// rotation applied on the sensor side of Mr, then (full) bias and scale.
// Rotation-only fits compare directions (normalised measurements, so an
// accelerometer gain error does not matter); full fits compare the raw
// vectors in g. Returns the RMS angle between measured and predicted, deg.
double ImuMountModel::fitFrom (const std::vector<CalSample> &samples, double Mr[9], double b[3], double &s, int rs, int ds, bool full, int iterations) const
{
	const int np = full ? 7 : 3;
	const int nr = 3 * samples.size ();

	auto residuals = [&] (const double Mt[9], const double bt[3], double st, std::vector<double> &r)
	{
		r.resize (nr);
		for (size_t k = 0; k < samples.size (); k++)
		{
			double up[3];
			upFromAxes (Mt, rs, ds, samples[k].raAxis, samples[k].decAxis, up);
			const double *a = samples[k].acc;
			if (full)
			{
				for (int i = 0; i < 3; i++)
					r[3 * k + i] = a[i] - (st * up[i] + bt[i]);
			}
			else
			{
				double n = norm3 (a);
				for (int i = 0; i < 3; i++)
					r[3 * k + i] = a[i] / n - up[i];
			}
		}
	};
	auto apply = [&] (const double p[7], double Mt[9], double bt[3], double &st)
	{
		double R[9];
		rodrigues (p, R);
		matMul (Mr, R, Mt);
		for (int i = 0; i < 3; i++)
			bt[i] = full ? b[i] + p[3 + i] : b[i];
		st = full ? s + p[6] : s;
	};
	auto cost = [] (const std::vector<double> &r)
	{
		double c = 0;
		for (double v : r)
			c += v * v;
		return c;
	};

	std::vector<double> r0, r1;
	residuals (Mr, b, s, r0);
	double c0 = cost (r0);
	double lambda = 1e-3;
	for (int it = 0; it < iterations; it++)
	{
		// numeric Jacobian at p = 0
		std::vector<double> J (nr * np);
		for (int j = 0; j < np; j++)
		{
			double p[7] = {0, 0, 0, 0, 0, 0, 0};
			const double h = 1e-6;
			p[j] = h;
			double Mt[9], bt[3], st;
			apply (p, Mt, bt, st);
			residuals (Mt, bt, st, r1);
			for (int i = 0; i < nr; i++)
				J[i * np + j] = (r1[i] - r0[i]) / h;
		}
		std::vector<double> JtJ (np * np, 0), Jtr (np, 0);
		for (int i = 0; i < nr; i++)
			for (int j = 0; j < np; j++)
			{
				Jtr[j] -= J[i * np + j] * r0[i];
				for (int k = 0; k < np; k++)
					JtJ[j * np + k] += J[i * np + j] * J[i * np + k];
			}

		bool improved = false;
		for (int tries = 0; tries < 8 && !improved; tries++)
		{
			std::vector<double> A = JtJ, step;
			for (int j = 0; j < np; j++)
				A[j * np + j] *= 1 + lambda;
			for (int j = 0; j < np; j++)
				A[j * np + j] += 1e-12;
			if (!solveLinear (A, Jtr, np, step))
				break;
			double p[7] = {0, 0, 0, 0, 0, 0, 0};
			for (int j = 0; j < np; j++)
				p[j] = step[j];
			double Mt[9], bt[3], st;
			apply (p, Mt, bt, st);
			residuals (Mt, bt, st, r1);
			double c1 = cost (r1);
			if (c1 < c0)
			{
				memcpy (Mr, Mt, sizeof (Mt));
				memcpy (b, bt, sizeof (bt));
				s = st;
				bool converged = c0 - c1 < 1e-14 * (1 + c0);
				r0 = r1;
				c0 = c1;
				lambda = std::max (lambda / 10, 1e-9);
				improved = true;
				if (converged)
					it = iterations;
			}
			else
				lambda *= 10;
		}
		if (!improved)
			break;
	}

	double sum2 = 0;
	for (const auto &cs : samples)
	{
		double up[3], m[3];
		upFromAxes (Mr, rs, ds, cs.raAxis, cs.decAxis, up);
		for (int i = 0; i < 3; i++)
			m[i] = full ? (cs.acc[i] - b[i]) / s : cs.acc[i];
		double e = angleDeg (up, m);
		sum2 += e * e;
	}
	return samples.empty () ? NAN : sqrt (sum2 / samples.size ());
}

bool ImuMountModel::fit (const std::vector<CalSample> &samples, int fullModelFrom)
{
	fitted = false;
	nFit = samples.size ();
	fitRms = NAN;
	if (samples.size () < 3)
		return false;

	// orientation first, from every axis-aligned start and both senses of
	// both axes; then bias and gain on the best, if there is enough to fit
	// them from
	double bestRms = INFINITY, bestM[9];
	int bestRs = 1, bestDs = 1;
	for (const auto &start : cubeRotations ())
		for (int rs = -1; rs <= 1; rs += 2)
			for (int ds = -1; ds <= 1; ds += 2)
			{
				double Mr[9], b[3] = {0, 0, 0}, s = 1;
				memcpy (Mr, start.data (), sizeof (Mr));
				double e = fitFrom (samples, Mr, b, s, rs, ds, false, 40);
				if (e < bestRms)
				{
					bestRms = e;
					memcpy (bestM, Mr, sizeof (Mr));
					bestRs = rs;
					bestDs = ds;
				}
			}
	if (!std::isfinite (bestRms))
		return false;

	memcpy (M, bestM, sizeof (M));
	raSign = bestRs;
	decSign = bestDs;
	bias[0] = bias[1] = bias[2] = 0;
	scale = 1;
	fullModel = false;
	fitRms = bestRms;
	if ((int) samples.size () >= fullModelFrom)
	{
		double Mr[9], b[3] = {0, 0, 0}, s = 1;
		memcpy (Mr, bestM, sizeof (Mr));
		double e = fitFrom (samples, Mr, b, s, raSign, decSign, true, 100);
		// a bias the size of gravity means the data did not constrain it
		if (std::isfinite (e) && e <= bestRms && s > 0.8 && s < 1.2 && norm3 (b) < 0.2)
		{
			memcpy (M, Mr, sizeof (M));
			memcpy (bias, b, sizeof (bias));
			scale = s;
			fullModel = true;
			fitRms = e;
		}
	}
	fitted = true;
	return true;
}

bool ImuMountModel::solveAxes (const double acc[3], double raAxis, double decAxis, double &raOut, double &decOut, double &condDeg) const
{
	raOut = decOut = condDeg = NAN;
	if (!fitted)
		return false;
	double m[3];
	correctAcc (acc, m);
	double x[2] = {raAxis, decAxis};
	double JtJ[4] = {0, 0, 0, 0};
	for (int it = 0; it < 30; it++)
	{
		double p[3], pr[3], pd[3];
		const double h = 1e-4;
		predictUp (x[0], x[1], p);
		predictUp (x[0] + h, x[1], pr);
		predictUp (x[0], x[1] + h, pd);
		double J[3][2], r[3];
		for (int i = 0; i < 3; i++)
		{
			J[i][0] = (pr[i] - p[i]) / h;
			J[i][1] = (pd[i] - p[i]) / h;
			r[i] = m[i] - p[i];
		}
		double a = 0, b = 0, d = 0, g0 = 0, g1 = 0;
		for (int i = 0; i < 3; i++)
		{
			a += J[i][0] * J[i][0];
			b += J[i][0] * J[i][1];
			d += J[i][1] * J[i][1];
			g0 += J[i][0] * r[i];
			g1 += J[i][1] * r[i];
		}
		JtJ[0] = a; JtJ[1] = b; JtJ[2] = b; JtJ[3] = d;
		double det = a * d - b * b;
		if (det <= 0)
			return false;
		double s0 = (d * g0 - b * g1) / det, s1 = (a * g1 - b * g0) / det;
		// keep steps sane near the degenerate poses
		double len = sqrt (s0 * s0 + s1 * s1);
		if (len > 20)
		{
			s0 *= 20 / len;
			s1 *= 20 / len;
		}
		x[0] += s0;
		x[1] += s1;
		if (len < 1e-6)
			break;
	}
	// largest eigenvalue of (J^T J)^-1, J in unit vector per degree
	double a = JtJ[0], b = JtJ[1], d = JtJ[3];
	double det = a * d - b * b;
	if (det <= 0)
		return false;
	double tr = a + d;
	double lmin = tr / 2 - sqrt (std::max (0.0, tr * tr / 4 - det));
	if (lmin <= 0)
		return false;
	condDeg = DEG / sqrt (lmin);
	raOut = x[0];
	decOut = x[1];
	return true;
}

std::vector<ImuMountModel::AxisSolution> ImuMountModel::solveAll (const double acc[3], double maxErrDeg) const
{
	std::vector<AxisSolution> out;
	if (!fitted)
		return out;
	for (double r0 = 0; r0 < 360; r0 += 30)
		for (double d0 = 0; d0 < 360; d0 += 30)
		{
			double r, d, cond;
			if (!solveAxes (acc, r0, d0, r, d, cond))
				continue;
			r = fmod (fmod (r, 360.0) + 360.0, 360.0);
			d = fmod (fmod (d, 360.0) + 360.0, 360.0);
			double e = errorDeg (r, d, acc);
			if (!(e <= maxErrDeg))
				continue;
			AxisSolution sol {r, d, e, cond};
			bool known = false;
			for (auto &o : out)
			{
				double dr = fabs (fmod (o.raAxis - r + 540.0, 360.0) - 180.0), dd = fabs (fmod (o.decAxis - d + 540.0, 360.0) - 180.0);
				if (dr < 1.0 && dd < 1.0)
				{
					known = true;
					if (e < o.errDeg)
						o = sol;
					break;
				}
			}
			if (!known)
				out.push_back (sol);
		}
	std::sort (out.begin (), out.end (), [] (const AxisSolution &a, const AxisSolution &b) { return a.errDeg < b.errDeg; });
	return out;
}

std::vector<ImuMountModel::AxisSolution> ImuMountModel::solveJoint (const double acc1[3], const double acc2[3], double dRa, double dDec, double maxErrDeg) const
{
	std::vector<AxisSolution> out;
	if (!fitted)
		return out;
	double m1[3], m2[3];
	correctAcc (acc1, m1);
	correctAcc (acc2, m2);
	auto residuals = [&] (double r, double d, double res[6])
	{
		double p1[3], p2[3];
		predictUp (r, d, p1);
		predictUp (r + dRa, d + dDec, p2);
		for (int i = 0; i < 3; i++)
		{
			res[i] = m1[i] - p1[i];
			res[3 + i] = m2[i] - p2[i];
		}
	};
	for (double r0 = 0; r0 < 360; r0 += 20)
		for (double d0 = 0; d0 < 360; d0 += 20)
		{
			double x[2] = {r0, d0};
			bool ok = true;
			double lmin = 0;
			for (int it = 0; it < 60 && ok; it++)
			{
				const double h = 1e-4;
				double r[6], rr[6], rd[6];
				residuals (x[0], x[1], r);
				residuals (x[0] + h, x[1], rr);
				residuals (x[0], x[1] + h, rd);
				double a = 0, b = 0, d = 0, g0 = 0, g1 = 0;
				for (int i = 0; i < 6; i++)
				{
					double j0 = -(rr[i] - r[i]) / h, j1 = -(rd[i] - r[i]) / h;
					a += j0 * j0;
					b += j0 * j1;
					d += j1 * j1;
					g0 += j0 * r[i];
					g1 += j1 * r[i];
				}
				double det = a * d - b * b;
				if (det <= 0)
				{
					ok = false;
					break;
				}
				lmin = (a + d) / 2 - sqrt (std::max (0.0, (a + d) * (a + d) / 4 - det));
				double s0 = (d * g0 - b * g1) / det, s1 = (a * g1 - b * g0) / det;
				double len = sqrt (s0 * s0 + s1 * s1);
				if (len > 20)
				{
					s0 *= 20 / len;
					s1 *= 20 / len;
				}
				x[0] += s0;
				x[1] += s1;
				if (len < 1e-7)
					break;
			}
			if (!ok)
				continue;
			double r = fmod (fmod (x[0], 360.0) + 360.0, 360.0);
			double d = fmod (fmod (x[1], 360.0) + 360.0, 360.0);
			double e1 = errorDeg (r, d, acc1), e2 = errorDeg (r + dRa, d + dDec, acc2);
			double e = sqrt ((e1 * e1 + e2 * e2) / 2);
			if (!(e <= maxErrDeg) || lmin <= 0)
				continue;
			AxisSolution sol {r, d, e, DEG / sqrt (lmin)};
			bool known = false;
			for (auto &o : out)
			{
				double dr = fabs (fmod (o.raAxis - r + 540.0, 360.0) - 180.0), dd = fabs (fmod (o.decAxis - d + 540.0, 360.0) - 180.0);
				if (dr < 1.0 && dd < 1.0)
				{
					known = true;
					if (e < o.errDeg)
						o = sol;
					break;
				}
			}
			if (!known)
				out.push_back (sol);
		}
	std::sort (out.begin (), out.end (), [] (const AxisSolution &a, const AxisSolution &b) { return a.errDeg < b.errDeg; });
	return out;
}

namespace
{
	void magBasis (double raAxis, double decAxis, double b[9])
	{
		double cr = cos (raAxis * DEG), sr = sin (raAxis * DEG), cd = cos (decAxis * DEG), sd = sin (decAxis * DEG);
		double r[3] = {1, cr, sr}, d[3] = {1, cd, sd};
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				b[i * 3 + j] = r[i] * d[j];
	}
}

bool ImuMountModel::fitMag (const std::vector<CalSample> &samples, int magMinSamples, double minSpanDeg)
{
	magFitted = false;
	magFitRms = NAN;
	std::vector<const CalSample *> use;
	double raLo = INFINITY, raHi = -INFINITY, decLo = INFINITY, decHi = -INFINITY;
	for (const auto &cs : samples)
		if (cs.magValid)
		{
			use.push_back (&cs);
			raLo = std::min (raLo, cs.raAxis);
			raHi = std::max (raHi, cs.raAxis);
			decLo = std::min (decLo, cs.decAxis);
			decHi = std::max (decHi, cs.decAxis);
		}
	nMagFit = use.size ();
	if ((int) use.size () < magMinSamples || raHi - raLo < minSpanDeg || decHi - decLo < minSpanDeg)
		return false;

	// the three axes share the design matrix: one normal matrix, three right-hand sides
	std::vector<double> A (81, 0);
	std::vector<double> rhs[3] = {std::vector<double> (9, 0), std::vector<double> (9, 0), std::vector<double> (9, 0)};
	for (const auto *cs : use)
	{
		double b[9];
		magBasis (cs->raAxis, cs->decAxis, b);
		for (int i = 0; i < 9; i++)
		{
			for (int j = 0; j < 9; j++)
				A[i * 9 + j] += b[i] * b[j];
			for (int k = 0; k < 3; k++)
				rhs[k][i] += b[i] * cs->mag[k];
		}
	}
	for (int k = 0; k < 3; k++)
	{
		std::vector<double> x;
		if (!solveLinear (A, rhs[k], 9, x))
			return false;
		for (int i = 0; i < 9; i++)
			magCoef[k * 9 + i] = x[i];
	}
	magFitted = true;
	double sum2 = 0;
	for (const auto *cs : use)
	{
		double e = magErrorUT (cs->raAxis, cs->decAxis, cs->mag);
		sum2 += e * e;
	}
	magFitRms = sqrt (sum2 / use.size ());
	return true;
}

void ImuMountModel::predictMag (double raAxis, double decAxis, double out[3]) const
{
	double b[9];
	magBasis (raAxis, decAxis, b);
	for (int k = 0; k < 3; k++)
	{
		out[k] = 0;
		for (int i = 0; i < 9; i++)
			out[k] += magCoef[k * 9 + i] * b[i];
	}
}

double ImuMountModel::magErrorUT (double raAxis, double decAxis, const double mag[3]) const
{
	if (!magFitted)
		return NAN;
	double p[3];
	predictMag (raAxis, decAxis, p);
	return sqrt ((mag[0] - p[0]) * (mag[0] - p[0]) + (mag[1] - p[1]) * (mag[1] - p[1]) + (mag[2] - p[2]) * (mag[2] - p[2]));
}

std::string ImuMountModel::describe () const
{
	if (!fitted)
		return "not calibrated";
	char buf[320];
	snprintf (buf, sizeof (buf), "%s fit of %d samples, rms %.3f deg; axes %s/%s; bias %+.4f %+.4f %+.4f g, scale %.4f; "
		"sensor x,y,z in tube frame (%+.2f %+.2f %+.2f) (%+.2f %+.2f %+.2f) (%+.2f %+.2f %+.2f)",
		fullModel ? "full" : "orientation-only", nFit, fitRms, raSign > 0 ? "+" : "-", decSign > 0 ? "+" : "-",
		bias[0], bias[1], bias[2], scale,
		M[0], M[3], M[6], M[1], M[4], M[7], M[2], M[5], M[8]);
	std::string out = buf;
	if (magFitted)
		snprintf (buf, sizeof (buf), "; magnetometer fit of %d samples, rms %.2f uT", nMagFit, magFitRms);
	else
		snprintf (buf, sizeof (buf), "; magnetometer not fitted (%d samples)", nMagFit);
	return out + buf;
}
