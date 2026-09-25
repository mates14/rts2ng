#include "stacker/mainwindow.h"
#include "gui/nightmode.h"

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QWidget>
#include <QDateTime>
#include <QFontMetrics>
#include <QVariantMap>
#include <QSettings>
#include <QShortcut>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

using namespace stacker;

namespace
{
	QString formatDuration (double seconds)
	{
		int total = (int) seconds;
		return QString ("%1:%2:%3")
			.arg (total / 3600, 2, 10, QChar ('0'))
			.arg ((total % 3600) / 60, 2, 10, QChar ('0'))
			.arg (total % 60, 2, 10, QChar ('0'));
	}

	bool sameExptime (double a, double b)
	{
		return a > b - 5e-4 && a < b + 5e-4;
	}
}

MainWindow::MainWindow (int argc, char **argv, QWidget *parent):
	QMainWindow (parent)
{
	setWindowTitle ("rts2-stacker");
	resize (1100, 700);

	QSplitter *mainSplitter = new QSplitter (Qt::Vertical, this);
	setCentralWidget (mainSplitter);

	QSplitter *topSplitter = new QSplitter (Qt::Horizontal, mainSplitter);

	// --- Left: the stack -------------------------------------------------
	QWidget *leftWidget = new QWidget (topSplitter);
	QVBoxLayout *leftLayout = new QVBoxLayout (leftWidget);
	leftWidget->setMinimumWidth (240);

	QGroupBox *stackBox = new QGroupBox ("Stack", leftWidget);
	QFormLayout *stackForm = new QFormLayout (stackBox);

	stackFramesLabel = new QLabel ("0", stackBox);
	QFont big = stackFramesLabel->font ();
	big.setPointSizeF (big.pointSizeF () * 1.6);
	big.setBold (true);
	stackFramesLabel->setFont (big);
	stackForm->addRow ("Frames:", stackFramesLabel);

	stackExposureLabel = new QLabel ("0 s", stackBox);
	stackExposureLabel->setFont (big);
	stackForm->addRow ("Total exposure:", stackExposureLabel);

	stackFilterLabel = new QLabel ("-", stackBox);
	stackForm->addRow ("Filter:", stackFilterLabel);

	stackCalibLabel = new QLabel ("-", stackBox);
	stackCalibLabel->setWordWrap (true);
	stackForm->addRow ("Calibrated with:", stackCalibLabel);

	viewStackRadio = new QRadioButton ("Stack", stackBox);
	viewFrameRadio = new QRadioButton ("Last frame", stackBox);
	viewStackRadio->setChecked (true);
	QHBoxLayout *viewLayout = new QHBoxLayout ();
	viewLayout->addWidget (viewStackRadio);
	viewLayout->addWidget (viewFrameRadio);
	stackForm->addRow ("Show:", viewLayout);

	resetButton = new QPushButton ("Reset (save stack)", stackBox);
	stackForm->addRow (resetButton);

	leftLayout->addWidget (stackBox);

	QGroupBox *calibBox = new QGroupBox ("Calibration", leftWidget);
	QFormLayout *calibForm = new QFormLayout (calibBox);
	calibDirLabel = new QLabel ("...", calibBox);
	calibDirLabel->setWordWrap (true);
	calibForm->addRow ("Directory:", calibDirLabel);
	flatStatusLabel = new QLabel ("n/a", calibBox);
	calibForm->addRow ("Flat for filter:", flatStatusLabel);
	leftLayout->addWidget (calibBox);

	QGroupBox *measureBox = new QGroupBox ("View && measure", leftWidget);
	QFormLayout *measureForm = new QFormLayout (measureBox);

	zoomSpin = new QDoubleSpinBox (measureBox);
	zoomSpin->setRange (gui::ImageCanvas::minZoom, gui::ImageCanvas::maxZoom);
	zoomSpin->setSingleStep (0.1);
	zoomSpin->setDecimals (2);
	zoomSpin->setValue (1.0);
	zoomSpin->setSuffix ("x");
	measureForm->addRow ("Zoom:", zoomSpin);

	measureSizeSpin = new QSpinBox (measureBox);
	measureSizeSpin->setRange (8, 512);
	measureSizeSpin->setValue (32);
	measureSizeSpin->setSuffix (" px");
	measureForm->addRow ("Measure box:", measureSizeSpin);

	fwhmXLabel = new QLabel ("n/a", measureBox);
	measureForm->addRow ("FWHM X:", fwhmXLabel);
	fwhmYLabel = new QLabel ("n/a", measureBox);
	measureForm->addRow ("FWHM Y:", fwhmYLabel);
	peakLabel = new QLabel ("n/a", measureBox);
	measureForm->addRow ("Peak:", peakLabel);

	leftLayout->addWidget (measureBox);
	leftLayout->addStretch ();

	// --- Centre: image ---------------------------------------------------
	canvas = new gui::ImageCanvas (topSplitter);

	// --- Right: camera controls -----------------------------------------
	QWidget *controlWidget = new QWidget (topSplitter);
	QVBoxLayout *controlLayout = new QVBoxLayout (controlWidget);
	controlWidget->setMinimumWidth (260);

	cameraCombo = new QComboBox (controlWidget);
	controlLayout->addWidget (new QLabel ("Camera:", controlWidget));
	controlLayout->addWidget (cameraCombo);

	// Red-on-black controls, as in rts2-viewer - remembered between runs.
	nightCheck = new QCheckBox ("Night mode (Ctrl+N)", controlWidget);
	controlLayout->addWidget (nightCheck);

	QGroupBox *exposeBox = new QGroupBox ("Expose", controlWidget);
	QFormLayout *exposeForm = new QFormLayout (exposeBox);

	// With a calibration directory only exposure times that have a master
	// dark are offered (exptimeCombo); without one, any (exptimeSpin).
	exptimeCombo = new QComboBox (exposeBox);
	exptimeSpin = new QDoubleSpinBox (exposeBox);
	exptimeSpin->setRange (0.0, 3600.0);
	exptimeSpin->setDecimals (3);
	exptimeSpin->setValue (1.0);
	exptimeSpin->setSuffix (" s");
	exptimeSpin->setVisible (false);
	QHBoxLayout *exptimeLayout = new QHBoxLayout ();
	exptimeLayout->addWidget (exptimeCombo);
	exptimeLayout->addWidget (exptimeSpin);
	exposeForm->addRow ("Exposure time:", exptimeLayout);

	// Default: keep exposing until stopped - that's the demonstration.
	repeatSpin = new QSpinBox (exposeBox);
	repeatSpin->setRange (0, 9999);
	repeatSpin->setValue (0);
	repeatSpin->setSpecialValueText ("∞ (until stopped)");
	exposeForm->addRow ("Repeat:", repeatSpin);

	exposeButton = new QPushButton ("Expose", exposeBox);
	runButton = new QPushButton ("Run", exposeBox);
	exposeForm->addRow (exposeButton, runButton);

	saveButton = new QPushButton (exposeBox);
	saveButton->setCheckable (true);
	saveButton->setChecked (false);
	exposeForm->addRow ("Save frames:", saveButton);
	updateSaveButton ();

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

	controlLayout->addWidget (exposeBox);
	controlLayout->addWidget (cameraBox);
	controlLayout->addWidget (coolingBox);
	controlLayout->addStretch ();

	topSplitter->addWidget (leftWidget);
	topSplitter->addWidget (canvas);
	topSplitter->addWidget (controlWidget);
	topSplitter->setStretchFactor (0, 0);
	topSplitter->setStretchFactor (1, 1);
	topSplitter->setStretchFactor (2, 0);

	// --- Bottom: status + log -------------------------------------------
	QSplitter *bottomSplitter = new QSplitter (Qt::Horizontal, mainSplitter);

	QWidget *statusWidget = new QWidget (bottomSplitter);
	QFormLayout *statusForm = new QFormLayout (statusWidget);
	statusWidget->setMinimumWidth (260);

	statusStateLabel = new QLabel ("not connected", statusWidget);
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

	connect (canvas, &gui::ImageCanvas::zoomChanged, this, &MainWindow::onCanvasZoomChanged);
	connect (zoomSpin, QOverload<double>::of (&QDoubleSpinBox::valueChanged), this, &MainWindow::onZoomSpinChanged);
	connect (measureSizeSpin, QOverload<int>::of (&QSpinBox::valueChanged), this, &MainWindow::onMeasureSizeChanged);
	connect (resetButton, &QPushButton::clicked, this, &MainWindow::onResetClicked);
	connect (viewStackRadio, &QRadioButton::toggled, this, &MainWindow::onViewToggled);
	connect (cameraCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onCameraComboChanged);
	connect (exposeButton, &QPushButton::clicked, this, &MainWindow::onExposeClicked);
	connect (runButton, &QPushButton::clicked, this, &MainWindow::onRunStopClicked);
	connect (saveButton, &QPushButton::toggled, this, &MainWindow::onSaveToggled);
	connect (filterCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onFilterComboChanged);
	connect (binningCombo, QOverload<int>::of (&QComboBox::currentIndexChanged), this, &MainWindow::onBinningComboChanged);
	connect (ccdSetSpin, &QDoubleSpinBox::editingFinished, this, &MainWindow::onCcdSetChanged);
	connect (coolingCheck, &QCheckBox::toggled, this, &MainWindow::onCoolingToggled);
	connect (nightCheck, &QCheckBox::toggled, this, &MainWindow::onNightModeToggled);
	connect (new QShortcut (QKeySequence ("Ctrl+N"), this), &QShortcut::activated, nightCheck, &QCheckBox::toggle);
	nightCheck->setChecked (QSettings ("rts2", "rts2-stacker").value ("nightMode", false).toBool ());

	clientThread = new ClientThread (argc, argv, this);
	connect (clientThread, &ClientThread::cameraCreated, this, &MainWindow::onCameraCreated);
	connect (clientThread, &ClientThread::calibrationReady, this, &MainWindow::onCalibrationReady);
	connect (clientThread, &ClientThread::progressUpdated, this, [this] (QString cameraName, double start, double end) { onProgressUpdated (cameraName, start, end); }, Qt::QueuedConnection);
	clientThread->start ();

	progressTimer = new QTimer (this);
	progressTimer->setInterval (200);
	connect (progressTimer, &QTimer::timeout, this, &MainWindow::onProgressTick);
	progressTimer->start ();

	log ("connecting, waiting for camera devices to become ready ...");
}

MainWindow::~MainWindow ()
{
	// The client saves every non-empty stack on its way out.
	clientThread->requestQuit ();
	clientThread->wait (10000);
}

void MainWindow::onCalibrationReady (QString dir, QVariantList darks, QStringList flats, QString error)
{
	if (!error.isEmpty ())
	{
		calibDirLabel->setText (dir);
		log ("CALIBRATION ERROR: " + error + " - not connecting");
		return;
	}

	calibDarks = darks;
	calibFlats = flats;

	if (dir.isEmpty ())
	{
		calibDirLabel->setText ("none - stacking raw frames");
		exptimeCombo->setVisible (false);
		exptimeSpin->setVisible (true);
		log ("no --calib directory given: frames are stacked without dark/flat correction");
	}
	else
	{
		calibDirLabel->setText (dir);
		flats.removeDuplicates ();
		log (QString ("calibration: %1 master darks, flats for %2").arg (darks.size ()).arg (flats.isEmpty () ? QString ("no filter") : flats.join (", ")));
	}

	updateExptimeChoices ();
	updateFlatStatus ();
}

void MainWindow::onCameraCreated (QString name, StackCamera *camera)
{
	cameras[name] = camera;

	cameraCombo->blockSignals (true);
	cameraCombo->addItem (name);
	cameraCombo->blockSignals (false);

	connect (camera, &StackCamera::imageReady, this, [this, name] (QImage image) { onImageReady (name, image); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::exposureStateChanged, this, [this, name] (bool exposing) { onExposureStateChanged (name, exposing); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::valueUpdated, this, [this, name] (QString valueName, double numericValue, QStringList choices) { onValueUpdated (name, valueName, numericValue, choices); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::stateTextChanged, this, [this, name] (QString stateText, bool hasError) { onStateTextChanged (name, stateText, hasError); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::rectangleUpdated, this, [this, name] (QString valueName, int x, int y, int w, int h) { onRectangleUpdated (name, valueName, x, y, w, h); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::fitResult, this, [this, name] (bool valid, double, double, double fwhmX, double fwhmY, double peak, double) { onFitResult (name, valid, fwhmX, fwhmY, peak); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::stackUpdated, this, [this, name] (int frames, double totalExposure, QString filter, QString calibration) { onStackUpdated (name, frames, totalExposure, filter, calibration); }, Qt::QueuedConnection);
	connect (camera, &StackCamera::stackMessage, this, [this, name] (QString message) { log (name + ": " + message); }, Qt::QueuedConnection);

	// Same startup-race replay as the viewer's onCameraCreated().
	{
		QMap<QString, double> initialValues;
		QMap<QString, QStringList> initialChoices;
		QMap<QString, QRect> initialRects;
		camera->snapshotValues (initialValues, initialChoices, initialRects);

		for (auto it = initialValues.constBegin (); it != initialValues.constEnd (); ++it)
			onValueUpdated (name, it.key (), it.value (), initialChoices.value (it.key ()));
		for (auto it = initialRects.constBegin (); it != initialRects.constEnd (); ++it)
			onRectangleUpdated (name, it.key (), it.value ().x (), it.value ().y (), it.value ().width (), it.value ().height ());
	}

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
	clientThread->requestShowStack (viewStackRadio->isChecked ());
	setWindowTitle (QString ("rts2-stacker - %1").arg (activeCamera));

	filterCombo->setEnabled (false);
	binningCombo->setEnabled (false);
	ccdSetSpin->setEnabled (false);
	coolingCheck->setEnabled (false);
	ccdTempLabel->setText ("...");
	statusFilterLabel->setText ("n/a");
	statusTempLabel->setText ("n/a");

	const CameraState &state = cameraStates.value (activeCamera);
	for (auto it = state.values.constBegin (); it != state.values.constEnd (); ++it)
		applyValueToWidgets (it.key (), it.value (), state.choices.value (it.key ()));

	updateStatusPanel ();
	updateStackPanel ();
	updateFitPanel ();
	updateExptimeChoices ();
	updateFlatStatus ();

	saveButton->blockSignals (true);
	saveButton->setChecked (state.saveEnabled);
	saveButton->blockSignals (false);
	updateSaveButton ();

	log ("switched to camera '" + activeCamera + "'");
}

void MainWindow::onImageReady (const QString &cameraName, QImage image)
{
	if (cameraName != activeCamera)
		return;
	canvas->setImage (image);
}

void MainWindow::onExposureStateChanged (const QString &cameraName, bool exposing)
{
	cameraStates[cameraName].exposing = exposing;

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

	if (valueName == "BINX" || valueName == "BINY")
		updateExptimeChoices ();
	else if (valueName == "filter")
		updateFlatStatus ();
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

	if (cameraName == activeCamera)
		updateStatusPanel ();
}

void MainWindow::onRectangleUpdated (const QString &cameraName, const QString &valueName, int x, int y, int w, int h)
{
	if (valueName == "SIZE")
	{
		cameraStates[cameraName].chipSize = QRect (x, y, w, h);
		if (cameraName == activeCamera)
		{
			updateExptimeChoices ();
			updateFlatStatus ();
		}
	}
	else if (valueName == "WINDOW")
		cameraStates[cameraName].lastWindow = QRect (x, y, w, h);
}

void MainWindow::onFitResult (const QString &cameraName, bool valid, double fwhmX, double fwhmY, double peak)
{
	CameraState &state = cameraStates[cameraName];
	state.fitValid = valid;
	state.fitFwhmX = fwhmX;
	state.fitFwhmY = fwhmY;
	state.fitPeak = peak;

	if (cameraName == activeCamera)
		updateFitPanel ();
}

void MainWindow::onStackUpdated (const QString &cameraName, int frames, double totalExposure, const QString &filter, const QString &calibration)
{
	CameraState &state = cameraStates[cameraName];
	state.stackFrames = frames;
	state.stackExposure = totalExposure;
	state.stackFilter = filter;
	state.stackCalibration = calibration;

	if (cameraName == activeCamera)
		updateStackPanel ();
}

void MainWindow::updateStackPanel ()
{
	const CameraState &state = cameraStates.value (activeCamera);
	stackFramesLabel->setText (QString::number (state.stackFrames));
	stackExposureLabel->setText (state.stackExposure < 600
		? QString ("%1 s").arg (state.stackExposure, 0, 'f', state.stackExposure < 10 ? 1 : 0)
		: formatDuration (state.stackExposure));
	stackFilterLabel->setText (state.stackFrames > 0 ? (state.stackFilter.isEmpty () ? QString ("(none)") : state.stackFilter) : QString ("-"));
	stackCalibLabel->setText (state.stackCalibration.isEmpty () ? QString ("-") : state.stackCalibration);
	resetButton->setEnabled (!activeCamera.isEmpty ());
}

void MainWindow::updateFitPanel ()
{
	const CameraState &state = cameraStates.value (activeCamera);
	if (!state.fitValid)
	{
		fwhmXLabel->setText ("n/a");
		fwhmYLabel->setText ("n/a");
		peakLabel->setText ("n/a");
		return;
	}
	fwhmXLabel->setText (QString ("%1 px").arg (state.fitFwhmX, 0, 'f', 2));
	fwhmYLabel->setText (QString ("%1 px").arg (state.fitFwhmY, 0, 'f', 2));
	peakLabel->setText (QString::number (state.fitPeak, 'f', 0));
}

void MainWindow::updateExptimeChoices ()
{
	if (calibDarks.isEmpty ())
		return;

	// Offer the exposure times whose dark matches what the camera will
	// read out now (full chip at the current binning); if the chip size
	// isn't known yet - or nothing matches because the reported SIZE
	// doesn't map onto the darks' dimensions - offer every dark there is,
	// and let StackCamera report a mismatch frame by frame.
	const CameraState &state = cameraStates.value (activeCamera);
	long expectW = 0, expectH = 0;
	if (state.chipSize.isValid ())
	{
		double bx = std::max (1.0, state.values.value ("BINX", 1.0));
		double by = std::max (1.0, state.values.value ("BINY", 1.0));
		expectW = std::lround (state.chipSize.width () / bx);
		expectH = std::lround (state.chipSize.height () / by);
	}

	auto collect = [this] (long w, long h)
	{
		QVector<double> times;
		for (const QVariant &v : calibDarks)
		{
			QVariantMap m = v.toMap ();
			if (w > 0 && (m["width"].toLongLong () != w || m["height"].toLongLong () != h))
				continue;
			double t = m["exptime"].toDouble ();
			if (std::none_of (times.begin (), times.end (), [t] (double o) { return sameExptime (o, t); }))
				times.append (t);
		}
		std::sort (times.begin (), times.end ());
		return times;
	};

	QVector<double> times = collect (expectW, expectH);
	if (times.isEmpty () && expectW > 0)
		times = collect (0, 0);

	double previous = exptimeCombo->count () > 0 ? exptimeCombo->currentData ().toDouble () : 1.0;

	exptimeCombo->blockSignals (true);
	exptimeCombo->clear ();
	int select = 0;
	double bestDiff = INFINITY;
	for (int i = 0; i < times.size (); i++)
	{
		exptimeCombo->addItem (QString ("%1 s").arg (times[i]), times[i]);
		double diff = std::abs (times[i] - previous);
		if (diff < bestDiff)
		{
			bestDiff = diff;
			select = i;
		}
	}
	exptimeCombo->setCurrentIndex (select);
	exptimeCombo->blockSignals (false);
}

void MainWindow::updateFlatStatus ()
{
	if (calibDarks.isEmpty ())
	{
		flatStatusLabel->setText ("n/a");
		flatStatusLabel->setStyleSheet ("");
		return;
	}
	const CameraState &state = cameraStates.value (activeCamera);
	QStringList opts = state.choices.value ("filter");
	int idx = (int) state.values.value ("filter", -1);
	if (idx < 0 || idx >= opts.size ())
	{
		flatStatusLabel->setText (calibFlats.isEmpty () ? "no flats" : "?");
		flatStatusLabel->setStyleSheet ("");
		return;
	}
	// Same exact-then-case-insensitive rule as CalibrationLibrary::findFlat().
	QString f = opts[idx];
	bool have = calibFlats.contains (f) || calibFlats.contains (f, Qt::CaseInsensitive);
	flatStatusLabel->setText (have ? QString ("%1: yes").arg (f) : QString ("%1: NONE - dark only").arg (f));
	flatStatusLabel->setStyleSheet (gui::statusStyle (have));
}

void MainWindow::updateStatusPanel ()
{
	if (activeCamera.isEmpty () || !cameraStates.contains (activeCamera))
		return;

	const CameraState &state = cameraStates[activeCamera];

	if (!state.stateText.isEmpty ())
	{
		statusStateLabel->setText (state.stateText);
		statusStateLabel->setStyleSheet (gui::statusStyle (!state.hasError));
	}

	double ccdTemp = state.values.value ("CCD_TEMP", NAN);
	double ccdSet = state.values.value ("CCD_SET", NAN);
	if (!std::isnan (ccdTemp))
	{
		QString text = QString ("%1 °C").arg (ccdTemp, 0, 'f', 1);
		if (!std::isnan (ccdSet))
		{
			text += QString (" / %1 °C").arg (ccdSet, 0, 'f', 1);
			statusTempLabel->setStyleSheet (gui::statusStyle (std::abs (ccdTemp - ccdSet) < 0.5));
		}
		statusTempLabel->setText (text);
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

	bool busy = !state.stateText.isEmpty () && state.stateText != "Idle" && !state.hasError;
	if (!busy || std::isnan (state.progressStart) || std::isnan (state.progressEnd) || state.progressEnd <= state.progressStart)
	{
		progressBar->setValue (0);
		progressBar->setFormat (busy ? (state.stateText.toLower () + "...") : "ready");
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
	auto applyCombo = [numericValue, &choices] (QComboBox *combo)
	{
		combo->blockSignals (true);
		if (!choices.isEmpty () && combo->count () != choices.size ())
		{
			combo->clear ();
			combo->addItems (choices);
		}
		combo->setCurrentIndex ((int) numericValue);
		combo->blockSignals (false);
		combo->setEnabled (true);
	};

	if (valueName == "filter")
		applyCombo (filterCombo);
	else if (valueName == "binning")
		applyCombo (binningCombo);
	else if (valueName == "CCD_TEMP")
		ccdTempLabel->setText (QString ("%1 °C").arg (numericValue, 0, 'f', 1));
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

double MainWindow::selectedExptime () const
{
	if (calibDarks.isEmpty ())
		return exptimeSpin->value ();
	return exptimeCombo->count () > 0 ? exptimeCombo->currentData ().toDouble () : NAN;
}

void MainWindow::sendFullChipWindow ()
{
	// The master darks are full-chip - make sure no subframe left over
	// from another client (rts2-viewer's windowing) is still configured.
	const CameraState &state = cameraStates.value (activeCamera);
	if (state.chipSize.isValid () && state.lastWindow != state.chipSize)
		clientThread->requestWindowChange (state.chipSize.x (), state.chipSize.y (), state.chipSize.width (), state.chipSize.height ());
}

void MainWindow::onExposeClicked ()
{
	if (runActive || activeCamera.isEmpty ())
		return;
	double exptime = selectedExptime ();
	if (std::isnan (exptime))
	{
		log ("no exposure time available (no master dark matches)");
		return;
	}
	sendFullChipWindow ();
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

	if (activeCamera.isEmpty () || std::isnan (selectedExptime ()))
		return;

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

void MainWindow::startNextRunExposure ()
{
	runIndex++;
	sendFullChipWindow ();
	// Read each time, so the exposure time can be changed mid-run - the
	// stack just sums on, each frame with its own dark.
	double exptime = selectedExptime ();
	log (QString ("run %1/%2: requesting %3 s exposure").arg (runIndex)
		.arg (runTotal == 0 ? QString ("∞") : QString::number (runTotal))
		.arg (exptime));
	clientThread->requestExposure (exptime);
}

void MainWindow::onResetClicked ()
{
	if (activeCamera.isEmpty ())
		return;
	clientThread->requestResetStack ();
}

void MainWindow::onViewToggled ()
{
	clientThread->requestShowStack (viewStackRadio->isChecked ());
}

void MainWindow::onSaveToggled (bool checked)
{
	updateSaveButton ();

	if (activeCamera.isEmpty ())
		return;

	cameraStates[activeCamera].saveEnabled = checked;
	log (checked ? "saving individual frames ON" : "saving individual frames OFF");
	clientThread->requestSaveToggle (checked);
}

void MainWindow::updateSaveButton ()
{
	bool enabled = saveButton->isChecked ();
	saveButton->setText (enabled ? "ON" : "OFF");
	saveButton->setStyleSheet (enabled ? gui::activeStyle () : QString ());
}

void MainWindow::onNightModeToggled (bool checked)
{
	gui::setNightMode (checked);
	QSettings ("rts2", "rts2-stacker").setValue ("nightMode", checked);

	// The palette covers ordinary widgets; the labels with their own
	// colours have to be redone.
	updateStatusPanel ();
	updateFlatStatus ();
	updateSaveButton ();
}

void MainWindow::onMeasureSizeChanged (int size)
{
	canvas->setMeasureSize (size);
}

void MainWindow::onZoomSpinChanged (double zoom)
{
	canvas->setZoom (zoom);
}

void MainWindow::onCanvasZoomChanged (double zoom)
{
	zoomSpin->setValue (zoom);
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
