#include "gui/mainwindow.h"

#include <QApplication>
#include <clocale>

int main (int argc, char **argv)
{
	QApplication app (argc, argv);

	// QApplication's constructor calls setlocale(LC_ALL, "") to pick up the
	// user's locale for Qt's own text/number formatting. That also changes
	// numeric parsing (strtod) process-wide, which breaks the kernel's
	// iniparser (rts2.ini floats like "14.7813631") on any locale that uses
	// a decimal comma instead of a decimal point. Put LC_NUMERIC back to
	// "C" so config parsing stays locale-independent, without touching
	// Qt's other locale-driven behavior.
	setlocale (LC_NUMERIC, "C");

	gui::MainWindow window (argc, argv);
	window.show ();

	return app.exec ();
}
