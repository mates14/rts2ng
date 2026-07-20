#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QRect>

#include "gui/sightitem.h"

namespace gui
{

/**
 * Central image display: a QGraphicsView/Scene canvas modeled on
 * fiber_pointing_client.py's approach (QGraphicsPixmapItem + draggable
 * SightGraphicsItem-style boxes), rebuilt in Qt5/C++. Unlike that script,
 * pixel data arrives already flipped top-down
 * (rts2image::Image::getChannelGrayscaleImage's invert_y=true), so - unlike
 * fiber_pointing_client.py's `graphicsView.scale(1, -1)` - no extra
 * view-level Y flip is needed here.
 *
 * Two independent boxes sit on top of the image: the red "measure" cursor
 * (always present, used for the focus centroid/FWHM fit) and the blue
 * "window" cursor (hidden until Windowing is switched on, used to pick
 * the chip subframe for the next exposure). Both report their rects in
 * the same top-down display-pixel coordinates as the displayed QImage.
 */
class ImageCanvas : public QGraphicsView
{
	Q_OBJECT

	public:
		explicit ImageCanvas (QWidget *parent = nullptr);

		QRect measureRect () const;
		void setMeasureSize (int size);

		QRect windowRect () const;
		// width/height separately: MainWindow computes these from its own
		// "size" spinbox divided by the current binning factor (WINDOW is
		// always unbinned chip pixels, but this box is drawn in display
		// pixels - see MainWindow::updateWindowBoxSize()), so the two axes
		// can legitimately differ if BINX != BINY.
		void setWindowSize (int width, int height);
		void setWindowingEnabled (bool enabled);
		bool windowingEnabled () const;

		QImage currentImage () const { return m_currentImage; }

	public slots:
		void setImage (const QImage &image);

	private:
		QRect itemRect (SightItem *item) const;

		QGraphicsScene *m_scene;
		QGraphicsPixmapItem *m_pixmapItem;
		SightItem *m_crosshair;
		SightItem *m_windowItem;
		QImage m_currentImage;
};

}
