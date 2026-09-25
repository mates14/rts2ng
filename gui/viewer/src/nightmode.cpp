#include "gui/nightmode.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace
{
	bool night = false;

	// What setNightMode (true) replaced, to put back on setNightMode (false).
	bool saved = false;
	QString savedStyle;
	QPalette savedPalette;

	// Kept dim on purpose - a bright red screen still costs dark adaptation.
	const QColor red (0xb0, 0x00, 0x00);
	const QColor brightRed (0xff, 0x30, 0x30);
	const QColor dimRed (0x50, 0x00, 0x00);
	const QColor black (0x00, 0x00, 0x00);

	QPalette nightPalette ()
	{
		QPalette p;
		p.setColor (QPalette::Window, black);
		p.setColor (QPalette::WindowText, red);
		p.setColor (QPalette::Base, QColor (0x22, 0x00, 0x00));  // lighter than Window, or empty check boxes vanish
		p.setColor (QPalette::AlternateBase, QColor (0x2a, 0x00, 0x00));
		p.setColor (QPalette::Text, red);
		p.setColor (QPalette::Button, QColor (0x1c, 0x00, 0x00));
		p.setColor (QPalette::ButtonText, red);
		p.setColor (QPalette::BrightText, brightRed);
		p.setColor (QPalette::ToolTipBase, black);
		p.setColor (QPalette::ToolTipText, red);
		p.setColor (QPalette::Highlight, QColor (0x60, 0x00, 0x00));
		p.setColor (QPalette::HighlightedText, brightRed);
		p.setColor (QPalette::Link, red);
		p.setColor (QPalette::PlaceholderText, dimRed);

		// Frames, group-box borders, bevels.
		p.setColor (QPalette::Light, QColor (0x40, 0x00, 0x00));
		p.setColor (QPalette::Midlight, QColor (0x30, 0x00, 0x00));
		p.setColor (QPalette::Mid, QColor (0x28, 0x00, 0x00));
		p.setColor (QPalette::Dark, QColor (0x18, 0x00, 0x00));
		p.setColor (QPalette::Shadow, black);

		for (auto role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText })
			p.setColor (QPalette::Disabled, role, dimRed);
		p.setColor (QPalette::Disabled, QPalette::Button, black);
		p.setColor (QPalette::Disabled, QPalette::Base, QColor (0x10, 0x00, 0x00));

		return p;
	}
}

bool gui::nightMode ()
{
	return night;
}

void gui::setNightMode (bool on)
{
	if (on == night)
		return;

	if (on)
	{
		if (!saved)
		{
			savedStyle = QApplication::style ()->objectName ();
			savedPalette = QApplication::palette ();
			saved = true;
		}
		QApplication::setStyle (QStyleFactory::create ("Fusion"));
		QApplication::setPalette (nightPalette ());
	}
	else if (saved)
	{
		QApplication::setStyle (QStyleFactory::create (savedStyle));
		QApplication::setPalette (savedPalette);
	}

	night = on;
}

QString gui::statusStyle (bool good)
{
	if (night)
		// Only red is available: fine is dim, trouble is a bright block.
		return good ? "background-color: #1a0000; color: #b00000;" : "background-color: #b00000; color: #000000;";
	return good ? "background-color: #CCFFCC;" : "background-color: #FFCCCC;";
}

QString gui::activeStyle ()
{
	return night ? "background-color: #600000; color: #ff3030;" : "background-color: #66CC66;";
}

QColor gui::nightText ()
{
	return red;
}
