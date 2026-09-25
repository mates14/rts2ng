#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QRadioButton>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QTimer>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cmath>

#include "stacker/stackclient.h"
#include "stacker/stackcamera.h"
#include "gui/imagecanvas.h"

namespace stacker
{

/** Per-camera cache, as in rts2-viewer's CameraState (gui/viewer/include/gui/mainwindow.h). */
struct CameraState
{
	QMap<QString, double> values;
	QMap<QString, QStringList> choices;
	bool exposing = false;
	double progressStart = NAN;
	double progressEnd = NAN;
	QString stateText;
	bool hasError = false;
	bool saveEnabled = false;

	QRect chipSize;
	QRect lastWindow;

	bool fitValid = false;
	double fitFwhmX = 0, fitFwhmY = 0, fitPeak = 0;

	int stackFrames = 0;
	double stackExposure = 0;
	QString stackFilter;
	QString stackCalibration;
};

/**
 * rts2-viewer's MainWindow (gui/viewer/src/mainwindow.cpp), cut down to
 * what a stacking demonstration needs: the focus graph, windowing and
 * shutter controls are gone (frames are always full-chip, so they match
 * the full-chip master darks), and a "Stack" panel shows how many frames
 * the sum holds, how it was calibrated, and resets (= saves) it.
 */
class MainWindow : public QMainWindow
{
	Q_OBJECT

	public:
		MainWindow (int argc, char **argv, QWidget *parent = nullptr);
		~MainWindow () override;

	private slots:
		void onCameraCreated (QString name, stacker::StackCamera *camera);
		void onCalibrationReady (QString dir, QVariantList darks, QStringList flats, QString error);
		void onExposeClicked ();
		void onRunStopClicked ();
		void onResetClicked ();
		void onViewToggled ();
		void onSaveToggled (bool checked);
		void onCameraComboChanged (int index);
		void onFilterComboChanged (int index);
		void onBinningComboChanged (int index);
		void onCcdSetChanged ();
		void onCoolingToggled (bool checked);
		void onMeasureSizeChanged (int size);
		void onZoomSpinChanged (double zoom);
		void onCanvasZoomChanged (double zoom);
		void onProgressTick ();

	private:
		void onImageReady (const QString &cameraName, QImage image);
		void onExposureStateChanged (const QString &cameraName, bool exposing);
		void onValueUpdated (const QString &cameraName, const QString &valueName, double numericValue, QStringList choices);
		void onProgressUpdated (const QString &cameraName, double start, double end);
		void onStateTextChanged (const QString &cameraName, const QString &stateText, bool hasError);
		void onRectangleUpdated (const QString &cameraName, const QString &valueName, int x, int y, int w, int h);
		void onFitResult (const QString &cameraName, bool valid, double fwhmX, double fwhmY, double peak);
		void onStackUpdated (const QString &cameraName, int frames, double totalExposure, const QString &filter, const QString &calibration);

		void applyValueToWidgets (const QString &valueName, double numericValue, const QStringList &choices);
		void updateStatusPanel ();
		void updateStackPanel ();
		void updateFitPanel ();
		void updateSaveButton ();
		void updateExptimeChoices ();
		void updateFlatStatus ();
		void sendFullChipWindow ();
		double selectedExptime () const;
		void startNextRunExposure ();
		void log (const QString &message);

		ClientThread *clientThread;
		QMap<QString, StackCamera *> cameras;
		QMap<QString, CameraState> cameraStates;
		QString activeCamera;

		bool runActive = false;
		int runTotal = 0;
		int runIndex = 0;

		QRect lastPushedMeasureRect;

		// Master darks as reported by StackClient::calibrationReady() -
		// empty (and exptimeSpin shown instead of exptimeCombo) when no
		// --calib was given.
		QVariantList calibDarks;
		QStringList calibFlats;

		gui::ImageCanvas *canvas;
		QPlainTextEdit *logView;

		QComboBox *cameraCombo;
		QComboBox *exptimeCombo;
		QDoubleSpinBox *exptimeSpin;
		QSpinBox *repeatSpin;
		QPushButton *exposeButton;
		QPushButton *runButton;
		QPushButton *saveButton;
		QProgressBar *progressBar;
		QLabel *elapsedLabel;
		QLabel *remainingLabel;

		QComboBox *filterCombo;
		QComboBox *binningCombo;
		QLabel *ccdTempLabel;
		QDoubleSpinBox *ccdSetSpin;
		QCheckBox *coolingCheck;

		// Stack panel
		QLabel *stackFramesLabel;
		QLabel *stackExposureLabel;
		QLabel *stackFilterLabel;
		QLabel *stackCalibLabel;
		QLabel *calibDirLabel;
		QLabel *flatStatusLabel;
		QRadioButton *viewStackRadio;
		QRadioButton *viewFrameRadio;
		QPushButton *resetButton;

		QDoubleSpinBox *zoomSpin;
		QSpinBox *measureSizeSpin;
		QLabel *fwhmXLabel;
		QLabel *fwhmYLabel;
		QLabel *peakLabel;

		QLabel *statusStateLabel;
		QLabel *statusTempLabel;
		QLabel *statusFilterLabel;

		QTimer *progressTimer;
};

}
