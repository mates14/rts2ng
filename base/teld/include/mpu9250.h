/*
 * MPU-9250 USB IMU (Arduino Nano) reader, and the mount geometry that turns
 * its gravity vector into a check of a German equatorial mount's axes.
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

// base note: wholly new. The sensor is described in
// base/teld/gemini-udp/mpu9250_nano_report.md: 115200 8N1, the board resets
// when the port is opened and says "# READY", then answers 'r' with one line
// of 11 raw fields. Lines starting with '#' are never data.
//
// Mpu9250Reader follows GeminiCaringLoop's pattern: a dedicated thread owns
// the serial port and does ordinary blocking I/O; the RTS2 thread only ever
// takes a mutex and copies plain data out. Nothing here knows about RTS2.
//
// ImuMountModel is the geometry. A sensor fixed anywhere on the part of the
// mount that moves with both axes (tube, saddle, Dec housing on the tube
// side) sees gravity at a direction fixed by the two axis angles - two
// numbers from two numbers, so a calibrated sensor checks both axes
// independently of the motor encoders. Its mounting orientation is not
// assumed; it is fitted from rest samples taken while the counters are
// trusted, see fit().

#ifndef __RTS2_TELD_MPU9250__
#define __RTS2_TELD_MPU9250__

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rts2teld
{

struct ImuSample
{
	double t = 0;			// Mpu9250Reader::now ()
	double acc[3] = {0, 0, 0};	// g
	double gyro[3] = {0, 0, 0};	// deg/s
	double temp = 0;		// deg C, die temperature
	bool magValid = false;
	double mag[3] = {0, 0, 0};	// uT, raw chip axes (NOT the accel frame - see the report)
	bool magOverflow = false;
};

/** mean and spread of the samples in a time window */
struct ImuAverage
{
	int n = 0;			// samples in the window, outliers included
	double acc[3] = {0, 0, 0};	// mean of the good samples
	double accStd = 0;		// g, RMS over the three axes of the per-axis standard deviation, good samples
	double gyro[3] = {0, 0, 0};
	double gyroStd = 0;		// deg/s, same
	double temp = 0;
	bool magValid = false;
	double mag[3] = {0, 0, 0};	// per-axis MEDIAN: single corrupt samples are common, see average()
	int magOutliers = 0;		// samples more than 3 uT from that median on some axis
	int accOutliers = 0;		// samples dropped from acc/gyro: off the window median by more than 0.03 g / 2 deg/s (corrupt serial lines)
	double t0 = 0, t1 = 0;
};

struct ImuStatus
{
	bool open = false;		// port is open
	bool ready = false;		// "# READY" seen (or the board did not reset and answers anyway)
	bool haveSample = false;
	ImuSample last;
	unsigned samples = 0;		// since start
	unsigned errors = 0;		// unparseable replies, '# ERR' lines, timeouts
	std::string lastMessage;	// last '#' line or I/O error
};

class Mpu9250Reader
{
	public:
		Mpu9250Reader (const std::string &device, double pollHz = 10.0);
		~Mpu9250Reader ();

		bool start ();
		void stop ();

		ImuStatus getStatus ();

		/** average of the samples newer than now () - windowSec; false when there are fewer than minSamples */
		bool average (double windowSec, int minSamples, ImuAverage &out);

		/** the clock sample timestamps are in (monotonic) */
		static double now ();

		const std::string &getDevice () const { return device; }

	private:
		std::string device;
		double pollInterval;
		int fd;
		std::thread worker;
		std::atomic<bool> running;

		std::mutex mutex_;
		ImuStatus status;
		std::deque<ImuSample> history;

		void run ();
		bool openPort ();
		void closePort ();
		// one line, without CR/LF; false on timeout or I/O error
		bool readLine (std::string &line, double timeoutSec);
		void note (const std::string &msg, bool error);
};

/** parse one 'r' reply into physical units; false if it is not a data line */
bool parseMpu9250Raw (const std::string &line, ImuSample &s);

/**
 * German equatorial mount axes -> gravity in the sensor frame.
 *
 * Mount frame: z along the polar axis towards the elevated pole, x towards
 * hour angle 0 on the equator. Axis angles are Gemini's counters in degrees
 * (counter / ticks per degree), CWD = (180, 180); the firmware's own relation
 * (see computeCounterError()) is RA axis 90 - HA, Dec axis Dec + 90 on the W
 * side, which gives the tube pointing T(ra, dec) * z with
 *	T = Rz(ra - 90) * Ry(180 - dec)
 * on both sides. The accelerometer at rest reads the "up" unit vector:
 *	a = scale * M^T * T^T * zenith + bias,   zenith = (cos lat, 0, sin lat)
 * M is the sensor's orientation on the tube. raSign/decSign allow for counters
 * running the other way (southern hemisphere, reversed motor wiring).
 *
 * Note what the sensor can and cannot see: a constant Dec counter error at
 * calibration time is a rotation about the Dec axis, indistinguishable from
 * mounting the sensor differently - it is absorbed into M. Anything that
 * changes after the calibration (a slipped clutch, a cold start away from
 * CWD) is seen on both axes, and RA zero errors are seen absolutely.
 */
class ImuMountModel
{
	public:
		struct CalSample
		{
			double raAxis, decAxis;	// deg, from the counters
			double acc[3];		// g, averaged at rest
			bool magValid = false;
			double mag[3] = {0, 0, 0};	// uT, raw chip axes, window median
		};

		ImuMountModel ();

		void setLatitude (double latDeg);

		bool valid () const { return fitted; }
		double rms () const { return fitRms; }		// deg, of the last fit
		int fitSamples () const { return nFit; }
		std::string describe () const;

		/**
		 * The magnetometer, as the sum of every field source fixed to the
		 * base, to the RA-turning part or to the sensor - the mount's own
		 * steel included - which in the sensor frame is linear in
		 * {1, cos ra, sin ra} x {1, cos dec, sin dec}: 9 terms per axis, 27
		 * in all, a linear least squares fit. Physical models (earth field
		 * + offsets) left 10-16 uT on SBT; this one 1.5 uT on poses it was
		 * not fitted to, with 0.4 uT noise. Needs magMinSamples samples
		 * spread over both axes; fitMag() says whether it got them.
		 */
		bool fitMag (const std::vector<CalSample> &samples, int magMinSamples = 20, double minSpanDeg = 60);
		bool magValid () const { return magFitted; }
		double magRms () const { return magFitRms; }	// uT
		int magSamples () const { return nMagFit; }
		void predictMag (double raAxis, double decAxis, double out[3]) const;
		/** |measured - predicted|, uT */
		double magErrorUT (double raAxis, double decAxis, const double mag[3]) const;

		/** fit to the samples; false (and invalid) with fewer than 3 */
		bool fit (const std::vector<CalSample> &samples, int fullModelFrom = 8);

		/** unit "up" vector predicted at these axis angles, sensor frame, without bias/scale */
		void predictUp (double raAxis, double decAxis, double out[3]) const;

		/** altitude of the tube's optical axis at these axis angles, deg (the mount geometry only, no sensor) */
		double tubeAltitude (double raAxis, double decAxis) const;

		/** the measured accelerometer vector with bias and scale removed, normalised */
		void correctAcc (const double acc[3], double out[3]) const;

		/** angle between measured and predicted up, deg */
		double errorDeg (double raAxis, double decAxis, const double acc[3]) const;

		/**
		 * Axis angles the sensor says the mount is at, searched from (raAxis,
		 * decAxis) - two solutions exist in general, this finds the one near
		 * the counters. condDeg is how far the solution can move per degree
		 * of gravity error along the worst direction; large near the poses
		 * where the two solutions merge (tube in the meridian plane).
		 */
		bool solveAxes (const double acc[3], double raAxis, double decAxis, double &raOut, double &decOut, double &condDeg) const;

		/**
		 * Every axis position that explains this gravity reading to within
		 * maxErrDeg, best first - in general two, mirror images of each
		 * other; one where they merge (tube in the meridian plane). Found
		 * from a grid of starting points, so it needs no prior position.
		 */
		// condDeg: axis degrees per degree of gravity noise along the worst direction
		struct AxisSolution { double raAxis, decAxis, errDeg, condDeg; };
		std::vector<AxisSolution> solveAll (const double acc[3], double maxErrDeg) const;

		/**
		 * The axis position at the first of two readings, taken before and
		 * after a known step (dRa, dDec, axis degrees) - every local best
		 * fit of both readings together, best first; errDeg is the RMS of
		 * the two angle errors. A step off the merge line (RA axis 0/180)
		 * leaves one good fit where one reading alone leaves two, or a
		 * whole valley of them on the line itself.
		 */
		std::vector<AxisSolution> solveJoint (const double acc1[3], const double acc2[3], double dRa, double dDec, double maxErrDeg) const;

	private:
		double zen[3];
		double M[9];		// row-major, sensor axes in tube coordinates (columns)
		double bias[3];
		double scale;
		int raSign, decSign;
		bool fitted;
		double fitRms;
		int nFit;
		bool fullModel;
		bool magFitted;
		double magCoef[27];
		double magFitRms;
		int nMagFit;

		void upFromAxes (const double Mr[9], int rs, int ds, double raAxis, double decAxis, double out[3]) const;
		double fitFrom (const std::vector<CalSample> &samples, double Mr[9], double b[3], double &s, int rs, int ds, bool full, int iterations) const;
};

}

#endif // __RTS2_TELD_MPU9250__
