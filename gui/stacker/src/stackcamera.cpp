#include "stacker/stackcamera.h"
#include "stacker/calibration.h"

#include <connection.h>
#include <imghdr.h>
#include <status.h>
#include <value.h>
#include <valuerectangle.h>

#include <fitsio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <sys/time.h>

using namespace stacker;

namespace
{
	double readPixelAt (const void *data, int dataType, long idx)
	{
		switch (dataType)
		{
			case RTS2_DATA_BYTE:     return ((const uint8_t *) data)[idx];
			case RTS2_DATA_SBYTE:    return ((const int8_t *) data)[idx];
			case RTS2_DATA_SHORT:    return ((const int16_t *) data)[idx];
			case RTS2_DATA_USHORT:   return ((const uint16_t *) data)[idx];
			case RTS2_DATA_LONG:     return ((const int32_t *) data)[idx];
			case RTS2_DATA_ULONG:    return ((const uint32_t *) data)[idx];
			case RTS2_DATA_LONGLONG: return (double) ((const int64_t *) data)[idx];
			case RTS2_DATA_FLOAT:    return ((const float *) data)[idx];
			case RTS2_DATA_DOUBLE:   return ((const double *) data)[idx];
			default:                 return 0.0;
		}
	}

	/**
	 * The viewer's "logfit" display stretch (gui/viewer/src/viewercamera.cpp,
	 * logFitGrayscale()) on an already-converted double buffer - see there
	 * for the derivation. It adapts to each image's own 10/50/90/99.95th
	 * percentiles, which is what makes the growing stack keep looking
	 * right as its sky level and dynamic range climb frame after frame.
	 */
	QImage logFitGrayscale (const std::vector<double> &data, long width, long height)
	{
		long n = width * height;
		if (n <= 0 || (long) data.size () < n)
			return QImage ();

		std::vector<double> sorted (data.begin (), data.begin () + n);
		std::sort (sorted.begin (), sorted.end ());

		auto quantileAt = [&sorted, n] (double q)
		{
			long idx = (long) (n * q);
			return sorted[std::max (0L, std::min (idx, n - 1))];
		};

		double qLow = quantileAt (0.1);
		double qMidLow = quantileAt (0.5);
		double qMidHigh = quantileAt (0.9);
		double qHigh = quantileAt (0.9995);

		double C = qLow - (qMidHigh - qLow) / 1000.0;

		double xData[4] = { qLow, qMidLow, qMidHigh, qHigh };
		double yTarget[4] = { 1.0, 255.0 / 8.0, 255.0 / 4.0, 255.0 };

		double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
		for (int i = 0; i < 4; i++)
		{
			double xi = std::log10 (std::max (xData[i] - C, 1e-10));
			double yi = std::log10 (yTarget[i]);
			sumX += xi; sumY += yi; sumXX += xi * xi; sumXY += xi * yi;
		}
		double denom = 4.0 * sumXX - sumX * sumX;
		double B = (std::abs (denom) > 1e-12) ? (4.0 * sumXY - sumX * sumY) / denom : 0.0;
		double A = (sumY - B * sumX) / 4.0;

		QImage img (width, height, QImage::Format_Grayscale8);
		for (long y = 0; y < height; y++)
		{
			unsigned char *line = img.scanLine (y);
			const double *row = data.data () + (height - 1 - y) * width;
			for (long x = 0; x < width; x++)
			{
				double transformed = std::pow (10.0, A + B * std::log10 (std::max (row[x] - C, 1e-10)));
				line[x] = (unsigned char) std::max (0, std::min ((int) std::floor (transformed), 255));
			}
		}
		return img;
	}

	std::string formatIsoTime (double ctime)
	{
		time_t t = (time_t) ctime;
		struct tm tm;
		gmtime_r (&t, &tm);
		char buf[40];
		strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%S", &tm);
		char ms[8];
		snprintf (ms, sizeof (ms), ".%03d", (int) ((ctime - t) * 1000.0));
		return std::string (buf) + ms;
	}

	std::string safeName (std::string s)
	{
		for (auto &c : s)
			if (!(std::isalnum ((unsigned char) c) || c == '_' || c == '.'))
				c = '_';
		return s;
	}

	double nowCtime ()
	{
		struct timeval tv;
		gettimeofday (&tv, nullptr);
		return tv.tv_sec + tv.tv_usec / 1e6;
	}
}

StackCamera::StackCamera (rts2core::Connection *_connection, CalibrationLibrary *_calib):
	rts2image::DevClientCameraImage (_connection), calib (_calib)
{
}

rts2image::Image *StackCamera::createImage (const struct timeval *expStart)
{
	// Same rule as ViewerCamera::createImage(): only frames that are kept
	// go to the configured path.
	if (saveImage && !archiveExpandPath.empty ())
		return new rts2image::Image (archiveExpandPath.c_str (), getExposureNumber (), expStart, connection, false, writeConnection, writeRTS2Values);
	return rts2image::DevClientCameraImage::createImage (expStart);
}

void StackCamera::setMeasureRegion (int x, int y, int w, int h)
{
	measureX.store (x);
	measureY.store (y);
	measureW.store (w);
	measureH.store (h);
}

std::string StackCamera::currentFilter ()
{
	// cameraImageReady() runs before DevClientCameraImage::writeFilter()
	// stamps the name into the image (kernel/src/devcliimg.cpp), so ask
	// the camera's "filter" value directly - the filter can't have moved
	// since this exposure, the next one hasn't been requested yet.
	auto *sel = dynamic_cast<rts2core::ValueSelection *> (getConnection ()->getValue ("filter"));
	if (!sel)
		return "";
	const char *name = sel->getSelName (sel->getValueInteger ());
	return name ? name : "";
}

void StackCamera::cameraImageReady (rts2image::Image *image)
{
	long w = image->getChannelWidth (0);
	long h = image->getChannelHeight (0);
	if (w <= 0 || h <= 0)
		return;

	const void *raw = image->getChannelData (0);
	int dataType = image->getDataType ();
	long n = w * h;

	double exptime = image->getExposureLength ();
	if (std::isnan (exptime))
		exptime = 0;
	double start = image->getExposureStart ();
	if (!(start > 0))
		start = nowCtime () - exptime;
	std::string filter = currentFilter ();

	std::vector<double> frame (n);
	for (long i = 0; i < n; i++)
		frame[i] = readPixelAt (raw, dataType, i);

	bool stackable = true;
	std::string darkName, flatName, flatFilter;

	if (calib)
	{
		// No dark of this exposure time and size means no honest
		// calibration of this frame - show it, but keep it out of the sum.
		const CalibFrame *dark = calib->findDark (exptime, w, h);
		std::string err;
		const std::vector<float> *darkPx = dark ? calib->pixels (dark, err) : nullptr;
		if (!darkPx)
		{
			stackable = false;
			if (dark)
				emit stackMessage (QString ("cannot read master dark: %1 - frame not stacked").arg (QString::fromStdString (err)));
			else
				emit stackMessage (QString ("no master dark for %1 s at %2x%3 - frame shown raw, not stacked").arg (exptime).arg (w).arg (h));
		}
		else
		{
			darkName = dark->name;
			for (long i = 0; i < n; i++)
				frame[i] -= (*darkPx)[i];

			const CalibFrame *flat = calib->findFlat (filter, w, h);
			const std::vector<float> *flatPx = flat ? calib->pixels (flat, err) : nullptr;
			if (flatPx)
			{
				flatName = flat->name;
				flatFilter = flat->filter;
				// Vignetted corners and dead columns have flat values near
				// zero; dividing by those would blow them up into the
				// brightest thing in the stack.
				for (long i = 0; i < n; i++)
				{
					float f = (*flatPx)[i];
					frame[i] = (f > 0.05f) ? frame[i] / f : 0.0;
				}
			}
			else if (warnedNoFlat != filter)
			{
				warnedNoFlat = filter;
				if (flat)
					emit stackMessage (QString ("cannot read master flat: %1 - stacking dark-subtracted only").arg (QString::fromStdString (err)));
				else
					emit stackMessage (QString ("no master flat for filter '%1' at %2x%3 - stacking dark-subtracted only").arg (QString::fromStdString (filter)).arg (w).arg (h));
			}
		}
	}

	if (stackable)
	{
		// A sum only means something for one field of view in one
		// filter - anything else closes the current stack first.
		if (stackFrames > 0 && (w != stackWidth || h != stackHeight))
			resetStack ("frame size changed");
		else if (stackFrames > 0 && filter != stackFilter)
			resetStack (QString ("filter changed %1 -> %2").arg (QString::fromStdString (stackFilter), QString::fromStdString (filter)));

		if (stackFrames == 0)
		{
			stack.assign (n, 0.0);
			stackWidth = w;
			stackHeight = h;
			stackFilter = filter;
			stackStart = start;
		}

		for (long i = 0; i < n; i++)
			stack[i] += frame[i];

		stackFrames++;
		stackExposure += exptime;
		stackEnd = start + exptime;
		if (!darkName.empty ())
		{
			stackDarks.insert (darkName);
			stackDarkTimes.insert (exptime);
		}
		if (!flatName.empty ())
		{
			stackFlats.insert (flatName);
			stackFlatFilters.insert (flatFilter);
		}
		else
			stackUnflatted = true;
	}

	lastFrame = std::move (frame);
	frameWidth = w;
	frameHeight = h;

	display ();
	emitStackState ();
}

const std::vector<double> &StackCamera::shown (long &w, long &h) const
{
	if (showStack.load () && stackFrames > 0)
	{
		w = stackWidth;
		h = stackHeight;
		return stack;
	}
	w = frameWidth;
	h = frameHeight;
	return lastFrame;
}

void StackCamera::display ()
{
	long w, h;
	const std::vector<double> &data = shown (w, h);
	if (data.empty ())
		return;
	QImage qimg = logFitGrayscale (data, w, h);
	if (qimg.isNull ())
		return;
	emit imageReady (qimg);
	runFit ();
}

void StackCamera::redisplay ()
{
	display ();
}

void StackCamera::refit ()
{
	runFit ();
}

void StackCamera::emitStackState ()
{
	QString cal;
	if (!calib)
		cal = "none (raw frames)";
	else if (stackFrames == 0)
		cal = "-";
	else
	{
		QStringList times;
		for (double t : stackDarkTimes)
			times << QString::number (t);
		cal = QString ("dark %1 s").arg (times.join (", "));
		if (stackFlatFilters.empty ())
			cal += ", no flat";
		else
		{
			QStringList filters;
			for (const auto &f : stackFlatFilters)
				filters << QString::fromStdString (f);
			cal += QString (", flat %1").arg (filters.join (", "));
			if (stackUnflatted)
				cal += " (not all frames)";
		}
	}
	emit stackUpdated (stackFrames, stackExposure, QString::fromStdString (stackFilter), cal);
}

void StackCamera::resetStack (const QString &reason)
{
	if (stackFrames > 0)
	{
		emit stackMessage (QString ("stack closed (%1)").arg (reason));
		saveStack ();
	}
	clearStack ();
	emitStackState ();
	// Nothing left to show in stack view but the last frame.
	if (showStack.load ())
		display ();
}

void StackCamera::clearStack ()
{
	stack.clear ();
	stackWidth = stackHeight = 0;
	stackFrames = 0;
	stackExposure = 0;
	stackStart = stackEnd = 0;
	stackFilter.clear ();
	stackDarks.clear ();
	stackFlats.clear ();
	stackDarkTimes.clear ();
	stackFlatFilters.clear ();
	stackUnflatted = false;
}

void StackCamera::saveStack ()
{
	namespace fs = std::filesystem;

	std::error_code ec;
	fs::create_directories (stackDir, ec);

	time_t t = (time_t) stackStart;
	struct tm tm;
	gmtime_r (&t, &tm);
	char stamp[32];
	strftime (stamp, sizeof (stamp), "%Y%m%d-%H%M%S", &tm);

	std::string base = std::string ("stack-") + stamp + "-" + safeName (getName ());
	if (!stackFilter.empty ())
		base += "-" + safeName (stackFilter);
	fs::path path = fs::path (stackDir) / (base + ".fits");
	for (int i = 1; fs::exists (path); i++)
		path = fs::path (stackDir) / (base + "-" + std::to_string (i) + ".fits");

	long n = (long) stack.size ();
	std::vector<float> out (stack.begin (), stack.end ());

	fitsfile *f = nullptr;
	int status = 0;
	long naxes[2] = { stackWidth, stackHeight };
	fits_create_file (&f, path.c_str (), &status);
	fits_create_img (f, FLOAT_IMG, 2, naxes, &status);
	fits_write_img (f, TFLOAT, 1, n, out.data (), &status);

	std::string dateObs = formatIsoTime (stackStart);
	std::string dateEnd = formatIsoTime (stackEnd);
	int ncombine = stackFrames;
	double exposure = stackExposure;
	int darkCor = calib && !stackDarks.empty ();
	int flatCor = !stackFlats.empty () && !stackUnflatted;

	fits_write_key (f, TSTRING, "IMAGETYP", (void *) "stack", "sum of calibrated frames", &status);
	fits_write_key (f, TSTRING, "ORIGIN", (void *) "rts2-stacker", nullptr, &status);
	fits_write_key (f, TSTRING, "CCD_NAME", (void *) getName (), "camera device", &status);
	fits_write_key (f, TSTRING, "DATE-OBS", (void *) dateObs.c_str (), "start of first frame (UTC)", &status);
	fits_write_key (f, TSTRING, "DATE-END", (void *) dateEnd.c_str (), "end of last frame (UTC)", &status);
	fits_write_key (f, TDOUBLE, "EXPTIME", &exposure, "[s] summed exposure time", &status);
	fits_write_key (f, TINT, "NCOMBINE", &ncombine, "frames summed, not registered", &status);
	if (!stackFilter.empty ())
		fits_write_key (f, TSTRING, "FILTER", (void *) stackFilter.c_str (), nullptr, &status);
	fits_write_key (f, TLOGICAL, "DARKCOR", &darkCor, "master dark subtracted", &status);
	fits_write_key (f, TLOGICAL, "FLATCOR", &flatCor, "master flat divided (all frames)", &status);
	if (calib)
		fits_write_key_longstr (f, "CALIBDIR", calib->directory ().c_str (), "calibration directory", &status);
	int i = 1;
	for (const auto &d : stackDarks)
	{
		std::string key = "DARK" + std::to_string (i++);
		fits_write_key (f, TSTRING, key.c_str (), (void *) d.c_str (), "master dark", &status);
	}
	i = 1;
	for (const auto &fl : stackFlats)
	{
		std::string key = "FLAT" + std::to_string (i++);
		fits_write_key (f, TSTRING, key.c_str (), (void *) fl.c_str (), "master flat", &status);
	}
	fits_write_date (f, &status);

	int closeStatus = 0;
	fits_close_file (f, &closeStatus);
	if (!status)
		status = closeStatus;

	if (status)
	{
		char text[FLEN_STATUS];
		fits_get_errstatus (status, text);
		QString msg = QString ("cannot save stack to %1: %2").arg (QString::fromStdString (path.string ()), text);
		emit stackMessage (msg);
		logStream (MESSAGE_ERROR) << msg.toStdString () << sendLog;
		return;
	}

	QString msg = QString ("saved stack of %1 frames (%2 s) to %3").arg (stackFrames).arg (stackExposure).arg (QString::fromStdString (path.string ()));
	emit stackMessage (msg);
	logStream (MESSAGE_INFO) << msg.toStdString () << sendLog;
}

void StackCamera::runFit ()
{
	// ViewerCamera::runFitOnData() (gui/viewer/src/viewercamera.cpp) on a
	// double buffer - median-of-border background, then flux-weighted
	// centroid and mean absolute deviation of the two 1D profiles, reported
	// as an equivalent Gaussian FWHM. See there for why each step is the
	// way it is.
	long width, height;
	const std::vector<double> &data = shown (width, height);
	if (data.empty ())
		return;

	int rx = std::max (0, std::min (measureX.load (), (int) width - 1));
	int ry = std::max (0, std::min (measureY.load (), (int) height - 1));
	int rw = std::min (measureW.load (), (int) width - rx);
	int rh = std::min (measureH.load (), (int) height - ry);

	const int border = 2;
	if (rw < 2 * border + 3 || rh < 2 * border + 3)
	{
		emit fitResult (false, 0, 0, 0, 0, 0, 0);
		return;
	}

	auto pixel = [&] (int dx, int dy)
	{
		long rawY = height - 1 - (ry + dy);
		return data[rawY * width + rx + dx];
	};

	std::vector<double> borderPixels;
	for (int dy = 0; dy < rh; dy++)
		for (int dx = 0; dx < rw; dx++)
			if (dx < border || dx >= rw - border || dy < border || dy >= rh - border)
				borderPixels.push_back (pixel (dx, dy));

	size_t mid = borderPixels.size () / 2;
	std::nth_element (borderPixels.begin (), borderPixels.begin () + mid, borderPixels.end ());
	double background = borderPixels[mid];
	if (borderPixels.size () % 2 == 0)
		background = (background + *std::max_element (borderPixels.begin (), borderPixels.begin () + mid)) / 2.0;

	double peak = 0;
	std::vector<double> profileX (rw, 0.0), profileY (rh, 0.0);
	for (int dy = 0; dy < rh; dy++)
		for (int dx = 0; dx < rw; dx++)
		{
			double r = pixel (dx, dy) - background;
			peak = std::max (peak, r);
			profileX[dx] += r;
			profileY[dy] += r;
		}

	double sumI = 0, sumIx = 0, sumIy = 0;
	for (int dx = 0; dx < rw; dx++)
	{
		sumI += profileX[dx];
		sumIx += profileX[dx] * dx;
	}
	for (int dy = 0; dy < rh; dy++)
		sumIy += profileY[dy] * dy;

	if (sumI <= 0)
	{
		emit fitResult (false, 0, 0, 0, 0, 0, 0);
		return;
	}

	double x0 = sumIx / sumI;
	double y0 = sumIy / sumI;

	double madX = 0, madY = 0;
	for (int dx = 0; dx < rw; dx++)
		madX += profileX[dx] * std::abs (dx - x0);
	for (int dy = 0; dy < rh; dy++)
		madY += profileY[dy] * std::abs (dy - y0);
	madX /= sumI;
	madY /= sumI;

	const double madToFwhm = 1.2533141373155003 * 2.3548200450309493;
	emit fitResult (true, rx + x0, ry + y0, madX * madToFwhm, madY * madToFwhm, peak, background);
}

void StackCamera::exposureStarted (bool expectImage)
{
	rts2image::DevClientCameraImage::exposureStarted (expectImage);
	emit exposureStateChanged (true);
}

void StackCamera::exposureEnd (bool expectImage)
{
	rts2image::DevClientCameraImage::exposureEnd (expectImage);
	emit exposureStateChanged (false);
}

void StackCamera::valueChanged (rts2core::Value *value)
{
	QString name = QString::fromStdString (value->getName ());

	if (auto *rect = dynamic_cast<rts2core::ValueRectangle *> (value))
	{
		QRect r (rect->getXInt (), rect->getYInt (), rect->getWidthInt (), rect->getHeightInt ());
		{
			std::lock_guard<std::mutex> lock (valuesMutex);
			lastRects[name] = r;
		}
		emit rectangleUpdated (name, r.x (), r.y (), r.width (), r.height ());
	}
	else if (auto *sel = dynamic_cast<rts2core::ValueSelection *> (value))
	{
		QStringList choices;
		for (int i = 0; i < sel->selSize (); i++)
			choices << QString::fromUtf8 (sel->getSelName (i));
		double numericValue = sel->getValueInteger ();
		{
			std::lock_guard<std::mutex> lock (valuesMutex);
			lastValues[name] = numericValue;
			lastChoices[name] = choices;
		}
		emit valueUpdated (name, numericValue, choices);
	}
	else
	{
		double numericValue = value->getValueDouble ();
		{
			std::lock_guard<std::mutex> lock (valuesMutex);
			lastValues[name] = numericValue;
		}
		emit valueUpdated (name, numericValue, QStringList ());
	}
}

void StackCamera::snapshotValues (QMap<QString, double> &values, QMap<QString, QStringList> &choices, QMap<QString, QRect> &rects) const
{
	std::lock_guard<std::mutex> lock (valuesMutex);
	values = lastValues;
	choices = lastChoices;
	rects = lastRects;
}

void StackCamera::stateChanged (rts2core::ServerState *state)
{
	rts2image::DevClientCameraImage::stateChanged (state);

	// Same fixed phase words as ViewerCamera::stateChanged().
	rts2core::Connection *conn = getConnection ();
	rts2_status_t real = conn->getRealState ();
	bool hasError = conn->getErrorState () != 0;

	QString phase;
	if (hasError)
		phase = "HW error";
	else if (real & (CAM_EXPOSING | CAM_EXPOSING_NOIM))
		phase = "Exposing";
	else if (real & CAM_READING)
		phase = "Reading";
	else if (real & CAM_SHIFT)
		phase = "Shifting";
	else if (real & CAM_FT)
		phase = "Frame transfer";
	else
		phase = "Idle";

	emit stateTextChanged (phase, hasError);
}
