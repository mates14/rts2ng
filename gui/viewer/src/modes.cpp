#include "gui/modes.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

using namespace gui;

namespace
{
	void classify (ExposureMode &mode)
	{
		mode.takesDarks = mode.script.contains (QRegularExpression ("(^|\\s)D\\s"));
		mode.usesCaltime = mode.script.contains ("$CALTIME");
	}
}

QVector<ExposureMode> gui::builtinModes ()
{
	ExposureMode light;
	light.label = "Light";
	light.script = "E $EXPTIME";
	classify (light);

	ExposureMode dark;
	dark.label = "Dark";
	dark.color = QColor ("red");
	dark.script = "D $EXPTIME";
	classify (dark);

	return { light, dark };
}

QVector<ExposureMode> gui::loadModes (const QString &path, QString &error)
{
	QFile file (path);
	if (!file.open (QIODevice::ReadOnly | QIODevice::Text))
	{
		error = QString ("cannot open modes file %1: %2").arg (path, file.errorString ());
		return {};
	}

	QVector<ExposureMode> modes;
	QString *lastValue = nullptr;
	int lineNumber = 0;
	QTextStream in (&file);
	while (!in.atEnd ())
	{
		QString line = in.readLine ();
		lineNumber++;
		QString trimmed = line.trimmed ();
		if (trimmed.isEmpty () || trimmed.startsWith ('#'))
			continue;

		auto fail = [&] (const QString &what)
		{
			error = QString ("%1:%2: %3").arg (path).arg (lineNumber).arg (what);
			return QVector<ExposureMode> ();
		};

		if (line[0].isSpace ())
		{
			if (!lastValue)
				return fail ("continuation line with nothing to continue");
			*lastValue += " " + trimmed;
			continue;
		}

		if (trimmed.startsWith ('['))
		{
			if (!trimmed.endsWith (']') || trimmed.size () < 3)
				return fail ("bad section header");
			ExposureMode mode;
			mode.label = trimmed.mid (1, trimmed.size () - 2).trimmed ();
			modes.push_back (mode);
			lastValue = nullptr;
			continue;
		}

		int eq = trimmed.indexOf ('=');
		if (eq < 0)
			return fail ("expected key = value");
		if (modes.isEmpty ())
			return fail ("key outside of a [mode] section");
		QString key = trimmed.left (eq).trimmed ();
		QString value = trimmed.mid (eq + 1).trimmed ();
		ExposureMode &mode = modes.back ();
		if (key == "color")
		{
			mode.color = QColor (value);
			if (!mode.color.isValid ())
				return fail ("unknown colour " + value);
			lastValue = nullptr;
		}
		else if (key == "script")
		{
			mode.script = value;
			lastValue = &mode.script;
		}
		else
		{
			return fail ("unknown key " + key);
		}
	}

	if (modes.isEmpty ())
	{
		error = path + ": no modes";
		return {};
	}

	// Checked here, not when the mode is used: a typo in a variable
	// would otherwise reach the script parser as a literal "$EXPTME".
	QRegularExpression var ("\\$([A-Za-z_]+)");
	for (ExposureMode &mode : modes)
	{
		if (mode.script.isEmpty ())
		{
			error = QString ("%1: mode '%2' has no script").arg (path, mode.label);
			return {};
		}
		// The script parser ends a script at the first '#' - with the
		// lines joined into one, a comment would silently drop every
		// command after it.
		if (mode.script.contains ('#'))
		{
			error = QString ("%1: mode '%2': no '#' comments inside a script, put them on lines of their own").arg (path, mode.label);
			return {};
		}
		auto it = var.globalMatch (mode.script);
		while (it.hasNext ())
		{
			QString name = it.next ().captured (1);
			if (name != "EXPTIME" && name != "CALTIME")
			{
				error = QString ("%1: mode '%2' uses unknown variable $%3 (only $EXPTIME and $CALTIME exist)").arg (path, mode.label, name);
				return {};
			}
		}
		classify (mode);
	}
	return modes;
}

QString gui::expandModeScript (const ExposureMode &mode, double exptime, double caltime)
{
	QString script = mode.script;
	script.replace ("$EXPTIME", QString::number (exptime, 'g', 10));
	script.replace ("$CALTIME", QString::number (caltime, 'g', 10));
	return script;
}
