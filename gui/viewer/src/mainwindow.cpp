#include "gui/mainwindow.h"

#include <connection.h>

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QWidget>
#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>
#include <QStandardItemModel>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

using namespace gui;

namespace
{
	QString formatDuration (double seconds)
	{
		int total = (int) seconds;
		int h = total / 3600;
		int m = (total % 3600) / 60;
		int s = total % 60;
		return QString ("%1:%2:%3")
			.arg (h, 2, 10, QChar ('0'))
			.arg (m, 2, 10, QChar ('0'))
			.arg (s, 2, 10, QChar ('0'));
	}
}

MainWindow::MainWindow (int argc, char **argv, QWidget *parent):
	QMainWindow (parent)
{
	setWindowTitle ("rts2-viewer");
	resize (1100, 700);

	// Top/bottom split: top row is image + controls (with room reserved on
	// the left for a future focus/guiding panel), bottom row is the status
	// corner + log. QSplitter (not dock widgets) so the two rows are a
	// fixed structure the user can resize but not rearrange - the point
	// raised was specifically that the window kept reflowing as the
	// camera did things, so the overall shape needs to stay put.
	QSplitter *mainSplitter = new QSplitter (Qt::Vertical, this);
	setCentralWidget (mainSplitter);

	QSplitter *topSplitter = new QSplitter (Qt::Horizontal, mainSplitter);

	// Focus helper - the room reserved here in round 5 for "focusing helper
	// + guiding"; guiding still isn't implemented, but focus now is: the
	// red measure cursor's size, a zoomed crop of the region under it with
	// the fitted centroid marked, and the fit's FWHM/peak numbers (see
	// ViewerCamera::runFit(), updateFocusPanel()).
	QGroupBox *focusPanel = new QGroupBox ("Focus && Guiding", topSplitter);
	QVBoxLayout *focusLayout = new QVBoxLayout (focusPanel);
	focusPanel->setMinimumWidth (220);

	QFormLayout *focusForm = new QFormLayout ();

	measureSizeSpin = new QSpinBox (focusPanel);
	measureSizeSpin->setRange (8, 512);
	measureSizeSpin->setValue (32);
	measureSizeSpin->setSuffix (" px");
	focusForm->addRow ("Measure box:", measureSizeSpin);

	fwhmXLabel = new QLabel ("n/a", focusPanel);
	focusForm->addRow ("FWHM X:", fwhmXLabel);

	fwhmYLabel = new QLabel ("n/a", focusPanel);
	focusForm->addRow ("FWHM Y:", fwhmYLabel);

	peakLabel = new QLabel ("n/a", focusPanel);
	focusForm->addRow ("Peak:", peakLabel);

	focusLayout->addLayout (focusForm);

	// Zoomed crop of the measure box, scaled up, with the fitted centroid
	// marked - a C++/Qt rebuild of fiber_pointing_client.py's
	// StarMatplotlibCanvas.plot_star().
	zoomedView = new QLabel (focusPanel);
	zoomedView->setFixedSize (200, 200);
	zoomedView->setAlignment (Qt::AlignCenter);
	zoomedView->setStyleSheet ("background-color: black; color: white;");
	zoomedView->setText ("no fit yet");
	focusLayout->addWidget (zoomedView);

	// Focus-progress graph (FWHM X/Y vs. image index or focuser position) -
	// opt-in, off by default, same reasoning as image saving: a viewer
	// used for casual framing doesn't need every frame silently piling
	// into a history. "Focuser position" only becomes selectable once
	// "focpos" is actually seen from the active camera (i.e. cross-linked
	// to a focuser) - updateFocusGraphAxisAvailability().
	QGroupBox *graphBox = new QGroupBox ("Focus progress", focusPanel);
	QVBoxLayout *graphLayout = new QVBoxLayout (graphBox);

	QHBoxLayout *graphControlsLayout = new QHBoxLayout ();
	focusGraphEnableCheck = new QCheckBox ("Track", graphBox);
	graphControlsLayout->addWidget (focusGraphEnableCheck);

	focusGraphResetButton = new QPushButton ("Reset", graphBox);
	focusGraphResetButton->setEnabled (false);
	graphControlsLayout->addWidget (focusGraphResetButton);
	graphLayout->addLayout (graphControlsLayout);

	focusGraphAxisCombo = new QComboBox (graphBox);
	focusGraphAxisCombo->addItem ("Image index");
	focusGraphAxisCombo->addItem ("Focuser position");
	focusGraphAxisCombo->setEnabled (false);
	{
		// Disabled until "focpos" is actually seen - no camera/focuser
		// linkage confirmed yet at construction time.
		QStandardItemModel *model = qobject_cast<QStandardItemModel *> (focusGraphAxisCombo->model ());
		if (model)
			model->item (1)->setEnabled (false);
	}
	graphLayout->addWidget (focusGraphAxisCombo);

	focusGraphWidget = new FocusGraphWidget (graphBox);
	graphLayout->addWidget (focusGraphWidget);

	focusLayout->addWidget (graphBox);

	focusLayout->addStretch ();
	connect (measureSizeSpin, QOverload<int>::of (&QSpinBox::valueChanged), this, &MainWindow::onMeasureSizeChanged);
	connect (focusGraphEnableCheck, &QCheckBox::toggled, this, &MainWindow::onFocusGraphEnableToggled);
	connect (focusGraphResetButton, &QPushButton::clicked, this, &MainWindow::onFocusGraphResetClicked);
	connect (focusGraphAxisCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onFocusGraphAxisChanged);

	canvas = new ImageCanvas (topSplitter);

	// --- Right-hand control column (camera selector, expose, filter/
	// binning, cooling) - modeled on fiber_pointing_client.py's own right
	// column, one panel instead of that script's per-section dock widgets
	// since there is only one camera visible at a time here. ------------
	QWidget *controlWidget = new QWidget (topSplitter);
	QVBoxLayout *controlLayout = new QVBoxLayout (controlWidget);
	controlWidget->setMinimumWidth (260);

	cameraCombo = new QComboBox (controlWidget);
	controlLayout->addWidget (new QLabel ("Camera:", controlWidget));
	controlLayout->addWidget (cameraCombo);

	QGroupBox *exposeBox = new QGroupBox ("Expose", controlWidget);
	QFormLayout *exposeForm = new QFormLayout (exposeBox);

	exptimeSpin = new QDoubleSpinBox (exposeBox);
	exptimeSpin->setRange (0.0, 3600.0);
	exptimeSpin->setDecimals (3);
	exptimeSpin->setValue (1.0);
	exptimeSpin->setSuffix (" s");
	exposeForm->addRow ("Exposure time:", exptimeSpin);

	repeatSpin = new QSpinBox (exposeBox);
	repeatSpin->setRange (0, 9999);
	repeatSpin->setValue (1);
	repeatSpin->setSpecialValueText ("∞ (until stopped)");
	exposeForm->addRow ("Repeat:", repeatSpin);

	exposeButton = new QPushButton ("Expose", exposeBox);
	runButton = new QPushButton ("Run", exposeBox);
	exposeForm->addRow (exposeButton, runButton);

	// Saving is purely client-side (DevClientCameraImage::saveImage,
	// ViewerClient::requestSaveToggle()) - off by default (see
	// ViewerClient::createOtherType()), so casual framing/focusing
	// doesn't silently fill the disk with FITS files until the user
	// deliberately turns it on for a real acquisition.
	saveButton = new QPushButton ("Saving: OFF", exposeBox);
	saveButton->setCheckable (true);
	exposeForm->addRow ("Save to disk:", saveButton);
	updateSaveButton ();

	// Progress bar + elapsed/remaining, the same PROTO_PROGRESS-derived
	// percentage rts2-mon's own display uses
	// (rts2core::Connection::getProgress(), kernel/src/connection.cpp) -
	// see ViewerClient::progress() and onProgressTick() below.
	progressBar = new QProgressBar (exposeBox);
	progressBar->setRange (0, 100);
	progressBar->setValue (0);
	progressBar->setFormat ("ready");
	exposeForm->addRow (progressBar);

	elapsedLabel = new QLabel ("--:--:--", exposeBox);
	exposeForm->addRow ("Elapsed:", elapsedLabel);

	remainingLabel = new QLabel ("--:--:--", exposeBox);
	exposeForm->addRow ("Remaining:", remainingLabel);

	QGroupBox *cameraBox = new QGroupBox ("Camera settings", controlWidget);
	QFormLayout *cameraForm = new QFormLayout (cameraBox);

	filterCombo = new QComboBox (cameraBox);
	filterCombo->setEnabled (false);
	cameraForm->addRow ("Filter:", filterCombo);

	binningCombo = new QComboBox (cameraBox);
	binningCombo->setEnabled (false);
	cameraForm->addRow ("Binning:", binningCombo);

	// SHUTTER is a plain ValueSelection ("LIGHT"/"DARK",
	// base/camd/include/camd.h's expType) - same generic
	// combo/applyValueToWidgets/requestValueChange mechanism as filter and
	// binning above. This is what actually lets a dark frame be taken.
	shutterCombo = new QComboBox (cameraBox);
	shutterCombo->setEnabled (false);
	cameraForm->addRow ("Shutter:", shutterCombo);

	QGroupBox *coolingBox = new QGroupBox ("Cooling", controlWidget);
	QFormLayout *coolingForm = new QFormLayout (coolingBox);

	ccdTempLabel = new QLabel ("...", coolingBox);
	coolingForm->addRow ("CCD temperature:", ccdTempLabel);

	ccdSetSpin = new QDoubleSpinBox (coolingBox);
	ccdSetSpin->setRange (-80.0, 40.0);
	ccdSetSpin->setDecimals (1);
	ccdSetSpin->setSuffix (" °C");
	ccdSetSpin->setEnabled (false);
	coolingForm->addRow ("Set point:", ccdSetSpin);

	coolingCheck = new QCheckBox ("Cooling on", coolingBox);
	coolingCheck->setEnabled (false);
	coolingForm->addRow (coolingCheck);

	// Windowing: off by default (canvas's blue cursor hidden) - the flow is
	// take an image, switch this on, position the blue cursor, then the
	// next exposure request applies it (sendWindowForNextExposure(), called
	// from onExposeClicked()/startNextRunExposure()). Off sends the full
	// chip rect instead, so turning it back off actually restores full-frame
	// reads rather than leaving the last window in place.
	QGroupBox *windowBox = new QGroupBox ("Windowing", controlWidget);
	QFormLayout *windowForm = new QFormLayout (windowBox);

	windowingCheck = new QCheckBox ("Enable windowing", windowBox);
	windowForm->addRow (windowingCheck);

	windowSizeSpin = new QSpinBox (windowBox);
	windowSizeSpin->setRange (8, 8192);
	windowSizeSpin->setValue (256);
	windowSizeSpin->setSuffix (" px");
	windowForm->addRow ("Window size:", windowSizeSpin);

	controlLayout->addWidget (exposeBox);
	controlLayout->addWidget (cameraBox);
	controlLayout->addWidget (coolingBox);
	controlLayout->addWidget (windowBox);
	controlLayout->addStretch ();

	topSplitter->addWidget (focusPanel);
	topSplitter->addWidget (canvas);
	topSplitter->addWidget (controlWidget);
	topSplitter->setStretchFactor (0, 0);
	topSplitter->setStretchFactor (1, 1);
	topSplitter->setStretchFactor (2, 0);

	// --- Bottom row: "Status" corner (active camera only) + log --------
	// Status is modeled on fiber_pointing_client.py's own state/chip-temp/
	// voltage/power/filter corner (refresh_state() in that script).
	// Read-only: a compact at-a-glance summary, not a second set of
	// controls. Voltage/power ("VOLTAGE"/"TEMPPWR") are gxccd-specific
	// Values (base/camd/gxccd/gxccd.cpp) - they simply stay "n/a" on
	// cameras that don't report them (e.g. the dummy driver used for
	// testing). ----------------------------------------------------------
	QSplitter *bottomSplitter = new QSplitter (Qt::Horizontal, mainSplitter);

	QWidget *statusWidget = new QWidget (bottomSplitter);
	QFormLayout *statusForm = new QFormLayout (statusWidget);
	statusWidget->setMinimumWidth (260);

	statusStateLabel = new QLabel ("not connected", statusWidget);
	// Fixed width (the widest of the fixed phase words this can ever show
	// - see ViewerCamera::stateChanged()) so the layout doesn't reflow as
	// the camera moves between Idle/Exposing/Reading/etc.
	{
		QFontMetrics fm (statusStateLabel->font ());
		int w = 0;
		for (const QString s : {"Idle", "Exposing", "Reading", "Shifting", "Frame transfer", "HW error"})
			w = qMax (w, fm.horizontalAdvance (s));
		statusStateLabel->setMinimumWidth (w + 12);
	}
	statusForm->addRow ("State:", statusStateLabel);

	statusTempLabel = new QLabel ("n/a", statusWidget);
	statusForm->addRow ("Chip temp / set:", statusTempLabel);

	statusVoltageLabel = new QLabel ("n/a", statusWidget);
	statusForm->addRow ("Voltage:", statusVoltageLabel);

	statusPowerLabel = new QLabel ("n/a", statusWidget);
	statusForm->addRow ("Cooling power:", statusPowerLabel);

	statusFilterLabel = new QLabel ("n/a", statusWidget);
	statusForm->addRow ("Filter:", statusFilterLabel);

	logView = new QPlainTextEdit (bottomSplitter);
	logView->setReadOnly (true);

	bottomSplitter->addWidget (statusWidget);
	bottomSplitter->addWidget (logView);
	bottomSplitter->setStretchFactor (0, 0);
	bottomSplitter->setStretchFactor (1, 1);

	mainSplitter->addWidget (topSplitter);
	mainSplitter->addWidget (bottomSplitter);
	mainSplitter->setStretchFactor (0, 3);
	mainSplitter->setStretchFactor (1, 1);

	connect (cameraCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onCameraComboChanged);
	connect (exposeButton, &QPushButton::clicked, this, &MainWindow::onExposeClicked);
	connect (runButton, &QPushButton::clicked, this, &MainWindow::onRunStopClicked);
	connect (saveButton, &QPushButton::toggled, this, &MainWindow::onSaveToggled);
	connect (filterCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onFilterComboChanged);
	connect (binningCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onBinningComboChanged);
	connect (shutterCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onShutterComboChanged);
	connect (ccdSetSpin, &QDoubleSpinBox::editingFinished, this, &MainWindow::onCcdSetChanged);
	connect (coolingCheck, &QCheckBox::toggled, this, &MainWindow::onCoolingToggled);
	connect (windowingCheck, &QCheckBox::toggled, this, &MainWindow::onWindowingToggled);
	connect (windowSizeSpin, QOverload<int>::of (&QSpinBox::valueChanged), this, &MainWindow::onWindowSizeChanged);

	// --- RTS2 connection, on its own thread (see viewerclient.h) --------
	clientThread = new ClientThread (argc, argv, this);
	connect (clientThread, &ClientThread::cameraCreated, this, &MainWindow::onCameraCreated);
	connect (clientThread, &ClientThread::progressUpdated, this, [this] (QString cameraName, double start, double end) { onProgressUpdated (cameraName, start, end); }, Qt::QueuedConnection);
	clientThread->start ();

	// Local wall-clock interpolation of the last known progress window -
	// PROTO_PROGRESS itself only arrives once per state change (exposure
	// start, then readout start), not continuously, same as rts2-mon.
	progressTimer = new QTimer (this);
	progressTimer->setInterval (200);
	connect (progressTimer, &QTimer::timeout, this, &MainWindow::onProgressTick);
	progressTimer->start ();

	log ("connecting, waiting for camera devices to become ready ...");
}

MainWindow::~MainWindow ()
{
	clientThread->requestQuit ();
	clientThread->wait (5000);
}

void MainWindow::onCameraCreated (QString name, ViewerCamera *camera)
{
	cameras[name] = camera;

	cameraCombo->blockSignals (true);
	cameraCombo->addItem (name);
	cameraCombo->blockSignals (false);

	connect (camera, &ViewerCamera::imageReady, this, [this, name] (QImage image) { onImageReady (name, image); }, Qt::QueuedConnection);
	connect (camera, &ViewerCamera::exposureStateChanged, this, [this, name] (bool exposing) { onExposureStateChanged (name, exposing); }, Qt::QueuedConnection);
	connect (camera, &ViewerCamera::valueUpdated, this, [this, name] (QString valueName, double numericValue, QStringList choices) { onValueUpdated (name, valueName, numericValue, choices); }, Qt::QueuedConnection);
	connect (camera, &ViewerCamera::stateTextChanged, this, [this, name] (QString stateText, bool hasError) { onStateTextChanged (name, stateText, hasError); }, Qt::QueuedConnection);
	connect (camera, &ViewerCamera::rectangleUpdated, this, [this, name] (QString valueName, int x, int y, int w, int h) { onRectangleUpdated (name, valueName, x, y, w, h); }, Qt::QueuedConnection);
	connect (camera, &ViewerCamera::fitResult, this, [this, name] (bool valid, double cx, double cy, double fwhmX, double fwhmY, double peak, double bg) { onFitResult (name, valid, cx, cy, fwhmX, fwhmY, peak, bg); }, Qt::QueuedConnection);

	log ("camera '" + name + "' ready");

	if (cameraCombo->count () == 1)
		onCameraComboChanged (0);
}

void MainWindow::onCameraComboChanged (int index)
{
	if (index < 0)
		return;

	activeCamera = cameraCombo->itemText (index);
	clientThread->setActiveCamera (activeCamera.toStdString ());
	setWindowTitle (QString ("rts2-viewer - %1").arg (activeCamera));

	// Reset to a "waiting" state, then immediately replay this camera's
	// last-known values from the cache - it may already have reported its
	// full state a while ago, before it became the active one.
	filterCombo->setEnabled (false);
	binningCombo->setEnabled (false);
	shutterCombo->setEnabled (false);
	ccdSetSpin->setEnabled (false);
	coolingCheck->setEnabled (false);
	ccdTempLabel->setText ("...");

	statusVoltageLabel->setText ("n/a");
	statusPowerLabel->setText ("n/a");
	statusFilterLabel->setText ("n/a");
	statusTempLabel->setText ("n/a");

	const CameraState &state = cameraStates.value (activeCamera);
	for (auto it = state.values.constBegin (); it != state.values.constEnd (); ++it)
		applyValueToWidgets (it.key (), it.value (), state.choices.value (it.key ()));

	updateStatusPanel ();
	updateFocusPanel ();
	updateWindowBoxSize ();
	updateFocusGraphAxisAvailability ();
	updateFocusGraph ();

	// Restore this camera's own saving state (per-camera, not global) -
	// blockSignals since this is just reflecting the cache, not a new
	// user action that should re-send anything.
	saveButton->blockSignals (true);
	saveButton->setChecked (state.saveEnabled);
	saveButton->blockSignals (false);
	updateSaveButton ();

	focusGraphEnableCheck->blockSignals (true);
	focusGraphEnableCheck->setChecked (state.focusGraphEnabled);
	focusGraphEnableCheck->blockSignals (false);
	focusGraphResetButton->setEnabled (state.focusGraphEnabled);
	focusGraphAxisCombo->setEnabled (state.focusGraphEnabled);

	log ("switched to camera '" + activeCamera + "'");
}

void MainWindow::onImageReady (const QString &cameraName, QImage image)
{
	// Snapshot what this image was actually taken with - BINX/BINY/WINDOW
	// may already have moved on to a newer, still-pending value (the user
	// changed binning/window but hasn't re-exposed yet), but the box drawn
	// over *this* image has to be interpreted against what *this* image
	// used, not that. See updateWindowBoxSize()/sendWindowForNextExposure().
	CameraState &state = cameraStates[cameraName];
	state.imageBinX = state.values.value ("BINX", 1.0);
	state.imageBinY = state.values.value ("BINY", 1.0);
	state.imageWindow = state.lastWindow;

	// Marks the *next* fitResult for this camera as belonging to a
	// genuinely new image, not a manual refit-on-drag - see onFitResult(),
	// which is what actually decides whether to append to focusHistory.
	state.pendingNewImageFit = true;

	if (cameraName != activeCamera)
		return;
	canvas->setImage (image);
	log ("new image received");
	updateFocusPanel ();
	updateWindowBoxSize ();
}

void MainWindow::onExposureStateChanged (const QString &cameraName, bool exposing)
{
	CameraState &state = cameraStates[cameraName];
	state.exposing = exposing;

	if (cameraName != activeCamera)
		return;

	if (!exposing && runActive)
	{
		if (runTotal != 0 && runIndex >= runTotal)
		{
			runActive = false;
			runButton->setText ("Run");
			exposeButton->setEnabled (true);
			repeatSpin->setEnabled (true);
			log ("run sequence finished");
		}
		else
		{
			startNextRunExposure ();
		}
	}
}

void MainWindow::onValueUpdated (const QString &cameraName, const QString &valueName, double numericValue, QStringList choices)
{
	CameraState &state = cameraStates[cameraName];
	state.values[valueName] = numericValue;
	if (!choices.isEmpty ())
		state.choices[valueName] = choices;

	if (cameraName != activeCamera)
		return;

	applyValueToWidgets (valueName, numericValue, choices.isEmpty () ? state.choices.value (valueName) : choices);
	updateStatusPanel ();

	if (valueName == "focpos")
		updateFocusGraphAxisAvailability ();

	// Deliberately NOT resizing the window box here on a live BINX/BINY
	// change: that value updates as soon as the user picks a new binning,
	// well before any new image actually uses it, and the box is drawn
	// over the *currently displayed* image - resizing it immediately made
	// it represent the wrong thing until the next exposure actually
	// landed. It's kept in sync with reality in onImageReady() instead,
	// snapshotting BINX/BINY/WINDOW at the moment each image arrives.
}

void MainWindow::onProgressUpdated (const QString &cameraName, double start, double end)
{
	CameraState &state = cameraStates[cameraName];
	state.progressStart = start;
	state.progressEnd = end;
}

void MainWindow::onStateTextChanged (const QString &cameraName, const QString &stateText, bool hasError)
{
	CameraState &state = cameraStates[cameraName];
	state.stateText = stateText;
	state.hasError = hasError;

	if (cameraName != activeCamera)
		return;

	updateStatusPanel ();
}

void MainWindow::onRectangleUpdated (const QString &cameraName, const QString &valueName, int x, int y, int w, int h)
{
	if (valueName == "SIZE")
		cameraStates[cameraName].chipSize = QRect (x, y, w, h);
	else if (valueName == "WINDOW")
		cameraStates[cameraName].lastWindow = QRect (x, y, w, h);
}

void MainWindow::onFitResult (const QString &cameraName, bool valid, double centroidX, double centroidY, double fwhmX, double fwhmY, double peak, double background)
{
	CameraState &state = cameraStates[cameraName];
	state.fitValid = valid;
	state.fitCentroidX = centroidX;
	state.fitCentroidY = centroidY;
	state.fitFwhmX = fwhmX;
	state.fitFwhmY = fwhmY;
	state.fitPeak = peak;
	state.fitBackground = background;

	// Only a fit for a genuinely new image belongs in the focus-progress
	// history - a manual refit (cursor just dragged, no new exposure)
	// would otherwise flood the graph with points that don't represent
	// actual focusing progress.
	bool isNewImageFit = state.pendingNewImageFit;
	state.pendingNewImageFit = false;

	if (isNewImageFit && valid && state.focusGraphEnabled)
	{
		FocusHistoryPoint pt;
		pt.imageIndex = state.focusHistory.size ();
		pt.focPos = state.values.contains ("focpos") ? state.values.value ("focpos") : NAN;
		pt.fwhmX = fwhmX;
		pt.fwhmY = fwhmY;
		state.focusHistory.append (pt);
	}

	if (cameraName != activeCamera)
		return;

	updateFocusPanel ();
	updateFocusGraph ();
}

void MainWindow::updateStatusPanel ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
		return;

	const CameraState &state = cameraStates[activeCamera];

	if (!state.stateText.isEmpty ())
	{
		statusStateLabel->setText (state.stateText);
		statusStateLabel->setStyleSheet (state.hasError ? "background-color: #FFCCCC;" : "background-color: #CCFFCC;");
	}

	double ccdTemp = state.values.value ("CCD_TEMP", NAN);
	double ccdSet = state.values.value ("CCD_SET", NAN);
	if (!std::isnan (ccdTemp))
	{
		QString text = QString ("%1 °C").arg (ccdTemp, 0, 'f', 1);
		if (!std::isnan (ccdSet))
		{
			text += QString (" / %1 °C").arg (ccdSet, 0, 'f', 1);
			bool close = std::abs (ccdTemp - ccdSet) < 0.5;
			statusTempLabel->setStyleSheet (close ? "background-color: #CCFFCC;" : "background-color: #FFCCCC;");
		}
		statusTempLabel->setText (text);
	}

	// VOLTAGE/TEMPPWR are gxccd-specific (base/camd/gxccd/gxccd.cpp) -
	// simply absent (state.values won't contain the key) on the dummy
	// driver and most other camera families, leaving the label at "n/a".
	if (state.values.contains ("VOLTAGE"))
	{
		double v = state.values.value ("VOLTAGE");
		statusVoltageLabel->setText (QString ("%1 V").arg (v, 0, 'f', 1));
		// Same 11.5-15.5 V "good" band fiber_pointing_client.py used for its
		// own (also 12V-nominal) camera power supply.
		statusVoltageLabel->setStyleSheet ((v > 11.5 && v < 15.5) ? "background-color: #CCFFCC;" : "background-color: #FFCCCC;");
	}

	if (state.values.contains ("TEMPPWR"))
	{
		double p = state.values.value ("TEMPPWR");
		statusPowerLabel->setText (QString ("%1 %").arg (p, 0, 'f', 0));
		// fiber_pointing_client.py flags >=90% cooling-power utilization as
		// the camera struggling to hold its set point.
		statusPowerLabel->setStyleSheet ((p < 90.0) ? "background-color: #CCFFCC;" : "background-color: #FFCCCC;");
	}

	if (state.choices.contains ("filter"))
	{
		int idx = (int) state.values.value ("filter", -1);
		QStringList opts = state.choices.value ("filter");
		if (idx >= 0 && idx < opts.size ())
			statusFilterLabel->setText (opts[idx]);
	}
}

void MainWindow::onProgressTick ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
		return;

	// SightItem isn't a QObject (QGraphicsItem doesn't derive from one), so
	// there is no "cursor moved" signal to react to - just check wherever
	// the red cursor currently is on every tick. Cheap (plain atomic
	// stores - ViewerCamera::setMeasureRegion()). Only when it actually
	// changed (dragged, or resized via measureSizeSpin) also ask for a
	// refit against the image already on screen - no need to wait for a
	// new exposure just to see the fit follow the cursor onto a different
	// star already in frame.
	if (cameras.contains (activeCamera))
	{
		QRect mr = canvas->measureRect ();
		if (mr != lastPushedMeasureRect)
		{
			lastPushedMeasureRect = mr;
			cameras[activeCamera]->setMeasureRegion (mr.x (), mr.y (), mr.width (), mr.height ());
			clientThread->requestRefit ();
		}
	}

	const CameraState &state = cameraStates[activeCamera];

	if (!state.exposing || std::isnan (state.progressStart) || std::isnan (state.progressEnd) || state.progressEnd <= state.progressStart)
	{
		progressBar->setValue (0);
		progressBar->setFormat (state.exposing ? "exposing..." : "ready");
		elapsedLabel->setText ("--:--:--");
		remainingLabel->setText ("--:--:--");
		return;
	}

	double now = QDateTime::currentMSecsSinceEpoch () / 1000.0;
	double total = state.progressEnd - state.progressStart;
	double elapsed = now - state.progressStart;
	double remaining = state.progressEnd - now;

	int percent = qBound (0, (int) (100.0 * elapsed / total), 100);
	progressBar->setValue (percent);

	if (runActive)
		progressBar->setFormat (QString ("%1% (%2/%3)").arg (percent).arg (runIndex).arg (runTotal == 0 ? QString ("∞") : QString::number (runTotal)));
	else
		progressBar->setFormat ("%p%");

	elapsedLabel->setText (formatDuration (qMax (0.0, elapsed)));
	remainingLabel->setText (formatDuration (qMax (0.0, remaining)));
}

void MainWindow::applyValueToWidgets (const QString &valueName, double numericValue, const QStringList &choices)
{
	if (valueName == "filter")
	{
		if (!choices.isEmpty () && filterCombo->count () != choices.size ())
		{
			filterCombo->blockSignals (true);
			filterCombo->clear ();
			filterCombo->addItems (choices);
			filterCombo->blockSignals (false);
		}
		filterCombo->blockSignals (true);
		filterCombo->setCurrentIndex ((int) numericValue);
		filterCombo->blockSignals (false);
		filterCombo->setEnabled (true);
	}
	else if (valueName == "binning")
	{
		if (!choices.isEmpty () && binningCombo->count () != choices.size ())
		{
			binningCombo->blockSignals (true);
			binningCombo->clear ();
			binningCombo->addItems (choices);
			binningCombo->blockSignals (false);
		}
		binningCombo->blockSignals (true);
		binningCombo->setCurrentIndex ((int) numericValue);
		binningCombo->blockSignals (false);
		binningCombo->setEnabled (true);
	}
	else if (valueName == "SHUTTER")
	{
		if (!choices.isEmpty () && shutterCombo->count () != choices.size ())
		{
			shutterCombo->blockSignals (true);
			shutterCombo->clear ();
			shutterCombo->addItems (choices);
			shutterCombo->blockSignals (false);
		}
		shutterCombo->blockSignals (true);
		shutterCombo->setCurrentIndex ((int) numericValue);
		shutterCombo->blockSignals (false);
		shutterCombo->setEnabled (true);
	}
	else if (valueName == "CCD_TEMP")
	{
		ccdTempLabel->setText (QString ("%1 °C").arg (numericValue, 0, 'f', 1));
	}
	else if (valueName == "CCD_SET")
	{
		ccdSetSpin->blockSignals (true);
		ccdSetSpin->setValue (numericValue);
		ccdSetSpin->blockSignals (false);
		ccdSetSpin->setEnabled (true);
	}
	else if (valueName == "COOLING")
	{
		coolingCheck->blockSignals (true);
		coolingCheck->setChecked (numericValue != 0);
		coolingCheck->blockSignals (false);
		coolingCheck->setEnabled (true);
	}
}

void MainWindow::onExposeClicked ()
{
	if (runActive)
		return;
	sendWindowForNextExposure ();
	double exptime = exptimeSpin->value ();
	log (QString ("requesting %1 s exposure").arg (exptime));
	clientThread->requestExposure (exptime);
}

void MainWindow::onRunStopClicked ()
{
	if (runActive)
	{
		runActive = false;
		runButton->setText ("Run");
		exposeButton->setEnabled (true);
		repeatSpin->setEnabled (true);
		clientThread->requestStop ();
		log ("run sequence stopped");
		return;
	}

	runActive = true;
	runIndex = 0;
	runTotal = repeatSpin->value ();
	runButton->setText ("Stop");
	exposeButton->setEnabled (false);
	repeatSpin->setEnabled (false);

	log (runTotal == 0
		? QString ("run sequence started (until stopped)")
		: QString ("run sequence started (%1 exposures)").arg (runTotal));

	startNextRunExposure ();
}

void MainWindow::onSaveToggled (bool checked)
{
	updateSaveButton ();

	if (activeCamera.isEmpty ())
		return;

	cameraStates[activeCamera].saveEnabled = checked;
	log (checked ? "image saving ON" : "image saving OFF");
	clientThread->requestSaveToggle (checked);
}

void MainWindow::updateSaveButton ()
{
	bool enabled = saveButton->isChecked ();
	saveButton->setText (enabled ? "Saving: ON" : "Saving: OFF");
	saveButton->setStyleSheet (enabled ? "background-color: #66CC66;" : "background-color: #CC6666;");
}

void MainWindow::onWindowingToggled (bool checked)
{
	canvas->setWindowingEnabled (checked);
	log (checked ? "windowing enabled - position the blue box, then Expose/Run" : "windowing disabled - next exposure reads the full chip");
}

void MainWindow::onWindowSizeChanged (int size)
{
	Q_UNUSED (size);
	updateWindowBoxSize ();
}

void MainWindow::onMeasureSizeChanged (int size)
{
	canvas->setMeasureSize (size);
}

void MainWindow::updateWindowBoxSize ()
{
	// WINDOW is always in absolute, unbinned chip pixels
	// (base/camd/include/camd.h's chipUsedReadout), but the blue box is
	// drawn in *display* pixels of the currently-shown image - one display
	// pixel already spans BINX x BINY real chip pixels, of *that image*,
	// not whatever binning is currently configured for the next one (the
	// two differ any time the user changes binning before re-exposing -
	// using the live value here resized the box to match a binning that
	// image on screen was never actually taken with). Keep windowSizeSpin's
	// own meaning as a fixed *physical* (unbinned) size, so the same
	// spinbox value keeps representing the same real chip area regardless
	// of binning: the on-screen box has to shrink as binning goes up,
	// exactly what the user described as "the blue box would have to be
	// 4x4 smaller" at 4x4 binning.
	double bx = 1.0, by = 1.0;
	if (!activeCamera.isEmpty () && cameraStates.contains (activeCamera))
	{
		const CameraState &state = cameraStates[activeCamera];
		bx = state.imageBinX;
		by = state.imageBinY;
	}
	if (bx <= 0)
		bx = 1.0;
	if (by <= 0)
		by = 1.0;

	int size = windowSizeSpin->value ();
	int dispW = std::max (1, (int) std::lround (size / bx));
	int dispH = std::max (1, (int) std::lround (size / by));
	canvas->setWindowSize (dispW, dispH);
}

void MainWindow::sendWindowForNextExposure ()
{
	if (activeCamera.isEmpty () || !cameras.contains (activeCamera))
		return;

	const CameraState &state = cameraStates.value (activeCamera);

	if (!canvas->windowingEnabled ())
	{
		// "off" means full chip, not "leave whatever was last set" - so a
		// previous window actually gets reset once the checkbox is
		// unchecked, rather than silently sticking. SIZE is already in
		// absolute unbinned chip coordinates, no conversion needed.
		if (!state.chipSize.isValid ())
			return;
		clientThread->requestWindowChange (state.chipSize.x (), state.chipSize.y (), state.chipSize.width (), state.chipSize.height ());
		return;
	}

	// The blue box is drawn over the currently-displayed image, so it has
	// to be converted using *that image's own* acquisition parameters
	// (imageBinX/imageBinY/imageWindow, snapshotted in onImageReady()) -
	// not the live values, which may already reflect a binning/window
	// change the user made after that image but hasn't re-exposed yet.
	// The displayed image's own pixel (0,0) is wherever imageWindow's
	// origin was, and each of its pixels spans imageBinX x imageBinY real
	// chip pixels - both have to be folded in to turn the blue box's
	// on-screen rect into the absolute chip rect WINDOW actually wants.
	double bx = state.imageBinX;
	double by = state.imageBinY;
	if (bx <= 0)
		bx = 1.0;
	if (by <= 0)
		by = 1.0;

	int originX = state.imageWindow.isValid () ? state.imageWindow.x () : 0;
	int originY = state.imageWindow.isValid () ? state.imageWindow.y () : 0;

	QRect rect = canvas->windowRect ();
	int chipX = originX + (int) std::lround (rect.x () * bx);
	int chipY = originY + (int) std::lround (rect.y () * by);
	int chipW = (int) std::lround (rect.width () * bx);
	int chipH = (int) std::lround (rect.height () * by);

	clientThread->requestWindowChange (chipX, chipY, chipW, chipH);
}

void MainWindow::updateFocusPanel ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
		return;

	const CameraState &state = cameraStates[activeCamera];

	if (!state.fitValid)
	{
		fwhmXLabel->setText ("n/a");
		fwhmYLabel->setText ("n/a");
		peakLabel->setText ("n/a");
		zoomedView->setPixmap (QPixmap ());
		zoomedView->setText ("no fit");
		return;
	}

	fwhmXLabel->setText (QString ("%1 px").arg (state.fitFwhmX, 0, 'f', 2));
	fwhmYLabel->setText (QString ("%1 px").arg (state.fitFwhmY, 0, 'f', 2));
	peakLabel->setText (QString::number (state.fitPeak, 'f', 0));

	QImage img = canvas->currentImage ();
	QRect rect = canvas->measureRect ();
	if (img.isNull () || rect.width () <= 0 || rect.height () <= 0 || !img.rect ().contains (rect))
	{
		zoomedView->setPixmap (QPixmap ());
		zoomedView->setText ("region out of bounds");
		return;
	}

	QImage crop = img.copy (rect).convertToFormat (QImage::Format_RGB32);
	QImage scaled = crop.scaled (zoomedView->width (), zoomedView->height (), Qt::KeepAspectRatio, Qt::FastTransformation);

	QPainter painter (&scaled);
	painter.setPen (QPen (Qt::red, 1));
	double scaleX = (double) scaled.width () / rect.width ();
	double scaleY = (double) scaled.height () / rect.height ();
	double localX = (state.fitCentroidX - rect.x ()) * scaleX;
	double localY = (state.fitCentroidY - rect.y ()) * scaleY;
	painter.drawLine (QPointF (localX - 8, localY), QPointF (localX + 8, localY));
	painter.drawLine (QPointF (localX, localY - 8), QPointF (localX, localY + 8));
	painter.end ();

	zoomedView->setText ("");
	zoomedView->setPixmap (QPixmap::fromImage (scaled));
}

void MainWindow::onFocusGraphEnableToggled (bool checked)
{
	if (!activeCamera.isEmpty () && cameraStates.contains (activeCamera))
		cameraStates[activeCamera].focusGraphEnabled = checked;

	focusGraphResetButton->setEnabled (checked);
	focusGraphAxisCombo->setEnabled (checked);
	log (checked ? "focus graph tracking ON" : "focus graph tracking OFF");
}

void MainWindow::onFocusGraphResetClicked ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
		return;
	cameraStates[activeCamera].focusHistory.clear ();
	updateFocusGraph ();
	log ("focus graph reset");
}

void MainWindow::onFocusGraphAxisChanged (int index)
{
	Q_UNUSED (index);
	updateFocusGraph ();
}

void MainWindow::updateFocusGraphAxisAvailability ()
{
	bool hasFocPos = !activeCamera.isEmpty () && cameraStates.contains (activeCamera)
		&& cameraStates[activeCamera].values.contains ("focpos");

	QStandardItemModel *model = qobject_cast<QStandardItemModel *> (focusGraphAxisCombo->model ());
	if (model)
	{
		QStandardItem *item = model->item (1);
		if (item)
			item->setEnabled (hasFocPos);
	}

	// Fall back to "Image index" if focuser position just became
	// unavailable (e.g. switched to a camera with no linked focuser)
	// while it was selected.
	if (!hasFocPos && focusGraphAxisCombo->currentIndex () == 1)
		focusGraphAxisCombo->setCurrentIndex (0);
}

void MainWindow::updateFocusGraph ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
	{
		focusGraphWidget->setPoints ({});
		return;
	}

	const CameraState &state = cameraStates[activeCamera];
	bool useFocPos = (focusGraphAxisCombo->currentIndex () == 1);

	QVector<FocusGraphWidget::Point> points;
	points.reserve (state.focusHistory.size ());
	for (const auto &hp : state.focusHistory)
	{
		FocusGraphWidget::Point pt;
		pt.x = useFocPos ? (std::isnan (hp.focPos) ? 0.0 : hp.focPos) : (double) hp.imageIndex;
		pt.fwhmX = hp.fwhmX;
		pt.fwhmY = hp.fwhmY;
		points.append (pt);
	}
	focusGraphWidget->setPoints (points);
}

void MainWindow::startNextRunExposure ()
{
	runIndex++;
	sendWindowForNextExposure ();
	double exptime = exptimeSpin->value ();
	log (QString ("run %1/%2: requesting %3 s exposure").arg (runIndex)
		.arg (runTotal == 0 ? QString ("∞") : QString::number (runTotal))
		.arg (exptime));
	clientThread->requestExposure (exptime);
}

void MainWindow::onFilterComboChanged (int index)
{
	if (index < 0 || activeCamera.isEmpty ())
		return;
	log (QString ("filter -> %1").arg (filterCombo->currentText ()));
	clientThread->requestValueChange ("filter", '=', index);
}

void MainWindow::onBinningComboChanged (int index)
{
	if (index < 0 || activeCamera.isEmpty ())
		return;
	log (QString ("binning -> %1").arg (binningCombo->currentText ()));
	clientThread->requestValueChange ("binning", '=', index);
}

void MainWindow::onShutterComboChanged (int index)
{
	if (index < 0 || activeCamera.isEmpty ())
		return;
	log (QString ("shutter -> %1").arg (shutterCombo->currentText ()));
	clientThread->requestValueChange ("SHUTTER", '=', index);
}

void MainWindow::onCcdSetChanged ()
{
	if (activeCamera.isEmpty ())
		return;
	double t = ccdSetSpin->value ();
	log (QString ("CCD set temperature -> %1 °C").arg (t));
	clientThread->requestValueChange ("CCD_SET", '=', t);
}

void MainWindow::onCoolingToggled (bool checked)
{
	if (activeCamera.isEmpty ())
		return;
	log (checked ? "cooling on" : "cooling off");
	clientThread->requestValueChange ("COOLING", '=', checked);
}

void MainWindow::log (const QString &message)
{
	QString stamp = QDateTime::currentDateTimeUtc ().toString ("yyyy-MM-dd hh:mm:ss");
	logView->appendPlainText (stamp + " - " + message);
}
