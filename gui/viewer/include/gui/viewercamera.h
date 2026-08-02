#pragma once

#include <QObject>
#include <QImage>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "devcliimg.h"

namespace gui
{

/**
 * Receives images and value updates from one camera device and
 * republishes them over Qt signals, so MainWindow (living on the GUI
 * thread) never touches rts2image::Image/rts2core::Value directly - see
 * ClientThread in viewerclient.h, which drives this on its own worker
 * thread.
 *
 * Deliberately built directly on rts2image::DevClientCameraImage
 * (kernel/include/devcliimg.h), not the classic FocusCameraClient/
 * DevClientCameraFoc chain - that adds focus-stepping logic (center(),
 * EVENT_INTEGRATE_START/STOP) a plain viewer has no use for.
 */
class ViewerCamera : public QObject, public rts2image::DevClientCameraImage
{
	Q_OBJECT

	public:
		explicit ViewerCamera (rts2core::Connection *_connection);

		/**
		 * Where the GUI's red "measure" cursor currently is, in the same
		 * top-down display-pixel coordinates as the QImage shown on
		 * screen - set directly from the GUI thread whenever it moves or
		 * is resized. Thread-safe (plain atomics, no RTS2/wire-protocol
		 * state touched), unlike everything routed through ViewerClient's
		 * idle()-drained queues: there is nothing here for two threads to
		 * race on. Read back on this object's own thread the next time an
		 * image arrives (cameraImageReady()) to run the centroid fit.
		 */
		void setMeasureRegion (int x, int y, int w, int h);

		/**
		 * Expand-path expression (image.h's %b/%y/%N/... syntax) used for
		 * frames that are actually being kept (saveImage on) - e.g.
		 * "%b%Y/%N/%c_%H%M%S-%s.fits", which lands under the observatory's
		 * shared archive (Configuration::observatoryBasePath(), rts2.ini's
		 * [observatory] base_path) in per-year/per-night subdirectories, no
		 * target involved. Set once at startup from ViewerClient (CLI
		 * option or rts2.ini's own [viewer] expand_path) - see
		 * createImage() override below for why this is deliberately NOT
		 * used for saveImage=0 frames.
		 */
		void setArchivePath (const std::string &expandPath) { archiveExpandPath = expandPath; }

		/**
		 * Re-run the centroid/FWHM fit against the last image already
		 * received, at whatever setMeasureRegion() currently says - no new
		 * exposure needed, e.g. when the user just drags the measure
		 * cursor to sit on a different star already in frame. Only ever
		 * called from this object's own worker thread (via ViewerClient's
		 * idle()-drained request queue, same as everything that isn't
		 * plain atomics), since it reads the cached lastImageData buffer
		 * that cameraImageReady() also (only) writes from there - see
		 * gui/STATUS.md.
		 */
		void refit ();

		/**
		 * Thread-safe copy of every value/rectangle this camera has ever
		 * reported via valueChanged() - see the mutex-guarded cache that
		 * method maintains below. Exists to close a real startup race: a
		 * freshly-connected device can flood its full metaInfo/value dump
		 * through valueChanged() (on this object's own worker thread)
		 * *before* MainWindow's queued onCameraCreated() slot - running on
		 * the GUI thread, competing with all its own rendering work - has
		 * even connected valueUpdated()/rectangleUpdated() to receive it.
		 * Anything emitted in that window is not lost (there is no signal
		 * connection yet to lose it from), but nothing was listening
		 * either, so it never reached MainWindow's per-camera cache -
		 * exactly matching a real report where binning/cooling/etc. controls
		 * stayed blank/disabled until an unrelated later change happened to
		 * retrigger valueChanged() for them. Call this once, right after
		 * connecting those signals, and replay the result the same way
		 * MainWindow already replays its own per-camera cache when
		 * switching the active camera (see onCameraComboChanged()).
		 */
		void snapshotValues (QMap<QString, double> &values, QMap<QString, QStringList> &choices, QMap<QString, QRect> &rects) const;

	signals:
		void imageReady (QImage image);
		void exposureStateChanged (bool exposing);

		/**
		 * One ValueRectangle Value changed ("WINDOW" - the current chip
		 * subframe, or "SIZE" - the full chip geometry, both
		 * base/camd/include/camd.h) - x/y/width/height, same coordinate
		 * convention as the Value itself (chip pixels, not display
		 * pixels).
		 */
		void rectangleUpdated (QString valueName, int x, int y, int w, int h);

		/**
		 * Result of the simple background-plane-subtracted centroid/
		 * dispersion fit (see cameraImageReady()) run on the region under
		 * setMeasureRegion(), in the same display-pixel coordinates as
		 * that region. valid is false (all other fields meaningless) if
		 * the region was degenerate (out of bounds/too small) for this
		 * image.
		 */
		void fitResult (bool valid, double centroidX, double centroidY, double fwhmX, double fwhmY, double peak, double background);

		/**
		 * Fired for every Value the device reports (its full state after
		 * connecting, then live updates) - MainWindow filters by
		 * valueName for the handful it cares about ("exposure", "binning",
		 * "filter", "CCD_TEMP", "CCD_SET", "COOLING"). For ValueSelection
		 * values (binning/filter), choices lists every option and
		 * numericValue is the selected index; for plain numeric/bool
		 * values, choices is empty and numericValue is the value itself.
		 */
		void valueUpdated (QString valueName, double numericValue, QStringList choices);

		/**
		 * One fixed phase word - "Idle"/"Exposing"/"Reading"/"Shifting"/
		 * "Frame transfer"/"HW error" - classified directly from the raw
		 * device-status bits (status.h's CAM_* / DEVICE_ERROR_MASK), not
		 * rts2-mon's own getStateString() text: that is a variable-length
		 * "|"-joined dump of every set bit ("0 EXPOSING | SHUTTER_CLEARED |
		 * BLOCK TELESCOPE MOVEMENT") - too noisy and too variable-width for
		 * a fixed GUI label. hasError mirrors
		 * Connection::getErrorState(), so MainWindow's status corner can
		 * flag it visually the same way fiber_pointing_client.py colours
		 * its own state label red/green.
		 */
		void stateTextChanged (QString stateText, bool hasError);

	protected:
		/**
		 * Only routes into the shared archive path when this frame is
		 * actually going to be kept (saveImage on) - a saveImage=0 frame
		 * still gets created-then-deleted (see kernel/src/devcliimg.cpp's
		 * processCameraImage() and gui/STATUS.md), but falls through to
		 * the base class's own scratch-cwd default instead, so that
		 * create-then-delete cycle never happens inside the archive other
		 * tools (backup/sync) rely on seeing only definitive images in.
		 */
		virtual rts2image::Image *createImage (const struct timeval *expStart) override;

		virtual void cameraImageReady (rts2image::Image *image) override;
		virtual void exposureStarted (bool expectImage) override;
		virtual void exposureEnd (bool expectImage) override;
		virtual void valueChanged (rts2core::Value *value) override;
		virtual void stateChanged (rts2core::ServerState *state) override;

	private:
		void runFitOnData (const void *data, int dataType, long width, long height);

		std::atomic<int> measureX { 0 };
		std::atomic<int> measureY { 0 };
		std::atomic<int> measureW { 32 };
		std::atomic<int> measureH { 32 };

		// Raw pixel cache backing refit() - worker-thread-only, both to
		// write (cameraImageReady()) and to read (refit(), itself only
		// ever invoked from this thread), so no locking needed.
		std::vector<char> lastImageData;
		long lastWidth = 0;
		long lastHeight = 0;
		int lastDataType = 0;

		std::string archiveExpandPath;

		// Written (under lock) from valueChanged() on this object's own
		// worker thread every time it fires, regardless of whether anyone
		// is listening yet - read (under lock) from the GUI thread by
		// snapshotValues(). Plain QMap/QRect data, not RTS2 Value/Connection
		// objects, so a simple mutex is enough - no RTS2 wire-protocol state
		// is touched from the GUI thread this way.
		mutable std::mutex valuesMutex;
		QMap<QString, double> lastValues;
		QMap<QString, QStringList> lastChoices;
		QMap<QString, QRect> lastRects;
};

}
