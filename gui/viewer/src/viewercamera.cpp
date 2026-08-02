#include "gui/viewercamera.h"

#include <connection.h>
#include <imghdr.h>
#include <status.h>
#include <value.h>
#include <valuerectangle.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace gui;

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

	double readPixel (const void *data, int dataType, long width, long x, long y)
	{
		return readPixelAt (data, dataType, y * width + x);
	}

	double det3 (double m[3][3])
	{
		return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
		     - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
		     + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	}

	/**
	 * The "logfit" display stretch - ported from the classic tree's
	 * current src/focusc/xfitsimage.cpp (its XFitsImage::drawImage(),
	 * explicitly commented there as "similar to f2cj.py"), the same
	 * algorithm fiber_pointing_client.py calls _logfit_to_screen and
	 * pyrt-f2cj implements too. Fits a curve
	 * log10(y) = A + B*log10(x - C) through four (pixel value -> target
	 * grey level) points taken from the image's own 10/50/90/99.95th
	 * percentiles - a log stretch that adapts to this image's own
	 * brightness distribution instead of a fixed linear cut, why it holds
	 * up much better than a plain quantile clip across wildly different
	 * exposure levels.
	 */
	QImage logFitGrayscale (const void *data, int dataType, long width, long height)
	{
		long n = width * height;
		if (n <= 0)
			return QImage ();

		std::vector<double> sorted;
		sorted.reserve (n);
		for (long i = 0; i < n; i++)
			sorted.push_back (readPixelAt (data, dataType, i));
		std::sort (sorted.begin (), sorted.end ());

		auto quantileAt = [&sorted, n] (double q)
		{
			long idx = (long) (n * q);
			if (idx < 0)
				idx = 0;
			if (idx >= n)
				idx = n - 1;
			return sorted[idx];
		};

		double qLow = quantileAt (0.1);
		double qMidLow = quantileAt (0.5);
		double qMidHigh = quantileAt (0.9);
		double qHigh = quantileAt (0.9995);

		// Fixed just below the noise floor, same as the reference
		// implementations - keeps log10(x - C) well-defined near the low
		// end without an arbitrary epsilon dominating the fit there.
		double C = qLow - (qMidHigh - qLow) / 1000.0;

		double xData[4] = { qLow, qMidLow, qMidHigh, qHigh };
		double yTarget[4] = { 1.0, 255.0 / 8.0, 255.0 / 4.0, 255.0 };

		// Ordinary least squares on log10(y) = A + B*log10(x - C) through
		// the four points above - same closed-form formula as
		// xfitsimage.cpp, not a general nonlinear solver (scipy's
		// curve_fit in the Python versions lands on the same line for
		// this 2-parameter linear-in-log-space model).
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
			// Top-down display row from FITS's native bottom-up storage -
			// same convention the rest of this file (runFitOnData()) uses.
			long rawY = height - 1 - y;
			for (long x = 0; x < width; x++)
			{
				double pixel = readPixel (data, dataType, width, x, rawY);
				double logArg = std::max (pixel - C, 1e-10);
				double transformed = std::pow (10.0, A + B * std::log10 (logArg));
				int grey = (int) std::floor (transformed);
				if (grey < 0)
					grey = 0;
				if (grey > 255)
					grey = 255;
				line[x] = (unsigned char) grey;
			}
		}
		return img;
	}
}

ViewerCamera::ViewerCamera (rts2core::Connection *_connection):
	rts2image::DevClientCameraImage (_connection)
{
}

rts2image::Image *ViewerCamera::createImage (const struct timeval *expStart)
{
	if (saveImage && !archiveExpandPath.empty ())
		return new rts2image::Image (archiveExpandPath.c_str (), getExposureNumber (), expStart, connection, false, writeConnection, writeRTS2Values);
	return rts2image::DevClientCameraImage::createImage (expStart);
}

void ViewerCamera::setMeasureRegion (int x, int y, int w, int h)
{
	measureX.store (x);
	measureY.store (y);
	measureW.store (w);
	measureH.store (h);
}

void ViewerCamera::cameraImageReady (rts2image::Image *image)
{
	long width = image->getChannelWidth (0);
	long height = image->getChannelHeight (0);

	if (width <= 0 || height <= 0)
		return;

	const void *raw = image->getChannelData (0);
	int dataType = image->getDataType ();

	// "logfit" display stretch - see logFitGrayscale() above. Replaces the
	// earlier getChannelGrayscaleImage()-based quantile clip, which held a
	// fixed 0.5% trim regardless of the image's own brightness
	// distribution; this adapts per image, same as classic rts2-xfocusc
	// (current src/focusc/xfitsimage.cpp) and fiber_pointing_client.py's
	// _logfit_to_screen.
	QImage qimg = logFitGrayscale (raw, dataType, width, height);
	if (qimg.isNull ())
		return;

	emit imageReady (qimg);

	// Cache the raw pixel data (not the 8-bit display copy above) so
	// refit() can re-run the fit later - e.g. the user just drags the
	// measure cursor onto a different star already in the frame - without
	// needing a new exposure.
	size_t byteSize = (size_t) width * (size_t) height * (size_t) image->getPixelByteSize ();
	lastImageData.assign ((const char *) raw, (const char *) raw + byteSize);
	lastWidth = width;
	lastHeight = height;
	lastDataType = dataType;

	runFitOnData (lastImageData.data (), lastDataType, lastWidth, lastHeight);
}

void ViewerCamera::refit ()
{
	if (lastImageData.empty ())
		return;
	runFitOnData (lastImageData.data (), lastDataType, lastWidth, lastHeight);
}

void ViewerCamera::runFitOnData (const void *data, int dataType, long width, long height)
{
	// setMeasureRegion() is in the same top-down display-pixel coordinates
	// as the QImage emitted from cameraImageReady(); data here is in
	// FITS's native bottom-up row order (same as getChannelGrayscaleImage's
	// own invert_y=true flips for display) - each row read is translated
	// (rawY = height-1-dispY) but every accumulated position stays in
	// display coordinates throughout, so the result lines up with the red
	// cursor MainWindow drew.
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

	// 1. Fit a background plane bg(x,y) = a + b*x + c*y from the outer
	// border-pixel-wide frame of the box only - a deliberately simple
	// stand-in for a full 2D Gaussian fit (see gui/STATUS.md for the
	// reasoning). Local (dx,dy) box coordinates keep the normal-equation
	// sums small and well-conditioned regardless of where the box sits on
	// the chip.
	double sum1 = 0, sumx = 0, sumy = 0, sumxx = 0, sumxy = 0, sumyy = 0;
	double sumz = 0, sumxz = 0, sumyz = 0;

	for (int dy = 0; dy < rh; dy++)
	{
		int rawY = (int) height - 1 - (ry + dy);
		for (int dx = 0; dx < rw; dx++)
		{
			if (!(dx < border || dx >= rw - border || dy < border || dy >= rh - border))
				continue;

			double z = readPixel (data, dataType, width, rx + dx, rawY);
			double px = dx, py = dy;

			sum1 += 1; sumx += px; sumy += py;
			sumxx += px * px; sumxy += px * py; sumyy += py * py;
			sumz += z; sumxz += px * z; sumyz += py * z;
		}
	}

	double m[3][3] = { { sum1, sumx, sumy }, { sumx, sumxx, sumxy }, { sumy, sumxy, sumyy } };
	double rhs[3] = { sumz, sumxz, sumyz };
	double det = det3 (m);

	double a = 0, b = 0, c = 0;
	if (std::abs (det) > 1e-9)
	{
		double m0[3][3] = { { rhs[0], m[0][1], m[0][2] }, { rhs[1], m[1][1], m[1][2] }, { rhs[2], m[2][1], m[2][2] } };
		double m1[3][3] = { { m[0][0], rhs[0], m[0][2] }, { m[1][0], rhs[1], m[1][2] }, { m[2][0], rhs[2], m[2][2] } };
		double m2[3][3] = { { m[0][0], m[0][1], rhs[0] }, { m[1][0], m[1][1], rhs[1] }, { m[2][0], m[2][1], rhs[2] } };
		a = det3 (m0) / det;
		b = det3 (m1) / det;
		c = det3 (m2) / det;
	}
	else if (sum1 > 0)
	{
		// Degenerate normal equations (shouldn't happen for a real 2D
		// border) - fall back to a flat background, the border's mean.
		a = sumz / sum1;
	}

	// 2. Subtract the fitted background from every pixel in the box
	// (interior included), clamp negative residuals to 0, and accumulate
	// the intensity-weighted barycenter and second moments (dispersion)
	// over what's left - Petr's "statistical tricks" in place of a true
	// nonlinear Gaussian fit.
	std::vector<double> residual (rw * rh);
	double sumI = 0, sumIx = 0, sumIy = 0, peak = 0;

	for (int dy = 0; dy < rh; dy++)
	{
		int rawY = (int) height - 1 - (ry + dy);
		for (int dx = 0; dx < rw; dx++)
		{
			double z = readPixel (data, dataType, width, rx + dx, rawY);
			double r = z - (a + b * dx + c * dy);
			if (r < 0)
				r = 0;
			residual[dy * rw + dx] = r;
			if (r > peak)
				peak = r;

			double px = rx + dx, py = ry + dy;
			sumI += r;
			sumIx += r * px;
			sumIy += r * py;
		}
	}

	if (sumI <= 0)
	{
		emit fitResult (false, 0, 0, 0, 0, 0, 0);
		return;
	}

	double cx = sumIx / sumI;
	double cy = sumIy / sumI;

	double sumVarX = 0, sumVarY = 0;
	for (int dy = 0; dy < rh; dy++)
	{
		for (int dx = 0; dx < rw; dx++)
		{
			double r = residual[dy * rw + dx];
			double px = rx + dx, py = ry + dy;
			sumVarX += r * (px - cx) * (px - cx);
			sumVarY += r * (py - cy) * (py - cy);
		}
	}

	const double sigmaToFwhm = 2.3548200450309493; // 2*sqrt(2*ln2)
	double fwhmX = std::sqrt (sumVarX / sumI) * sigmaToFwhm;
	double fwhmY = std::sqrt (sumVarY / sumI) * sigmaToFwhm;

	emit fitResult (true, cx, cy, fwhmX, fwhmY, peak, a);
}

void ViewerCamera::exposureStarted (bool expectImage)
{
	rts2image::DevClientCameraImage::exposureStarted (expectImage);
	emit exposureStateChanged (true);
}

void ViewerCamera::exposureEnd (bool expectImage)
{
	rts2image::DevClientCameraImage::exposureEnd (expectImage);
	emit exposureStateChanged (false);
}

void ViewerCamera::valueChanged (rts2core::Value *value)
{
	QString name = QString::fromStdString (value->getName ());

	if (auto *rect = dynamic_cast<rts2core::ValueRectangle *> (value))
	{
		emit rectangleUpdated (name, rect->getXInt (), rect->getYInt (), rect->getWidthInt (), rect->getHeightInt ());
	}
	else if (auto *sel = dynamic_cast<rts2core::ValueSelection *> (value))
	{
		QStringList choices;
		for (int i = 0; i < sel->selSize (); i++)
			choices << QString::fromUtf8 (sel->getSelName (i));
		emit valueUpdated (name, sel->getValueInteger (), choices);
	}
	else
	{
		emit valueUpdated (name, value->getValueDouble (), QStringList ());
	}
}

void ViewerCamera::stateChanged (rts2core::ServerState *state)
{
	rts2image::DevClientCameraImage::stateChanged (state);

	// Connection::getStateString() (what rts2-mon shows) is a "|"-joined
	// dump of every set status/shutter/bop bit ("0 EXPOSING |
	// SHUTTER_CLEARED | BLOCK TELESCOPE MOVEMENT") - fine for a terminal
	// monitor, too noisy and variable-width for a fixed GUI label. Classify
	// the real device-status bits (status.h) directly into one fixed,
	// human phase word instead - checked in priority order, since more
	// than one can be set at once (e.g. reading while shifting).
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
