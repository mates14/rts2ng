#pragma once

#include <QColor>
#include <QString>
#include <QVector>

namespace gui
{

/**
 * One entry of the viewer's exposure-mode selector: what Expose (once)
 * and Run (repeatedly) do on the selected camera. The script is RTS2
 * script language, the same that rts2-scriptexec and the executor run
 * (base/script) - "E 10" a light frame, "D 10" a dark, "W1.NEON=on" a
 * value on another device, "sleep 2", "for 3 { ... }" - with $EXPTIME and
 * $CALTIME replaced by the viewer's two time inputs before it runs.
 *
 * Modes come from a site file (see loadModes()), so an instrument's
 * observing sequences are configuration, not a viewer of its own.
 * The user documentation is gui/viewer/viewer-scripting.md.
 */
struct ExposureMode
{
	QString label;
	/** Invalid colour = no colour of its own. */
	QColor color;
	QString script;
	/** The script takes darks ("D"), so the mode is shown as a warning. */
	bool takesDarks = false;
	/** The script uses $CALTIME, so the calibration time input matters. */
	bool usesCaltime = false;
};

/**
 * Light and dark, what the viewer did before modes existed - used when no
 * modes file is configured.
 */
QVector<ExposureMode> builtinModes ();

/**
 * Read a modes file:
 *
 *   # comment
 *   [Plain exposure]
 *   color = cyan
 *   script = E $EXPTIME
 *
 * One section per mode, in the order they appear in the selector. A line
 * starting with whitespace continues the value above it, so long scripts
 * can be split over lines. color is any Qt colour name or #rrggbb.
 *
 * Returns an empty vector and sets error when the file cannot be read or
 * is not valid.
 */
QVector<ExposureMode> loadModes (const QString &path, QString &error);

/**
 * The mode's script with $EXPTIME and $CALTIME filled in.
 */
QString expandModeScript (const ExposureMode &mode, double exptime, double caltime);

}
