#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QTimer>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

#include "gui/viewerclient.h"
#include "gui/viewercamera.h"
#include "gui/imagecanvas.h"
#include "gui/focusgraphwidget.h"

namespace gui
{

/**
 * One entry in a camera's focus-progress history (see
 * CameraState::focusHistory) - only ever appended for a fit that came
 * from a genuinely new image (MainWindow::onFitResult()'s
 * pendingNewImageFit check), never from a manual refit-on-drag.
 */
struct FocusHistoryPoint
{
	int imageIndex;
	double focPos; // NAN if "focpos" wasn't reported by this camera at all
	double fwhmX;
	double fwhmY;
};

/**
 * Multiple cameras may be connected at once (rts2, like
 * fiber_pointing_client.py's G1/G2 setup, routinely runs more than one) -
 * every camera keeps receiving in the background regardless of which one
 * is selected in the camera combo box, so its last-known state (filter,
 * binning, temperatures, progress, ...) is cached here and replayed onto
 * the control widgets the moment it becomes the active one again, rather
 * than waiting for the device to resend it.
 */
struct CameraState
{
	QMap<QString, double> values;
	QMap<QString, QStringList> choices;
	bool exposing = false;
	double progressStart = NAN;
	double progressEnd = NAN;
	QString stateText;
	bool hasError = false;
	// Mirrors DevClientCameraImage::saveImage - see
	// ViewerClient::createOtherType(), which forces every newly-created
	// camera to start with saving off.
	bool saveEnabled = false;

	// Full chip geometry ("SIZE" ValueRectangle) - what "windowing off"
	// resets WINDOW to. Invalid (QRect()) until the device has reported it.
	QRect chipSize;

	// Last "WINDOW" the device itself reported - this is the *live*,
	// currently-configured value (what the *next* exposure will use), not
	// necessarily what the currently-displayed image was actually taken
	// with (the two differ any time binning/window is changed after the
	// last image but before a new one arrives). Live BINX/BINY are already
	// covered by the generic `values` map the same way.
	QRect lastWindow;

	// Snapshot of BINX/BINY/lastWindow taken the moment each image
	// actually arrives (onImageReady()) - what that image was genuinely
	// taken with, which is what the blue cursor's on-screen size/position
	// must be interpreted against (it's drawn over *this* image), not
	// whatever binning/window is currently pending for the next one. See
	// updateWindowBoxSize()/sendWindowForNextExposure().
	double imageBinX = 1.0;
	double imageBinY = 1.0;
	QRect imageWindow;

	// Last focus fit result (ViewerCamera::runFit()), in display-pixel
	// coordinates - see updateFocusPanel().
	bool fitValid = false;
	double fitCentroidX = 0, fitCentroidY = 0;
	double fitFwhmX = 0, fitFwhmY = 0;
	double fitPeak = 0, fitBackground = 0;

	// Set in onImageReady(), consumed in onFitResult(): distinguishes a
	// fit that just arrived for a genuinely new image from one triggered
	// by a manual refit (cursor moved, no new exposure) - only the former
	// belongs in the focus-progress graph.
	bool pendingNewImageFit = false;

	// Focus-progress graph history - see FocusHistoryPoint above.
	// Per-camera, like everything else here, and off by default (a
	// deliberate opt-in, same reasoning as image saving).
	QVector<FocusHistoryPoint> focusHistory;
	bool focusGraphEnabled = false;
};

class MainWindow : public QMainWindow
{
	Q_OBJECT

	public:
		MainWindow (int argc, char **argv, QWidget *parent = nullptr);
		~MainWindow () override;

	private slots:
		void onCameraCreated (QString name, gui::ViewerCamera *camera);
		void onExposeClicked ();
		void onRunStopClicked ();
		void onSaveToggled (bool checked);
		void onCameraComboChanged (int index);
		void onFilterComboChanged (int index);
		void onBinningComboChanged (int index);
		void onShutterComboChanged (int index);
		void onCcdSetChanged ();
		void onCoolingToggled (bool checked);
		void onWindowingToggled (bool checked);
		void onWindowSizeChanged (int size);
		void onMeasureSizeChanged (int size);
		void onFocusGraphEnableToggled (bool checked);
		void onFocusGraphResetClicked ();
		void onFocusGraphAxisChanged (int index);
		void onProgressTick ();

	private:
		void onImageReady (const QString &cameraName, QImage image);
		void onExposureStateChanged (const QString &cameraName, bool exposing);
		void onValueUpdated (const QString &cameraName, const QString &valueName, double numericValue, QStringList choices);
		void onProgressUpdated (const QString &cameraName, double start, double end);
		void onStateTextChanged (const QString &cameraName, const QString &stateText, bool hasError);
		void onRectangleUpdated (const QString &cameraName, const QString &valueName, int x, int y, int w, int h);
		void onFitResult (const QString &cameraName, bool valid, double centroidX, double centroidY, double fwhmX, double fwhmY, double peak, double background);

		void applyValueToWidgets (const QString &valueName, double numericValue, const QStringList &choices);
		void updateStatusPanel ();
		void updateSaveButton ();
		void updateFocusPanel ();
		void updateFocusGraph ();
		void updateFocusGraphAxisAvailability ();
		void updateWindowBoxSize ();
		void sendWindowForNextExposure ();
		void startNextRunExposure ();
		void log (const QString &message);

		ClientThread *clientThread;
		QMap<QString, ViewerCamera *> cameras;
		QMap<QString, CameraState> cameraStates;
		QString activeCamera;

		// Run/Stop sequence state (repeated exposures - see onRunStopClicked).
		// runTotal < 0 means "run until Stop is pressed" (repeatSpin's
		// special zero value); runIndex counts completed exposures so far.
		bool runActive = false;
		int runTotal = 0;
		int runIndex = 0;

		// Last measure-cursor rect pushed to the active camera (see
		// onProgressTick()) - compared each tick so a refit is only
		// requested when the cursor actually moved/resized, not on every
		// single 200ms tick regardless.
		QRect lastPushedMeasureRect;

		ImageCanvas *canvas;
		QPlainTextEdit *logView;

		QComboBox *cameraCombo;
		QDoubleSpinBox *exptimeSpin;
		QSpinBox *repeatSpin;
		QPushButton *exposeButton;
		QPushButton *runButton;
		QPushButton *saveButton;
		QProgressBar *progressBar;
		QLabel *elapsedLabel;
		QLabel *remainingLabel;
		QTimer *progressTimer;

		QComboBox *filterCombo;
		QComboBox *binningCombo;
		QComboBox *shutterCombo;
		QLabel *ccdTempLabel;
		QDoubleSpinBox *ccdSetSpin;
		QCheckBox *coolingCheck;

		// Windowing (chip subframe for the next exposure) - blue cursor on
		// the canvas, applied via ViewerClient::requestWindowChange() right
		// before every exposure request (see sendWindowForNextExposure()).
		QCheckBox *windowingCheck;
		QSpinBox *windowSizeSpin;

		// Top-left "Focus & Guiding" panel - currently just focus: the red
		// cursor's size, a zoomed crop of the region under it with the
		// fitted centroid marked, and the fit's FWHM/peak numbers (see
		// ViewerCamera::runFit(), updateFocusPanel()). Guiding is not
		// implemented - this is the "room reserved" panel from round 5.
		QSpinBox *measureSizeSpin;
		QLabel *zoomedView;
		QLabel *fwhmXLabel;
		QLabel *fwhmYLabel;
		QLabel *peakLabel;

		// Focus-progress graph (FWHM X in red, FWHM Y in blue, vs. image
		// index or focuser position) - opt-in like image saving, per
		// camera like everything else here. "Focuser position" is only
		// selectable once "focpos" has actually been seen from the active
		// camera (i.e. it's cross-linked to a focuser) -
		// updateFocusGraphAxisAvailability().
		QCheckBox *focusGraphEnableCheck;
		QPushButton *focusGraphResetButton;
		QComboBox *focusGraphAxisCombo;
		FocusGraphWidget *focusGraphWidget;

		// Bottom-left "Status" panel (active camera only) - modeled on
		// fiber_pointing_client.py's own state/chip-temp/voltage/power/
		// filter corner, but read-only: these mirror widgets already
		// interactive elsewhere (Cooling group, filter combo) rather than
		// replacing them. statusStateLabel shows one fixed-width phase
		// word (see ViewerCamera::stateChanged()), not RTS2's own raw
		// "|"-joined status string - that varies in both content and
		// length as the device does things, which made the window keep
		// reflowing.
		QLabel *statusStateLabel;
		QLabel *statusTempLabel;
		QLabel *statusVoltageLabel;
		QLabel *statusPowerLabel;
		QLabel *statusFilterLabel;
};

}
