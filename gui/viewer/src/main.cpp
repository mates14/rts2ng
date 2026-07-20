#include "gui/mainwindow.h"

#include <QApplication>

int main (int argc, char **argv)
{
	QApplication app (argc, argv);

	gui::MainWindow window (argc, argv);
	window.show ();

	return app.exec ();
}
