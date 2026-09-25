#include "stacker/mainwindow.h"

#include <QApplication>
#include <clocale>

int main (int argc, char **argv)
{
	QApplication app (argc, argv);

	// See gui/viewer/src/main.cpp - keep rts2.ini float parsing locale-independent.
	setlocale (LC_NUMERIC, "C");

	stacker::MainWindow window (argc, argv);
	window.show ();

	return app.exec ();
}
