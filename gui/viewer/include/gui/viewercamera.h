#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>
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
};

}
