#pragma once

#include <QObject>
#include <QImage>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "devcliimg.h"

namespace stacker
{

class CalibrationLibrary;

/**
 * rts2-viewer's ViewerCamera (gui/viewer/include/gui/viewercamera.h),
 * cloned for the stacker: every image the camera sends is dark-subtracted
 * and flat-fielded with the matching master frames from a
 * CalibrationLibrary, then added pixel by pixel into a running sum (no
 * registration - the telescope's guider is trusted to keep the field
 * put). resetStack() writes that sum to a FITS file and starts a new one.
 *
 * Threading is the viewer's: everything but setMeasureRegion()/
 * setShowStack() (plain atomics) and snapshotValues() (mutex) runs on the
 * RTS2 client's worker thread, and results leave through queued signals.
 */
class StackCamera : public QObject, public rts2image::DevClientCameraImage
{
	Q_OBJECT

	public:
		/** calib may be nullptr - frames are then stacked raw. */
		StackCamera (rts2core::Connection *_connection, CalibrationLibrary *calib);

		void setMeasureRegion (int x, int y, int w, int h);
		void setArchivePath (const std::string &expandPath) { archiveExpandPath = expandPath; }
		void setStackDir (const std::string &dir) { stackDir = dir; }

		/**
		 * Which of the two cached images - the running stack or the last
		 * calibrated frame - imageReady()/fitResult() report on. Takes
		 * effect from the next image, or right away through redisplay().
		 */
		void setShowStack (bool show) { showStack.store (show); }

		/** Worker thread only: re-run the fit on the displayed image at the current measure region. */
		void refit ();

		/** Worker thread only: re-emit imageReady() + fitResult() for the displayed image (after setShowStack()). */
		void redisplay ();

		/**
		 * Worker thread only: save the stack (if it holds any frame) and
		 * start an empty one. reason goes to the log line.
		 */
		void resetStack (const QString &reason);

		void snapshotValues (QMap<QString, double> &values, QMap<QString, QStringList> &choices, QMap<QString, QRect> &rects) const;

	signals:
		void imageReady (QImage image);
		void exposureStateChanged (bool exposing);
		void rectangleUpdated (QString valueName, int x, int y, int w, int h);
		void fitResult (bool valid, double centroidX, double centroidY, double fwhmX, double fwhmY, double peak, double background);
		void valueUpdated (QString valueName, double numericValue, QStringList choices);
		void stateTextChanged (QString stateText, bool hasError);

		/** After every change to the stack: frames summed, their total exposure time, filter, and what calibrated them. */
		void stackUpdated (int frames, double totalExposure, QString filter, QString calibration);

		/** One line for the GUI log - skipped frames, saved stacks, calibration problems. */
		void stackMessage (QString message);

	protected:
		virtual rts2image::Image *createImage (const struct timeval *expStart) override;
		virtual void cameraImageReady (rts2image::Image *image) override;
		virtual void exposureStarted (bool expectImage) override;
		virtual void exposureEnd (bool expectImage) override;
		virtual void valueChanged (rts2core::Value *value) override;
		virtual void stateChanged (rts2core::ServerState *state) override;

	private:
		std::string currentFilter ();
		void saveStack ();
		void clearStack ();
		void emitStackState ();
		void display ();
		void runFit ();
		const std::vector<double> &shown (long &w, long &h) const;

		CalibrationLibrary *calib;
		std::string archiveExpandPath;
		std::string stackDir = ".";

		std::atomic<int> measureX { 0 };
		std::atomic<int> measureY { 0 };
		std::atomic<int> measureW { 32 };
		std::atomic<int> measureH { 32 };
		std::atomic<bool> showStack { true };

		// Worker-thread-only state, FITS native (bottom-up) row order.
		std::vector<double> lastFrame;   // last frame, calibrated as far as possible
		long frameWidth = 0;
		long frameHeight = 0;
		std::vector<double> stack;       // sum of calibrated frames
		long stackWidth = 0;
		long stackHeight = 0;

		int stackFrames = 0;
		double stackExposure = 0;
		double stackStart = 0;           // ctime of the first frame's exposure start
		double stackEnd = 0;             // ctime of the last frame's exposure end
		std::string stackFilter;
		std::set<std::string> stackDarks;
		std::set<std::string> stackFlats;
		std::set<double> stackDarkTimes;       // for the GUI's short summary
		std::set<std::string> stackFlatFilters;
		bool stackUnflatted = false;     // some frame went in without a flat
		std::string warnedNoFlat;        // filter already warned about, so the log isn't flooded

		mutable std::mutex valuesMutex;
		QMap<QString, double> lastValues;
		QMap<QString, QStringList> lastChoices;
		QMap<QString, QRect> lastRects;
};

}
